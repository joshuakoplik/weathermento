// IRAM budget relief for WiFi + TLS + Bluetooth on the ESP32 (Arduino 2.0.x).
//
// With the PSRAM cache workaround, the framework's linker script places a list
// of newlib functions in IRAM. Adding the Bluetooth controller (~30 KB of IRAM)
// overflowed iram0_0_seg by ~1.8 KB. HTTPClient is the only user of strptime
// (2.1 KB of IRAM), and only inside setCookie(), which returns early unless a
// cookie jar is set — we never set one. Defining strptime here satisfies the
// link from flash, so newlib's IRAM copy is never pulled in.
#include <time.h>

extern "C" char* strptime(const char*, const char*, struct tm*) {
  return nullptr;   // unreachable in this firmware (no HTTPClient cookie jar)
}
