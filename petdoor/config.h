// config.h — all tunable settings for PetDoor.
//
// Edit this file directly, or (preferred) copy secrets.example.h to secrets.h
// and put your beacon identity there. secrets.h is git-ignored, so your local
// settings survive `git pull` and never end up in the public repo.
//
// Every value below is wrapped in #ifndef, so anything defined in secrets.h
// (or via -D build flags) wins over the default here.

#pragma once

#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#endif

// ===========================================================================
// 1. BEACON IDENTITY  — which BLE device is "the key"
// ===========================================================================
// Matchers are ANDed: every matcher you enable must match. Leave a matcher at
// its placeholder value to disable it.
//
// If NO matcher is configured the firmware boots into discovery mode, scans,
// and prints every device it sees so you can find your beacon. Nothing will
// ever actuate the door until you configure an identity.

// Beacon MAC address, lower or upper case. "" disables MAC matching.
// This is the simplest and most reliable matcher for a Minew beacon, because
// Minew beacons advertise a fixed public address (unlike phones/AirTags,
// which rotate their MAC every ~15 minutes and cannot be tracked this way).
//
// One MAC:
//   #define BEACON_MAC "ac:23:3f:11:22:33"
//
// Several — separate with commas. ANY of them opens the door, which is what
// you want for more than one animal, or for a spare beacon you can swap in
// without reflashing:
//   #define BEACON_MAC "ac:23:3f:11:22:33, ac:23:3f:11:22:44"
//
// Note the matchers are still ANDed with BEACON_UUID below: the MAC list means
// "any of these addresses", not "any of these OR the UUID".
#ifndef BEACON_MAC
#define BEACON_MAC ""
#endif

// How many addresses BEACON_MAC may list. Each costs 18 bytes of RAM, and the
// list is walked in the BLE callback, so keep it small.
#ifndef BEACON_MAX_MACS
#define BEACON_MAX_MACS 4
#endif

// iBeacon identity. "" disables iBeacon matching.
// Use this instead of (or in addition to) the MAC if you want to be able to
// swap in a replacement beacon without reflashing the ESP32 — just program the
// new beacon with the same UUID/major/minor.
#ifndef BEACON_UUID
#define BEACON_UUID ""
#endif

// -1 means "any". Only consulted when BEACON_UUID is set.
#ifndef BEACON_MAJOR
#define BEACON_MAJOR -1
#endif
#ifndef BEACON_MINOR
#define BEACON_MINOR -1
#endif

// ===========================================================================
// 2. PROXIMITY  — how near is "near"
// ===========================================================================
// Run `c` in the serial console to see live filtered RSSI and distance while
// you walk around, then set these. See docs/TUNING.md.
//
// REQUIRED: RSSI_ENTER_DBM must be greater (less negative) than RSSI_EXIT_DBM.
// The gap between them is the hysteresis dead zone that stops the door from
// flapping when you stand right at the threshold.

#ifndef RSSI_ENTER_DBM
#define RSSI_ENTER_DBM -65
#endif

#ifndef RSSI_EXIT_DBM
#define RSSI_EXIT_DBM -75
#endif

// Signal must sit at/above RSSI_ENTER_DBM this long before the door opens.
// Short, because arriving should feel responsive.
#ifndef ENTER_CONFIRM_MS
#define ENTER_CONFIRM_MS 1500
#endif

// Signal must sit at/below RSSI_EXIT_DBM this long before the door closes.
// Long, because a false close is far worse than a late close. This also rides
// out the dropouts that made the first prototype unusable.
#ifndef EXIT_CONFIRM_MS
#define EXIT_CONFIRM_MS 15000
#endif

// An advertisement older than this no longer counts as a live reading.
#ifndef SAMPLE_MAX_AGE_MS
#define SAMPLE_MAX_AGE_MS 3000
#endif

// Ignore the beacon until we have at least this many samples, so one stray
// packet can never move the door.
#ifndef MIN_SAMPLES_FOR_FIX
#define MIN_SAMPLES_FOR_FIX 3
#endif

// Median window: rejects the isolated deep fades and spikes that dominate BLE
// RSSI noise. Must be odd, 3..15. Bigger = steadier but slower to react.
#ifndef RSSI_MEDIAN_WINDOW
#define RSSI_MEDIAN_WINDOW 7
#endif

// EWMA smoothing applied after the median. 0.0..1.0; lower = smoother/slower.
#ifndef RSSI_EWMA_ALPHA
#define RSSI_EWMA_ALPHA 0.35f
#endif

// Path-loss exponent for the distance estimate: ~2.0 open air, 2.5–3.0 through
// a coop wall, 3.0+ cluttered. Only affects the displayed distance, never the
// open/close decision (which uses RSSI directly).
#ifndef PATH_LOSS_EXPONENT
#define PATH_LOSS_EXPONENT 2.5f
#endif

// Fallback "calibrated RSSI at 1 metre", used for the displayed distance when
// the beacon's own advertisement does not carry one.
//
// iBeacon frames include this figure and it is always preferred when present.
// Eddystone beacons (including Minew tags in Eddystone mode) and most sensor
// tags do not, and without this fallback they display "?" instead of a
// distance.
//
// To measure yours: hold the beacon exactly 1 m from the ESP32, run `c`, and
// read the *filtered* value. -59 is the iBeacon convention and a reasonable
// starting point. Set to 0 to go back to showing "?".
//
// Display only — no open/close decision uses the distance estimate.
#ifndef BEACON_MEASURED_POWER_DBM
#define BEACON_MEASURED_POWER_DBM -59
#endif

// ===========================================================================
// 3. DOOR / RELAY WIRING
// ===========================================================================
// !! Read docs/SAFETY.md before wiring a real door motor. !!

