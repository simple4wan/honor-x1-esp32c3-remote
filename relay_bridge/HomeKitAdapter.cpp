#include "HomeKitAdapter.h"

#include "HonorBleBridge.h"
#include "HomeSpan.h"

namespace HomeKitAdapter {

static SpanCharacteristic *activeCharacteristic = nullptr;

static void syncTvState(bool connected) {
  if (!activeCharacteristic) return;

  activeCharacteristic->setVal(connected ? 1 : 0);
  Serial.printf("[HK] Active synced -> %s\n",
                connected ? "ON" : "OFF");
}

struct HonorTelevision : Service::Television {
  SpanCharacteristic *active;
  SpanCharacteristic *remoteKey;

  explicit HonorTelevision(const char *name) : Service::Television() {
    active = new Characteristic::Active(0);
    activeCharacteristic = active;

    new Characteristic::ConfiguredName(name);
    remoteKey = new Characteristic::RemoteKey();
  }

  boolean update() override {
    if (active->updated()) {
      HonorBle::setPower(active->getNewVal());
    }

    if (remoteKey->updated()) {
      const int key = remoteKey->getNewVal();

      switch (key) {
        case 4:  HonorBle::sendKey(HonorBle::UP); break;
        case 5:  HonorBle::sendKey(HonorBle::DOWN); break;
        case 6:  HonorBle::sendKey(HonorBle::LEFT); break;
        case 7:  HonorBle::sendKey(HonorBle::RIGHT); break;
        case 8:  HonorBle::sendKey(HonorBle::OK); break;
        case 9:  HonorBle::sendKey(HonorBle::BACK); break;
        case 11: HonorBle::sendKey(HonorBle::HOME); break;
        case 15: HonorBle::sendKey(HonorBle::MENU); break;
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
      const int value = volumeSelector->getNewVal();
      HonorBle::sendKey(value == 0
          ? HonorBle::VOLUME_UP
          : HonorBle::VOLUME_DOWN);
    }

    if (mute->updated()) {
      HonorBle::sendKey(HonorBle::MUTE);
    }

    return true;
  }
};

void begin() {
  homeSpan.setLogLevel(0);
  homeSpan.setConnectionTimes(15, 60, 3);
  homeSpan.enableAutoStartAP();
  homeSpan.begin(Category::Television, "荣耀智慧屏");

  SPAN_ACCESSORY();

  SpanService *input = new Service::InputSource();
  new Characteristic::ConfiguredName("荣耀智慧屏");
  new Characteristic::Identifier(1);
  new Characteristic::IsConfigured(1);
  new Characteristic::CurrentVisibilityState(0);

  HonorSpeaker *speaker = new HonorSpeaker();

  (new HonorTelevision("荣耀智慧屏"))
      ->addLink(input)
      ->addLink(speaker);

  HonorBle::setTvStateCallback(syncTvState);

  Serial.println("[HK] 荣耀智慧屏 HomeKit Television ready");
}

void loop() {
  homeSpan.poll();
}

}  // namespace HomeKitAdapter
