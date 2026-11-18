#pragma once
#include <Arduino.h>

// Bluetooth link to the machine — real-time shot counter + live telemetry.
// Protocol per pylamarzocco (clients/_bluetooth.py): authenticate by writing
// the cloud-issued bleAuthToken, then subscribe to the shot-counter
// characteristic, which pushes small JSON notifications.
namespace lmble {

enum class Link : uint8_t { Off, NoToken, Scanning, NotFound, Connecting, AuthFailed,
                            NoShotCounter, Live };

void        begin();          // spawn the BLE task (after settings.load())
Link        link();
const char* linkName(Link l);
void        forgetMachine();  // drop the saved address and rescan

}  // namespace lmble
