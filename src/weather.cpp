#include "weather.h"
#include "storage.h"
#include "wx_ca.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_system.h>

namespace weather {

static constexpr uint32_t REFRESH_MS = 15UL * 60 * 1000;   // Open-Meteo updates ~15 min
static constexpr uint32_t RETRY_MS   = 60UL * 1000;
static constexpr time_t   CLOCK_SANE = 1700000000;         // NTP has landed (TLS needs it)

static Snapshot          g_snap;
static SemaphoreHandle_t g_mtx;
static volatile uint32_t g_ver = 0;
static TaskHandle_t      g_task;

// Crash-loop guard. RTC slow memory survives a panic reboot but not a power
// cycle; the magic word tells the two apart. Two crash-reboots in a row pause
// fetching (and so the weather screen's data path) for that boot, so a bad
// response or bug can never brick the device into a reboot loop.
RTC_NOINIT_ATTR static uint32_t s_rtcMagic;
RTC_NOINIT_ATTR static uint32_t s_rtcCrashes;
static constexpr uint32_t RTC_MAGIC = 0x57454154;   // "WEAT"
static volatile bool g_paused = false;
bool paused() { return g_paused; }

Snapshot snapshot() {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  Snapshot s = g_snap;
  xSemaphoreGive(g_mtx);
  return s;
}
uint32_t version() { return g_ver; }
void refresh() { g_paused = false; if (g_task) xTaskNotifyGive(g_task); }

static void setError(const String& e) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_snap.error = e;
  xSemaphoreGive(g_mtx);
  ++g_ver;
}

// ── HTTPS GET ────────────────────────────────────────────────────────────────
// Verified against WX_ROOT_CAS first. If the TLS connection itself fails (e.g.
// Open-Meteo moved to a CA we don't ship), retry once unverified: the data is
// public weather, nothing secret is sent, and a stale root shouldn't blank the
// idle screen. An HTTP-level error (server answered) is never retried.
static bool httpGet(const String& url, String& body, String& err) {
  for (int pass = 0; pass < 2; ++pass) {
    WiFiClientSecure tls;
    if (pass == 0) tls.setCACert(WX_ROOT_CAS);
    else           tls.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(10000);
    http.setConnectTimeout(8000);
    if (!http.begin(tls, url)) { err = "bad url"; return false; }
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
      body = http.getString();
      http.end();
      if (pass) Serial.println("[wx] warning: fetched without certificate verification");
      return true;
    }
    http.end();
    if (code > 0) { err = "http " + String(code); return false; }
    err = HTTPClient::errorToString(code);
    Serial.printf("[wx] %s (pass %d)\n", err.c_str(), pass);
  }
  return false;
}

