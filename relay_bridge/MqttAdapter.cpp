#include "MqttAdapter.h"

#include "HonorBleBridge.h"
#include "config.h"

#include <PubSubClient.h>
#include <WiFi.h>

namespace MqttAdapter {

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

static bool lastTvState = false;
static bool lastTvStateValid = false;
static uint32_t nextReconnectAt = 0;

static String topic(const char *suffix) {
  String value(MQTT_BASE_TOPIC);
  value += "/";
  value += suffix;
  return value;
}

static void publishState(bool force = false) {
  if (!mqtt.connected()) return;

  const bool state = HonorBle::isTvConnected();
  if (!force && lastTvStateValid && state == lastTvState) return;

  lastTvState = state;
  lastTvStateValid = true;

  mqtt.publish(topic("state").c_str(), state ? "ON" : "OFF", true);
}

static bool parseKey(const String& payload, uint8_t& key) {
  String value = payload;
  value.trim();
  value.toUpperCase();

  if (value == "UP") key = HonorBle::UP;
  else if (value == "DOWN") key = HonorBle::DOWN;
  else if (value == "LEFT") key = HonorBle::LEFT;
  else if (value == "RIGHT") key = HonorBle::RIGHT;
  else if (value == "OK") key = HonorBle::OK;
  else if (value == "BACK") key = HonorBle::BACK;
  else if (value == "HOME") key = HonorBle::HOME;
  else if (value == "MENU") key = HonorBle::MENU;
  else if (value == "POWER") key = HonorBle::POWER;
  else if (value == "MUTE") key = HonorBle::MUTE;
  else if (value == "VOLUME_UP" || value == "VOL_UP") key = HonorBle::VOLUME_UP;
  else if (value == "VOLUME_DOWN" || value == "VOL_DOWN") key = HonorBle::VOLUME_DOWN;
  else return false;

  return true;
}

static void onMessage(char *incomingTopic, byte *payload, unsigned int length) {
  String value;
  value.reserve(length);

  for (unsigned int i = 0; i < length; ++i) {
    value += (char)payload[i];
  }

  if (String(incomingTopic) == topic("power/set")) {
    value.trim();
    value.toUpperCase();

    if (value == "ON") HonorBle::setPower(true);
    else if (value == "OFF") HonorBle::setPower(false);

    return;
  }

  if (String(incomingTopic) == topic("key/set")) {
    uint8_t key = 0;
    if (parseKey(value, key)) {
      HonorBle::sendKey(key);
    } else {
      Serial.printf("[MQTT] Unknown key payload: %s\n", value.c_str());
    }
  }
}

static void publishDiscoveryButton(const char *objectId,
                                   const char *name,
                                   const char *payloadPress) {
  char discoveryTopic[160];
  snprintf(discoveryTopic, sizeof(discoveryTopic),
           "%s/button/%s/%s/config",
           MQTT_DISCOVERY_PREFIX,
           MQTT_DEVICE_ID,
           objectId);

  char payload[768];
  snprintf(payload, sizeof(payload),
           "{"
           "\"name\":\"%s\","
           "\"unique_id\":\"%s_%s\","
           "\"command_topic\":\"%s/key/set\","
           "\"payload_press\":\"%s\","
           "\"availability_topic\":\"%s/availability\","
           "\"payload_available\":\"online\","
           "\"payload_not_available\":\"offline\","
           "\"device\":{"
             "\"identifiers\":[\"%s\"],"
             "\"name\":\"Honor X1 BLE Bridge\","
             "\"manufacturer\":\"DIY / ESP32-C3\","
             "\"model\":\"Honor X1 Smart Home BLE Bridge\""
           "}"
           "}",
           name,
           MQTT_DEVICE_ID, objectId,
           MQTT_BASE_TOPIC,
           payloadPress,
           MQTT_BASE_TOPIC,
           MQTT_DEVICE_ID);

  mqtt.publish(discoveryTopic, payload, true);
}

static void publishDiscovery() {
  char discoveryTopic[160];
  snprintf(discoveryTopic, sizeof(discoveryTopic),
           "%s/switch/%s/power/config",
           MQTT_DISCOVERY_PREFIX,
           MQTT_DEVICE_ID);

  char payload[768];
  snprintf(payload, sizeof(payload),
           "{"
           "\"name\":\"Power\","
           "\"unique_id\":\"%s_power\","
           "\"command_topic\":\"%s/power/set\","
           "\"state_topic\":\"%s/state\","
           "\"payload_on\":\"ON\","
           "\"payload_off\":\"OFF\","
           "\"state_on\":\"ON\","
           "\"state_off\":\"OFF\","
           "\"availability_topic\":\"%s/availability\","
           "\"payload_available\":\"online\","
           "\"payload_not_available\":\"offline\","
           "\"device\":{"
             "\"identifiers\":[\"%s\"],"
             "\"name\":\"Honor X1 BLE Bridge\","
             "\"manufacturer\":\"DIY / ESP32-C3\","
             "\"model\":\"Honor X1 Smart Home BLE Bridge\""
           "}"
           "}",
           MQTT_DEVICE_ID,
           MQTT_BASE_TOPIC,
           MQTT_BASE_TOPIC,
           MQTT_BASE_TOPIC,
           MQTT_DEVICE_ID);

  mqtt.publish(discoveryTopic, payload, true);

  publishDiscoveryButton("up", "Up", "UP");
  publishDiscoveryButton("down", "Down", "DOWN");
  publishDiscoveryButton("left", "Left", "LEFT");
  publishDiscoveryButton("right", "Right", "RIGHT");
  publishDiscoveryButton("ok", "OK", "OK");
  publishDiscoveryButton("back", "Back", "BACK");
  publishDiscoveryButton("home", "Home", "HOME");
  publishDiscoveryButton("menu", "Menu", "MENU");
  publishDiscoveryButton("mute", "Mute", "MUTE");
  publishDiscoveryButton("volume_up", "Volume Up", "VOLUME_UP");
  publishDiscoveryButton("volume_down", "Volume Down", "VOLUME_DOWN");
}

static void connect() {
  if (!MQTT_ENABLED || mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (strlen(MQTT_HOST) == 0) return;

  const uint32_t now = millis();
  if (nextReconnectAt && (int32_t)(now - nextReconnectAt) < 0) return;

  String clientId = MQTT_CLIENT_ID;
  clientId += "-";
  clientId += String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF), HEX);

