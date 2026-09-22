// ble_scanner.h — continuous BLE scanning and target identification.
//
// Runs one never-ending scan and reports EVERY advertisement, including
// repeats from a device already seen. That last part is the whole point: with
// the library default (`wantDuplicates = false`) an infinite scan calls back
// exactly once per device, so a beacon's RSSI is sampled at boot and never
// again. See docs/ARCHITECTURE.md.
//
// Samples for the target beacon are handed to the control task through a
// FreeRTOS queue, so the BLE callback stays short and no lock is shared with
// the proximity filter.

#pragma once

#include <Arduino.h>

#include "beacon.h"
#include "config.h"

struct BleSample {
  int rssi;
  int8_t measuredPower;  // 0 when the frame carried no calibrated power
  uint32_t atMs;
};

namespace BleScanner {

// Brings up the BLE stack and starts scanning. Returns false if the stack or
// the scan failed to start.
bool begin();

// True when config.h names a beacon to look for. When false the firmware is in
// discovery-only mode and will never touch the door.
bool isConfigured();

// Human-readable description of the configured matchers.
String describeTarget();

// ---------------------------------------------------------------------------
// Runtime target list, persisted in NVS so it survives a power cycle AND a
// reflash. Anything stored here overrides the compile-time BEACON_MAC, so a
// beacon can be swapped without rebuilding — which matters when the board has
// no auto-reset wiring and every flash is a manual button dance.
// ---------------------------------------------------------------------------

// The list currently in force, as "aa:bb:.., cc:dd:..".
String targetMacsCsv();

// True when the active list came from NVS rather than from BEACON_MAC.
bool targetMacsAreStored();

// Validates and persists a comma-separated list. Returns false and leaves the
// stored list untouched if any entry is not a MAC, or if there are more than
// BEACON_MAX_MACS of them; `error` then explains why.
//
// The caller must restart for this to take effect — the BLE callback reads the
// live list on every advertisement, so swapping it underneath is a data race.
bool storeTargetMacs(const char *csv, String &error);

// Forgets the stored list, falling back to the compile-time BEACON_MAC.
void clearStoredTargetMacs();

// Proximity thresholds, persisted alongside the MAC list. Returns false from
// loadStoredThresholds() when nothing has been saved, leaving the compile-time
// RSSI_ENTER_DBM / RSSI_EXIT_DBM in force.
bool loadStoredThresholds(int &enterDbm, int &exitDbm);
void storeThresholds(int enterDbm, int exitDbm);
void clearStoredThresholds();

// Dwell/lockout timing, persisted alongside the rest.
bool loadStoredTiming(uint32_t &enterMs, uint32_t &exitMs, uint32_t &lockoutMs);
void storeTiming(uint32_t enterMs, uint32_t exitMs, uint32_t lockoutMs);
void clearStoredTiming();

// Relay interlock dead time. Returns 0 when nothing is stored.
uint32_t loadStoredDirectionGap();
void storeDirectionGap(uint32_t ms);

// Relay pulse width. 0 means "nothing saved, use the compiled-in default".
// The lock: when set, proximity may no longer OPEN the door. Persisted,
// because a lock a power blip clears is not a lock.
bool loadStoredLock();
void storeLock(bool locked);

uint32_t loadStoredPulseMs();
void storePulseTrain(uint8_t count, uint32_t gapMs);
bool loadStoredPulseTrain(uint8_t &count, uint32_t &gapMs);
void storePulseMs(uint32_t ms);

// How long the door takes to travel, in ms. Drives the "moving" LED pattern
// and the annunciator; see DOOR_TRAVEL_MS in config.h. 0 is a legal stored
// value (it means "do not announce"), so these cannot use 0 as the
// nothing-saved sentinel — loadStoredTravelMs() reports that separately.
bool loadStoredTravelMs(uint32_t &ms);
void storeTravelMs(uint32_t ms);

// The annunciator's wiring: which pin, active or passive, and its polarity.
// Persisted because finding an undocumented board's buzzer pin is a search,
// and a search you have to repeat after every power cut is not a fix.
bool loadStoredChime(int &pin, bool &passive, bool &activeLow);
void storeChime(int pin, bool passive, bool activeLow);

// The position switches: which pins, and their polarity. Persisted for the
// same reason as the buzzer — this is wiring, and wiring should survive both a
// power cut and the next flash.
bool loadStoredSensors(int &openPin, int &closedPin, bool &activeLow);
void storeSensors(int openPin, int closedPin, bool activeLow);

// How often the door calls in: settle, minimum interval, heartbeat. All three
// in one record because they only make sense together.
bool loadStoredUpload(uint32_t &settleMs, uint32_t &minIntervalMs, uint32_t &heartbeatMs);
void storeUpload(uint32_t settleMs, uint32_t minIntervalMs, uint32_t heartbeatMs);

// "Bluedroid" or "NimBLE" — which host stack this build is using. Reported in
// the boot banner and in `s`, because it changes the RF code path and is the
// first thing to state in a bug report.
const char *stackName();

bool loadStoredFilter(uint8_t &windowSize, float &alpha);
void storeFilter(uint8_t windowSize, float alpha);
void clearStoredFilter();

// The fast (open-path) filter, persisted separately so that restoring the slow
// pair to its default does not silently reset the open latency too.
bool loadStoredFastFilter(uint8_t &windowSize, float &alpha);
void storeFastFilter(uint8_t windowSize, float alpha);
void clearStoredFastFilter();

// Pops one queued sample. Returns false if the queue was empty.
bool popSample(BleSample &out);

// Blocks until a sample arrives or `timeoutMs` elapses. Returns false on
// timeout.
//
// Waking on the sample rather than on a fixed tick removes up to one whole
// control period of latency between the radio hearing the beacon and the door
// deciding anything. The timeout still guarantees the control loop runs when
// the beacon has gone silent — which is how a missing beacon gets noticed.
bool waitForSample(BleSample &out, uint32_t timeoutMs);

// Restarts the scan if the radio has gone quiet. Call from the control loop.
// Returns true if it performed a restart.
bool serviceWatchdog(uint32_t nowMs);

uint32_t lastAdvMs();

// Milliseconds since the last advertisement from ANY device, clamped at 0.
//
// Always prefer this over `nowMs - lastAdvMs()`. The BLE callback task writes
// the timestamp, so it can be a few milliseconds AHEAD of the `nowMs` the
// control task sampled at the top of its tick. The bare subtraction then
// underflows to ~2^32, which reads as "the radio has been silent for 49 days"
// — wrong in the status output, and enough to trip both the scan watchdog and
// the LED health check.
uint32_t advAgeMs(uint32_t nowMs);

uint32_t advCount();
uint32_t droppedSamples();

// How many times the watchdog has restarted a wedged scan since boot. Should
// be 0; anything climbing points at a brownout.
uint32_t scanRestarts();
bool targetEverSeen();

// Battery telemetry for the target, from its Eddystone-TLM frames.
// Returns false when the beacon has never sent one (iBeacon-only beacons and
// many vendors' defaults do not).
bool targetTelemetry(EddystoneTlm &out, uint32_t &ageMs, uint32_t nowMs);

// Discovery mode maintains the table of everything nearby. It costs heap and
// CPU in the BLE callback, so it defaults on only when no beacon is configured.
void setDiscoverEnabled(bool enabled);
bool discoverEnabled();
void dumpTable(Stream &out, uint32_t nowMs);

}  // namespace BleScanner
