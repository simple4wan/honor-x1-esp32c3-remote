#pragma once

#include <Arduino.h>

namespace HonorBle {

enum Key : uint8_t {
  BACK        = 0x29,
  HOME        = 0x4A,
  RIGHT       = 0x4F,
  LEFT        = 0x50,
  DOWN        = 0x51,
  UP          = 0x52,
  OK          = 0x58,
  POWER       = 0x66,
  VOICE       = 0x75,
  MENU        = 0x76,
  MUTE        = 0x7F,
  VOLUME_UP   = 0x80,
  VOLUME_DOWN = 0x81
};

using TvStateCallback = void (*)(bool connected);

void begin();
void loop();

bool isTvConnected();
bool hasTvBond();

void sendKey(uint8_t key);
void setPower(bool on);
void requestWakePulse();

// Used by PhysicalRemote to forward the original 8-byte HID report to the TV.
void relayRemoteReport(const uint8_t *data, size_t len);

void setTvStateCallback(TvStateCallback callback);

}  // namespace HonorBle
