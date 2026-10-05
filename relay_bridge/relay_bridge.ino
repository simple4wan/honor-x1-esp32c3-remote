#include <Arduino.h>
#include "HomeSpan.h"
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

// Relay prototype: ESP32 is simultaneously a HID peripheral for the TV
// and a BLE central for the original HDRC-BV1 remote.

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
// HDRC-BV1-TEST remote can remain paired independently.

static NimBLEHIDDevice *hidDevice = nullptr;
static NimBLECharacteristic *inputReport = nullptr;
static NimBLECharacteristic *inputReport2 = nullptr;
static NimBLECharacteristic *inputReport5A = nullptr;
static NimBLECharacteristic *outputReport5A = nullptr;
static bool bleConnected = false;
static SpanCharacteristic *homeKitActive = nullptr;

static bool tvBondedThisBoot = false;
static bool wakePulseActive = false;
static uint32_t wakePulseUntil = 0;
static bool remoteConnected = false;
static bool remoteDoConnect = false;
static uint32_t remoteRetryAt = 0;
static const NimBLEAdvertisedDevice *remoteAdv = nullptr;
static NimBLEAddress remoteConnectAddr("18:70:3B:76:B8:45", BLE_ADDR_PUBLIC);
static NimBLEClient *remoteClient = nullptr;
static NimBLERemoteCharacteristic *remoteReport1 = nullptr;

static void configureWakeAdvertising();
static void configurePairingAdvertising();
static void configureBondedIdleAdvertising();
static bool hasTvBond();
static void requestWakePulse();
static void startOriginalRemoteScan();

// Exact keyboard-style report format observed from HDRC-BV1-TEST:
// [modifier, reserved, key1, key2, key3, key4, key5, key6]
static const uint8_t reportMap[] = {
  0x05,0x01,0x09,0x06,0xA1,0x01,0x05,0x07,0x09,0x06,0xA1,0x01,0x85,0x01,0x95,0x08,0x75,0x08,0x15,0x00,0x25,0xFF,0x19,0x00,0x29,0xFF,0x81,0x00,0xC0,
  0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0x02,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x18,0x09,0xB5,0x09,0xB6,0x09,0xB7,0x09,0xCD,0x09,0x40,0x09,0xE5,0x09,0xE7,0x09,0xE9,0x09,0xEA,0x09,0xE9,0x09,0xEA,0x0A,0x2E,0x02,0x0A,0x2D,0x02,0x0A,0x83,0x01,0x0A,0x8A,0x01,0x0A,0x92,0x01,0x0A,0x94,0x01,0x0A,0x21,0x02,0x0A,0x23,0x02,0x0A,0x24,0x02,0x0A,0x25,0x02,0x0A,0x26,0x02,0x0A,0x27,0x02,0x0A,0x2A,0x02,0x81,0x02,0xC0,
  0x05,0x00,0x09,0x00,0xA1,0x01,0x85,0x5A,0x95,0xFF,0x75,0x08,0x15,0x00,0x25,0xFF,0x19,0x00,0x29,0xFF,0x81,0x00,0xC0,0xC0
};


static void relayRemoteReport(NimBLERemoteCharacteristic *chr, uint8_t *data,
                              size_t len, bool isNotify) {
  if (len != 8) {
    Serial.printf("[RELAY] Ignore Report1 len=%u\n", (unsigned)len);
    return;
  }

  Serial.printf("[RELAY] RPT1 %02X %02X %02X %02X %02X %02X %02X %02X\n",
                data[0], data[1], data[2], data[3],
                data[4], data[5], data[6], data[7]);

  if (!bleConnected || !inputReport) {
    if (len >= 3 && data[2] == 0x66) {
      Serial.println("[RELAY] Power pressed while TV disconnected -> wake pulse");
      requestWakePulse();
    } else {
      Serial.println("[RELAY] TV not connected; report not forwarded");
    }
    return;
  }

  inputReport->setValue(data, len);
  inputReport->notify();
}

class RemoteClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *client) override {
    remoteConnected = true;
    Serial.printf("[REMOTE] Connected: %s\n",
                  client->getPeerAddress().toString().c_str());
  }

  void onDisconnect(NimBLEClient *client, int reason) override {
    remoteConnected = false;
    remoteReport1 = nullptr;
    remoteDoConnect = false;
    remoteRetryAt = millis() + 1500;
    Serial.printf("[REMOTE] Disconnected reason=%d; retry in 1500 ms\n", reason);
  }

  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("[REMOTE] Security encrypted=%d bonded=%d peer=%s\n",
                  info.isEncrypted(), info.isBonded(),
                  info.getAddress().toString().c_str());
    if (!info.isEncrypted()) {
      NimBLEClient *client = NimBLEDevice::getClientByHandle(info.getConnHandle());
      if (client) client->disconnect();
    }
  }
};

static RemoteClientCallbacks remoteClientCallbacks;

class RemoteScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *device) override {
    const std::string addr = device->getAddress().toString();
    const bool targetAddr =
        (addr == "18:70:3b:76:b8:45" || addr == "18:70:3B:76:B8:45");
    const bool targetName =
        device->haveName() && device->getName() == "HDRC-BV1";

    // A bonded HDRC-BV1 may advertise without Local Name during normal
    // reconnect/wake. Its fixed public address is the authoritative match.
    if (!targetAddr && !targetName) return;

    Serial.printf("[REMOTE] Found original HDRC-BV1 %s name=%s RSSI=%d\n",
                  addr.c_str(),
                  device->haveName() ? device->getName().c_str() : "<none>",
                  device->getRSSI());

    NimBLEDevice::getScan()->stop();
    remoteAdv = device;

    // NimBLE-Arduino may expose a bonded peer's resolved scan address as
    // 00:00:00:00:00:00. The HDRC-BV1 has a fixed public identity, so fall
    // back to it exactly like the verified native NimBLE sniffer does.
    if (device->getAddress().isNull()) {
      remoteConnectAddr = NimBLEAddress("18:70:3B:76:B8:45", BLE_ADDR_PUBLIC);
      Serial.println("[REMOTE] Scan address is null; fallback to 18:70:3B:76:B8:45");
    } else {
      remoteConnectAddr = device->getAddress();
    }

    remoteDoConnect = true;
    Serial.printf("[REMOTE] Connect scheduled target=%s\n",
                  remoteConnectAddr.toString().c_str());
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    if (!remoteConnected && !remoteDoConnect) {
      NimBLEDevice::getScan()->start(0, false, true);
    }
  }
};

static RemoteScanCallbacks remoteScanCallbacks;

