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

// Firmware version, reported in the banner, in `s`, and with every upload so a
// server collecting from several doors can tell which build produced an event.
// Bump it when you change behaviour someone might need to correlate against.
#ifndef PETDOOR_VERSION
#define PETDOOR_VERSION "1.0.0"
#endif

// Set automatically by the compiler — useful when several people build from
// the same version string and you need to tell their binaries apart.
#define PETDOOR_BUILD (__DATE__ " " __TIME__)

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

// ---------------------------------------------------------------------------
// BLE host stack.
//
// 0 = Bluedroid, the stack bundled with the ESP32 Arduino core. Nothing to
//     install; this is the default so a fresh checkout builds with no extra
//     steps.
// 1 = NimBLE, via the NimBLE-Arduino library (Library Manager → "NimBLE-Arduino",
//     2.5.1 or later). Same Arduino IDE, one extra library.
//
// NimBLE is the same radio and the same controller; it replaces only the host
// stack, and it is far smaller. Measured on a minimal scan sketch:
//
//     Bluedroid   1,072,811 bytes flash   39,860 bytes RAM
//     NimBLE        592,219 bytes flash   34,528 bytes RAM
//     saving          469 KB flash         5.2 KB RAM
//
// That is the single largest saving available to this firmware — Bluedroid is
// over half the image. Turn it on if you are short of flash, especially with
// PETDOOR_ENABLE_WIFI on.
//
// It is opt-in rather than automatic on purpose. Switching BLE stacks changes
// the code that drives the radio on a device that moves a door; that should be
// a decision you made, not a side effect of which libraries happen to be
// installed. The boot banner and `s` both report which stack is running.
#ifndef PETDOOR_USE_NIMBLE
#define PETDOOR_USE_NIMBLE 0
#endif

// NimBLE only. How long to wait for a scan response before reporting an
// advertisement with just the data it already carried.
//
// This exists because NimBLE's own default is 10240 ms, and that default is
// written for scans that END. Ours never does — begin() starts an endless scan.
// A beacon that advertises as scannable (ADV_IND / ADV_SCAN_IND) but does not
// answer scan requests would sit on NimBLE's waiting list for those 10.24 s,
// which is more than three times SAMPLE_MAX_AGE_MS: the door would read "no
// fix" permanently and never open.
//
// 100 ms is far longer than a scan response actually takes (it follows the
// advertisement within the same advertising event) and far shorter than
// anything the door cares about. Non-scannable broadcast-only beacons are
// reported immediately and never touch this path at all.
#ifndef NIMBLE_SCAN_RSP_TIMEOUT_MS
#define NIMBLE_SCAN_RSP_TIMEOUT_MS 100
#endif

// ---------------------------------------------------------------------------
// Fast path — the OPEN decision only.
//
// The two settings above shape the signal the CLOSE decision reads. The two
// below shape a second, deliberately twitchy filter over the same samples that
// only the open decision reads. Why two:
//
//   Opening late can shut an animal out; opening early only lets in a draught.
//   Closing early can shut a door on an animal. The two directions do not want
//   the same amount of smoothing, and a single filter has to compromise.
//
// Before this split, making the door open promptly meant shortening the median
// window for BOTH decisions, which is what stripped the close path of its
// immunity to dropouts. Now the slow pair can go back to being genuinely slow
// (7 / 0.35 rides out a multi-second fade) without costing any open latency.
//
// A single strong spike still cannot open the door: the fast filter has to stay
// above RSSI_ENTER_DBM for the whole of ENTER_CONFIRM_MS. The dwell timer is
// what confirms, so the filter is free to be fast.
#ifndef RSSI_FAST_WINDOW
#define RSSI_FAST_WINDOW 1
#endif

