/*
  XIAO ESP32-C6 <-> CTP500 thermal printer bridge

  Adapted from the known-working CTP500 Python program:
  - Browser handles text/image rendering.
  - ESP32-C6 provides a Wi-Fi AP + web UI.
  - ESP32-C6 connects to the printer using BLE GATT.
  - Printer bytes follow the known-working Python protocol exactly:
      ESC @
      1D 49 F0 19
      GS v 0 raster header + bitmap
      0A 0A 0A 9A

  Required:
    - Seeed XIAO ESP32-C6
    - ESP32 Arduino core >= 3.x
    - NimBLE-Arduino by h2zero

  IMPORTANT:
    The C6 is BLE-only. It cannot use the Python program's
    Bluetooth Classic RFCOMM transport. The BLE characteristic
    selected in the web UI must be the printer's write characteristic.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <NimBLEDevice.h>
#include <vector>

const char *AP_SSID = "CTP500-Printer";
const char *AP_PASSWORD = "printit123";

String printerMac = "03:4f:3b:05:83:df";

const int PRINTER_WIDTH_PX = 384;
const int BYTES_PER_ROW = PRINTER_WIDTH_PX / 8;

// Keep this below the negotiated BLE payload size.
const int BLE_MAX_CHUNK = 20;
const int BLE_CHUNK_DELAY_MS = 12;

WebServer server(80);

NimBLEClient *bleClient = nullptr;
NimBLERemoteCharacteristic *writeChar = nullptr;
bool bleConnected = false;

String lastPrintError = "";

// ---------------------------------------------------------------------
// BLE
// ---------------------------------------------------------------------

size_t effectiveChunkSize() {
  if (bleClient && bleClient->isConnected()) {
    uint16_t mtu = bleClient->getMTU();
    if (mtu > 3) {
      size_t payload = mtu - 3;
      return min((size_t)BLE_MAX_CHUNK, payload);
    }
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

    json += "{\"name\":\"" + name +
            "\",\"address\":\"" +
            String(d->getAddress().toString().c_str()) +
            "\"}";
  }

  json += "]";
  scan->clearResults();
  return json;
}

bool bleConnectToPrinter(const String &mac) {
  if (bleClient && bleClient->isConnected()) {
    bleClient->disconnect();
  }

  if (!bleClient) {
    bleClient = NimBLEDevice::createClient();
  }

  Serial.printf("Connecting to %s...\n", mac.c_str());

  NimBLEAddress publicAddr(std::string(mac.c_str()), BLE_ADDR_PUBLIC);
  bool ok = bleClient->connect(publicAddr, false);

  if (!ok) {
    Serial.println("Public address failed; trying random address...");
    NimBLEAddress randomAddr(std::string(mac.c_str()), BLE_ADDR_RANDOM);
    ok = bleClient->connect(randomAddr, false);
  }

  bleConnected = ok && bleClient->isConnected();
  writeChar = nullptr;

  if (bleConnected) {
    Serial.printf(
      "Connected. MTU=%u, payload=%u\n",
      bleClient->getMTU(),
      (unsigned)effectiveChunkSize()
    );
  }

  return bleConnected;
}

String bleDiscoverServices() {
  if (!bleClient || !bleClient->isConnected()) return "[]";

  const std::vector<NimBLERemoteService *> &services =
      bleClient->getServices(true);

  String json = "[";
  bool firstService = true;

  for (auto *svc : services) {
    if (!firstService) json += ",";
    firstService = false;

    json += "{\"service\":\"" +
            String(svc->getUUID().toString().c_str()) +
            "\",\"chars\":[";

    const std::vector<NimBLERemoteCharacteristic *> &chars =
        svc->getCharacteristics(true);

    bool firstChar = true;

    for (auto *ch : chars) {
      if (!firstChar) json += ",";
      firstChar = false;

      String props = "";
      if (ch->canWrite()) props += "W";
      if (ch->canWriteNoResponse()) props += "w";
      if (ch->canNotify()) props += "N";
      if (ch->canRead()) props += "R";

      json += "{\"uuid\":\"" +
              String(ch->getUUID().toString().c_str()) +
              "\",\"props\":\"" + props + "\"}";
    }

    json += "]}";
  }

  json += "]";
  return json;
}

bool bleSelectCharacteristic(const String &serviceUUID,
                             const String &charUUID) {
  if (!bleClient || !bleClient->isConnected()) return false;

  NimBLERemoteService *svc =
      bleClient->getService(serviceUUID.c_str());

  if (!svc) return false;

  NimBLERemoteCharacteristic *ch =
      svc->getCharacteristic(charUUID.c_str());

  if (!ch) return false;

  if (!ch->canWrite() && !ch->canWriteNoResponse()) {
    return false;
  }

  writeChar = ch;
  return true;
}

// Send exactly the bytes supplied, broken into BLE-sized writes.
bool bleSend(const uint8_t *data, size_t len) {
  if (!writeChar) {
    lastPrintError = "No BLE write characteristic selected";
    return false;
  }

  size_t chunk = effectiveChunkSize();
  bool withResponse = writeChar->canWrite();

  size_t sent = 0;

  while (sent < len) {
    size_t n = min(chunk, len - sent);

    if (!writeChar->writeValue(data + sent, n, withResponse)) {
      lastPrintError =
          "BLE write failed at " +
          String((unsigned)sent) +
          "/" +
          String((unsigned)len);

      Serial.println(lastPrintError);
      return false;
    }

    sent += n;
    delay(BLE_CHUNK_DELAY_MS);
  }

  return true;
}

// ---------------------------------------------------------------------
// KNOWN-WORKING CTP500 PROTOCOL
// ---------------------------------------------------------------------

bool initializePrinter() {
  const uint8_t cmd[] = {0x1b, 0x40};

  Serial.println("Sending ESC @");

  return bleSend(cmd, sizeof(cmd));
}

bool sendStartPrintSequence() {
  const uint8_t cmd[] = {0x1d, 0x49, 0xf0, 0x19};

  Serial.println("Sending start sequence");

  return bleSend(cmd, sizeof(cmd));
}

bool sendEndPrintSequence() {
  const uint8_t cmd[] = {0x0a, 0x0a, 0x0a, 0x9a};

  Serial.println("Sending end sequence");

  return bleSend(cmd, sizeof(cmd));
}

/*
  This is the important part copied conceptually from the known-working
  Python printImage():

      1D 76 30 00
      xL xH
      yL yH
      bitmap

  width = 384 px = 48 bytes/row
*/
bool sendRaster(uint16_t width,
                uint16_t height,
                const uint8_t *bitmap) {

  uint16_t widthBytes = width / 8;

  uint8_t header[8] = {
    0x1d, 0x76, 0x30, 0x00,
    (uint8_t)(widthBytes & 0xff),
    (uint8_t)((widthBytes >> 8) & 0xff),
    (uint8_t)(height & 0xff),
    (uint8_t)((height >> 8) & 0xff)
  };

  Serial.printf(
      "Raster: %u x %u (%u bytes)\n",
      width,
      height,
      (unsigned)(widthBytes * height)
  );

  if (!bleSend(header, sizeof(header))) {
    lastPrintError = "Failed sending raster header: " + lastPrintError;
    return false;
  }

  size_t total = (size_t)widthBytes * height;

  if (!bleSend(bitmap, total)) {
    lastPrintError = "Failed sending bitmap: " + lastPrintError;
    return false;
  }

  return true;
}

