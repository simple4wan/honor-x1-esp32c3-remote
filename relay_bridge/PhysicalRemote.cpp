#include "PhysicalRemote.h"

#include "HonorBleBridge.h"
#include "config.h"

#include <NimBLEDevice.h>
#include "host/ble_store.h"

namespace PhysicalRemote {

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

static void startScan();
static bool hasBond();
static void captureBootSecurityStore();
static void forcePersistSecurityIfChanged();

static void relayReport(NimBLERemoteCharacteristic *, uint8_t *data,
                        size_t len, bool) {
  HonorBle::relayRemoteReport(data, len);
}

class RemoteClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *client) override {
    remoteConnected = true;
    remoteSecurityReady = false;

    const bool securityStarted = client->secureConnection(true);
    if (!securityStarted) {
      Serial.printf("[REMOTE] Security start failed lastError=%d\n",
                    client->getLastError());
    }
  }

  void onDisconnect(NimBLEClient *, int reason) override {
    remoteConnected = false;
    remoteSecurityReady = false;
    remoteReport1 = nullptr;
    remoteDoConnect = false;
    remoteRetryAt = millis() + 3000;

    Serial.printf("[REMOTE] Disconnected reason=%d; retry in 3000 ms\n",
                  reason);
  }

  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    remoteSecurityReady = info.isEncrypted();

    if (info.isEncrypted()) {
      Serial.printf("[REMOTE] Security restored bonded=%d peer=%s\n",
                    info.isBonded(), info.getAddress().toString().c_str());
      forcePersistSecurityIfChanged();
      return;
    }

    Serial.printf("[REMOTE] Security failed peer=%s\n",
                  info.getAddress().toString().c_str());

    NimBLEClient *client =
        NimBLEDevice::getClientByHandle(info.getConnHandle());
    if (client) client->disconnect();
  }
};

static RemoteClientCallbacks remoteClientCallbacks;

static bool advHasMfg(const NimBLEAdvertisedDevice *device,
                      uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
  if (!device->haveManufacturerData()) return false;

  std::string mfg = device->getManufacturerData();
  return mfg.size() >= 4 &&
         (uint8_t)mfg[0] == b0 &&
         (uint8_t)mfg[1] == b1 &&
         (uint8_t)mfg[2] == b2 &&
         (uint8_t)mfg[3] == b3;
}

class RemoteScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *device) override {
    const NimBLEAddress configuredRemote(ORIGINAL_REMOTE_MAC, BLE_ADDR_PUBLIC);

    const bool targetAddr =
        !device->getAddress().isNull() &&
        device->getAddress() == configuredRemote;

    const bool targetName =
        device->haveName() &&
        device->getName() == ORIGINAL_REMOTE_NAME;

    if (!targetAddr && !targetName) return;
    if (!device->isConnectable()) return;

    const bool mfg0400 = advHasMfg(device, 0x02, 0x7D, 0x04, 0x00);
    const bool mfg0411 = advHasMfg(device, 0x02, 0x7D, 0x04, 0x11);

    Serial.printf("[REMOTE] Found %s RSSI=%d state=%s\n",
                  ORIGINAL_REMOTE_NAME,
                  device->getRSSI(),
                  mfg0400 ? "04 00" : (mfg0411 ? "04 11" : "other"));

    remoteAdv = device;
    remoteConnectAddr = device->getAddress().isNull()
        ? NimBLEAddress(ORIGINAL_REMOTE_MAC, BLE_ADDR_PUBLIC)
        : device->getAddress();

    NimBLEDevice::getScan()->stop();
    remoteDoConnect = true;
  }

  void onScanEnd(const NimBLEScanResults&, int) override {
    if (!remoteConnected && !remoteDoConnect) {
      NimBLEDevice::getScan()->start(0, false, true);
    }
  }
};

static RemoteScanCallbacks remoteScanCallbacks;

