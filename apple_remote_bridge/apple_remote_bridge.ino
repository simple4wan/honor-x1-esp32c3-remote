#include <Arduino.h>
#include "HomeSpan.h"
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

static void wifiEventLogger(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.println("[WIFI] STA connected to AP");
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("[WIFI] GOT IP: %s\n", WiFi.localIP().toString().c_str());
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("[WIFI] STA disconnected, reason=%u\n",
                  (unsigned)info.wifi_sta_disconnected.reason);
  }
}

// iPhone HomeKit Television -> Honor X1 BLE HID bridge.
// The TV should pair this ESP32-C3 as a normal BLE keyboard, so the original
// HDRC-BV1 remote can remain paired independently.

static NimBLEHIDDevice *hidDevice = nullptr;
static NimBLECharacteristic *inputReport = nullptr;
static bool bleConnected = false;

// Exact keyboard-style report format observed from HDRC-BV1:
// [modifier, reserved, key1, key2, key3, key4, key5, key6]
static const uint8_t reportMap[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x06,        // Usage (Keyboard)
  0xA1, 0x01,        // Collection (Application)
  0x05, 0x07,        // Usage Page (Keyboard/Keypad)
  0x85, 0x01,        // Report ID (1)
  0x95, 0x08,        // Report Count (8)
  0x75, 0x08,        // Report Size (8)
  0x15, 0x00,        // Logical Minimum (0)
  0x25, 0xFF,        // Logical Maximum (255)
  0x19, 0x00,        // Usage Minimum (0)
  0x29, 0xFF,        // Usage Maximum (255)
  0x81, 0x00,        // Input (Data,Array,Abs)
  0xC0               // End Collection
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo& connInfo) override {
    bleConnected = true;
    Serial.printf("[BLE] TV connected: %s\n", connInfo.getAddress().toString().c_str());
  }

  void onDisconnect(NimBLEServer *server, NimBLEConnInfo& connInfo, int reason) override {
    bleConnected = false;
    Serial.printf("[BLE] TV disconnected, reason=%d\n", reason);
    NimBLEDevice::getAdvertising()->start();
  }
};

static void sendHonorKey(uint8_t key) {
  if (!bleConnected || !inputReport) {
    Serial.printf("[BLE] Not connected; key 0x%02X ignored\n", key);
    return;
  }

  uint8_t press[8] = {0, 0, key, 0, 0, 0, 0, 0};
  uint8_t release[8] = {0};

  inputReport->setValue(press, sizeof(press));
  inputReport->notify();
  delay(90);

  inputReport->setValue(release, sizeof(release));
  inputReport->notify();

  Serial.printf("[BLE] Sent Honor key 0x%02X\n", key);
}

static void setupHonorBleKeyboard() {
  NimBLEDevice::init("HDRC-BV1");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // Bonding + Secure Connections, no MITM/passkey UI.
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  hidDevice = new NimBLEHIDDevice(server);
  inputReport = hidDevice->getInputReport(1);
  hidDevice->setManufacturer("ESP32 Honor Bridge");
  hidDevice->setPnp(0x02, 0x05AC, 0x0220, 0x0100);
  hidDevice->setHidInfo(0x00, 0x02);
  hidDevice->setReportMap((uint8_t*)reportMap, sizeof(reportMap));
  hidDevice->setBatteryLevel(100);

  server->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName("HDRC-BV1");
  adv->addServiceUUID(hidDevice->getHidService()->getUUID());
  adv->setAppearance(HID_KEYBOARD);
  adv->enableScanResponse(true);
  adv->start();

  Serial.println("[BLE] Advertising as 'HDRC-BV1' HID keyboard");
}

// Values captured from the original HDRC-BV1.
enum HonorKey : uint8_t {
  HONOR_BACK       = 0x29,
  HONOR_HOME       = 0x4A,
  HONOR_RIGHT      = 0x4F,
  HONOR_LEFT       = 0x50,
  HONOR_DOWN       = 0x51,
  HONOR_UP         = 0x52,
  HONOR_OK         = 0x58,
  HONOR_POWER      = 0x66,
  HONOR_VOICE      = 0x75,
  HONOR_MENU       = 0x76,
  HONOR_MUTE       = 0x7F,
  HONOR_VOLUME_UP  = 0x80,
  HONOR_VOLUME_DOWN= 0x81
};