static String urlEncode(const String& s) {
  static const char* hex = "0123456789ABCDEF";
  String o;
  for (size_t i = 0; i < s.length(); ++i) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

// ── forecast ─────────────────────────────────────────────────────────────────
static bool fetch() {
  const bool f = settings.fahrenheit;
  char url[512];
  snprintf(url, sizeof url,
    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
      "weather_code,is_day,wind_speed_10m"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
      "precipitation_probability_max,sunrise,sunset"
    "&forecast_days=%d&timezone=auto&timeformat=unixtime%s",
    settings.wxLat, settings.wxLon, DAYS,
    f ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");

  String body, err;
  if (!httpGet(url, body, err)) { setError(err); return false; }

  JsonDocument doc;
  DeserializationError je = deserializeJson(doc, body);
  if (je) { setError(String("json ") + je.c_str()); return false; }
  if (doc["error"] | false) { setError(doc["reason"] | "api error"); return false; }

  JsonObjectConst cur = doc["current"], daily = doc["daily"];
  if (cur.isNull() || daily.isNull()) { setError("no data"); return false; }

  Snapshot s;
  s.valid      = true;
  s.fahrenheit = f;
  s.temp       = cur["temperature_2m"]       | 0.f;
  s.feels      = cur["apparent_temperature"] | s.temp;
  s.code       = cur["weather_code"]         | -1;
  s.isDay      = (cur["is_day"] | 1) != 0;
  s.humidity   = cur["relative_humidity_2m"] | 0;
  s.wind       = cur["wind_speed_10m"]       | 0.f;
  s.fetchedAt  = time(nullptr);

  // daily.time is the UTC instant of local midnight; shift by the location's
  // offset so gmtime() yields the location's own weekday.
  long off = doc["utc_offset_seconds"] | 0L;
  JsonArrayConst t = daily["time"];
  for (int i = 0; i < DAYS && i < (int)t.size(); ++i) {
    Day& d = s.days[i];
    d.code      = daily["weather_code"][i]                  | -1;
    d.hi        = daily["temperature_2m_max"][i]            | 0.f;
    d.lo        = daily["temperature_2m_min"][i]            | 0.f;
    d.precipPct = daily["precipitation_probability_max"][i] | 0;
    time_t local = (time_t)(t[i].as<int64_t>() + off);
    struct tm tm; gmtime_r(&local, &tm);
    d.wday = tm.tm_wday;
  }
  s.sunrise = daily["sunrise"][0] | (int64_t)0;
  s.sunset  = daily["sunset"][0]  | (int64_t)0;

  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_snap = s;
  xSemaphoreGive(g_mtx);
  ++g_ver;
  Serial.printf("[wx] %.1f%s code %d\n", s.temp, f ? "F" : "C", s.code);
  return true;
}

static void task(void*) {
  bool     due    = true;
  uint32_t nextAt = 0;
  bool     lastF  = settings.fahrenheit;
  for (;;) {
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000))) due = true;
    if (s_rtcCrashes && millis() > 180000) s_rtcCrashes = 0;   // 3 min stable
    if (g_paused) continue;
    if (settings.fahrenheit != lastF) { lastF = settings.fahrenheit; due = true; }
    if (!due && (int32_t)(millis() - nextAt) >= 0) due = true;
    if (!due) continue;
    if (!settings.wxHasLoc) continue;                 // stays due until a city is set
    if (WiFi.status() != WL_CONNECTED) continue;      // lmcloud owns the WiFi link
    if (time(nullptr) < CLOCK_SANE) continue;         // cert validity needs real time
    bool ok = fetch();
    due    = false;
    nextAt = millis() + (ok ? REFRESH_MS : RETRY_MS);
  }
}

void begin() {
  g_mtx = xSemaphoreCreateMutex();
  esp_reset_reason_t r = esp_reset_reason();
  bool crashed = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
                 r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
  if (s_rtcMagic != RTC_MAGIC) { s_rtcMagic = RTC_MAGIC; s_rtcCrashes = 0; }
  s_rtcCrashes = crashed ? s_rtcCrashes + 1 : 0;
  if (s_rtcCrashes >= 2) {
    g_paused = true;
    g_snap.error = "paused after a crash";
    Serial.printf("[wx] %u crash reboots in a row (reason %d) - weather paused\n",
                  (unsigned)s_rtcCrashes, (int)r);
  }
  // Low priority, core 0 alongside lmcloud — a slow fetch never stalls the UI.
  xTaskCreatePinnedToCore(task, "weather", 16 * 1024, nullptr, 1, &g_task, 0);
}

// ── geocoding ────────────────────────────────────────────────────────────────
static bool startsWithCI(const String& hay, const String& needle) {
  if (needle.isEmpty() || hay.length() < needle.length()) return false;
  return hay.substring(0, needle.length()).equalsIgnoreCase(needle);
}

