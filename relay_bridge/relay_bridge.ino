#include <Arduino.h>
#include "config.h"
#include "HomeSpan.h"
#include <NimBLEDevice.h>
#include "host/ble_store.h"
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
static bool remoteSecurityReady = false;
static bool remoteDoConnect = false;
static uint32_t remoteRetryAt = 0;
static const NimBLEAdvertisedDevice *remoteAdv = nullptr;
static NimBLEAddress remoteConnectAddr(ORIGINAL_REMOTE_MAC, BLE_ADDR_PUBLIC);
static NimBLEClient *remoteClient = nullptr;
static NimBLERemoteCharacteristic *remoteReport1 = nullptr;
static bool remoteBootOurSecValid = false;
static bool remoteBootPeerSecValid = false;
static struct ble_store_value_sec remoteBootOurSec = {};
static struct ble_store_value_sec remoteBootPeerSec = {};

static void configureWakeAdvertising();
static void configurePairingAdvertising();
static void configureBondedIdleAdvertising();
static bool hasTvBond();
static void requestWakePulse();
static void startOriginalRemoteScan();
static bool hasOriginalRemoteBond();
static void captureRemoteBootSecurityStore();
static void forcePersistRemoteSecurityIfChanged();

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

  if (!bleConnected || !inputReport) {
    if (len >= 3 && data[2] == 0x66) {
      Serial.println("[RELAY] Power pressed while TV disconnected -> wake pulse");
      requestWakePulse();
    }
    return;
  }

  inputReport->setValue(data, len);
  inputReport->notify();
}

class RemoteClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *client) override {
    remoteConnected = true;
    remoteSecurityReady = false;
    // Start bonded encryption immediately; HDRC-BV1 reconnect is timing-sensitive.
    const bool securityStarted = client->secureConnection(true);
    if (!securityStarted) {
      Serial.printf("[REMOTE] Security start failed lastError=%d\n", client->getLastError());
    }
  }

  void onDisconnect(NimBLEClient *client, int reason) override {
    remoteConnected = false;
    remoteSecurityReady = false;
    remoteReport1 = nullptr;
    remoteDoConnect = false;
    remoteRetryAt = millis() + 3000;
    Serial.printf("[REMOTE] Disconnected reason=%d; retry in 3000 ms\n", reason);
  }

  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    remoteSecurityReady = info.isEncrypted();

    if (info.isEncrypted()) {
      Serial.printf("[REMOTE] Security restored bonded=%d peer=%s\n",
                    info.isBonded(), info.getAddress().toString().c_str());
      forcePersistRemoteSecurityIfChanged();
    } else {
      Serial.printf("[REMOTE] Security failed peer=%s\n",
                    info.getAddress().toString().c_str());
      NimBLEClient *client = NimBLEDevice::getClientByHandle(info.getConnHandle());
      if (client) client->disconnect();
    }
  }
};

static bool remoteAdvHasMfg(const NimBLEAdvertisedDevice *device,
                            uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
  if (!device->haveManufacturerData()) return false;
  std::string mfg = device->getManufacturerData();
  return mfg.size() >= 4 &&
         (uint8_t)mfg[0] == b0 &&
         (uint8_t)mfg[1] == b1 &&
         (uint8_t)mfg[2] == b2 &&
         (uint8_t)mfg[3] == b3;
}

static RemoteClientCallbacks remoteClientCallbacks;

class RemoteScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *device) override {
    const std::string addr = device->getAddress().toString();
    const NimBLEAddress configuredRemote(ORIGINAL_REMOTE_MAC, BLE_ADDR_PUBLIC);
    const bool targetAddr =
        !device->getAddress().isNull() && device->getAddress() == configuredRemote;
    const bool targetName =
        device->haveName() && device->getName() == ORIGINAL_REMOTE_NAME;

    // A bonded HDRC-BV1 may advertise without Local Name during normal
    // reconnect/wake. Its fixed public address is the authoritative match.
    if (!targetAddr && !targetName) return;

    // Never connect from the remote's non-connectable wake/status advertisements.
    if (!device->isConnectable()) return;

