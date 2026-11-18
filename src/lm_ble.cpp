#include "lm_ble.h"
#include "lm_cloud.h"
#include "storage.h"

#include <NimBLEDevice.h>
#include <ArduinoJson.h>

namespace lmble {

// GATT characteristics (service UUID isn't needed: we search all services)
static const char* AUTH_CHAR = "0d0b7847-e12b-09a8-b04b-8e0922a9abab";
static const char* SHOT_CHAR = "0e0b7847-e12b-09a8-b04b-8e0922a9abab";
static const char* NAME_PREFIXES[] = { "MICRA", "MINI", "LINEA", "GS3" };

static volatile Link g_link = Link::Off;
static volatile bool g_forget = false;
static NimBLEClient* g_client = nullptr;
static volatile bool g_skipReplay = false;   // first notify after subscribe = stale replay

Link link() { return g_link; }
void forgetMachine() { g_forget = true; }
const char* linkName(Link l) {
  switch (l) {
    case Link::Off:           return "OFF";
    case Link::NoToken:       return "NO TOKEN";
    case Link::Scanning:      return "SEARCHING";
    case Link::NotFound:      return "NOT FOUND";
    case Link::Connecting:    return "CONNECTING";
    case Link::AuthFailed:    return "AUTH FAILED";
    case Link::NoShotCounter: return "UNSUPPORTED";
    case Link::Live:          return "LIVE";
  }
  return "?";
}
static void setLink(Link l) {
  if (g_link == l) return;
  g_link = l;
  Serial.printf("[ble] %s\n", linkName(l));
  lmcloud::setBleLive(l == Link::Live);
}

// ── notifications ────────────────────────────────────────────────────────────
// Runs in the NimBLE host task: parse and hand off, nothing slow.
static void onShotNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (g_skipReplay) { g_skipReplay = false; return; }
  while (len && data[len - 1] == 0) --len;               // NUL-padded
  JsonDocument d;
  if (deserializeJson(d, (const char*)data, len)) return;  // e.g. truncated statistics blob
  using E = lmcloud::BleEvent;

  if (!d["BrewingStartedGroup1DoseIndex"].isNull()) lmcloud::bleEvent(E::BrewStart);
  if (!d["BrewingUpdateGroup1Time"].isNull()) {
    float t   = d["BrewingUpdateGroup1Time"] | 0.f;
    bool  ev  = d["BrewingUpdateGroup1Ev"] | true;
    bool  bf  = d["BrewingUpdateGroup1Backflush"] | false;
    if (bf)       lmcloud::bleEvent(E::Backflushing);
    else if (ev)  lmcloud::bleEvent(E::BrewTick, t);
    else          lmcloud::bleEvent(E::BrewStop, t);
  }
  if (!d["BrewingStoppedGroup1Time"].isNull() || !d["BrewingStoppedGroup1DoseIndex"].isNull() ||
      !d["BrewingStoppedGroup1StopType"].isNull())
    lmcloud::bleEvent(E::BrewStop, d["BrewingStoppedGroup1Time"] | 0.f);
  if (!d["FlushStoppedGroup1Time"].isNull() || !d["FlushStoppedGroup1DoseIndex"].isNull())
    lmcloud::bleEvent(E::FlushStop, d["FlushStoppedGroup1Time"] | 0.f);

  if (!d["CoffeeBoiler1UpdateTemperature"].isNull())
    lmcloud::bleEvent(E::CoffeeTemp, d["CoffeeBoiler1UpdateTemperature"] | 0.f);
  if (!d["SteamBoilerUpdateTemperature"].isNull())
    lmcloud::bleEvent(E::SteamTemp, d["SteamBoilerUpdateTemperature"] | 0.f);
  const char* mode = d["MachineMode"] | "";
  if (!strcmp(mode, "BrewingMode")) lmcloud::bleEvent(E::ModeOn);
  else if (!strcmp(mode, "StandBy")) lmcloud::bleEvent(E::ModeStandby);
}

// ── connection steps ─────────────────────────────────────────────────────────
static bool nameMatches(const std::string& n) {
  for (auto p : NAME_PREFIXES) if (n.rfind(p, 0) == 0) return true;
  return false;
}

