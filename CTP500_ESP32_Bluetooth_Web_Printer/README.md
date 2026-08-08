# CTP500 Bluetooth-to-Wi-Fi bridge for XIAO ESP32-C6

This firmware turns a Seed Studio XIAO ESP32-C6 into a small bridge that:

- hosts its own Wi-Fi access point
- exposes a simple web interface at http://192.168.4.1/
- connects to a BLE printer and sends text to it

## What changed

The original desktop app used Bluetooth Classic RFCOMM. The XIAO ESP32-C6 does not support Bluetooth Classic, so this version uses BLE instead.

## Uploading

1. Open the `.ino` file in the Arduino IDE or VS Code with the Arduino extension.
2. Select board: `Seeed XIAO ESP32C6`.
3. Install these libraries:
   - `WiFi`
   - `WebServer`
   - `NimBLE-Arduino`
4. Upload the sketch.

## Using it

1. Power the XIAO ESP32-C6.
2. Connect to Wi-Fi access point `CTP500-Bridge` with password `12345678`.
3. Open `http://192.168.4.1/` in a browser.
4. Type text and press Print.

## Notes

If the printer does not advertise under the name `CTP500` or uses different BLE service/characteristic UUIDs, update these constants at the top of the sketch:

- `kPrinterName`
- `kPrinterServiceUuid`
- `kPrinterTxUuid`
