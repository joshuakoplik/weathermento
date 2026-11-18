#pragma once
#include <Preferences.h>
#include "config.h"

// Thin NVS wrapper holding all user-editable + crypto-persistent state.
struct Settings {
  String wifiSsid, wifiPass;
  String lmUser, lmPass, lmSerial;
  bool   darkMode   = false;
  bool   fahrenheit = false;
  uint8_t timeZoneIndex = cfg::DEF_TIME_ZONE_INDEX;
  uint8_t shotHoldIndex = cfg::DEF_SHOT_HOLD_INDEX;
  uint8_t dimSec    = 0;     // backlight dim after N s idle (0 = never)
  uint8_t sleepMin  = 0;     // AXP power-off after N min idle (0 = never)
  // weather — the idle screen
  String  wxCity;            // display name from geocoding ("Portland, Oregon")
  float   wxLat = 0, wxLon = 0;
  bool    wxHasLoc = false;
  uint8_t wxIdleSec = 60;    // return to weather after N s untouched (0 = never)
  // screen rotation (landscape, either way up)
  bool    rotAuto    = true;  // follow the accelerometer
  bool    rotFlipped = false; // FIXED mode: upside-down
  int8_t  rotRefAxis = -1;    // learned: accel axis that reads gravity when upright
  int8_t  rotRefSign = 0;     //   (0 = x, 1 = y; -1 = not learned yet) and its sign
  // Bluetooth link to the machine (shot counter, live temps)
  bool    bleEnabled = true;
  String  bleToken;           // bleAuthToken from the cloud's /things (cached so
                              // BLE works even when LM's cloud is down)
  String  bleAddr;            // machine's BLE address once found by scanning
  uint8_t bleAddrType = 0;
  // crypto material — generated once, never shown
  String  instId;          // lowercase uuid
  uint8_t ecPriv[32];      // P-256 private scalar (raw big-endian)
  bool    ecPrivValid = false;

  void load();
  void save();
  void factoryReset();

private:
  Preferences p_;
};

extern Settings settings;