    const bool mfg0400 = remoteAdvHasMfg(device, 0x02, 0x7D, 0x04, 0x00);
    const bool mfg0411 = remoteAdvHasMfg(device, 0x02, 0x7D, 0x04, 0x11);
    Serial.printf("[REMOTE] Found connectable HDRC-BV1 RSSI=%d state=%s\n",
                  device->getRSSI(),
                  mfg0400 ? "04 00" : (mfg0411 ? "04 11" : "other"));

    remoteAdv = device;

    if (device->getAddress().isNull()) {
      remoteConnectAddr = NimBLEAddress(ORIGINAL_REMOTE_MAC, BLE_ADDR_PUBLIC);
    } else {
      remoteConnectAddr = device->getAddress();
    }

    NimBLEDevice::getScan()->stop();
    remoteDoConnect = true;

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

  Serial.printf("[REMOTE] Connecting bonded=%d\n", hasOriginalRemoteBond());
  // Skip MTU exchange during connect; HDRC-BV1 reports are only 8 bytes.
  // This reduces radio/control traffic while TV + remote links coexist.
  if (!remoteClient->connect(remoteConnectAddr, true, false, false)) {
    Serial.println("[REMOTE] connect failed");
    remoteRetryAt = millis() + 3000;
    return false;
  }

  // Security was started asynchronously inside onConnect(), at the earliest
  // possible point. Wait here only for the authentication callback/result.
  const uint32_t securityWaitStart = millis();
  while (remoteClient->isConnected() && !remoteSecurityReady &&
         (uint32_t)(millis() - securityWaitStart) < 5000) {
    delay(10);
  }

  if (!remoteClient->isConnected() || !remoteSecurityReady) {
    Serial.printf("[REMOTE] Immediate security did not complete; connected=%d lastError=%d\n",
                  remoteClient->isConnected(), remoteClient->getLastError());
    remoteRetryAt = millis() + 3000;
    if (remoteClient->isConnected()) remoteClient->disconnect();
    return false;
  }

  Serial.printf("[REMOTE] Immediate security ready after %lu ms\n",
                (unsigned long)(millis() - securityWaitStart));

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
  Serial.println("[REMOTE] Physical remote relay ready");
  return true;
}

static void startOriginalRemoteScan() {
  if (remoteConnected || remoteDoConnect) return;
  if (remoteRetryAt && (int32_t)(millis() - remoteRetryAt) < 0) return;
  remoteRetryAt = 0;

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&remoteScanCallbacks, false);
  scan->setDuplicateFilter(1);
  scan->setInterval(100);
  scan->setWindow(80);
  scan->setActiveScan(true);
  scan->start(0, false, true);
}

static bool readRemoteSecurityStore(struct ble_store_value_sec *our,
                                    struct ble_store_value_sec *peer) {
  const NimBLEAddress remote(ORIGINAL_REMOTE_MAC, BLE_ADDR_PUBLIC);

  struct ble_store_key_sec key = {};
  key.peer_addr = *remote.getBase();
  key.idx = 0;

  const int ourRc = ble_store_read_our_sec(&key, our);
  const int peerRc = ble_store_read_peer_sec(&key, peer);
  return ourRc == 0 && peerRc == 0;
}

static void captureRemoteBootSecurityStore() {
  struct ble_store_value_sec our = {};
  struct ble_store_value_sec peer = {};

  if (readRemoteSecurityStore(&our, &peer)) {
    remoteBootOurSec = our;
    remoteBootPeerSec = peer;
    remoteBootOurSecValid = true;
    remoteBootPeerSecValid = true;

  } else {
    remoteBootOurSecValid = false;
    remoteBootPeerSecValid = false;
    Serial.println("[SECSTORE] WARNING: remote security snapshot unavailable");
  }
}