static bool connectOriginalRemote() {
  if (!remoteAdv) return false;

  remoteClient = NimBLEDevice::getClientByPeerAddress(remoteConnectAddr);
  if (!remoteClient) remoteClient = NimBLEDevice::getDisconnectedClient();

  if (!remoteClient) {
    remoteClient = NimBLEDevice::createClient();
    if (!remoteClient) {
      Serial.println("[REMOTE] createClient failed");
      return false;
    }
    remoteClient->setClientCallbacks(&remoteClientCallbacks, false);
    // HID traffic is tiny; favor link robustness over aggressive timeout.
    // Supervision timeout unit is 10 ms: 1000 = 10 seconds.
    remoteClient->setConnectionParams(16, 24, 0, 1000);
    remoteClient->setConnectTimeout(10000);
    remoteClient->setConnectRetries(2);
  }

  Serial.printf("[REMOTE] Connecting to %s\n",
                remoteConnectAddr.toString().c_str());
  // Skip MTU exchange during connect; HDRC-BV1 reports are only 8 bytes.
  // This reduces radio/control traffic while TV + remote links coexist.
  if (!remoteClient->connect(remoteConnectAddr, true, false, false)) {
    Serial.println("[REMOTE] connect failed");
    remoteRetryAt = millis() + 1500;
    return false;
  }

  // The original remote sometimes accepts the ACL connection while still
  // waking from a low-power state, then times out if SMP/encryption starts
  // immediately. Give it a short settling window before restoring the bond.
  Serial.println("[REMOTE] Connected; wait 500 ms before security restore");
  delay(500);

  if (!remoteClient->secureConnection()) {
    Serial.printf("[REMOTE] secureConnection failed lastError=%d\n",
                  remoteClient->getLastError());
    remoteRetryAt = millis() + 1500;
    if (remoteClient->isConnected()) remoteClient->disconnect();
    return false;
  }

  NimBLERemoteService *hid = remoteClient->getService(NimBLEUUID((uint16_t)0x1812));
  if (!hid) {
    Serial.println("[REMOTE] HID service 0x1812 not found");
    remoteClient->disconnect();
    return false;
  }

  remoteReport1 = nullptr;

  // HDRC-BV1 has several characteristics with the same UUID 0x2A4D.
  // Select the keyboard input report by reading descriptor 0x2908:
  //   [0x01, 0x01] = Report ID 1, Input.
  const auto& reportChars = hid->getCharacteristics(true);
  for (auto *chr : reportChars) {
    if (!chr || chr->getUUID() != NimBLEUUID((uint16_t)0x2A4D)) continue;

    NimBLERemoteDescriptor *ref =
        chr->getDescriptor(NimBLEUUID((uint16_t)0x2908));
    if (!ref) {
      Serial.printf("[REMOTE] 0x2A4D handle=0x%04X has no Report Reference\n",
                    chr->getHandle());
      continue;
    }

    NimBLEAttValue v = ref->readValue();
    if (v.size() < 2) {
      Serial.printf("[REMOTE] 0x2A4D handle=0x%04X bad Report Reference len=%u\n",
                    chr->getHandle(), (unsigned)v.size());
      continue;
    }

    const uint8_t reportId = v.data()[0];
    const uint8_t reportType = v.data()[1];
    Serial.printf("[REMOTE] Report char handle=0x%04X id=%u type=%u notify=%d\n",
                  chr->getHandle(), reportId, reportType, chr->canNotify());

    if (reportId == 1 && reportType == 1) {
      remoteReport1 = chr;
      break;
    }
  }

  if (!remoteReport1) {
    Serial.println("[REMOTE] Report ID 1 input characteristic not found");
    remoteClient->disconnect();
    return false;
  }

  if (!remoteReport1->canNotify() ||
      !remoteReport1->subscribe(true, relayRemoteReport, true)) {
    Serial.println("[REMOTE] subscribe Report ID 1 failed");
    remoteClient->disconnect();
    return false;
  }

  remoteConnected = true;
  Serial.printf("[REMOTE] Report ID 1 subscribed handle=0x%04X; physical remote relay ready\n",
                remoteReport1->getHandle());
  return true;
}

static void startOriginalRemoteScan() {
  if (remoteConnected || remoteDoConnect) return;
  if (remoteRetryAt && (int32_t)(millis() - remoteRetryAt) < 0) return;
  remoteRetryAt = 0;

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&remoteScanCallbacks, false);
  scan->setInterval(100);
  scan->setWindow(80);
  scan->setActiveScan(true);
  scan->start(0, false, true);
  Serial.println("[REMOTE] Scanning for original HDRC-BV1");
}

static bool hasTvBond() {
  const std::string remoteAddr = "18:70:3b:76:b8:45";
  const int count = NimBLEDevice::getNumBonds();

  for (int i = 0; i < count; ++i) {
    NimBLEAddress peer = NimBLEDevice::getBondedAddress(i);
    std::string addr = peer.toString();
    if (addr != remoteAddr && addr != "18:70:3B:76:B8:45") {
      return true;
    }
  }
  return false;
}