bool printBitmap(uint16_t height, const uint8_t *bitmap) {
  lastPrintError = "";

  if (!bleConnected || !bleClient || !bleClient->isConnected()) {
    lastPrintError = "Printer is not connected";
    return false;
  }

  if (!writeChar) {
    lastPrintError = "No BLE write characteristic selected";
    return false;
  }

  if (height == 0) {
    lastPrintError = "Image height is zero";
    return false;
  }

  if (!initializePrinter()) return false;
  delay(500);

  if (!sendStartPrintSequence()) return false;
  delay(500);

  if (!sendRaster(PRINTER_WIDTH_PX, height, bitmap)) return false;
  delay(500);

  if (!sendEndPrintSequence()) return false;

  Serial.println("PRINT COMPLETE");
  return true;
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
<title>CTP500 Printer</title>

<style>
body {
  font-family: system-ui, sans-serif;
  max-width: 500px;
  margin: auto;
  padding: 16px;
  background: #f4f4f4;
}

section {
  background: white;
  border-radius: 10px;
  padding: 14px;
  margin-bottom: 16px;
  box-shadow: 0 1px 3px rgba(0,0,0,.12);
}

button {
  padding: 10px 14px;
  margin: 4px 4px 4px 0;
  border: 0;
  border-radius: 6px;
  background: #333;
  color: white;
}

button:disabled {
  background: #999;
}

textarea,
input[type=text],
select {
  width: 100%;
  box-sizing: border-box;
  padding: 8px;
  margin: 6px 0;
}

canvas {
  width: 100%;
  image-rendering: pixelated;
  border: 1px solid #ccc;
  background: white;
}

#log {
  font-family: monospace;
  font-size: 12px;
  white-space: pre-wrap;
  background: #111;
  color: #0f0;
  padding: 8px;
  max-height: 180px;
  overflow: auto;
}
</style>
</head>

