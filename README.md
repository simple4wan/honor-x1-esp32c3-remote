# Honor X1 ESP32-C3 BLE remote sniffer

Experimental ESP32-C3 firmware for inspecting the BLE HID remote used by Honor/Huawei Smart Screen X1.

Target remote observed in testing:

- BLE name: `HDRC-BV1`
- HID service: `0x1812`
- Report Map: `0x2A4B`
- Report: `0x2A4D`
- Report Reference descriptor: `0x2908`
- CCCD: `0x2902`

## What the firmware does

1. Scans for `HDRC-BV1`.
2. Connects as a BLE Central.
3. Requests BLE security/bonding using Just Works.
4. Finds the HID service.
5. Reads and prints the HID Report Map.
6. Enumerates all HID Report characteristics.
7. Reads their Report Reference descriptors.
8. Subscribes to notification-capable reports.
9. Prints raw report bytes when a remote key is pressed.

## Recommended test procedure

For the first test, unplug the TV from mains so it cannot compete for the remote connection.

Power the ESP32-C3, then wake the remote. If the remote refuses a new bonded host, put the remote into its pairing mode before retrying.

Serial log is normally on the board's standard ESP-IDF console at 115200 baud.

Look for output such as:

```
Found HDRC-BV1
Connected
HID service: ...
Report Map chunk: ...
Report[0] id=... type=...
REPORT handle=... id=... data=...
```

Press only the **power key** first and save the serial output.

## Web flashing

GitHub Actions produces:

`honor-x1-sniffer-esp32c3.factory.bin`

This is a merged image intended to be flashed at offset `0x0` with an ESP32-C3 compatible Web Serial flasher.

## Build

The repository builds automatically on push with ESP-IDF v5.5.5.

Local build, if needed:

```bash
idf.py set-target esp32c3
idf.py build
idf.py merge-bin -o honor-x1-sniffer-esp32c3.factory.bin -f raw
```