static void configureBondedIdleAdvertising() {
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();

  // Daily reconnect state for an already bonded TV.
  // Keep the HID identity/name/UUID, but do NOT expose the original
  // pairing-mode manufacturer marker 02 7D 04 00.
  NimBLEAdvertisementData advData;
  advData.setFlags(0x05);
  advData.addServiceUUID(NimBLEUUID((uint16_t)0x1812));
  advData.setAppearance(0x03C1);

  NimBLEAdvertisementData scanData;
  scanData.setName("HDRC-BV1");

  adv->stop();
  adv->setConnectableMode(BLE_GAP_CONN_MODE_UND);
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->enableScanResponse(true);
  adv->setScanFilter(false, false);
  adv->setMinInterval(0x30);
  adv->setMaxInterval(0x60);
  const bool started = adv->start();

  Serial.printf("[BLE] Bonded idle ADV started=%d as HDRC-BV1 (no pairing MFG)\n",
                started);
}

static void configurePairingAdvertising() {
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();

  static const uint8_t pairingMfg[] = {0x02, 0x7D, 0x04, 0x00};

  NimBLEAdvertisementData advData;
  advData.setFlags(0x05);
  advData.addServiceUUID(NimBLEUUID((uint16_t)0x1812));
  advData.setAppearance(0x03C1);
  advData.setManufacturerData(std::vector<uint8_t>(pairingMfg, pairingMfg + sizeof(pairingMfg)));

  NimBLEAdvertisementData scanData;
  scanData.setName("HDRC-BV1");

  adv->stop();
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->enableScanResponse(true);
  adv->setMinInterval(0x30);
  adv->setMaxInterval(0x60);
  adv->start();

  Serial.println("[BLE] Pairing ADV started as HDRC-BV1 (TV pairing mode)");
}


