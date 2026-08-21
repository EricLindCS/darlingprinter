/*
  XIAO ESP32-C6 <-> CTP500 thermal printer bridge (cloud-connected)

  - First boot (or BOOT button held at power-up): opens its own AP
    "CTP500-Setup" with a captive-portal-style config page for WiFi +
    cloud API settings. Settings persist in flash (Preferences/NVS).
  - Normal boot: joins your saved WiFi, then polls your cloud API for
    queued print jobs and relays them to the printer over BLE.
  - The same web page (now served on your home WiFi's IP once connected)
    still has BLE scan/connect/select-characteristic tools, plus a local
    manual test-print panel - useful for pairing and debugging without
    involving the cloud at all.
  - BLE pairing info (MAC, service UUID, characteristic UUID) is saved
    to flash too, so a reboot doesn't require re-pairing.
  - The password field is never sent back to the browser (so it's not
    sitting in page source) - submitting the settings form with it left
    blank keeps the previously saved password rather than clearing it.

  Required libraries (Arduino Library Manager):
    - NimBLE-Arduino (h2zero)
  Built into the esp32 core (no install needed):
    - WiFi, WebServer, Preferences, DNSServer, HTTPClient, WiFiClientSecure

  Board: Seeed XIAO ESP32C6, esp32 core >= 3.0
*/

#include <WiFi.h>
#include <WebServer.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <vector>

// ---------------------------------------------------------------------
// CONSTANTS
// ---------------------------------------------------------------------
const int PRINTER_WIDTH_PX = 384;
const int BYTES_PER_ROW = PRINTER_WIDTH_PX / 8; // 48
const int BLE_MAX_CHUNK = 20;
const int BLE_CHUNK_DELAY_MS = 12;
const unsigned long STA_CONNECT_TIMEOUT_MS = 45000;
const unsigned long BLE_RETRY_INTERVAL_MS = 10000;
const size_t MAX_PRINT_BYTES = 100000;

// XIAO ESP32C6 BOOT button - hold during power-up to force setup mode
const int BOOT_BUTTON_PIN = 9;

// ---------------------------------------------------------------------
// PERSISTENT CONFIG
// ---------------------------------------------------------------------
Preferences prefs;

struct Config {
  String ssid, pass;
  String apiBase, deviceId, apiKey;
  String bleMac, bleSvc, bleChar;
  int pollSeconds = 5;
} cfg;

void loadConfig() {
  prefs.begin("ctp500", true);
  cfg.ssid       = prefs.getString("ssid", "");
  cfg.pass       = prefs.getString("pass", "");
  cfg.apiBase    = prefs.getString("apiBase", "darlingprinter.onrender.com/api/v1");
  cfg.deviceId   = prefs.getString("devId", "Siona");
  cfg.apiKey     = prefs.getString("apiKey", "2a92edb35301dbc4149c09af563ec95c1f456c0f07ec2f20");
  cfg.bleMac     = prefs.getString("bleMac", "03:4f:3b:05:83:df");
  cfg.bleSvc     = prefs.getString("bleSvc", "49535343-fe7d-4ae5-8fa9-9fafd205e455");
  cfg.bleChar    = prefs.getString("bleChar", "49535343-8841-43f4-a8d4-ecbe34729bb3");
  cfg.pollSeconds = prefs.getInt("pollSec", 5);


  prefs.end();


  Serial.println("========== PREFS ==========");
    Serial.printf("SSID:        %s\n", cfg.ssid.c_str());
    Serial.printf("Password:    %s\n", cfg.pass.c_str());
    Serial.printf("API Base:    %s\n", cfg.apiBase.c_str());
    Serial.printf("Device ID:   %s\n", cfg.deviceId.c_str());
    Serial.printf("API Key:     %s\n", cfg.apiKey.c_str());
    Serial.printf("BLE MAC:     %s\n", cfg.bleMac.c_str());
    Serial.printf("BLE Service: %s\n", cfg.bleSvc.c_str());
    Serial.printf("BLE Char:    %s\n", cfg.bleChar.c_str());
    Serial.printf("Poll Sec:    %d\n", cfg.pollSeconds);
    Serial.println("============================");
}

