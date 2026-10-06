#include "HonorBleBridge.h"

#include "config.h"
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

namespace HonorBle {

static NimBLEHIDDevice *hidDevice = nullptr;
static NimBLECharacteristic *inputReport = nullptr;
static NimBLECharacteristic *inputReport2 = nullptr;
static NimBLECharacteristic *inputReport5A = nullptr;
static NimBLECharacteristic *outputReport5A = nullptr;

static bool bleConnected = false;
static bool tvBondedThisBoot = false;
static bool wakePulseActive = false;
static uint32_t wakePulseUntil = 0;
static TvStateCallback tvStateCallback = nullptr;

static void configureWakeAdvertising();
static void configurePairingAdvertising();
static void configureBondedIdleAdvertising();

static const uint8_t reportMap[] = {
  0x05,0x01,0x09,0x06,0xA1,0x01,0x05,0x07,0x09,0x06,0xA1,0x01,0x85,0x01,0x95,0x08,0x75,0x08,0x15,0x00,0x25,0xFF,0x19,0x00,0x29,0xFF,0x81,0x00,0xC0,
  0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0x02,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x18,0x09,0xB5,0x09,0xB6,0x09,0xB7,0x09,0xCD,0x09,0x40,0x09,0xE5,0x09,0xE7,0x09,0xE9,0x09,0xEA,0x09,0xE9,0x09,0xEA,0x0A,0x2E,0x02,0x0A,0x2D,0x02,0x0A,0x83,0x01,0x0A,0x8A,0x01,0x0A,0x92,0x01,0x0A,0x94,0x01,0x0A,0x21,0x02,0x0A,0x23,0x02,0x0A,0x24,0x02,0x0A,0x25,0x02,0x0A,0x26,0x02,0x0A,0x27,0x02,0x0A,0x2A,0x02,0x81,0x02,0xC0,
  0x05,0x00,0x09,0x00,0xA1,0x01,0x85,0x5A,0x95,0xFF,0x75,0x08,0x15,0x00,0x25,0xFF,0x19,0x00,0x29,0xFF,0x81,0x00,0xC0,0xC0
};

static const char *addrTypeName(uint8_t type) {
  switch (type) {
    case BLE_ADDR_PUBLIC: return "PUBLIC";
    case BLE_ADDR_RANDOM: return "RANDOM";
#ifdef BLE_ADDR_PUBLIC_ID
    case BLE_ADDR_PUBLIC_ID: return "PUBLIC_ID";
#endif
#ifdef BLE_ADDR_RANDOM_ID
    case BLE_ADDR_RANDOM_ID: return "RANDOM_ID";
#endif
    default: return "UNKNOWN";
  }
}

static void notifyTvState(bool connected) {
  if (tvStateCallback) tvStateCallback(connected);
}

bool hasTvBond() {
  const NimBLEAddress remote(ORIGINAL_REMOTE_MAC, BLE_ADDR_PUBLIC);
  const int count = NimBLEDevice::getNumBonds();

  for (int i = 0; i < count; ++i) {
    if (NimBLEDevice::getBondedAddress(i) != remote) return true;
  }
  return false;
}

static void configureBondedIdleAdvertising() {
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();

  NimBLEAdvertisementData advData;
  advData.setFlags(0x05);
  advData.addServiceUUID(NimBLEUUID((uint16_t)0x1812));
  advData.setAppearance(0x03C1);

  NimBLEAdvertisementData scanData;
  scanData.setName(ORIGINAL_REMOTE_NAME);

  adv->stop();
  adv->setConnectableMode(BLE_GAP_CONN_MODE_UND);
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->enableScanResponse(true);
  adv->setScanFilter(false, false);
  adv->setMinInterval(0x30);
  adv->setMaxInterval(0x60);

  const bool started = adv->start();
  Serial.printf("[BLE] Bonded idle ADV started=%d as %s\n",
                started, ORIGINAL_REMOTE_NAME);
}

static void configurePairingAdvertising() {
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  static const uint8_t pairingMfg[] = {0x02, 0x7D, 0x04, 0x00};

  NimBLEAdvertisementData advData;
  advData.setFlags(0x05);
  advData.addServiceUUID(NimBLEUUID((uint16_t)0x1812));
  advData.setAppearance(0x03C1);
  advData.setManufacturerData(
      std::vector<uint8_t>(pairingMfg, pairingMfg + sizeof(pairingMfg)));

  NimBLEAdvertisementData scanData;
  scanData.setName(ORIGINAL_REMOTE_NAME);

  adv->stop();
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->enableScanResponse(true);
  adv->setMinInterval(0x30);
  adv->setMaxInterval(0x60);
  adv->start();

  Serial.printf("[BLE] Pairing ADV started as %s\n", ORIGINAL_REMOTE_NAME);
}

static void logWakeDiagnostics() {
  NimBLEAddress ownAddr = NimBLEDevice::getAddress();
  Serial.printf("[WAKE-DIAG] own=%s type=%u(%s) public=%d random=%d rpa=%d nrpa=%d static=%d\n",
                ownAddr.toString().c_str(),
                ownAddr.getType(), addrTypeName(ownAddr.getType()),
                ownAddr.isPublic(), ownAddr.getType() == BLE_ADDR_RANDOM,
                ownAddr.isRpa(), ownAddr.isNrpa(), ownAddr.isStatic());

  Serial.printf("[WAKE-DIAG] bonds=%u\n",
                (unsigned)NimBLEDevice::getNumBonds());
}

static void configureWakeAdvertising() {
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();

  logWakeDiagnostics();

  NimBLEAddress ownAddr = NimBLEDevice::getAddress();
  uint8_t mac[6] = {0};
  unsigned int b[6] = {0};
  const std::string own = ownAddr.toString();

  if (sscanf(own.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
             &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
    for (int i = 0; i < 6; ++i) mac[i] = (uint8_t)b[i];
  }

  std::vector<uint8_t> mfg = {
      0x02, 0x7D, 0x03, 0x00,
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
      0x01, 0x01
  };

  NimBLEAdvertisementData advData;
  advData.setFlags(0x04);
  advData.addServiceUUID(NimBLEUUID((uint16_t)0x1812));
  advData.setAppearance(0x03C1);
  advData.setManufacturerData(mfg);

  adv->stop();
  adv->setConnectableMode(BLE_GAP_CONN_MODE_UND);
  adv->setDiscoverableMode(BLE_GAP_DISC_MODE_NON);
  adv->setAdvertisementData(advData);
  adv->enableScanResponse(false);
  adv->setScanFilter(false, false);
  adv->setMinInterval(0x20);
  adv->setMaxInterval(0x30);

  const bool started = adv->start();
  Serial.printf("[WAKE] ADV start=%d identity=%s\n",
                started, ownAddr.toString().c_str());
}

void requestWakePulse() {
  Serial.println("[WAKE] Start transient wake ADV");
  configureWakeAdvertising();
  wakePulseActive = true;
  wakePulseUntil = millis() + 8000;
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo& connInfo) override {
    bleConnected = true;
    wakePulseActive = false;
    notifyTvState(true);
    Serial.printf("[BLE] TV connected: %s\n",
                  connInfo.getAddress().toString().c_str());
  }

  void onDisconnect(NimBLEServer *, NimBLEConnInfo&, int reason) override {
    bleConnected = false;
    notifyTvState(false);
    wakePulseActive = false;

    Serial.printf("[BLE] TV disconnected, reason=%d\n", reason);
    if (tvBondedThisBoot || hasTvBond()) {
      configureBondedIdleAdvertising();
    } else {
      configurePairingAdvertising();
    }
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    tvBondedThisBoot = connInfo.isBonded();
    Serial.printf("[BLE] TV security encrypted=%d authenticated=%d bonded=%d peer=%s\n",
                  connInfo.isEncrypted(), connInfo.isAuthenticated(),
                  connInfo.isBonded(),
                  connInfo.getAddress().toString().c_str());
  }
};

void sendKey(uint8_t key) {
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

void relayRemoteReport(const uint8_t *data, size_t len) {
  if (len != 8) {
    Serial.printf("[RELAY] Ignore Report1 len=%u\n", (unsigned)len);
    return;
  }

  if (!bleConnected || !inputReport) {
    if (len >= 3 && data[2] == POWER) {
      Serial.println("[RELAY] Power pressed while TV disconnected -> wake pulse");
      requestWakePulse();
    }
    return;
  }

  inputReport->setValue(data, len);
  inputReport->notify();
}

void setPower(bool on) {
  Serial.printf("[CONTROL] Power requested: %s (BLE=%s)\n",
                on ? "ON" : "OFF",
                bleConnected ? "connected" : "disconnected");

  if (bleConnected) {
    if (on) return;

    Serial.println("[CONTROL] OFF -> Power, Right, OK shutdown macro");
    sendKey(POWER);
    delay(900);
    sendKey(RIGHT);
    delay(250);
    sendKey(OK);
    return;
  }

  if (on) requestWakePulse();
}

void setTvStateCallback(TvStateCallback callback) {
  tvStateCallback = callback;
  if (tvStateCallback) tvStateCallback(bleConnected);
}

bool isTvConnected() {
  return bleConnected;
}

void begin() {
  NimBLEDevice::init(ORIGINAL_REMOTE_NAME);
  NimBLEDevice::setPower(9);
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(false);

  hidDevice = new NimBLEHIDDevice(server);
  inputReport = hidDevice->getInputReport(1);
  inputReport5A = hidDevice->getInputReport(0x5A);
  outputReport5A = hidDevice->getOutputReport(0x5A);
  inputReport2 = hidDevice->getInputReport(2);

  hidDevice->setManufacturer("Realtek BT");
  hidDevice->setPnp(0x01, 0x7D02, 0x0002, 0x0003);

  NimBLEService *deviceInfo = hidDevice->getDeviceInfoService();

  auto *modelChr =
      deviceInfo->createCharacteristic(NimBLEUUID((uint16_t)0x2A24), NIMBLE_PROPERTY::READ);
  modelChr->setValue("Model Nbr 0.9");

  auto *serialChr =
      deviceInfo->createCharacteristic(NimBLEUUID((uint16_t)0x2A25), NIMBLE_PROPERTY::READ);
  serialChr->setValue("RTKBeeSerialNum");

  auto *firmwareChr =
      deviceInfo->createCharacteristic(NimBLEUUID((uint16_t)0x2A26), NIMBLE_PROPERTY::READ);
  firmwareChr->setValue("RTKBeeFirmwareRev");

  auto *hardwareChr =
      deviceInfo->createCharacteristic(NimBLEUUID((uint16_t)0x2A27), NIMBLE_PROPERTY::READ);
  hardwareChr->setValue("RTKBeeHardwareRev");

  auto *softwareChr =
      deviceInfo->createCharacteristic(NimBLEUUID((uint16_t)0x2A28), NIMBLE_PROPERTY::READ);
  softwareChr->setValue("RTKBeeSoftwareRev");

  hidDevice->setHidInfo(0x00, 0x02);
  hidDevice->setReportMap((uint8_t *)reportMap, sizeof(reportMap));
  hidDevice->setBatteryLevel(100);

  server->start();

  Serial.printf("[BLE] Bonds=%d tv=%d\n",
                NimBLEDevice::getNumBonds(), hasTvBond());

  if (hasTvBond()) {
    configureBondedIdleAdvertising();
  } else {
    configurePairingAdvertising();
  }
}

void loop() {
  if (wakePulseActive && !bleConnected &&
      (int32_t)(millis() - wakePulseUntil) >= 0) {
    wakePulseActive = false;

    if (tvBondedThisBoot || hasTvBond()) {
      Serial.println("[WAKE] Wake pulse expired -> bonded idle ADV");
      configureBondedIdleAdvertising();
    } else {
      Serial.println("[WAKE] Wake pulse expired -> pairing ADV");
      configurePairingAdvertising();
    }
  }
}

}  // namespace HonorBle
