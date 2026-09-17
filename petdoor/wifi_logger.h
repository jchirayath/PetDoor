// wifi_logger.h — opportunistic batch upload of the event log over WiFi.
//
// DISABLED BY DEFAULT. Without WIFI_SSID set, none of this runs and the radio
// is never brought up, so the "no WiFi, no cloud" behaviour is unchanged unless
// you opt in.
//
// Why "opportunistic" and not real-time:
//
// The ESP32 has ONE 2.4 GHz radio shared between WiFi and BLE. Associating with
// an access point takes 2-6 seconds of radio-intensive work — the POST itself
// is trivial by comparison — and during that window BLE sampling is starved.
//
// Uploading on each event would be the worst possible schedule, because events
// happen when the animal is AT THE DOOR. It would blind the radio at precisely
// the moment detection matters, and FIX_LOST is itself an event, so a burst
// that starves BLE can trigger another burst.
//
// So: events are written to NVS the instant they happen (no radio involved),
// and the upload waits until nothing is going on — the beacon is absent, the
// door is closed, and both have been settled for WIFI_IDLE_SETTLE_MS. In
// practice that means the log reaches your endpoint a minute or two after the
// animal leaves, rather than at some fixed hour or during an approach.
//
// The upload runs in its own task so a slow association cannot stall door
// decisions, and it aborts if the animal returns mid-flush.

#pragma once

#include <Arduino.h>

#include "config.h"

namespace WifiLogger {

// True when WIFI_SSID is configured. Everything else is a no-op when false.
bool isEnabled();

// Starts the uploader task. Does NOT bring the radio up.
void begin();

// Called from the control loop with the current idle state. Arms an upload
// once the conditions have held for WIFI_IDLE_SETTLE_MS.
void tick(uint32_t nowMs, bool idle);

// Force an upload now, regardless of idle state. Console `u`.
void requestFlushNow();

// True while the radio is up — BLE sampling is degraded during this.
bool busy();

void printStatus(Stream &out);

}  // namespace WifiLogger