void saveNetworkConfig(const String &ssid, const String &pass, const String &apiBase,
                        const String &devId, const String &apiKey, int pollSec) {
  prefs.begin("ctp500", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("apiBase", apiBase);
  prefs.putString("devId", devId);
  prefs.putString("apiKey", apiKey);
  prefs.putInt("pollSec", pollSec);
  prefs.end();

  // Update the in-memory copy too so the running sketch reflects the
  // change immediately (matters mainly right before the reboot below).
  cfg.ssid = ssid; cfg.pass = pass; cfg.apiBase = apiBase;
  cfg.deviceId = devId; cfg.apiKey = apiKey; cfg.pollSeconds = pollSec;
}

void saveBleConfig(const String &mac, const String &svc, const String &chr) {
  prefs.begin("ctp500", false);
  prefs.putString("bleMac", mac);
  prefs.putString("bleSvc", svc);
  prefs.putString("bleChar", chr);
  prefs.end();
}

void clearConfig() {
  prefs.begin("ctp500", false);
  prefs.clear();
  prefs.end();
}

// ---------------------------------------------------------------------
// STATE
// ---------------------------------------------------------------------
WebServer server(80);
DNSServer dnsServer;
bool apModeActive = false;

NimBLEClient *bleClient = nullptr;
NimBLERemoteCharacteristic *writeChar = nullptr;
bool bleConnected = false;
String lastPrintError = "";
unsigned long lastBleAttemptMs = 0;
unsigned long lastPollMs = 0;
String lastPollStatus = "never polled";

// ---------------------------------------------------------------------
// BASE64 DECODE (self-contained - avoids relying on a library API that
// may differ across core versions)
// ---------------------------------------------------------------------
static int8_t b64Table[256];
void initBase64Table() {
  const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (int i = 0; i < 256; i++) b64Table[i] = -1;
  for (int i = 0; i < 64; i++) b64Table[(uint8_t)alphabet[i]] = (int8_t)i;
}

size_t base64Decode(const String &input, uint8_t *out, size_t outMax) {
  size_t o = 0;
  int val = 0, bits = -8;
  for (size_t i = 0; i < input.length(); i++) {
    uint8_t c = input[i];
    if (c == '=' ) break;
    int8_t d = b64Table[c];
    if (d < 0) continue; // skip whitespace/newlines
    val = (val << 6) + d;
    bits += 6;
    if (bits >= 0) {
      if (o >= outMax) break;
      out[o++] = (uint8_t)((val >> bits) & 0xFF);
      bits -= 8;
    }
  }
  return o;
}

// ---------------------------------------------------------------------
// TINY JSON FIELD EXTRACTION (server output format is fixed/simple, so
// this avoids pulling in ArduinoJson as a dependency)
// ---------------------------------------------------------------------
long extractJsonInt(const String &body, const String &key) {
  String needle = "\"" + key + "\":";
  int p = body.indexOf(needle);
  if (p < 0) return -1;
  int start = p + needle.length();
  int end = start;
  while (end < (int)body.length() && (isDigit(body[end]) || body[end] == '-')) end++;
  if (end == start) return -1;
  return body.substring(start, end).toInt();
}

String extractJsonString(const String &body, const String &key) {
  String needle = "\"" + key + "\":\"";
  int p = body.indexOf(needle);
  if (p < 0) return "";
  int start = p + needle.length();
  int end = body.indexOf('"', start);
  if (end < 0) return "";
  return body.substring(start, end);
}

// ---------------------------------------------------------------------
// BLE
// ---------------------------------------------------------------------

size_t effectiveChunkSize() {
  if (bleClient && bleClient->isConnected()) {
    uint16_t mtu = bleClient->getMTU();
    if (mtu > 3) return min((size_t)BLE_MAX_CHUNK, (size_t)(mtu - 3));
  }
  return 20;
}

String bleScanDevices(int seconds = 4) {
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults results = scan->getResults(seconds * 1000, false);

  String json = "[";
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice *d = results.getDevice(i);
    if (i > 0) json += ",";
    String name = d->getName().c_str();
    name.replace("\"", "'");
    json += "{\"name\":\"" + name + "\",\"address\":\"" +
            String(d->getAddress().toString().c_str()) + "\"}";
  }
  json += "]";
  scan->clearResults();
  return json;
}

bool bleConnectToPrinter(const String &mac) {
  if (bleClient && bleClient->isConnected()) bleClient->disconnect();
  if (!bleClient) bleClient = NimBLEDevice::createClient();

  Serial.printf("[BLE] Connecting to %s...\n", mac.c_str());
  NimBLEAddress publicAddr(std::string(mac.c_str()), BLE_ADDR_PUBLIC);
  bool ok = bleClient->connect(publicAddr, false);
  if (!ok) {
    NimBLEAddress randomAddr(std::string(mac.c_str()), BLE_ADDR_RANDOM);
    ok = bleClient->connect(randomAddr, false);
  }

  bleConnected = ok && bleClient->isConnected();
  writeChar = nullptr;
  if (bleConnected) {
    Serial.printf("[BLE] Connected. MTU=%u\n", bleClient->getMTU());
  }
  return bleConnected;
}

String bleDiscoverServices() {
  if (!bleClient || !bleClient->isConnected()) return "[]";
  const std::vector<NimBLERemoteService *> &services = bleClient->getServices(true);
  String json = "[";
  bool firstSvc = true;
  for (auto *svc : services) {
    if (!firstSvc) json += ",";
    firstSvc = false;
    json += "{\"service\":\"" + String(svc->getUUID().toString().c_str()) + "\",\"chars\":[";
    const std::vector<NimBLERemoteCharacteristic *> &chars = svc->getCharacteristics(true);
    bool firstChar = true;
    for (auto *ch : chars) {
      if (!firstChar) json += ",";
      firstChar = false;
      String props = "";
      if (ch->canWrite()) props += "W";
      if (ch->canWriteNoResponse()) props += "w";
      if (ch->canNotify()) props += "N";
      if (ch->canRead()) props += "R";
      json += "{\"uuid\":\"" + String(ch->getUUID().toString().c_str()) + "\",\"props\":\"" + props + "\"}";
    }
    json += "]}";
  }
  json += "]";
  return json;
}