#ifndef PIN_RELAY_OPEN
#define PIN_RELAY_OPEN 16
#endif

#ifndef PIN_RELAY_CLOSE
#define PIN_RELAY_CLOSE 17
#endif

#ifndef PIN_STATUS_LED
#define PIN_STATUS_LED 23
#endif

// Most cheap blue relay boards are ACTIVE LOW: the coil energises when the
// input is pulled to GND. Set to 1 for those. Set to 0 for active-high boards
// and for logic-level MOSFET drivers. Getting this wrong means the door runs
// backwards or runs constantly — verify with docs/WIRING.md before connecting
// the motor.
#ifndef RELAY_ACTIVE_LOW
#define RELAY_ACTIVE_LOW 0
#endif

// Momentary pulse length. Matches a door controller with separate OPEN and
// CLOSE momentary inputs. If your motor instead needs the relay held closed
// for the whole travel, see docs/WIRING.md.
#ifndef RELAY_PULSE_MS
#define RELAY_PULSE_MS 200
#endif

// Minimum gap between any two actuations. Protects the motor from thrash.
// Keep this well below EXIT_CONFIRM_MS or it will delay closing.
#ifndef MIN_ACTUATION_INTERVAL_MS
#define MIN_ACTUATION_INTERVAL_MS 5000
#endif

// Dead time with the opposite relay released before this one is asserted, so
// the two can never be energised close enough together to fight each other.
// Both energised at once is a short across the motor's direction contacts.
//
// A mechanical relay releases in roughly 5-15 ms, so 250 ms leaves well over
// an order of magnitude of margin. It was 1000 ms originally, which is safe but
// needlessly slow: pulse() blocks for this long before every actuation, so it
// is a direct, per-actuation delay on the door opening.
//
// Raise it if your relays are slow, if you can hear them chattering, or if the
// motor controller needs longer to see the previous direction released. The
// runtime `w` -> `gap N` console command enforces a 100 ms floor.
#ifndef DIRECTION_CHANGE_GAP_MS
#define DIRECTION_CHANGE_GAP_MS 250
#endif

// After boot the door state is unknown. We refuse to close for this long,
// giving the beacon a chance to be heard first. Prevents a power blip at dusk
// from closing the door on an animal that is actually right there.
#ifndef BOOT_GRACE_MS
#define BOOT_GRACE_MS 30000
#endif

// Beacon battery level (from Eddystone-TLM) at or below which the status LED
// switches to its continuous low-battery flash. A CR2032 reads ~3000 mV fresh
// and is effectively flat by ~2200 mV, so this leaves time to act.
// Set to 0 to disable the warning.
#ifndef BEACON_LOW_BATTERY_MV
#define BEACON_LOW_BATTERY_MV 2400
#endif

// ===========================================================================
// 4. BLE SCANNING
// ===========================================================================
// Near-100% duty cycle. Window must be <= interval. This, plus duplicate
// reporting (see ble_scanner.cpp), is what gives a steady sample stream —
// the thing the first prototype got wrong.

#ifndef SCAN_INTERVAL_MS
#define SCAN_INTERVAL_MS 100
#endif

#ifndef SCAN_WINDOW_MS
#define SCAN_WINDOW_MS 99
#endif

// Active scanning also requests scan-response data, which is what carries the
// device name on most beacons. Set to 0 for passive scanning (slightly lower
// power, no names).
#ifndef SCAN_ACTIVE
#define SCAN_ACTIVE 1
#endif

// If not one single advertisement arrives from ANY device for this long, the
// BLE stack is wedged. Tear the scan down and restart it.
#ifndef SCAN_WATCHDOG_MS
#define SCAN_WATCHDOG_MS 15000
#endif

// Events kept in the persistent log. Each entry is 16 bytes, held in NVS and
// rewritten on every event, so keep it modest: 128 entries is 2 KB and covers
// weeks of a door that cycles a few times a day.
#ifndef EVENT_LOG_CAPACITY
#define EVENT_LOG_CAPACITY 128
#endif

// ===========================================================================
// 5. DIAGNOSTICS
// ===========================================================================

#ifndef SERIAL_BAUD
#define SERIAL_BAUD 115200
#endif

// Control loop IDLE period. The loop normally wakes the moment a BLE sample
// arrives; this is only the ceiling on how long it will sit waiting when the
// beacon is silent.
//
// It also sets how often the status LED is refreshed, so it bounds the
// shortest LED pattern that can be rendered: a feature shorter than about
// twice this value will alias into flicker. 100 ms supports the 200 ms blip
// and the 5 Hz fault flutter. Raising it much above 250 ms will visibly break
// those patterns.
#ifndef CONTROL_TICK_MS
#define CONTROL_TICK_MS 100
#endif

// Max distinct devices held in the discovery table.
#ifndef DEVICE_TABLE_SIZE
#define DEVICE_TABLE_SIZE 40
#endif

// Per-device throttle on discovery-table writes, so a busy RF environment
// cannot flood the heap from the BLE callback.
#ifndef TABLE_MIN_UPDATE_MS
#define TABLE_MIN_UPDATE_MS 500
#endif

// How often discovery mode prints the table.
#ifndef DISCOVER_DUMP_INTERVAL_MS
#define DISCOVER_DUMP_INTERVAL_MS 2000
#endif

// How often calibration mode prints a reading.
#ifndef CALIBRATE_INTERVAL_MS
#define CALIBRATE_INTERVAL_MS 500
#endif

// Allow `o` / `x` serial commands to drive the relays directly. Invaluable
// while wiring; set to 0 for an unattended deployment.
#ifndef ALLOW_MANUAL_SERIAL_CONTROL
#define ALLOW_MANUAL_SERIAL_CONTROL 1
#endif
