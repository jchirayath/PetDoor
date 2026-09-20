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

// Reported to the server with each upload, so reboots are visible remotely.
void setBootCount(uint32_t n);

// Called from the control loop with the current idle state. Arms an upload
// once the conditions have held for WIFI_IDLE_SETTLE_MS.
void tick(uint32_t nowMs, bool idle);

// Force an upload now, regardless of idle state. Console `u`.
void requestFlushNow();

// True while the radio is up — BLE sampling is degraded during this.
bool busy();

void printStatus(Stream &out);

// ---------------------------------------------------------------------------
// Over-the-air firmware update.
//
// Opens a time-boxed window during which the radio is up and listening for an
// upload, then shuts it again. It is not left listening permanently for the
// same reason uploads are not continuous: WiFi and BLE share one antenna, and
// a radio that is always serving WiFi is not hearing the beacon.
//
// This exists to replace the IO0/EN button sequence on boards with no
// auto-reset wiring — you open the window from the serial console, or from
// anywhere, without touching the hardware.
// ---------------------------------------------------------------------------

// Opens the window. Refuses while the animal is present, since the radio is
// about to be shared and an update reboots the door.
void beginOtaWindow(bool petPresent);

// Unused stack on the uploader task, in bytes. 0 when it is not running.
uint32_t stackFreeBytes();

bool otaWindowOpen();
void closeOtaWindow();

#if REMOTE_CONFIG
// Pop one command the server sent back with an upload, or false if none.
//
// Called by controlTask, never by the WiFi task: the door and the tracker are
// owned by one task and configuration changes them, so the commands cross the
// same way BLE samples do — through a queue, applied by their owner.
// `out` must have room for REMOTE_CMD_MAX_LEN bytes.
bool popCommand(char *out);

// What to report on the next upload: a short human-readable result, so the
// server can show whether a command was applied rather than just delivered.
void setAck(const char *text);
#endif

}  // namespace WifiLogger