bool bleSelectCharacteristic(const String &serviceUUID, const String &charUUID) {
  if (!bleClient || !bleClient->isConnected()) return false;
  NimBLERemoteService *svc = bleClient->getService(serviceUUID.c_str());
  if (!svc) return false;
  NimBLERemoteCharacteristic *ch = svc->getCharacteristic(charUUID.c_str());
  if (!ch) return false;
  if (!ch->canWrite() && !ch->canWriteNoResponse()) return false;
  writeChar = ch;
  return true;
}

bool bleSend(const uint8_t *data, size_t len) {
  if (!writeChar) { lastPrintError = "No BLE write characteristic selected"; return false; }
  size_t chunk = effectiveChunkSize();
  bool withResponse = writeChar->canWrite();
  size_t sent = 0;
  while (sent < len) {
    size_t n = min(chunk, len - sent);
    if (!writeChar->writeValue(data + sent, n, withResponse)) {
      lastPrintError = "BLE write failed at " + String((unsigned)sent) + "/" + String((unsigned)len);
      Serial.println(lastPrintError);
      return false;
    }
    sent += n;
    delay(BLE_CHUNK_DELAY_MS);
  }
  return true;
}

// Periodic, throttled auto-reconnect using saved pairing info - lets the
// bridge recover on its own if the printer was off or out of range.
void maintainBleConnection() {
  if (bleConnected && writeChar) return;
  if (cfg.bleMac.length() == 0) return;
  if (millis() - lastBleAttemptMs < BLE_RETRY_INTERVAL_MS) return;
  lastBleAttemptMs = millis();

  if (!bleConnected) {
    Serial.println("[BLE] Auto-reconnect attempt...");
    bleConnectToPrinter(cfg.bleMac);
  }
  if (bleConnected && !writeChar && cfg.bleSvc.length() && cfg.bleChar.length()) {
    bleSelectCharacteristic(cfg.bleSvc, cfg.bleChar);
  }
}

// ---------------------------------------------------------------------
// PRINTER PROTOCOL
// ---------------------------------------------------------------------

bool initializePrinter()      { const uint8_t c[] = {0x1b, 0x40}; return bleSend(c, sizeof(c)); }
bool sendStartPrintSequence() { const uint8_t c[] = {0x1d, 0x49, 0xf0, 0x19}; return bleSend(c, sizeof(c)); }
bool sendEndPrintSequence()   { const uint8_t c[] = {0x0a, 0x0a, 0x0a, 0x9a}; return bleSend(c, sizeof(c)); }

bool sendRaster(uint16_t width, uint16_t height, const uint8_t *bitmap) {
  uint16_t widthBytes = width / 8;
  uint8_t header[8] = {
    0x1d, 0x76, 0x30, 0x00,
    (uint8_t)(widthBytes & 0xff), (uint8_t)((widthBytes >> 8) & 0xff),
    (uint8_t)(height & 0xff),     (uint8_t)((height >> 8) & 0xff)
  };
  if (!bleSend(header, sizeof(header))) { lastPrintError = "raster header: " + lastPrintError; return false; }
  size_t total = (size_t)widthBytes * height;
  if (!bleSend(bitmap, total)) { lastPrintError = "bitmap data: " + lastPrintError; return false; }
  return true;
}

bool printBitmap(uint16_t height, const uint8_t *bitmap) {
  lastPrintError = "";
  if (!bleConnected || !bleClient || !bleClient->isConnected()) { lastPrintError = "Printer not connected"; return false; }
  if (!writeChar) { lastPrintError = "No BLE write characteristic selected"; return false; }
  if (height == 0) { lastPrintError = "Image height is zero"; return false; }

  if (!initializePrinter()) return false;
  delay(500);
  if (!sendStartPrintSequence()) return false;
  delay(500);
  if (!sendRaster(PRINTER_WIDTH_PX, height, bitmap)) return false;
  delay(500);
  if (!sendEndPrintSequence()) return false;

  Serial.println("[print] complete");
  return true;
}

// ---------------------------------------------------------------------
// CLOUD POLLING
// ---------------------------------------------------------------------

bool httpBeginAuto(HTTPClient &http, WiFiClientSecure &secureClient, const String &url) {
  if (url.startsWith("https://")) {
    secureClient.setInsecure(); // skips cert validation - fine for a hobby project;
                                 // pin a real CA for anything more sensitive
    return http.begin(secureClient, url);
  }
  return http.begin(url);
}

