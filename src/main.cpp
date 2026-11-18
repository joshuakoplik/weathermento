#include <M5Unified.h>
#include "config.h"
#include "storage.h"
#include "lm_crypto.h"
#include "lm_cloud.h"
#include "ui.h"
#include "weather.h"
#include "netlock.h"
#include "lm_ble.h"
#include <mbedtls/platform.h>
#include <esp_heap_caps.h>

// TLS allocations → PSRAM. The prebuilt framework sets
// CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC, forcing every mbedTLS buffer into internal
// RAM; with the Bluetooth controller resident there's no longer room for the LM
// WebSocket session plus a weather fetch ("SSL - Memory allocation failed").
// Big blocks (record buffers, certs) go to PSRAM; small hot ones stay internal.
// Either side falls back to the other. (Same idea as IDF's EXTERNAL_MEM_ALLOC.)
static void* tlsCalloc(size_t n, size_t size) {
  constexpr uint32_t CAP_EXT = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;   // (PS is a
  constexpr uint32_t CAP_INT = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT; // CPU reg macro)
  bool big = n * size >= 512;
  void* p = heap_caps_calloc(n, size, big ? CAP_EXT : CAP_INT);
  if (!p) p = heap_caps_calloc(n, size, big ? CAP_INT : CAP_EXT);
  return p;
}
static void tlsFree(void* p) { heap_caps_free(p); }

// The UI loop runs the Setup → weather city lookup (HTTPS + JSON) inline; the
// Arduino default 8 KB loopTask stack overflows during the TLS handshake.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static TaskHandle_t g_cloudTask;
static void cloudTask(void*) {
  lmcloud::begin();
  for (;;) { lmcloud::loop(); vTaskDelay(pdMS_TO_TICKS(10)); }
}

// Free the Core2's internal I2C bus (SDA 21 / SCL 22: AXP192 PMIC, FT6336
// touch, RTC, IMU) before M5.begin(). The upload tool's RTS reset restarts only
// the ESP32; if it lands mid-transfer a slave can be left driving SDA low.
// M5GFX then can't read the AXP192, fails to recognise the Core2 and never
// brings up the panel or touch (black screen, backlight on, until a power
// cycle). Standard recovery: clock SCL until SDA is released, then a STOP.
static void i2cBusRecover(int sda = 21, int scl = 22) {
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, OUTPUT_OPEN_DRAIN);
  digitalWrite(scl, HIGH);
  delayMicroseconds(10);
  int pulses = 0;
  for (; pulses < 9 && digitalRead(sda) == LOW; ++pulses) {
    digitalWrite(scl, LOW);  delayMicroseconds(10);
    digitalWrite(scl, HIGH); delayMicroseconds(10);
  }
  // STOP: SDA low → high while SCL is high
  pinMode(sda, OUTPUT_OPEN_DRAIN);
  digitalWrite(sda, LOW);  delayMicroseconds(10);
  digitalWrite(scl, HIGH); delayMicroseconds(10);
  digitalWrite(sda, HIGH); delayMicroseconds(10);
  pinMode(sda, INPUT); pinMode(scl, INPUT);
  if (pulses) Serial.printf("[boot] I2C bus was stuck; freed after %d clocks\n", pulses);
}

// Black-screen-after-upload guard. After esptool's reset the firmware can
// come up with the panel and touch dead (backlight on), while a second reset
// (the side button) fixes it. Detect that state and restart once or twice by
// ourselves. RTC slow memory survives esp_restart(); the magic word guards
// against power-on garbage.
RTC_NOINIT_ATTR static uint32_t s_hwMagic, s_hwRetries;
static constexpr uint32_t HW_MAGIC = 0x48575354;   // "HWST"

static bool hwOk() {
  return M5.getBoard() == m5::board_t::board_M5StackCore2 && M5.Display.touch() != nullptr;
}
static void hwReport(const char* tag) {
  Serial.printf("[%s] reset reason %d, board %d (Core2 = %d), touch %s, display %dx%d, "
                "self-restarts %u\n", tag,
                (int)esp_reset_reason(), (int)M5.getBoard(),
                (int)m5::board_t::board_M5StackCore2,
                M5.Display.touch() ? "yes" : "NO",
                (int)M5.Display.width(), (int)M5.Display.height(),
                (unsigned)(s_hwMagic == HW_MAGIC ? s_hwRetries : 0));
}

void setup() {
  Serial.begin(115200);
  i2cBusRecover();
  auto c = M5.config();
  c.internal_spk    = true;
  c.internal_mic    = false;   // never configure the PDM mic (no audio input used)
  c.serial_baudrate = 115200;
  c.fallback_board  = m5::board_t::board_M5StackCore2;   // if autodetect comes up empty
  M5.begin(c);
  M5.Mic.end();                // belt and braces: make sure the mic driver is stopped
  M5.Speaker.setVolume(cfg::BEEP_VOLUME);   // quieter touch beeps
  if (s_hwMagic != HW_MAGIC) { s_hwMagic = HW_MAGIC; s_hwRetries = 0; }
  hwReport("boot");
  if (!hwOk()) {
    if (s_hwRetries < 2) {
      ++s_hwRetries;
      Serial.println("[boot] display/touch not detected - restarting to retry");
      Serial.flush(); delay(100);
      esp_restart();
    }
    Serial.println("[boot] display/touch still not detected after retries");
  } else {
    s_hwRetries = 0;
  }

  settings.load();
  ui::begin();
  if (settings.wifiSsid.isEmpty() || settings.lmUser.isEmpty()) {
    ui::forceSetup();                       // first boot — straight to credentials
  } else {
    ui::splash("waking up …");
  }

  netlock::init();                          // before the network tasks start
  mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);   // before any TLS
  if (!lmcrypto::init()) { ui::splash("crypto init failed"); delay(3000); }
  // Network/cloud on core 0; UI/touch stays on core 1 (Arduino loop).
  xTaskCreatePinnedToCore(cloudTask, "lmcloud", 12*1024, nullptr, 2, &g_cloudTask, 0);
  weather::begin();                         // own task; waits for WiFi + a city
  lmble::begin();                           // Bluetooth shot counter (own task)
}

void loop() {
  // serial debug shell: '0'..'9' force screen ('9'=weather) ('1'→brewing@24.7s), 's' screenshot,
  // 'b' arm backflush (same as tapping START), 'w' forget weather location, 'i' hardware info
  while (Serial.available()) {
    int c=Serial.read();
    if (c>='0'&&c<='9') ui::debugScreen(c-'0', c=='1'?24.7f:0);
    else if (c=='d'||c=='l') ui::setDark(c=='d');
    else if (c=='b') lmcloud::startBackflush();
    else if (c=='i') hwReport("info");
    else if (c=='w') { settings.wxHasLoc=false; settings.wxCity=""; settings.save();
                       Serial.println("[wx] location cleared"); }
    else if (c=='s') { vTaskSuspend(g_cloudTask); ui::screenshot(); vTaskResume(g_cloudTask); }
  }
  ui::tick();
  lmcloud::uiTick();
  delay(5);
}