<body>

<h2>1. Connect</h2>

<section>
<input type="text" id="mac" value="03:4f:3b:05:83:df">

<button onclick="scanDevices()">Scan</button>
<button onclick="connectPrinter()">Connect</button>

<select id="scanResults"></select>

<div id="bleStatus">Not connected</div>

<div id="discoverBlock" style="display:none">

<p>Select the printer WRITE characteristic:</p>

<select id="serviceSelect" onchange="fillChars()"></select>
<select id="charSelect"></select>

<button onclick="selectChar()">Use Characteristic</button>

</div>
</section>


<h2>2. Print Text</h2>

<section>

<textarea
  id="textInput"
  rows="5"
  placeholder="Type something..."
></textarea>

<label>
Font size:
<input
  type="range"
  id="fontSize"
  min="12"
  max="48"
  value="28"
>
</label>

<button onclick="printText()">Print Text</button>

</section>


<h2>3. Print Image</h2>

<section>

<input type="file" id="imgFile" accept="image/*">

<br>

<label>
Threshold:
<input
  type="range"
  id="threshold"
  min="0"
  max="255"
  value="128"
>
</label>

<br>

<button onclick="printImage()">Print Image</button>

</section>


<h2>Preview</h2>

<section>
<canvas id="canvas" width="384" height="100"></canvas>
</section>


<h2>Log</h2>
<div id="log"></div>


<script>

const PW = 384;
const BPR = 48;

function log(msg) {
  const l = document.getElementById("log");
  l.textContent += msg + "\\n";
  l.scrollTop = l.scrollHeight;
}


// ------------------------------------------------------------
// BLE
// ------------------------------------------------------------

async function scanDevices() {
  log("Scanning...");

  try {
    const r = await fetch("/ble/scan");
    const list = await r.json();

    const sel = document.getElementById("scanResults");
    sel.innerHTML = "";

    list.forEach(d => {
      const opt = document.createElement("option");

      opt.value = d.address;
      opt.textContent =
        (d.name || "(no name)") + " - " + d.address;

      sel.appendChild(opt);
    });

    sel.onchange = () => {
      document.getElementById("mac").value = sel.value;
    };

    log("Found " + list.length + " device(s)");

  } catch(e) {
    log("Scan error: " + e);
  }
}