int search(const String& query, Place* out, int max, String& err) {
  String q = query; q.trim();
  if (q.length() < 2) { err = "too short"; return 0; }

  // "45.52, -122.68" — take coordinates verbatim.
  float a, b; char tail;
  if (sscanf(q.c_str(), "%f , %f %c", &a, &b, &tail) == 2 &&
      a >= -90 && a <= 90 && b >= -180 && b <= 180) {
    out[0].name = q; out[0].detail = "coordinates"; out[0].lat = a; out[0].lon = b;
    return 1;
  }

  // "Portland, Oregon" / "Paris, FR" — Open-Meteo matches the place name (or
  // postal code) only, so search on the part before the comma and use the rest
  // to float matching results to the top.
  String place = q, qual;
  int comma = q.indexOf(',');
  if (comma >= 0) { place = q.substring(0, comma); qual = q.substring(comma + 1); }
  place.trim(); qual.trim();

  String body;
  String url = "https://geocoding-api.open-meteo.com/v1/search?count=10&language=en&format=json&name="
               + urlEncode(place);
  if (!httpGet(url, body, err)) return 0;

  JsonDocument doc;
  if (deserializeJson(doc, body)) { err = "bad reply"; return 0; }
  JsonArrayConst res = doc["results"];
  if (res.isNull() || res.size() == 0) { err = "no match for \"" + place + "\""; return 0; }

  auto matches = [&](JsonObjectConst r) {
    return !qual.isEmpty() &&
           (startsWithCI(r["admin1"] | "", qual) || startsWithCI(r["country"] | "", qual) ||
            qual.equalsIgnoreCase(r["country_code"] | ""));
  };
  int n = 0;
  for (int pass = 0; pass < 2 && n < max; ++pass) {     // qualifier matches first
    for (JsonObjectConst r : res) {
      if (n >= max) break;
      if (!qual.isEmpty() && matches(r) != (pass == 0)) continue;
      if (qual.isEmpty() && pass == 1) break;
      Place& p = out[n++];
      const char* nm = r["name"] | "";
      const char* a1 = r["admin1"] | "";
      const char* co = r["country"] | "";
      p.lat  = r["latitude"]  | 0.f;
      p.lon  = r["longitude"] | 0.f;
      p.name = nm;
      const char* region = *a1 ? a1 : co;
      if (*region && strcmp(region, nm)) { p.name += ", "; p.name += region; }
      p.detail = a1;
      if (*co) { if (p.detail.length()) p.detail += ", "; p.detail += co; }
    }
  }
  return n;
}

// ── WMO codes ────────────────────────────────────────────────────────────────
const char* codeLabel(int c) {
  switch (c) {
    case 0:  return "CLEAR";
    case 1:  return "MOSTLY CLEAR";
    case 2:  return "PARTLY CLOUDY";
    case 3:  return "OVERCAST";
    case 45: case 48: return "FOG";
    case 51: case 53: case 55: return "DRIZZLE";
    case 56: case 57: return "ICY DRIZZLE";
    case 61: return "LIGHT RAIN";
    case 63: return "RAIN";
    case 65: return "HEAVY RAIN";
    case 66: case 67: return "FREEZING RAIN";
    case 71: return "LIGHT SNOW";
    case 73: return "SNOW";
    case 75: return "HEAVY SNOW";
    case 77: return "SNOW GRAINS";
    case 80: return "LIGHT SHOWERS";
    case 81: return "SHOWERS";
    case 82: return "HEAVY SHOWERS";
    case 85: case 86: return "SNOW SHOWERS";
    case 95: return "THUNDERSTORM";
    case 96: case 99: return "HAIL STORM";
    default: return "";
  }
}

Icon codeIcon(int c) {
  if (c == 0 || c == 1)            return Icon::Clear;
  if (c == 2)                      return Icon::PartCloud;
  if (c == 3)                      return Icon::Cloud;
  if (c == 45 || c == 48)          return Icon::Fog;
  if (c >= 51 && c <= 57)          return Icon::Drizzle;
  if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return Icon::Rain;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86)   return Icon::Snow;
  if (c >= 95)                     return Icon::Storm;
  return Icon::Unknown;
}

}  // namespace weather