static bool secRecordChanged(const struct ble_store_value_sec& a,
                             const struct ble_store_value_sec& b) {
  return a.bond_count != b.bond_count ||
         a.key_size != b.key_size ||
         a.ediv != b.ediv ||
         a.rand_num != b.rand_num ||
         a.ltk_present != b.ltk_present ||
         (a.ltk_present && memcmp(a.ltk, b.ltk, sizeof(a.ltk)) != 0) ||
         a.irk_present != b.irk_present ||
         (a.irk_present && memcmp(a.irk, b.irk, sizeof(a.irk)) != 0) ||
         a.csrk_present != b.csrk_present ||
         (a.csrk_present && memcmp(a.csrk, b.csrk, sizeof(a.csrk)) != 0) ||
         a.authenticated != b.authenticated ||
         a.sc != b.sc;
}

static void forcePersistRemoteSecurityIfChanged() {
  struct ble_store_value_sec our = {};
  struct ble_store_value_sec peer = {};

  if (!readRemoteSecurityStore(&our, &peer)) {
    Serial.println("[SECSTORE] force-persist skipped: current remote security records unavailable");
    return;
  }

  const bool changed =
      !remoteBootOurSecValid || !remoteBootPeerSecValid ||
      secRecordChanged(our, remoteBootOurSec) ||
      secRecordChanged(peer, remoteBootPeerSec);

  if (!changed) return;


  struct ble_store_key_sec ourKey = {};
  ourKey.peer_addr = our.peer_addr;
  ourKey.idx = 0;

  struct ble_store_key_sec peerKey = {};
  peerKey.peer_addr = peer.peer_addr;
  peerKey.idx = 0;

  const int delOur = ble_store_delete_our_sec(&ourKey);
  const int delPeer = ble_store_delete_peer_sec(&peerKey);

  const int writeOur = ble_store_write_our_sec(&our);
  const int writePeer = ble_store_write_peer_sec(&peer);

  if (writeOur == 0 && writePeer == 0) {
    remoteBootOurSec = our;
    remoteBootPeerSec = peer;
    remoteBootOurSecValid = true;
    remoteBootPeerSecValid = true;
    Serial.println("[SECSTORE] Updated physical-remote security keys persisted to NVS");
  } else {
    Serial.printf("[SECSTORE] WARNING: NVS key refresh incomplete delOUR=%d delPEER=%d writeOUR=%d writePEER=%d\n",
                  delOur, delPeer, writeOur, writePeer);
  }
}

static bool hasOriginalRemoteBond() {
  const NimBLEAddress remote(ORIGINAL_REMOTE_MAC, BLE_ADDR_PUBLIC);
  const int count = NimBLEDevice::getNumBonds();

  for (int i = 0; i < count; ++i) {
    if (NimBLEDevice::getBondedAddress(i) == remote) return true;
  }
  return false;
}

static bool hasTvBond() {
  const NimBLEAddress remote(ORIGINAL_REMOTE_MAC, BLE_ADDR_PUBLIC);
  const int count = NimBLEDevice::getNumBonds();

  for (int i = 0; i < count; ++i) {
    if (NimBLEDevice::getBondedAddress(i) != remote) return true;
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
  NimBLEDevice::init(ORIGINAL_REMOTE_NAME);
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
  captureRemoteBootSecurityStore();
  Serial.printf("[BLE] Bonds=%d remote=%d tv=%d\n",
                NimBLEDevice::getNumBonds(),
                hasOriginalRemoteBond(),
                hasTvBond());

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
    configureBondedIdleAdvertising();
  } else {
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
          Serial.println("[HK] OFF -> Power, Right, OK shutdown macro");
          sendHonorKey(HONOR_POWER);
          delay(900);
          sendHonorKey(HONOR_RIGHT);
          delay(250);
          sendHonorKey(HONOR_OK);
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
  Serial.println("Honor X1 HomeKit BLE Relay");

  setupHonorBleKeyboard();

  // First-time remote pairing: hold HOME+MENU on the original HDRC-BV1.
  startOriginalRemoteScan();

  homeSpan.setLogLevel(0);
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
    remoteDoConnect = false;
    if (!connectOriginalRemote()) {
      if (!remoteRetryAt) remoteRetryAt = millis() + 3000;
    }
  }

  if (!remoteConnected && !remoteDoConnect &&
      remoteRetryAt && (int32_t)(millis() - remoteRetryAt) >= 0) {
    startOriginalRemoteScan();
  }
}