async function connectPrinter() {

  const mac =
    document.getElementById("mac").value.trim();

  log("Connecting to " + mac + "...");

  try {

    const r = await fetch(
      "/ble/connect",
      {
        method: "POST",
        headers: {
          "Content-Type":
            "application/x-www-form-urlencoded"
        },
        body:
          "mac=" + encodeURIComponent(mac)
      }
    );

    const j = await r.json();

    if (j.connected) {

      document.getElementById("bleStatus")
        .textContent = "Connected";

      log("Connected");

      discover();

    } else {

      document.getElementById("bleStatus")
        .textContent = "Connection failed";

      log("Connection failed");
    }

  } catch(e) {
    log("Connection error: " + e);
  }
}


let servicesData = [];


async function discover() {

  const r = await fetch("/ble/services");
  servicesData = await r.json();

  const svcSel =
    document.getElementById("serviceSelect");

  svcSel.innerHTML = "";

  servicesData.forEach(s => {

    const opt =
      document.createElement("option");

    opt.value = s.service;
    opt.textContent = s.service;

    svcSel.appendChild(opt);
  });

  document.getElementById("discoverBlock")
    .style.display = "block";

  fillChars();

  log(
    "Discovered " +
    servicesData.length +
    " service(s)"
  );
}


function fillChars() {

  const svcUUID =
    document.getElementById("serviceSelect").value;

  const svc =
    servicesData.find(
      s => s.service === svcUUID
    );

  const chSel =
    document.getElementById("charSelect");

  chSel.innerHTML = "";

  if (!svc) return;

  svc.chars.forEach(c => {

    const opt =
      document.createElement("option");

    opt.value = c.uuid;

    opt.textContent =
      c.uuid + " [" + c.props + "]";

    chSel.appendChild(opt);
  });
}


async function selectChar() {

  const service =
    document.getElementById("serviceSelect").value;

  const char =
    document.getElementById("charSelect").value;

  const r = await fetch(
    "/ble/select",
    {
      method: "POST",
      headers: {
        "Content-Type":
          "application/x-www-form-urlencoded"
      },
      body:
        "service=" +
        encodeURIComponent(service) +
        "&char=" +
        encodeURIComponent(char)
    }
  );

  const j = await r.json();

  log(
    j.ok
      ? "Write characteristic selected"
      : "Failed to select characteristic"
  );
}


// ------------------------------------------------------------
// TEXT RENDERING
//
// Equivalent purpose to the Python create_text() +
// get_wrapped_text(), but performed in the browser.
// ------------------------------------------------------------

function wrapText(ctx, text, maxWidth) {

  const output = [];

  text.split(/\r?\n/).forEach(paragraph => {

    if (paragraph.length === 0) {
      output.push("");
      return;
    }

    const words = paragraph.split(/\s+/);
    let line = "";

    words.forEach(word => {

      const candidate =
        line ? line + " " + word : word;

      if (
        ctx.measureText(candidate).width
        <= maxWidth
      ) {

        line = candidate;

      } else {

        if (line) output.push(line);

        line = word;
      }
    });

    if (line) output.push(line);
  });

  return output;
}


function renderText(text) {

  const fontSize =
    parseInt(
      document.getElementById("fontSize").value,
      10
    );

  const measure =
    document.createElement("canvas")
      .getContext("2d");

  measure.font =
    fontSize + "px monospace";

  const lines =
    wrapText(
      measure,
      text,
      PW - 8
    );

  const lineHeight =
    Math.ceil(fontSize * 1.25);

  const height =
    Math.max(
      lines.length * lineHeight + 12,
      10
    );

  const canvas =
    document.getElementById("canvas");

  canvas.width = PW;
  canvas.height = height;

  const ctx =
    canvas.getContext("2d");

  ctx.fillStyle = "white";
  ctx.fillRect(0, 0, PW, height);

  ctx.fillStyle = "black";
  ctx.font =
    fontSize + "px monospace";
  ctx.textBaseline = "top";

  lines.forEach((line, i) => {

    ctx.fillText(
      line,
      4,
      6 + i * lineHeight
    );

  });

  return canvas;
}


