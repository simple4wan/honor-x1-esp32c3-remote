# Honor X1 Apple Remote Bridge (ESP32-C3)

This project bridges the iPhone HomeKit / Control Center Television Remote to an Honor Smart Screen X1 over BLE HID.

Architecture:

```
iPhone Control Center Remote
        |
        | HomeKit / Wi-Fi
        v
     ESP32-C3
        |
        | BLE HID keyboard
        v
 Honor Smart Screen X1
```

## Captured HDRC-BV1 keys

| Function | HID key |
| --- | --- |
| Home | 0x4A |
| Power | 0x66 |
| Back | 0x29 |
| Voice trigger | 0x75 |
| Menu | 0x76 |
| Up | 0x52 |
| Down | 0x51 |
| Left | 0x50 |
| Right | 0x4F |
| OK | 0x58 |
| Volume + | 0x80 |
| Volume - | 0x81 |

The voice button also starts a vendor Report ID 0x5A audio stream on the original remote. Audio forwarding is not implemented yet.

## First setup

1. Flash `honor-x1-apple-remote-bridge.factory.bin` at offset `0x0`.
2. Open serial at 115200.
3. Configure HomeSpan Wi-Fi using its setup AP / serial CLI.
4. Add the accessory to Apple Home. HomeSpan's default setup code is `466-37-726`; change it after first setup.
5. On the Honor X1 Bluetooth settings, pair **Honor Remote Bridge** as a Bluetooth keyboard.
6. Open iPhone Control Center -> Remote and select the Honor X1 television accessory.

## Current mapping

- Arrow pad -> Honor arrow keys
- Center -> OK
- Back -> Back
- Play/Pause -> Home (temporary)
- Info -> Menu
- Volume -> Volume + / -
- HomeKit TV Active toggle -> Honor Power toggle

## Notes

The ESP32 is deliberately advertised to the TV as a normal BLE keyboard rather than cloning the HDRC-BV1 identity. This gives the best chance of keeping the original Honor remote paired at the same time.