static const char* addrTypeName(uint8_t type) {
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

static void logWakeDiagnostics() {
  NimBLEAddress ownAddr = NimBLEDevice::getAddress();
  Serial.printf("[WAKE-DIAG] own=%s type=%u(%s) public=%d random=%d rpa=%d nrpa=%d static=%d\n",
                ownAddr.toString().c_str(),
                ownAddr.getType(), addrTypeName(ownAddr.getType()),
                ownAddr.isPublic(), ownAddr.getType() == BLE_ADDR_RANDOM,
                ownAddr.isRpa(), ownAddr.isNrpa(), ownAddr.isStatic());

  Serial.printf("[WAKE-DIAG] bonds=%u\n", (unsigned)NimBLEDevice::getNumBonds());
  for (int i = 0; i < NimBLEDevice::getNumBonds(); ++i) {
    NimBLEAddress peer = NimBLEDevice::getBondedAddress(i);
    Serial.printf("[WAKE-DIAG] bond[%d]=%s type=%u(%s)\n",
                  i, peer.toString().c_str(), peer.getType(), addrTypeName(peer.getType()));
  }

  Serial.println("[WAKE-DIAG] target legacy ADV_IND: conn_mode=UND disc_mode=NON flags=0x04 interval=0x20..0x30");
}

static void configureWakeAdvertising() {
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();

  logWakeDiagnostics();

  NimBLEAddress ownAddr = NimBLEDevice::getAddress();

  // The original HDRC-BV1 wake advertisement embeds its own public BLE
  // identity address in manufacturer data. Since the TV is now bonded to
  // this ESP32, advertise the ESP32's bonded identity instead.
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

  // Original HDRC-BV1 wake capture was legacy event type 0 = ADV_IND:
  // connectable undirected advertising, non-discoverable, Flags 0x04.
  adv->setConnectableMode(BLE_GAP_CONN_MODE_UND);
  adv->setDiscoverableMode(BLE_GAP_DISC_MODE_NON);
  adv->setAdvertisementData(advData);
  adv->enableScanResponse(false);
  adv->setScanFilter(false, false);
  adv->setMinInterval(0x20);
  adv->setMaxInterval(0x30);

  const bool advStarted = adv->start();
  Serial.printf("[WAKE-DIAG] ADV_IND start=%d conn_mode=UND disc_mode=NON scan_rsp=0 filter=none\n",
                advStarted);

  Serial.printf("[BLE] Wake ADV identity: %s type=%u(%s)\n",
                ownAddr.toString().c_str(), ownAddr.getType(), addrTypeName(ownAddr.getType()));
  Serial.printf("[BLE] Wake ADV MFG=02 7D 03 00 %02X %02X %02X %02X %02X %02X 01 01\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void requestWakePulse() {
  Serial.println("[WAKE] Normal ADV -> transient wake ADV");
  configureWakeAdvertising();
  wakePulseActive = true;
  wakePulseUntil = millis() + 8000;
  Serial.println("[WAKE] Wake pulse armed for 8000 ms");
}


class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo& connInfo) override {
    bleConnected = true;
    wakePulseActive = false;
    if (homeKitActive) {
      homeKitActive->setVal(1);
      Serial.println("[HK] Active synced -> ON (TV BLE connected)");
    }
    Serial.printf("[BLE] TV connected: %s\n", connInfo.getAddress().toString().c_str());
    if (!remoteConnected && !remoteDoConnect) {
      Serial.println("[REMOTE] Ensure scan after TV connection");
      startOriginalRemoteScan();
    }
  }

  void onDisconnect(NimBLEServer *server, NimBLEConnInfo& connInfo, int reason) override {
    bleConnected = false;
    if (homeKitActive) {
      homeKitActive->setVal(0);
      Serial.println("[HK] Active synced -> OFF (TV BLE disconnected)");
    }
    Serial.printf("[BLE] TV disconnected, reason=%d\n", reason);
    wakePulseActive = false;
    if (tvBondedThisBoot || hasTvBond()) {
      Serial.println("[BLE] TV disconnected -> bonded idle ADV; Power will trigger transient wake ADV");
      configureBondedIdleAdvertising();
    } else {
      Serial.println("[BLE] TV disconnected without TV bond -> pairing ADV");
      configurePairingAdvertising();
    }
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    tvBondedThisBoot = connInfo.isBonded();
    Serial.printf("[BLE] TV security encrypted=%d authenticated=%d bonded=%d peer=%s\n",
                  connInfo.isEncrypted(), connInfo.isAuthenticated(), connInfo.isBonded(),
                  connInfo.getAddress().toString().c_str());
    if (!remoteConnected && !remoteDoConnect) {
      startOriginalRemoteScan();
    }
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
  NimBLEDevice::setPower(9);

  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  // Own the post-disconnect advertising state machine ourselves.
  // NimBLEServer otherwise restarts advertising AFTER onDisconnect(),
  // which can overwrite the wake ADV configured in that callback.
  server->advertiseOnDisconnect(false);
  Serial.println("[BLE] Server auto-advertise-on-disconnect disabled");

  hidDevice = new NimBLEHIDDevice(server);
  inputReport = hidDevice->getInputReport(1);
  inputReport5A = hidDevice->getInputReport(0x5A);
  outputReport5A = hidDevice->getOutputReport(0x5A);
  inputReport2 = hidDevice->getInputReport(2);

  hidDevice->setManufacturer("Realtek BT");
  hidDevice->setPnp(0x01, 0x7D02, 0x0002, 0x0003);

  // Clone original HDRC-BV1 Device Information characteristics.
  NimBLEService *deviceInfo = hidDevice->getDeviceInfoService();

  NimBLECharacteristic *modelChr =
      deviceInfo->createCharacteristic(NimBLEUUID((uint16_t)0x2A24), NIMBLE_PROPERTY::READ);
  modelChr->setValue("Model Nbr 0.9");

  NimBLECharacteristic *serialChr =
      deviceInfo->createCharacteristic(NimBLEUUID((uint16_t)0x2A25), NIMBLE_PROPERTY::READ);
  serialChr->setValue("RTKBeeSerialNum");

  NimBLECharacteristic *firmwareChr =
      deviceInfo->createCharacteristic(NimBLEUUID((uint16_t)0x2A26), NIMBLE_PROPERTY::READ);
  firmwareChr->setValue("RTKBeeFirmwareRev");

  NimBLECharacteristic *hardwareChr =
      deviceInfo->createCharacteristic(NimBLEUUID((uint16_t)0x2A27), NIMBLE_PROPERTY::READ);
  hardwareChr->setValue("RTKBeeHardwareRev");

  NimBLECharacteristic *softwareChr =
      deviceInfo->createCharacteristic(NimBLEUUID((uint16_t)0x2A28), NIMBLE_PROPERTY::READ);
  softwareChr->setValue("RTKBeeSoftwareRev");
  hidDevice->setHidInfo(0x00, 0x02);
  hidDevice->setReportMap((uint8_t*)reportMap, sizeof(reportMap));
  hidDevice->setBatteryLevel(100);

  server->start();
  if (hasTvBond()) {
    Serial.println("[BLE] Existing TV bond detected -> start bonded idle ADV");
    configureBondedIdleAdvertising();
  } else {
    Serial.println("[BLE] No TV bond detected -> start pairing ADV");
    configurePairingAdvertising();
  }
}

// Values captured from the original HDRC-BV1-TEST.
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
    homeKitActive = active;
    configuredName = new Characteristic::ConfiguredName(name);
    remoteKey = new Characteristic::RemoteKey();
  }

  boolean update() override {
    if (active->updated()) {
      const bool wantOn = active->getNewVal();
      Serial.printf("[HK] Power requested: %s (BLE=%s)\n",
                    wantOn ? "ON" : "OFF",
                    bleConnected ? "connected" : "disconnected");

      if (bleConnected) {
        if (wantOn) {
          Serial.println("[HK] Ignore duplicate ON while TV BLE is connected");
        } else {
          sendHonorKey(HONOR_POWER);
        }
      } else {
        if (wantOn) {
          Serial.println("[HK] ON while BLE disconnected -> wake pulse");
          requestWakePulse();
        } else {
          Serial.println("[HK] Ignore OFF while TV BLE is disconnected");
        }
      }
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

  // First-time remote pairing: hold HOME+MENU on the original HDRC-BV1.
  startOriginalRemoteScan();

  homeSpan.setLogLevel(1);
  // Give ESP32-C3 enough time to associate and complete DHCP before retrying.
  // HomeSpan Setup AP otherwise retries WiFi.begin() too aggressively on some APs.
  homeSpan.setConnectionTimes(15, 60, 3);
  homeSpan.enableAutoStartAP();
  homeSpan.begin(Category::Television, "荣耀智慧屏");

  SPAN_ACCESSORY();

  // A minimal input source helps iOS treat this as a real Television accessory.
  SpanService *input = new Service::InputSource();
  new Characteristic::ConfiguredName("荣耀智慧屏");
  new Characteristic::Identifier(1);
  new Characteristic::IsConfigured(1);
  new Characteristic::CurrentVisibilityState(0);

  HonorSpeaker *speaker = new HonorSpeaker();

  (new HonorTelevision("荣耀智慧屏"))
    ->addLink(input)
    ->addLink(speaker);

  Serial.println("[HK] 荣耀智慧屏 HomeKit Television ready");
  Serial.println("[BLE] TV side: ESP32 HDRC-BV1 advertising state ready");
}

void loop() {
  homeSpan.poll();

  if (wakePulseActive && !bleConnected &&
      (int32_t)(millis() - wakePulseUntil) >= 0) {
    wakePulseActive = false;
    if (tvBondedThisBoot || hasTvBond()) {
      Serial.println("[WAKE] Wake pulse expired -> restore bonded idle ADV");
      configureBondedIdleAdvertising();
    } else {
      Serial.println("[WAKE] Wake pulse expired -> restore pairing ADV");
      configurePairingAdvertising();
    }
  }

  if (remoteDoConnect) {
    Serial.println("[REMOTE] Processing scheduled connect");
    remoteDoConnect = false;
    if (!connectOriginalRemote()) {
      if (!remoteRetryAt) remoteRetryAt = millis() + 1500;
    }
  }

  if (!remoteConnected && !remoteDoConnect &&
      remoteRetryAt && (int32_t)(millis() - remoteRetryAt) >= 0) {
    startOriginalRemoteScan();
  }
}
