#pragma once

// -----------------------------------------------------------------------------
// Original Honor HDRC-BV1 physical remote
// -----------------------------------------------------------------------------

// BLE Public Address of YOUR physical remote.
// Android: nRF Connect -> scan for HDRC-BV1.
// Linux: bluetoothctl -> scan on.
// iOS normally does not expose the real BLE MAC address to apps.
#define ORIGINAL_REMOTE_MAC "18:70:3B:76:B8:45"
#define ORIGINAL_REMOTE_NAME "HDRC-BV1"

// -----------------------------------------------------------------------------
// Optional Home Assistant / MQTT integration
// -----------------------------------------------------------------------------
//
// HomeKit works independently of MQTT.
//
// To use Home Assistant directly:
//   1. Set MQTT_ENABLED to 1.
//   2. Fill MQTT_HOST and optional username/password.
//   3. Rebuild and flash update.bin.
//   4. Home Assistant MQTT Discovery will create Power + remote buttons.
//
// Existing HomeKit, BLE bonds and Wi-Fi configuration are unaffected.

#define MQTT_ENABLED 0

#define MQTT_HOST ""
#define MQTT_PORT 1883
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""

#define MQTT_BASE_TOPIC "honor_x1"
#define MQTT_DISCOVERY_PREFIX "homeassistant"
#define MQTT_DEVICE_ID "honor_x1_bridge"
#define MQTT_CLIENT_ID "honor-x1-bridge"