// 0.9 reaches ~90% of a step in one sample. Raise ENTER_CONFIRM_MS, not this,
// if the door opens too eagerly — the dwell is the safety, this is the speed.
#ifndef RSSI_FAST_ALPHA
#define RSSI_FAST_ALPHA 0.9f
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
// 4b. WIFI LOG UPLOAD  — optional, off unless WIFI_SSID is set
// ===========================================================================
// Compile the WiFi subsystem in at all. Setting this to 0 removes the uploader,
// OTA and the whole network stack from the binary.
//
// Measured, classic ESP32:
//     with WiFi     1,757,751 flash (89% of min_spiffs)   65,588 static RAM
//     without       1,118,063 flash (56%)                 46,188 static RAM
//                    -639,688 flash                       -19,400 RAM
//
// That is 33 percentage points of the partition and ~19 KB of RAM for a feature
// a local-only door never uses. If you are not uploading logs and not using
// over-the-air updates, turn it off — you get the space back and the radio is
// never shared with Bluetooth at all.
//
// Leaving it at 1 does NOT bring the radio up: with no WIFI_SSID the stack is
// linked but idle. This flag is about the binary, WIFI_SSID about the runtime.
#ifndef PETDOOR_ENABLE_WIFI
#define PETDOOR_ENABLE_WIFI 1
#endif

// Leave WIFI_SSID empty and the radio is never brought up: no WiFi, no cloud,
// exactly as before. Put credentials in secrets.h, not here.
//
// !! The ESP32 shares one antenna between WiFi and BLE. Bringing WiFi up
// !! starves BLE sampling for the 2-6 seconds an association takes, so uploads
// !! are deferred until the beacon is absent and the door is closed. Do not
// !! change that to upload on every event — see wifi_logger.h.

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

// Where the CSV batch is POSTed. Empty means LOCAL LOGGING ONLY: events are
// still recorded to the NVS ring and still roll oldest-out, nothing is sent
// anywhere, and the radio is never brought up.
#ifndef LOG_ENDPOINT_URL
#define LOG_ENDPOINT_URL ""
#endif

// Shared key for authenticating uploads. Optional — leave it empty and events
// are POSTed unsigned, which is fine on a network you trust entirely.
//
// When set, the body is signed with HMAC-SHA256 and the signature sent in a
// header. The KEY ITSELF NEVER CROSSES THE WIRE, so a plain-HTTP endpoint is
// still safe from forgery: an eavesdropper can read the door events but cannot
// invent new ones.
//
// Deliberately HTTP rather than HTTPS. A TLS handshake costs 1-3 seconds of
// radio time and ~40 KB of heap on every burst, and radio time is exactly what
// starves BLE sampling. Door events are low-secrecy but high-integrity: it
// matters much more that nobody can forge "door opened" than that a sniffer on
// your LAN learns the door opened. HMAC buys the integrity for free.
//
// If the endpoint is across the public internet, put it behind a VPN or a
// reverse proxy that terminates TLS, rather than paying for TLS on the ESP32.
#ifndef LOG_SHARED_KEY
#define LOG_SHARED_KEY ""
#endif

// Compile in TLS support for the log upload.
//
// OFF by default because it does not fit on a classic ESP32. Measured on real
// hardware: with WiFi, BLE and this firmware resident, free heap is ~80 KB, and
// a TLS handshake drove the low-water mark to 18 KB and then failed to connect.
// It also costs ~170 KB of flash whether or not the endpoint uses it.
//
// Uploads do not need it: each one is signed with HMAC-SHA256 so it cannot be
// forged or replayed, and the key never crosses the wire. TLS would add
// confidentiality, not integrity.
//
// Turn it on only if you have the headroom — an S3 or C6 with PSRAM — and your
// endpoint is HTTPS-only. Then set LOG_ENDPOINT_URL to an https:// address.
#ifndef LOG_ALLOW_TLS
#define LOG_ALLOW_TLS 0
#endif

// Identifies this door to the server, so one endpoint can collect from several.
#ifndef LOG_DEVICE_ID
#define LOG_DEVICE_ID "petdoor"
#endif

// Everything must have been quiet this long before an upload is allowed.
#ifndef WIFI_IDLE_SETTLE_MS
#define WIFI_IDLE_SETTLE_MS 60000
#endif

