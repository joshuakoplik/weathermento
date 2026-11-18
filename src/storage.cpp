#include "storage.h"
#include <string.h>

Settings settings;
static SemaphoreHandle_t s_mtx = nullptr;
static inline void take(){ if(!s_mtx) s_mtx=xSemaphoreCreateMutex();
                           xSemaphoreTake(s_mtx,portMAX_DELAY); }
static inline void give(){ xSemaphoreGive(s_mtx); }

void Settings::load() {
  take();
  p_.begin("lm", false);
  wifiSsid = p_.getString("ws", cfg::DEF_WIFI_SSID);
  wifiPass = p_.getString("wp", cfg::DEF_WIFI_PASS);
  lmUser   = p_.getString("lu", cfg::DEF_LM_USER);
  lmPass   = p_.getString("lp", cfg::DEF_LM_PASS);
  lmSerial = p_.getString("ls", cfg::DEF_LM_SERIAL);
  darkMode   = p_.getBool("dm", false);
  fahrenheit = p_.getBool("fh", false);
  timeZoneIndex = p_.getUChar("tz", cfg::DEF_TIME_ZONE_INDEX);
  if (timeZoneIndex >= cfg::TIME_ZONE_COUNT) timeZoneIndex = cfg::DEF_TIME_ZONE_INDEX;
  shotHoldIndex = p_.getUChar("sh", cfg::DEF_SHOT_HOLD_INDEX);
  if (shotHoldIndex >= cfg::SHOT_HOLD_COUNT) shotHoldIndex = cfg::DEF_SHOT_HOLD_INDEX;
  dimSec     = p_.getUChar("ds", 0);
  sleepMin   = p_.getUChar("sm", 0);
  // The weather build is an always-on wall display: never dim, never sleep,
  // whatever an earlier install (or an old Setup tap) saved. The UI no longer
  // offers either setting.
  dimSec = 0; sleepMin = 0;
  wxCity     = p_.getString("wc", "");
  wxLat      = p_.getFloat("wla", 0);
  wxLon      = p_.getFloat("wlo", 0);
  wxHasLoc   = p_.getBool("wok", false);
  wxIdleSec  = p_.getUChar("wi", 60);
  rotAuto    = p_.getBool("ra", true);
  rotFlipped = p_.getBool("rf", false);
  rotRefAxis = p_.getChar("rx", -1);
  rotRefSign = p_.getChar("rs", 0);
  bleEnabled = p_.getBool("be", true);
  bleToken   = p_.getString("bt", "");
  bleAddr    = p_.getString("ba", "");
  bleAddrType= p_.getUChar("bat", 0);
  instId   = p_.getString("iid", "");
  size_t n = p_.getBytes("epk", ecPriv, sizeof ecPriv);
  ecPrivValid = (n == sizeof ecPriv);
  p_.end();
  give();
}

void Settings::save() {
  take();
  p_.begin("lm", false);
  p_.putString("ws", wifiSsid);
  p_.putString("wp", wifiPass);
  p_.putString("lu", lmUser);
  p_.putString("lp", lmPass);
  p_.putString("ls", lmSerial);
  p_.putBool  ("dm", darkMode);
  p_.putBool  ("fh", fahrenheit);
  p_.putUChar ("tz", timeZoneIndex);
  p_.putUChar ("sh", shotHoldIndex);
  p_.putUChar ("ds", dimSec);
  p_.putUChar ("sm", sleepMin);
  p_.putBool  ("wxv", true);
  p_.putString("wc", wxCity);
  p_.putFloat ("wla", wxLat);
  p_.putFloat ("wlo", wxLon);
  p_.putBool  ("wok", wxHasLoc);
  p_.putUChar ("wi", wxIdleSec);
  p_.putBool  ("ra", rotAuto);
  p_.putBool  ("rf", rotFlipped);
  p_.putChar  ("rx", rotRefAxis);
  p_.putChar  ("rs", rotRefSign);
  p_.putBool  ("be", bleEnabled);
  p_.putString("bt", bleToken);
  p_.putString("ba", bleAddr);
  p_.putUChar ("bat", bleAddrType);
  p_.putString("iid", instId);
  if (ecPrivValid) p_.putBytes("epk", ecPriv, sizeof ecPriv);
  p_.end();
  give();
}

void Settings::factoryReset() {
  p_.begin("lm", false);
  p_.clear();
  p_.end();
  memset(ecPriv, 0, sizeof ecPriv);
  ecPrivValid = false;
  instId = "";
  load();
}