void ackJob(long jobId, bool ok, const String &err) {
  String url = cfg.apiBase + "/api/device/jobs/" + String(jobId) + "/ack";
  HTTPClient http;
  WiFiClientSecure secureClient;
  if (!httpBeginAuto(http, secureClient, url)) return;
  http.addHeader("X-Device-Id", cfg.deviceId);
  http.addHeader("X-Api-Key", cfg.apiKey);
  http.addHeader("Content-Type", "application/json");
  String errEsc = err; errEsc.replace("\"", "'");
  String payload = String("{\"status\":\"") + (ok ? "done" : "failed") + "\",\"error\":\"" + errEsc + "\"}";
  http.POST(payload);
  http.end();
}

void pollCloudIfDue() {
  if (cfg.apiBase.length() == 0 || cfg.deviceId.length() == 0 || cfg.apiKey.length() == 0) return;
  if (millis() - lastPollMs < (unsigned long)cfg.pollSeconds * 1000UL) return;
  lastPollMs = millis();

  if (!bleConnected || !writeChar) {
    lastPollStatus = "skipped - printer BLE not ready";
    return;
  }

  String url = cfg.apiBase + "/api/device/next-job";

  Serial.print("[cloud] GET ");
  Serial.println(url);

  HTTPClient http;
  WiFiClientSecure secureClient;
  if (!httpBeginAuto(http, secureClient, url)) { lastPollStatus = "http.begin failed"; return; }
  http.addHeader("X-Device-Id", cfg.deviceId);
  http.addHeader("X-Api-Key", cfg.apiKey);


  WiFiClient testClient;

  Serial.printf("[cloud] TCP test %s:3000...\n", "192.168.1.152");

  if (testClient.connect("192.168.1.152", 3000)) {
      Serial.println("[cloud] TCP OK");
      testClient.stop();
  } else {
      Serial.println("[cloud] TCP FAILED");
  }

  int code = http.GET();

  if (code == 204) { http.end(); lastPollStatus = "queue empty"; return; }
  if (code != 200) {
    lastPollStatus = "poll HTTP " + String(code);
    Serial.println("[cloud] " + lastPollStatus);
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  long jobId = extractJsonInt(body, "id");
  uint16_t height = (uint16_t)extractJsonInt(body, "height");
  String b64 = extractJsonString(body, "bitmap");

  if (jobId < 0 || height == 0 || b64.length() == 0) {
    lastPollStatus = "malformed job response";
    return;
  }

  size_t expected = (size_t)BYTES_PER_ROW * height;
  if (expected > MAX_PRINT_BYTES) {
    lastPollStatus = "job too large";
    ackJob(jobId, false, lastPollStatus);
    return;
  }

  std::vector<uint8_t> bitmap(expected);
  size_t decoded = base64Decode(b64, bitmap.data(), bitmap.size());
  if (decoded < expected) {
    lastPollStatus = "decode size mismatch";
    ackJob(jobId, false, lastPollStatus);
    return;
  }

  Serial.printf("[cloud] printing job #%ld (%u rows)\n", jobId, height);
  bool ok = printBitmap(height, bitmap.data());
  ackJob(jobId, ok, ok ? "" : lastPrintError);
  lastPollStatus = ok ? ("printed job #" + String(jobId)) : ("job #" + String(jobId) + " failed: " + lastPrintError);

  // Drain the queue faster than the normal interval if something was
  // just printed - there might be more waiting.
  if (ok) lastPollMs = millis() - (cfg.pollSeconds * 1000UL) + 1000;
}

// ---------------------------------------------------------------------
// WIFI MODES
// ---------------------------------------------------------------------

void startApMode() {
  apModeActive = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("CTP500-Setup", "");
  delay(200);
  IPAddress apIP = WiFi.softAPIP();
  dnsServer.start(53, "*", apIP);
  Serial.print("[setup] AP mode - join WiFi 'CTP500-Setup' then open http://");
  Serial.println(apIP);
}

// Add near the top with other globals
void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("[wifi] Disconnected, reason code: %d\n", info.wifi_sta_disconnected.reason);
  }
}

bool connectStaWithTimeout(unsigned long timeoutMs) {
    apModeActive = false;

    WiFi.mode(WIFI_STA);

    WiFi.disconnect(true);
    delay(1000);

    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());

    Serial.printf(
        "[wifi] Connecting to '%s' (password length: %d)...\n",
        cfg.ssid.c_str(),
        cfg.pass.length()
    );

    unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - start < timeoutMs) {
        delay(300);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf(
            "[wifi] Failed. WiFi.status() = %d\n",
            WiFi.status()
        );
    } else {
        Serial.print("[wifi] Connected! IP: ");
        Serial.println(WiFi.localIP());
    }

    return WiFi.status() == WL_CONNECTED;
}

