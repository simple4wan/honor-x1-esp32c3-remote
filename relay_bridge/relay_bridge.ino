#include <Arduino.h>
#include <WiFi.h>

#include "HonorBleBridge.h"
#include "HomeKitAdapter.h"
#include "MqttAdapter.h"
#include "PhysicalRemote.h"

static void wifiEventLogger(arduino_event_id_t event,
                            arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.println("[WIFI] STA connected to AP");
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("[WIFI] GOT IP: %s\n",
                  WiFi.localIP().toString().c_str());
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("[WIFI] STA disconnected, reason=%u\n",
                  (unsigned)info.wifi_sta_disconnected.reason);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  WiFi.onEvent(wifiEventLogger);

  Serial.println();
  Serial.println("Honor X1 Smart Home BLE Bridge");

  // BLE peripheral for the TV.
  HonorBle::begin();

  // BLE central for the original HDRC-BV1 remote.
  PhysicalRemote::begin();

  // Apple HomeKit / Apple TV Remote.
  HomeKitAdapter::begin();

  // Optional direct Home Assistant integration.
  MqttAdapter::begin();

  Serial.println("[SYSTEM] Bridge ready");
}

void loop() {
  HomeKitAdapter::loop();
  HonorBle::loop();
  PhysicalRemote::loop();
  MqttAdapter::loop();
}