static bool scanForMachine() {
  setLink(Link::Scanning);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100); scan->setWindow(99);
  NimBLEScanResults res = scan->getResults(6000, false);   // blocking, 6 s
  const NimBLEAdvertisedDevice* best = nullptr;
  for (int i = 0; i < res.getCount(); ++i) {
    const NimBLEAdvertisedDevice* d = res.getDevice(i);
    if (!d->haveName() || !nameMatches(d->getName())) continue;
    Serial.printf("[ble] found %s %s rssi %d\n", d->getName().c_str(),
                  d->getAddress().toString().c_str(), d->getRSSI());
    // prefer a name carrying our serial, else the strongest signal
    bool hasSerial = settings.lmSerial.length() &&
                     d->getName().find(settings.lmSerial.c_str()) != std::string::npos;
    if (!best || hasSerial || d->getRSSI() > best->getRSSI()) best = d;
    if (hasSerial) break;
  }
  bool ok = best != nullptr;
  if (ok) {
    settings.bleAddr     = best->getAddress().toString().c_str();
    settings.bleAddrType = best->getAddress().getType();
    settings.save();
  }
  scan->clearResults();
  return ok;
}

// Search the already-discovered services. Do NOT pass refresh=true here:
// rediscovery deletes and recreates every service/characteristic object, so a
// pointer from an earlier lookup would dangle (that crashed in writeValue).
static NimBLERemoteCharacteristic* findChar(const char* uuid) {
  for (auto* svc : g_client->getServices(false)) {
    if (auto* c = svc->getCharacteristic(uuid)) return c;
  }
  return nullptr;
}

static bool authenticate(NimBLERemoteCharacteristic* auth) {
  const String& tok = settings.bleToken;
  if (!auth->writeValue((const uint8_t*)tok.c_str(), tok.length(), true)) return false;
  for (int i = 0; i < 10; ++i) {               // machine answers 0x01 on success
    if (i) delay(50);
    NimBLEAttValue v = auth->readValue();
    if (v.size() == 1 && v[0] == 0x01) return true;
  }
  return false;
}

// One full session: connect → auth → subscribe → stay until the link drops.
// Returns the delay before the next attempt.
static uint32_t session() {
  NimBLEAddress addr(std::string(settings.bleAddr.c_str()), settings.bleAddrType);
  setLink(Link::Connecting);
  if (!g_client) {
    g_client = NimBLEDevice::createClient();
    g_client->setConnectTimeout(8000);
  }
  static int s_connFails = 0;
  if (!g_client->connect(addr)) {
    Serial.printf("[ble] connect to %s failed\n", settings.bleAddr.c_str());
    if (++s_connFails >= 4) {                  // maybe the address changed: rescan
      s_connFails = 0; settings.bleAddr = ""; settings.save();
    }
    return 10000;
  }
  s_connFails = 0;

  g_client->getServices(true);                 // discover once per connection
  Serial.printf("[ble] connected, rssi %d\n", g_client->getRssi());
  NimBLERemoteCharacteristic* auth = findChar(AUTH_CHAR);
  NimBLERemoteCharacteristic* shot = findChar(SHOT_CHAR);
  if (!auth) { Serial.println("[ble] no auth characteristic"); g_client->disconnect(); return 60000; }
  if (!authenticate(auth)) {
    setLink(Link::AuthFailed);
    lmcloud::requestThings();                   // token may have rotated
    g_client->disconnect();
    return 60000;
  }
  if (!shot || !shot->canNotify()) {
    setLink(Link::NoShotCounter);               // older machine firmware
    g_client->disconnect();
    return 600000;
  }
  g_skipReplay = true;
  if (!shot->subscribe(true, onShotNotify)) {
    Serial.println("[ble] subscribe failed");
    g_client->disconnect();
    return 10000;
  }
  setLink(Link::Live);
  while (g_client->isConnected() && settings.bleEnabled && !g_forget) delay(250);
  if (g_client->isConnected()) g_client->disconnect();
  setLink(Link::Off);
  return 2000;
}

static void task(void*) {
  NimBLEDevice::init("");
  NimBLEDevice::setMTU(517);                   // statistics blobs run ~450 bytes
  uint32_t backoff = 0;
  for (;;) {
    if (backoff) { delay(backoff); backoff = 0; }
    if (g_forget) { g_forget = false; settings.bleAddr = ""; settings.save(); }
    if (!settings.bleEnabled) { setLink(Link::Off); backoff = 1000; continue; }
    if (settings.bleToken.isEmpty()) { setLink(Link::NoToken); backoff = 2000; continue; }
    if (settings.bleAddr.isEmpty()) {
      if (!scanForMachine()) { setLink(Link::NotFound); backoff = 30000; continue; }
    }
    backoff = session();
  }
}

void begin() {
  // core 1 with the UI: core 0 already carries WiFi, lmcloud and NimBLE's own
  // host task; this one mostly sleeps or blocks on BLE operations.
  xTaskCreatePinnedToCore(task, "lmble", 6 * 1024, nullptr, 1, nullptr, 1);
}

}  // namespace lmble