// ---------------------------------------------------------------------
// WEB UI
// ---------------------------------------------------------------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CTP500 Bridge Setup</title>
<style>
body { font-family: system-ui, sans-serif; max-width: 520px; margin: auto; padding: 16px; background: #f4f4f4; }
section { background: white; border-radius: 10px; padding: 14px; margin-bottom: 16px; box-shadow: 0 1px 3px rgba(0,0,0,.12); }
button { padding: 10px 14px; margin: 4px 4px 4px 0; border: 0; border-radius: 6px; background: #333; color: white; cursor:pointer; }
button.danger { background: #a33; }
label { display:block; margin-top:8px; font-size: 13px; color:#444; }
input[type=text], input[type=password], input[type=number], select, textarea {
  width: 100%; box-sizing: border-box; padding: 8px; margin: 4px 0;
}
canvas { width: 100%; image-rendering: pixelated; border: 1px solid #ccc; background: white; }
#log { font-family: monospace; font-size: 12px; white-space: pre-wrap; background: #111; color: #0f0; padding: 8px; max-height: 160px; overflow: auto; border-radius:6px; }
.status { font-size: 13px; color:#555; }
.hint { font-size: 12px; color: #888; margin-top: -2px; }
</style>
</head>
<body>

<h2>Status</h2>
<section id="statusBox" class="status">Loading...</section>

<h2>Network &amp; Cloud</h2>
<section>
  <label>Home WiFi SSID</label>
  <input type="text" id="ssid">
  <label>Home WiFi Password</label>
  <input type="text" id="pass">
  <label>Cloud API base URL (e.g. https://print.example.com)</label>
  <input type="text" id="apiBase">
  <label>Device ID</label>
  <input type="text" id="devId">
  <label>Device API Key</label>
  <input type="text" id="apiKey">
  <label>Poll interval (seconds)</label>
  <input type="number" id="pollSec" value="5" min="2">
  <button onclick="saveNetwork()">Save &amp; Reboot</button>
  <button class="danger" onclick="resetConfig()">Reset All Settings</button>
</section>

<h2>Printer (Bluetooth)</h2>
<section>
  <input type="text" id="mac" placeholder="Printer MAC address">
  <button onclick="scanDevices()">Scan</button>
  <button onclick="connectPrinter()">Connect</button>
  <select id="scanResults"></select>
  <div class="status" id="bleStatus">Not connected</div>
  <div id="discoverBlock" style="display:none">
    <p class="status">Select the printer WRITE characteristic:</p>
    <select id="serviceSelect" onchange="fillChars()"></select>
    <select id="charSelect"></select>
    <button onclick="selectChar()">Use Characteristic</button>
  </div>
</section>

<h2>Manual Test Print</h2>
<section>
  <textarea id="textInput" rows="4" placeholder="Type something..."></textarea>
  <label>Font size</label>
  <input type="range" id="fontSize" min="12" max="48" value="28">
  <button onclick="printText()">Print Text</button>
  <br><br>
  <input type="file" id="imgFile" accept="image/*">
  <label>Threshold</label>
  <input type="range" id="threshold" min="0" max="255" value="128">
  <button onclick="printImage()">Print Image</button>
  <canvas id="canvas" width="384" height="80"></canvas>
</section>

<h2>Log</h2>
<div id="log"></div>

<script>
const PW = 384, BPR = 48;
function log(m){ const l=document.getElementById('log'); l.textContent += m + "\n"; l.scrollTop = l.scrollHeight; }

// Only fills a field on the very first load - after that, the field
// belongs to whatever the user is typing, and the periodic refresh
// below leaves it alone. (Previously this ran on every poll and
// clobbered in-progress typing every 5s.)
let formPopulated = false;
function setIfNotEditing(id, value){
  if (formPopulated) return;
  document.getElementById(id).value = value;
}

async function loadStatus(){
  try {
    const r = await fetch('/status');
    const j = await r.json();
    document.getElementById('statusBox').innerHTML =
      'Mode: ' + (j.apMode ? 'Setup AP' : 'Connected') + '<br>' +
      'WiFi: ' + (j.wifiConnected ? ('connected, IP ' + j.ip) : 'not connected') + '<br>' +
      'BLE: ' + (j.bleConnected ? 'connected' : 'not connected') + (j.charSelected ? ' (characteristic set)' : '') + '<br>' +
      'Last poll: ' + j.lastPollStatus;

    setIfNotEditing('ssid', j.ssid || '');
    setIfNotEditing('pass', j.pass || '');
    setIfNotEditing('apiBase', j.apiBase || '');
    setIfNotEditing('devId', j.deviceId || '');
    setIfNotEditing('apiKey', j.apiKey || '');
    setIfNotEditing('pollSec', j.pollSeconds || 5);
    setIfNotEditing('mac', j.bleMac || '');

    formPopulated = true;
  } catch(e) { log('status error: ' + e); }
}
loadStatus();
setInterval(loadStatus, 5000);

async function saveNetwork(){
  const body = new URLSearchParams({
    ssid: document.getElementById('ssid').value,
    pass: document.getElementById('pass').value,
    apiBase: document.getElementById('apiBase').value,
    devId: document.getElementById('devId').value,
    apiKey: document.getElementById('apiKey').value,
    pollSec: document.getElementById('pollSec').value
  });
  await fetch('/config/network', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body});
  log('Saved. Rebooting...');
  setTimeout(() => alert('Rebooting - reconnect to the printer\'s new network or check its new IP.'), 300);
}

async function resetConfig(){
  if (!confirm('This clears all saved settings. Continue?')) return;
  await fetch('/config/reset', {method:'POST'});
  log('Settings cleared. Rebooting into setup mode...');
}

async function scanDevices(){
  log('Scanning...');
  const r = await fetch('/ble/scan');
  const list = await r.json();
  const sel = document.getElementById('scanResults');
  sel.innerHTML = '';
  list.forEach(d => {
    const opt = document.createElement('option');
    opt.value = d.address;
    opt.textContent = (d.name || '(no name)') + ' - ' + d.address;
    sel.appendChild(opt);
  });
  sel.onchange = () => { document.getElementById('mac').value = sel.value; };
  log('Found ' + list.length + ' device(s)');
}

async function connectPrinter(){
  const mac = document.getElementById('mac').value.trim();
  log('Connecting to ' + mac + '...');
  const r = await fetch('/ble/connect', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'mac=' + encodeURIComponent(mac)});
  const j = await r.json();
  document.getElementById('bleStatus').textContent = j.connected ? 'Connected' : 'Connection failed';
  log(j.connected ? 'Connected' : 'Connection failed');
  if (j.connected) discover();
}

let servicesData = [];
async function discover(){
  const r = await fetch('/ble/services');
  servicesData = await r.json();
  const svcSel = document.getElementById('serviceSelect');
  svcSel.innerHTML = '';
  servicesData.forEach(s => {
    const opt = document.createElement('option');
    opt.value = s.service; opt.textContent = s.service;
    svcSel.appendChild(opt);
  });
  document.getElementById('discoverBlock').style.display = 'block';
  fillChars();
  log('Discovered ' + servicesData.length + ' service(s)');
}

function fillChars(){
  const svcUUID = document.getElementById('serviceSelect').value;
  const svc = servicesData.find(s => s.service === svcUUID);
  const chSel = document.getElementById('charSelect');
  chSel.innerHTML = '';
  if (!svc) return;
  svc.chars.forEach(c => {
    const opt = document.createElement('option');
    opt.value = c.uuid; opt.textContent = c.uuid + ' [' + c.props + ']';
    chSel.appendChild(opt);
  });
}

async function selectChar(){
  const service = document.getElementById('serviceSelect').value;
  const char = document.getElementById('charSelect').value;
  const r = await fetch('/ble/select', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'service='+encodeURIComponent(service)+'&char='+encodeURIComponent(char)});
  const j = await r.json();
  log(j.ok ? 'Write characteristic selected (saved)' : 'Failed to select characteristic');
}

function wrapText(ctx, text, maxWidth){
  const out = [];
  text.split(/\r?\n/).forEach(p => {
    if (p.length === 0) { out.push(''); return; }
    const words = p.split(/\s+/);
    let line = '';
    words.forEach(w => {
      const cand = line ? line + ' ' + w : w;
      if (ctx.measureText(cand).width <= maxWidth) line = cand;
      else { if (line) out.push(line); line = w; }
    });
    if (line) out.push(line);
  });
  return out;
}

function canvasToBitmap(canvas){
  const h = canvas.height;
  const ctx = canvas.getContext('2d');
  const img = ctx.getImageData(0, 0, PW, h);
  const bitmap = new Uint8Array(BPR * h);
  for (let y = 0; y < h; y++){
    for (let xB = 0; xB < BPR; xB++){
      let v = 0;
      for (let bit = 0; bit < 8; bit++){
        const x = xB*8+bit;
        const idx = (y*PW+x)*4;
        const lum = 0.299*img.data[idx]+0.587*img.data[idx+1]+0.114*img.data[idx+2];
        if (lum < 128) v |= (1 << (7-bit));
      }
      bitmap[y*BPR+xB] = v;
    }
  }
  return bitmap;
}

function bytesToHex(bytes){
  let s = '';
  for (let i=0;i<bytes.length;i++) s += bytes[i].toString(16).padStart(2,'0');
  return s;
}

async function sendPrint(bitmap, height){
  const CHUNK = 512;
  for (let off=0; off<bitmap.length; off+=CHUNK){
    const end = Math.min(off+CHUNK, bitmap.length);
    const chunk = bitmap.slice(off, end);
    const body = JSON.stringify({offset: off, height, data: bytesToHex(chunk)});
    const r = await fetch('/print/chunk', {method:'POST', headers:{'Content-Type':'application/json'}, body});
    const j = await r.json();
    if (!j.ok) throw new Error(j.error || 'chunk failed');
  }
  const r = await fetch('/print/finish', {method:'POST'});
  const j = await r.json();
  if (!j.ok) throw new Error(j.error || 'print failed');
  log('Print complete');
}

function printText(){
  const text = document.getElementById('textInput').value;
  if (!text.trim()) { alert('Type something first'); return; }
  const fontSize = parseInt(document.getElementById('fontSize').value, 10);
  const measure = document.createElement('canvas').getContext('2d');
  measure.font = fontSize + 'px monospace';
  const lines = wrapText(measure, text, PW - 8);
  const lineHeight = Math.ceil(fontSize * 1.25);
  const height = Math.max(lineHeight * lines.length + 12, 10);
  const canvas = document.getElementById('canvas');
  canvas.width = PW; canvas.height = height;
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = 'white'; ctx.fillRect(0,0,PW,height);
  ctx.fillStyle = 'black'; ctx.font = fontSize + 'px monospace'; ctx.textBaseline = 'top';
  lines.forEach((l,i) => ctx.fillText(l, 4, 6+i*lineHeight));
  sendPrint(canvasToBitmap(canvas), height).catch(e => { log('Print failed: '+e.message); alert(e.message); });
}

function printImage(){
  const file = document.getElementById('imgFile').files[0];
  if (!file) { alert('Pick an image first'); return; }
  const reader = new FileReader();
  reader.onload = e => {
    const img = new Image();
    img.onload = () => {
      const h = Math.max(1, Math.round(img.height * (PW/img.width)));
      const canvas = document.getElementById('canvas');
      canvas.width = PW; canvas.height = h;
      const ctx = canvas.getContext('2d');
      ctx.fillStyle = 'white'; ctx.fillRect(0,0,PW,h);
      ctx.drawImage(img, 0, 0, PW, h);
      sendPrint(canvasToBitmap(canvas), h).catch(err => { log('Print failed: '+err.message); alert(err.message); });
    };
    img.src = e.target.result;
  };
  reader.readAsDataURL(file);
}
</script>
</body>
</html>
)rawliteral";

// ---------------------------------------------------------------------
// WEB HANDLERS
// ---------------------------------------------------------------------

void sendJson(const String &json) { server.send(200, "application/json", json); }
void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }

void handleConfigNetwork() {
  String ssid    = server.arg("ssid");    ssid.trim();
  String pass    = server.arg("pass");    pass.trim();
  String apiBase = server.arg("apiBase"); apiBase.trim();
  String devId   = server.arg("devId");   devId.trim();
  String apiKey  = server.arg("apiKey");  apiKey.trim();
  int pollSec    = server.arg("pollSec").toInt();
  if (pollSec <= 0) pollSec = 5;

  saveNetworkConfig(ssid, pass, apiBase, devId, apiKey, pollSec);
  sendJson("{\"ok\":true}");
  delay(300);
  ESP.restart();
}

void handleConfigReset() {
  clearConfig();
  sendJson("{\"ok\":true}");
  delay(300);
  ESP.restart();
}

void handleBleScan() { sendJson(bleScanDevices()); }

void handleBleConnect() {
  String mac = server.arg("mac");
  bool ok = bleConnectToPrinter(mac);
  if (ok) cfg.bleMac = mac; // persisted once a characteristic is also chosen
  sendJson(String("{\"connected\":") + (ok ? "true" : "false") + "}");
}

void handleBleServices() { sendJson(bleDiscoverServices()); }

void handleBleSelect() {
  String svc = server.arg("service");
  String chr = server.arg("char");
  bool ok = bleSelectCharacteristic(svc, chr);
  if (ok) {
    cfg.bleSvc = svc;
    cfg.bleChar = chr;
    saveBleConfig(cfg.bleMac, cfg.bleSvc, cfg.bleChar);
  }
  sendJson(String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

// ---- Local manual print buffer (same chunked-JSON approach that works
// reliably, kept for pairing/testing without the cloud involved) ----
std::vector<uint8_t> printBuffer;
uint16_t printHeight = 0;

void handlePrintChunk() {
  String body = server.arg("plain");
  if (body.length() == 0) { sendJson("{\"ok\":false,\"error\":\"empty request\"}"); return; }

  int offsetPos = body.indexOf("\"offset\":");
  int heightPos = body.indexOf("\"height\":");
  int dataPos = body.indexOf("\"data\":\"");
  if (offsetPos < 0 || heightPos < 0 || dataPos < 0) {
    sendJson("{\"ok\":false,\"error\":\"bad chunk JSON\"}"); return;
  }

  int offsetStart = offsetPos + 9;
  int offsetEnd = body.indexOf(',', offsetStart);
  size_t offset = body.substring(offsetStart, offsetEnd).toInt();

  int heightStart = heightPos + 9;
  int heightEnd = body.indexOf(',', heightStart);
  if (heightEnd < 0) heightEnd = body.indexOf('}', heightStart);
  uint16_t height = (uint16_t)body.substring(heightStart, heightEnd).toInt();

  int dataStart = dataPos + 8;
  int dataEnd = body.indexOf('"', dataStart);
  if (dataEnd < 0) { sendJson("{\"ok\":false,\"error\":\"bad hex data\"}"); return; }
  String hex = body.substring(dataStart, dataEnd);
  if (hex.length() % 2 != 0) { sendJson("{\"ok\":false,\"error\":\"odd hex length\"}"); return; }

  if (offset == 0) {
    printHeight = height;
    size_t expected = (size_t)BYTES_PER_ROW * printHeight;
    if (printHeight == 0 || expected > MAX_PRINT_BYTES) {
      sendJson("{\"ok\":false,\"error\":\"invalid image size\"}"); return;
    }
    printBuffer.clear();
    printBuffer.resize(expected);
  }

  if (offset >= printBuffer.size() || offset + hex.length() / 2 > printBuffer.size()) {
    sendJson("{\"ok\":false,\"error\":\"chunk outside image\"}"); return;
  }

  for (size_t i = 0; i < hex.length(); i += 2) {
    char tmp[3] = { hex[i], hex[i+1], 0 };
    printBuffer[offset + i / 2] = (uint8_t)strtoul(tmp, nullptr, 16);
  }
  sendJson("{\"ok\":true}");
}

void handlePrintFinish() {
  size_t expected = (size_t)BYTES_PER_ROW * printHeight;
  if (printHeight == 0 || printBuffer.size() != expected) {
    sendJson("{\"ok\":false,\"error\":\"no complete image\"}"); return;
  }
  bool ok = printBitmap(printHeight, printBuffer.data());
  if (!ok) {
    String err = lastPrintError; err.replace("\"", "'");
    sendJson(String("{\"ok\":false,\"error\":\"") + err + "\"}");
    return;
  }
  sendJson("{\"ok\":true}");
  printBuffer.clear();
  printHeight = 0;
}

void handleStatus() {
  String json = "{";
  json += "\"apMode\":" + String(apModeActive ? "true" : "false") + ",";
  json += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"ip\":\"" + String(apModeActive ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
  json += "\"bleConnected\":" + String(bleConnected ? "true" : "false") + ",";
  json += "\"charSelected\":" + String(writeChar ? "true" : "false") + ",";
  String pollEsc = lastPollStatus; pollEsc.replace("\"", "'");
  json += "\"lastPollStatus\":\"" + pollEsc + "\",";
  json += "\"ssid\":\"" + cfg.ssid + "\",";
  json += "\"pass\":\"" + cfg.pass + "\",";
  json += "\"apiBase\":\"" + cfg.apiBase + "\",";
  json += "\"deviceId\":\"" + cfg.deviceId + "\",";
  json += "\"apiKey\":\"" + cfg.apiKey + "\",";
  json += "\"pollSeconds\":" + String(cfg.pollSeconds) + ",";
  json += "\"bleMac\":\"" + cfg.bleMac + "\"";
  json += "}";
  sendJson(json);
}

void setupRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config/network", HTTP_POST, handleConfigNetwork);
  server.on("/config/reset", HTTP_POST, handleConfigReset);
  server.on("/ble/scan", HTTP_GET, handleBleScan);
  server.on("/ble/connect", HTTP_POST, handleBleConnect);
  server.on("/ble/services", HTTP_GET, handleBleServices);
  server.on("/ble/select", HTTP_POST, handleBleSelect);
  server.on("/print/chunk", HTTP_POST, handlePrintChunk);
  server.on("/print/finish", HTTP_POST, handlePrintFinish);
  server.on("/status", HTTP_GET, handleStatus);
  // Captive portal helpers - many OSes probe these to detect a portal
  server.onNotFound(handleRoot);
}

// ---------------------------------------------------------------------
// SETUP / LOOP
// ---------------------------------------------------------------------

void setup() {

  prefs.begin("ctp500", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();



    Serial.begin(115200);
    delay(300);
    Serial.println("\nStarting CTP500 cloud bridge...");

    initBase64Table();
    loadConfig();

    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    bool forceSetup = (digitalRead(BOOT_BUTTON_PIN) == LOW);

    WiFi.setSleep(false);

    // -------------------------------------------------------------
    // WiFi setup
    // -------------------------------------------------------------
    if (cfg.ssid.length() == 0 || forceSetup) {
        if (forceSetup) {
            Serial.println("[setup] BOOT button held - forcing setup mode");
        }

        startApMode();

    } else if (!connectStaWithTimeout(STA_CONNECT_TIMEOUT_MS)) {
        Serial.println("[wifi] Connect failed - falling back to setup mode");
        startApMode();

    } else {
        Serial.print("[wifi] Connected. IP: ");
        Serial.println(WiFi.localIP());
    }

    // -------------------------------------------------------------
    // BLE
    // -------------------------------------------------------------
    NimBLEDevice::init("XIAO-CTP500-Bridge");

    // -------------------------------------------------------------
    // Web server
    // -------------------------------------------------------------
    setupRoutes();
    server.begin();

    lastPollMs = millis();

    Serial.println("Web server started.");

    if (!apModeActive && WiFi.status() == WL_CONNECTED) {
        if (cfg.bleMac.length() == 0 ||
            cfg.bleSvc.length() == 0 ||
            cfg.bleChar.length() == 0) {

            Serial.println("[setup] WiFi configured, but BLE is not configured.");
            Serial.print("[setup] Configure BLE at http://");
            Serial.print(WiFi.localIP());
            Serial.println("/");
        }
    }
}

void loop() {
    if (apModeActive) dnsServer.processNextRequest();
    server.handleClient();

    if (!apModeActive && WiFi.status() == WL_CONNECTED) {
        maintainBleConnection();
        pollCloudIfDue();
    }
}