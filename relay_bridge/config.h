#pragma once

// Original Honor HDRC-BV1 physical remote.
//
// IMPORTANT:
// This must be the BLE Public Address of YOUR physical remote.
// If you replace the remote, update this value and rebuild the firmware.
//
// How to find it:
//   - Android: scan with nRF Connect / similar BLE scanner and look for HDRC-BV1.
//   - Linux: use bluetoothctl and scan while pressing a key / HOME+MENU.
//   - See README.md for details.
//
// iOS normally does not expose the real BLE MAC address to apps.
#define ORIGINAL_REMOTE_MAC "18:70:3B:76:B8:45"

// Advertising / local name used by the original physical remote.
#define ORIGINAL_REMOTE_NAME "HDRC-BV1"