struct HonorTelevision : Service::Television {
  SpanCharacteristic *active;
  SpanCharacteristic *remoteKey;
  SpanCharacteristic *configuredName;

  HonorTelevision(const char *name) : Service::Television() {
    active = new Characteristic::Active(0);
    configuredName = new Characteristic::ConfiguredName(name);
    remoteKey = new Characteristic::RemoteKey();
  }

  boolean update() override {
    if (active->updated()) {
      Serial.printf("[HK] Power requested: %s\n", active->getNewVal() ? "ON" : "OFF");
      // Honor remote uses the same toggle Power key for on/off.
      sendHonorKey(HONOR_POWER);
    }

    if (remoteKey->updated()) {
      const int key = remoteKey->getNewVal();
      Serial.printf("[HK] RemoteKey=%d\n", key);
      switch (key) {
        case 4:  sendHonorKey(HONOR_UP); break;
        case 5:  sendHonorKey(HONOR_DOWN); break;
        case 6:  sendHonorKey(HONOR_LEFT); break;
        case 7:  sendHonorKey(HONOR_RIGHT); break;
        case 8:  sendHonorKey(HONOR_OK); break;
        case 9:  sendHonorKey(HONOR_BACK); break;
        case 11: sendHonorKey(HONOR_HOME); break; // temporary mapping
        case 15: sendHonorKey(HONOR_MENU); break; // Info -> Menu
        default:
          Serial.printf("[HK] Unmapped RemoteKey=%d\n", key);
          break;
      }
    }

    return true;
  }
};

struct HonorSpeaker : Service::TelevisionSpeaker {
  SpanCharacteristic *volumeSelector;
  SpanCharacteristic *mute;

  HonorSpeaker() : Service::TelevisionSpeaker() {
    new Characteristic::VolumeControlType(3);
    volumeSelector = new Characteristic::VolumeSelector();
    mute = new Characteristic::Mute(0);
  }

  boolean update() override {
    if (volumeSelector->updated()) {
      // HomeKit VolumeSelector: 0 = increment, 1 = decrement.
      int v = volumeSelector->getNewVal();
      Serial.printf("[HK] VolumeSelector=%d\n", v);
      sendHonorKey(v == 0 ? HONOR_VOLUME_UP : HONOR_VOLUME_DOWN);
    }

    if (mute->updated()) {
      Serial.printf("[HK] Mute=%d\n", mute->getNewVal());
      // HID Keyboard/Keypad usage 0x7F = Mute. Honor X1 acceptance is being tested.
      sendHonorKey(HONOR_MUTE);
    }
    return true;
  }
};

void setup() {
  Serial.begin(115200);
  delay(300);
  WiFi.onEvent(wifiEventLogger);

  Serial.println();
  Serial.println("Honor X1 Apple Remote Bridge");
  Serial.println("----------------------------");
  Serial.println("HomeKit default setup code: 466-37-726");
  Serial.println("Change it later from HomeSpan CLI with: S <8-digit-code>");

  setupHonorBleKeyboard();

  homeSpan.setLogLevel(1);
  // Give ESP32-C3 enough time to associate and complete DHCP before retrying.
  // HomeSpan Setup AP otherwise retries WiFi.begin() too aggressively on some APs.
  homeSpan.setConnectionTimes(15, 60, 3);
  homeSpan.enableAutoStartAP();
  homeSpan.begin(Category::Television, "Honor X1");

  SPAN_ACCESSORY();

  // A minimal input source helps iOS treat this as a real Television accessory.
  SpanService *input = new Service::InputSource();
  new Characteristic::ConfiguredName("Honor X1");
  new Characteristic::Identifier(1);
  new Characteristic::IsConfigured(1);
  new Characteristic::CurrentVisibilityState(0);

  HonorSpeaker *speaker = new HonorSpeaker();

  (new HonorTelevision("Honor X1"))
    ->addLink(input)
    ->addLink(speaker);

  Serial.println("[HK] HomeKit Television ready");
  Serial.println("[BLE] Pair 'HDRC-BV1' from the TV Bluetooth settings");
}

void loop() {
  homeSpan.poll();
}