static bool connectRemote() {
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
    remoteClient->setConnectionParams(16, 24, 0, 1000);
    remoteClient->setConnectTimeout(10000);
    remoteClient->setConnectRetries(2);
  }

  Serial.printf("[REMOTE] Connecting bonded=%d\n", hasBond());

  // No MTU exchange: the keyboard report is only 8 bytes.
  if (!remoteClient->connect(remoteConnectAddr, true, false, false)) {
    Serial.println("[REMOTE] connect failed");
    remoteRetryAt = millis() + 3000;
    return false;
  }

  const uint32_t waitStart = millis();
  while (remoteClient->isConnected() && !remoteSecurityReady &&
         (uint32_t)(millis() - waitStart) < 5000) {
    delay(10);
  }

  if (!remoteClient->isConnected() || !remoteSecurityReady) {
    Serial.printf("[REMOTE] Security timeout connected=%d lastError=%d\n",
                  remoteClient->isConnected(),
                  remoteClient->getLastError());

    remoteRetryAt = millis() + 3000;
    if (remoteClient->isConnected()) remoteClient->disconnect();
    return false;
  }

  Serial.printf("[REMOTE] Security ready after %lu ms\n",
                (unsigned long)(millis() - waitStart));

  NimBLERemoteService *hid =
      remoteClient->getService(NimBLEUUID((uint16_t)0x1812));

  if (!hid) {
    Serial.println("[REMOTE] HID service 0x1812 not found");
    remoteClient->disconnect();
    return false;
  }

  remoteReport1 = nullptr;

  const auto& reportChars = hid->getCharacteristics(true);
  for (auto *chr : reportChars) {
    if (!chr || chr->getUUID() != NimBLEUUID((uint16_t)0x2A4D)) continue;

    NimBLERemoteDescriptor *ref =
        chr->getDescriptor(NimBLEUUID((uint16_t)0x2908));
    if (!ref) continue;

    NimBLEAttValue v = ref->readValue();
    if (v.size() < 2) continue;

    if (v.data()[0] == 1 && v.data()[1] == 1) {
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
      !remoteReport1->subscribe(true, relayReport, true)) {
    Serial.println("[REMOTE] subscribe Report ID 1 failed");
    remoteClient->disconnect();
    return false;
  }

  remoteConnected = true;
  Serial.println("[REMOTE] Physical remote relay ready");
  return true;
}

static void startScan() {
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

static bool readSecurityStore(struct ble_store_value_sec *our,
                              struct ble_store_value_sec *peer) {
  const NimBLEAddress remote(ORIGINAL_REMOTE_MAC, BLE_ADDR_PUBLIC);

  struct ble_store_key_sec key = {};
  key.peer_addr = *remote.getBase();
  key.idx = 0;

  const int ourRc = ble_store_read_our_sec(&key, our);
  const int peerRc = ble_store_read_peer_sec(&key, peer);

  return ourRc == 0 && peerRc == 0;
}

static void captureBootSecurityStore() {
  struct ble_store_value_sec our = {};
  struct ble_store_value_sec peer = {};

  if (readSecurityStore(&our, &peer)) {
    remoteBootOurSec = our;
    remoteBootPeerSec = peer;
    remoteBootOurSecValid = true;
    remoteBootPeerSecValid = true;
    return;
  }

  remoteBootOurSecValid = false;
  remoteBootPeerSecValid = false;
  Serial.println("[SECSTORE] WARNING: remote security snapshot unavailable");
}

static bool secRecordChanged(const struct ble_store_value_sec& a,
                             const struct ble_store_value_sec& b) {
  return a.bond_count != b.bond_count ||
         a.key_size != b.key_size ||
         a.ediv != b.ediv ||
         a.rand_num != b.rand_num ||
         a.ltk_present != b.ltk_present ||
         (a.ltk_present &&
          memcmp(a.ltk, b.ltk, sizeof(a.ltk)) != 0) ||
         a.irk_present != b.irk_present ||
         (a.irk_present &&
          memcmp(a.irk, b.irk, sizeof(a.irk)) != 0) ||
         a.csrk_present != b.csrk_present ||
         (a.csrk_present &&
          memcmp(a.csrk, b.csrk, sizeof(a.csrk)) != 0) ||
         a.authenticated != b.authenticated ||
         a.sc != b.sc;
}

static void forcePersistSecurityIfChanged() {
  struct ble_store_value_sec our = {};
  struct ble_store_value_sec peer = {};

  if (!readSecurityStore(&our, &peer)) {
    Serial.println("[SECSTORE] Current remote security records unavailable");
    return;
  }

  const bool changed =
      !remoteBootOurSecValid ||
      !remoteBootPeerSecValid ||
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

    Serial.println("[SECSTORE] Updated remote security keys persisted to NVS");
    return;
  }

  Serial.printf("[SECSTORE] WARNING: NVS refresh incomplete delOUR=%d delPEER=%d writeOUR=%d writePEER=%d\n",
                delOur, delPeer, writeOur, writePeer);
}

static bool hasBond() {
  const NimBLEAddress remote(ORIGINAL_REMOTE_MAC, BLE_ADDR_PUBLIC);
  const int count = NimBLEDevice::getNumBonds();

  for (int i = 0; i < count; ++i) {
    if (NimBLEDevice::getBondedAddress(i) == remote) return true;
  }

  return false;
}

bool isConnected() {
  return remoteConnected;
}

void begin() {
  captureBootSecurityStore();

  Serial.printf("[REMOTE] Bond=%d target=%s\n",
                hasBond(), ORIGINAL_REMOTE_MAC);

  startScan();
}

void loop() {
  if (remoteDoConnect) {
    remoteDoConnect = false;

    if (!connectRemote() && !remoteRetryAt) {
      remoteRetryAt = millis() + 3000;
    }
  }

  if (!remoteConnected &&
      !remoteDoConnect &&
      remoteRetryAt &&
      (int32_t)(millis() - remoteRetryAt) >= 0) {
    startScan();
  }
}

}  // namespace PhysicalRemote
