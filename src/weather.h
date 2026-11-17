#pragma once
#include <Arduino.h>

// Open-Meteo weather client (https://open-meteo.com — free, no API key).
// Runs its own low-priority task on core 0; never touches the UI directly.
namespace weather {

constexpr int DAYS = 4;

struct Day {
  int8_t  code = -1;            // WMO weather code
  float   hi = 0, lo = 0;       // in the display unit (°C or °F)
  uint8_t precipPct = 0;        // max precipitation probability
  uint8_t wday = 0;             // 0=Sun … 6=Sat, location-local
};

struct Snapshot {
  bool    valid = false;        // at least one good fetch
  bool    fahrenheit = false;   // unit the numbers below are in
  float   temp = 0, feels = 0;
  int8_t  code = -1;
  bool    isDay = true;
  uint8_t humidity = 0;
  float   wind = 0;             // km/h or mph, matching `fahrenheit`
  Day     days[DAYS];
  int64_t sunrise = 0, sunset = 0;   // today, unix seconds
  time_t  fetchedAt = 0;             // unix seconds of last good fetch
  String  error;                     // last failure reason ("" when fine)
};

void     begin();              // spawn the fetch task (after settings.load())
Snapshot snapshot();           // thread-safe copy
uint32_t version();            // bumps on every new snapshot — cheap change check
void     refresh();            // fetch now (location or unit changed)

// Blocking city lookup via Open-Meteo geocoding. On success fills a display
// name ("Portland, Oregon") and coordinates. Call from the UI thread.
bool geocode(const String& query, String& name, float& lat, float& lon, String& err);

// WMO code → short caps label / icon family.
const char* codeLabel(int code);
enum class Icon : uint8_t { Clear, PartCloud, Cloud, Fog, Drizzle, Rain, Snow, Storm, Unknown };
Icon codeIcon(int code);

}  // namespace weather