  const String availability = topic("availability");

  bool ok = false;

  if (strlen(MQTT_USERNAME) > 0) {
    ok = mqtt.connect(clientId.c_str(),
                      MQTT_USERNAME,
                      MQTT_PASSWORD,
                      availability.c_str(),
                      0,
                      true,
                      "offline");
  } else {
    ok = mqtt.connect(clientId.c_str(),
                      availability.c_str(),
                      0,
                      true,
                      "offline");
  }

  if (!ok) {
    Serial.printf("[MQTT] Connect failed state=%d\n", mqtt.state());
    nextReconnectAt = now + 5000;
    return;
  }

  nextReconnectAt = 0;

  mqtt.publish(availability.c_str(), "online", true);
  mqtt.subscribe(topic("power/set").c_str());
  mqtt.subscribe(topic("key/set").c_str());

  publishDiscovery();
  publishState(true);

  Serial.printf("[MQTT] Connected to %s:%u\n",
                MQTT_HOST, (unsigned)MQTT_PORT);
}

void begin() {
  if (!MQTT_ENABLED) {
    Serial.println("[MQTT] Disabled in config.h");
    return;
  }

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(1024);

  Serial.printf("[MQTT] Enabled broker=%s:%u base=%s\n",
                MQTT_HOST, (unsigned)MQTT_PORT, MQTT_BASE_TOPIC);
}

void loop() {
  if (!MQTT_ENABLED) return;

  connect();

  if (!mqtt.connected()) return;

  mqtt.loop();
  publishState();
}

}  // namespace MqttAdapter