// ------------------------------------------------------------
// CANVAS -> EXACT PRINTER BITMAP
//
// The Python program eventually sends a 1-bit bitmap.
// We reproduce that representation here.
//
// Printer expects:
// 384 px / 8 = 48 bytes per row.
// ------------------------------------------------------------

function canvasToBitmap(canvas) {

  const h = canvas.height;

  const ctx =
    canvas.getContext("2d");

  const img =
    ctx.getImageData(
      0,
      0,
      PW,
      h
    );

  const bitmap =
    new Uint8Array(
      BPR * h
    );

  for (let y = 0; y < h; y++) {

    for (
      let xByte = 0;
      xByte < BPR;
      xByte++
    ) {

      let value = 0;

      for (
        let bit = 0;
        bit < 8;
        bit++
      ) {

        const x =
          xByte * 8 + bit;

        const idx =
          (y * PW + x) * 4;

        const r = img.data[idx];
        const g = img.data[idx + 1];
        const b = img.data[idx + 2];

        // Black pixels become 1.
        const luminance =
          0.299 * r +
          0.587 * g +
          0.114 * b;

        const black =
          luminance < 128;

        if (black) {
          value |=
            (1 << (7 - bit));
        }
      }

      bitmap[
        y * BPR + xByte
      ] = value;
    }
  }

  return bitmap;
}


// ------------------------------------------------------------
// PRINT
//
// IMPORTANT: The HTTP request contains only a small JSON-ish
// request rather than a raw binary body. The ESP32 reconstructs
// the raster bytes itself.
//
// This avoids the Content-Length/raw-body problem from the
// previous sketch.
// ------------------------------------------------------------

function bytesToHex(bytes) {

  let s = "";

  for (let i = 0; i < bytes.length; i++) {

    s += bytes[i]
      .toString(16)
      .padStart(2, "0");
  }

  return s;
}


async function sendPrint(bitmap, height) {

  log(
    "Preparing " +
    height +
    " rows..."
  );

  // Send the bitmap in small HTTP chunks.
  // Each chunk contains at most 512 bitmap bytes,
  // encoded as hexadecimal.
  const CHUNK = 512;

  for (
    let offset = 0;
    offset < bitmap.length;
    offset += CHUNK
  ) {

    const end =
      Math.min(
        offset + CHUNK,
        bitmap.length
      );

    const chunk =
      bitmap.slice(offset, end);

    const body =
      JSON.stringify({
        offset: offset,
        height: height,
        data: bytesToHex(chunk)
      });

    const r =
      await fetch(
        "/print/chunk",
        {
          method: "POST",
          headers: {
            "Content-Type":
              "application/json"
          },
          body: body
        }
      );

    const j = await r.json();

    if (!j.ok) {
      throw new Error(
        j.error || "print chunk failed"
      );
    }

    log(
      "Uploaded " +
      end +
      "/" +
      bitmap.length +
      " bytes"
    );
  }

  const r =
    await fetch(
      "/print/finish",
      {
        method: "POST"
      }
    );

  const j =
    await r.json();

  if (!j.ok) {
    throw new Error(
      j.error || "printer failed"
    );
  }

  log("PRINT COMPLETE");
}


async function printText() {

  const text =
    document
      .getElementById("textInput")
      .value;

  if (!text.trim()) {
    alert("Type something first");
    return;
  }

  try {

    const canvas =
      renderText(text);

    const bitmap =
      canvasToBitmap(canvas);

    await sendPrint(
      bitmap,
      canvas.height
    );

  } catch(e) {

    log(
      "Print failed: " + e.message
    );

    alert(
      "Print failed: " + e.message
    );
  }
}