// Do not upload more often than this even if events keep arriving.
#ifndef WIFI_MIN_UPLOAD_INTERVAL_MS
#define WIFI_MIN_UPLOAD_INTERVAL_MS 300000
#endif

// Give up on an association after this long and go back to sleep, so a missing
// access point cannot hold the radio indefinitely.
#ifndef WIFI_CONNECT_TIMEOUT_MS
#define WIFI_CONNECT_TIMEOUT_MS 15000
#endif

// How long an over-the-air update window stays open before the radio is handed
// back to BLE. Long enough to start an upload, short enough that forgetting to
// close it is not a problem.
#ifndef OTA_WINDOW_MS
#define OTA_WINDOW_MS 300000
#endif

// Password required to push an update. Strongly recommended: without it anyone
// on the network can reflash the door. Empty disables OTA entirely.
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif

// NTP server used to set the clock, so log entries carry real timestamps.
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
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

// Task stack sizes, in bytes. Check `task stacks` in the `s` output before
// changing these: it reports how much each task has never used. Too small is a
// crash on an unusual input; too large is heap doing nothing.
//
// Measured on hardware after exercising the heaviest paths (discovery dump plus
// a log upload): control peaked at ~1,970 bytes used, uploader at ~2,650. These
// sizes leave roughly 3 KB and 1.4 KB of margin respectively, and hand about
// 5 KB back to the heap versus the 8192/6144 they started at.
//
// Raise WIFI_TASK_STACK if you enable LOG_ALLOW_TLS — a TLS handshake needs
// several KB more stack than a plain POST.
#ifndef CONTROL_TASK_STACK
#define CONTROL_TASK_STACK 5120
#endif
//
// 4096 was tried and measured at only ~1.4 KB of headroom, which is too thin
// for a task handling variable-length HTTP responses — a stack overflow is a
// hard crash, not a degraded upload. 5120 restores ~2.4 KB while still handing
// 1 KB back versus the original 6144.
#ifndef WIFI_TASK_STACK
#define WIFI_TASK_STACK 5120
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

// How long YOUR door takes to travel from fully open to fully closed, in ms.
// 0 means "not measured" and disables the checks below.
//
// The firmware never waits for this — it has no position feedback and cannot
// know when travel actually finishes. It is here because two other settings are
// only sensible in relation to it, and getting them wrong produces a door that
// visibly starts moving and then stops partway:
//
//   MIN_ACTUATION_INTERVAL_MS  must be >= travel time, or the firmware can
//                              command a reversal while the door is still
//                              moving. Most controllers treat a second command
//                              mid-travel as "stop".
//   RELAY_PULSE_MS             only needs to exceed travel time if your motor
//                              has no limit switches and you are driving it
//                              directly. See docs/WIRING.md.
//
// Measure it with a stopwatch: `o`, wait for it to settle, `x`, and time the
// close. The reference build measures ~15 s.
#ifndef DOOR_TRAVEL_MS
#define DOOR_TRAVEL_MS 0
#endif

// How long a manual `o` keeps the door open before automatic control resumes.
//
// Without this, `o` is a single pulse and nothing more: the control task runs a
// few milliseconds later, sees an open door with no beacon in range, and closes
// it again. The door appears to "start opening and then stop", which is not a
// relay fault or a beacon fault — it is the firmware doing exactly what it was
// told. Anyone wiring, testing, or propping the door open to clean the coop
// hits this immediately.
//
// The hold suppresses automatic CLOSING only. Automatic opening is never
// blocked, so a beacon arriving during a hold still finds an open door, and a
// manual `x` cannot strand an animal outside — proximity can always reopen.
// That asymmetry is the same one the whole project is built on: an open door is
// the safe failure.
//
// Cleared early by `x`, or by `O` (capital) to hand control straight back.
// Deliberately NOT persisted to NVS: a power cut should always come back under
// automatic control, never stuck open because of a command typed days ago.
//
// 0 disables the hold entirely and restores the old single-pulse behaviour.
#ifndef MANUAL_HOLD_MS
#define MANUAL_HOLD_MS 300000
#endif
