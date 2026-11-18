#include "netlock.h"
#include <Arduino.h>

namespace netlock {
static SemaphoreHandle_t s_mtx = nullptr;
void init() { if (!s_mtx) s_mtx = xSemaphoreCreateRecursiveMutex(); }
void take() { if (s_mtx) xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
void give() { if (s_mtx) xSemaphoreGiveRecursive(s_mtx); }
}