function printImage() {

  const file =
    document
      .getElementById("imgFile")
      .files[0];

  if (!file) {
    alert("Pick an image first");
    return;
  }

  const threshold =
    parseInt(
      document
        .getElementById("threshold")
        .value,
      10
    );

  const reader =
    new FileReader();

  reader.onload = e => {

    const img =
      new Image();

    img.onload = async () => {

      try {

        const h =
          Math.max(
            1,
            Math.round(
              img.height *
              (PW / img.width)
            )
          );

        const canvas =
          document
            .getElementById("canvas");

        canvas.width = PW;
        canvas.height = h;

        const ctx =
          canvas.getContext("2d");

        ctx.fillStyle = "white";
        ctx.fillRect(
          0,
          0,
          PW,
          h
        );

        ctx.drawImage(
          img,
          0,
          0,
          PW,
          h
        );

        // Apply threshold explicitly.
        const imageData =
          ctx.getImageData(
            0,
            0,
            PW,
            h
          );

        for (
          let i = 0;
          i < imageData.data.length;
          i += 4
        ) {

          const lum =
            0.299 * imageData.data[i] +
            0.587 * imageData.data[i + 1] +
            0.114 * imageData.data[i + 2];

          const v =
            lum < threshold
              ? 0
              : 255;

          imageData.data[i] = v;
          imageData.data[i + 1] = v;
          imageData.data[i + 2] = v;
          imageData.data[i + 3] = 255;
        }

        ctx.putImageData(
          imageData,
          0,
          0
        );

        const bitmap =
          canvasToBitmap(canvas);

        await sendPrint(
          bitmap,
          h
        );

      } catch(e) {

        log(
          "Print failed: " + e.message
        );

        alert(
          "Print failed: " + e.message
        );
      }
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

void sendJson(const String &json) {
  server.send(
      200,
      "application/json",
      json
  );
}

void handleRoot() {
  server.send_P(
      200,
      "text/html",
      INDEX_HTML
  );
}

void handleBleScan() {
  sendJson(bleScanDevices());
}

void handleBleConnect() {

  String mac =
      server.arg("mac");

  bool ok =
      bleConnectToPrinter(mac);

  sendJson(
      String("{\"connected\":") +
      (ok ? "true" : "false") +
      "}"
  );
}

void handleBleServices() {
  sendJson(
      bleDiscoverServices()
  );
}

void handleBleSelect() {

  String svc =
      server.arg("service");

  String chr =
      server.arg("char");

  bool ok =
      bleSelectCharacteristic(
          svc,
          chr
      );

  sendJson(
      String("{\"ok\":") +
      (ok ? "true" : "false") +
      "}"
  );
}

// ---------------------------------------------------------------------
// PRINT BUFFER
// ---------------------------------------------------------------------

std::vector<uint8_t> printBuffer;
uint16_t printHeight = 0;

void handlePrintChunk() {

  String body =
      server.arg("plain");

  if (body.length() == 0) {
    sendJson(
      "{\"ok\":false,\"error\":\"empty request\"}"
    );
    return;
  }

  // Simple extraction of offset, height and data.
  int offsetPos =
      body.indexOf("\"offset\":");

  int heightPos =
      body.indexOf("\"height\":");

  int dataPos =
      body.indexOf("\"data\":\"");

  if (
      offsetPos < 0 ||
      heightPos < 0 ||
      dataPos < 0
  ) {
    sendJson(
      "{\"ok\":false,\"error\":\"bad chunk JSON\"}"
    );
    return;
  }

  int offsetStart =
      offsetPos + 9;

  int offsetEnd =
      body.indexOf(
          ',',
          offsetStart
      );

  size_t offset =
      body.substring(
          offsetStart,
          offsetEnd
      ).toInt();

  int heightStart =
      heightPos + 9;

  int heightEnd =
      body.indexOf(
          ',',
          heightStart
      );

  if (heightEnd < 0) {
    heightEnd =
        body.indexOf(
            '}',
            heightStart
        );
  }

  uint16_t height =
      (uint16_t)body.substring(
          heightStart,
          heightEnd
      ).toInt();

  int dataStart =
      dataPos + 8;

  int dataEnd =
      body.indexOf(
          '"',
          dataStart
      );

  if (dataEnd < 0) {
    sendJson(
      "{\"ok\":false,\"error\":\"bad hex data\"}"
    );
    return;
  }

  String hex =
      body.substring(
          dataStart,
          dataEnd
      );

  if (hex.length() % 2 != 0) {
    sendJson(
      "{\"ok\":false,\"error\":\"odd hex length\"}"
    );
    return;
  }

  // Start a new image whenever offset == 0.
  if (offset == 0) {

    printHeight = height;

    size_t expected =
        (size_t)BYTES_PER_ROW *
        printHeight;

    if (
        printHeight == 0 ||
        expected > 100000
    ) {
      sendJson(
        "{\"ok\":false,\"error\":\"invalid image size\"}"
      );
      return;
    }

    printBuffer.clear();
    printBuffer.resize(expected);
  }

  if (
      offset >= printBuffer.size() ||
      offset + hex.length() / 2 >
          printBuffer.size()
  ) {
    sendJson(
      "{\"ok\":false,\"error\":\"chunk outside image\"}"
    );
    return;
  }

  for (
      size_t i = 0;
      i < hex.length();
      i += 2
  ) {

    char tmp[3];

    tmp[0] = hex[i];
    tmp[1] = hex[i + 1];
    tmp[2] = 0;

    printBuffer[
        offset + i / 2
    ] =
        (uint8_t)strtoul(
            tmp,
            nullptr,
            16
        );
  }

  sendJson(
      "{\"ok\":true}"
  );
}

void handlePrintFinish() {

  size_t expected =
      (size_t)BYTES_PER_ROW *
      printHeight;

  if (
      printHeight == 0 ||
      printBuffer.size() != expected
  ) {
    sendJson(
      "{\"ok\":false,\"error\":\"no complete image\"}"
    );
    return;
  }

  Serial.printf(
      "Printing %u rows (%u bytes)\n",
      printHeight,
      (unsigned)printBuffer.size()
  );

  bool ok =
      printBitmap(
          printHeight,
          printBuffer.data()
      );

  if (!ok) {

    String err =
        lastPrintError;

    err.replace("\"", "'");

    sendJson(
      String("{\"ok\":false,\"error\":\"") +
      err +
      "\"}"
    );

    return;
  }

  sendJson(
      "{\"ok\":true}"
  );

  printBuffer.clear();
  printHeight = 0;
}

void handleStatus() {

  String json =
      "{\"wifi\":true,\"ble_connected\":";

  json +=
      bleConnected
        ? "true"
        : "false";

  json +=
      ",\"char_selected\":";

  json +=
      writeChar
        ? "true"
        : "false";

  json += "}";

  sendJson(json);
}

// ---------------------------------------------------------------------
// SETUP / LOOP
// ---------------------------------------------------------------------

void setup() {

  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println(
      "Starting CTP500 XIAO C6 bridge..."
  );

  WiFi.mode(WIFI_AP);

  WiFi.softAP(
      AP_SSID,
      AP_PASSWORD
  );

  Serial.print(
      "AP IP: "
  );

  Serial.println(
      WiFi.softAPIP()
  );

  NimBLEDevice::init(
      "XIAO-CTP500-Bridge"
  );

  server.on(
      "/",
      HTTP_GET,
      handleRoot
  );

  server.on(
      "/ble/scan",
      HTTP_GET,
      handleBleScan
  );

  server.on(
      "/ble/connect",
      HTTP_POST,
      handleBleConnect
  );

  server.on(
      "/ble/services",
      HTTP_GET,
      handleBleServices
  );

  server.on(
      "/ble/select",
      HTTP_POST,
      handleBleSelect
  );

  server.on(
      "/print/chunk",
      HTTP_POST,
      handlePrintChunk
  );

  server.on(
      "/print/finish",
      HTTP_POST,
      handlePrintFinish
  );

  server.on(
      "/status",
      HTTP_GET,
      handleStatus
  );

  server.begin();

  Serial.println(
      "Web server started."
  );

  Serial.println(
      "Connect to Wi-Fi: CTP500-Printer"
  );

  Serial.println(
      "Open: http://192.168.4.1"
  );
}

void loop() {
  server.handleClient();
}
