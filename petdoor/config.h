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
#define PETDOOR_VERSION "1.1.0"
#endif

// The commit this was built from, when the build command says so:
//
//   --build-property "compiler.cpp.extra_flags=-DPETDOOR_GIT=\\"$(git rev-parse --short HEAD)\\""
//
// Empty otherwise, and nothing depends on it. It exists because a version
// string that only moves on release days cannot answer "which code is on the
// door", and the build timestamp only answers "which of my local builds" —
// neither maps to anything anyone else can check out. Reported with every
// upload, so the server's firmware history records it too.
#ifndef PETDOOR_GIT
#define PETDOOR_GIT ""
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
// DEFAULT SINCE THE BLUEDROID BUILD STARTED PANICKING. This was opt-in for a
// long time, on the reasoning that changing the radio stack under a door should
// be a deliberate choice rather than a side effect of which libraries happen to
// be installed. That reasoning was sound; it was simply outweighed by
// measurements from a door in service:
//
//                        Bluedroid        NimBLE
//   flash (min_spiffs)   92%              69%
//   free heap            63 KB            134 KB
//   heap LOW-WATER       6.9 KB           80 KB
//
// 6.9 KB is the least free RAM the door ever had, and an allocation failing
// down there panics the chip — which is what reset reason 4 on three
// consecutive boots turned out to be. Bluedroid still builds and still works;
// it simply has no headroom left once WiFi, the uploader, OTA and the network
// console are all resident.
//
// Set this to 0 for the Bluedroid build. The boot banner and `s` both report
// which stack is running, so a door can always be asked rather than assumed.
#ifndef PETDOOR_USE_NIMBLE
#define PETDOOR_USE_NIMBLE 1
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

// Momentary pulse length — how long the relay holds the controller's button
// down. Matches a door controller with separate OPEN and CLOSE momentary
// inputs. If your motor instead needs the relay held closed for the whole
// travel, see docs/WIRING.md.
//
// 1000 ms, NOT the 200 ms this used to default to, and the difference is not a
// margin: on the reference controller a 500 ms press is SWALLOWED and a
// 1000 ms press works, every time. A relay that clicks into a door that does
// not move is the single most-reported symptom of this project, and for a
// whole day of bench work the cause was this number. Measure yours before
// lowering it; see docs/REQUIREMENTS.md §1.
//
// The cost of a long press is that the control task sits in delay() for it,
// so no BLE sample is drained and no presence re-evaluated during the press.
// That is bounded at ONE press: everything between presses — the wake probe,
// the repeat gap, the whole verification window — is non-blocking. See
// petdoor/actuator.h.
#ifndef RELAY_PULSE_MS
#define RELAY_PULSE_MS 1000
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

// How far the battery must climb back ABOVE the threshold before the warning
// clears. A coin cell's reading is not monotonic — it sags under the beacon's
// transmit pulse and recovers between them, and it rises with temperature, so a
// cell sitting at the threshold crosses it repeatedly. Without this margin that
// is a latch that flaps, which means a log entry and an email per flap.
//
// Only the LATCH uses this. The reported millivolts are always whatever the
// beacon last said.
#ifndef BEACON_LOW_BATTERY_CLEAR_MV
#define BEACON_LOW_BATTERY_CLEAR_MV 150
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

// Hold a freshly flashed image "unconfirmed" until it proves it can still be
// reached, and let the bootloader put the old one back if it cannot.
//
// The ESP32 supports this natively and the Arduino core already wires it up —
// but the core CONFIRMS THE IMAGE AT BOOT, before any of this firmware runs, so
// rollback can never trigger. On a door you can walk up to that is harmless. On
// one screwed to a wall it means a bad push is permanent: an image that boots
// but cannot join WiFi is unreachable forever, and the only fix is a ladder.
//
// With this on, the door overrides the core's hook, and marks the image good
// only after a SUCCESSFUL UPLOAD — the exact capability that makes it
// manageable. Reboot before that happens and the bootloader reverts to the
// previous image.
//
// The honest cost: if your log server is down for a long stretch AND the door
// reboots, it rolls back a perfectly good image. That is the right way round —
// re-pushing is easy, a ladder is not.
//
// Forced off when there is no WiFi, since there would be nothing to prove.
#ifndef OTA_REQUIRE_CONFIRM
#define OTA_REQUIRE_CONFIRM 1
#endif

// Call in at least this often even when the animal is home and the door is
// open. Without it the door only ever uploads while the beacon is away, so a
// pet that stays in keeps the door silent — and a silent door collects no
// commands. One radio burst every half hour is a small price for never losing
// the channel. 0 disables the heartbeat.
#ifndef WIFI_HEARTBEAT_MS
#define WIFI_HEARTBEAT_MS 1800000
#endif

// Accept configuration and commands from the log server, carried back in its
// reply to an upload. Off is the old behaviour: the door talks, never listens.
//
// WHY THIS EXISTS. Once a door is screwed to a wall the serial console is gone,
// and the OTA window could only ever be opened by pressing `p` ON THAT CONSOLE.
// So the one feature meant to remove physical access could not itself be
// reached without it. This closes that loop: the door already contacts the
// server every few minutes, so the reply is a channel that already exists.
//
// WHAT IT CANNOT CHANGE, and this is deliberate: the WiFi credentials, the log
// endpoint, the shared key and the OTA password. Those four are the lifeline
// the channel itself depends on. A remote command that broke any of them would
// take the door offline permanently, with no way back except a ladder and a
// USB cable. Everything else about the door's behaviour is fair game.
//
// Every reply must be signed with LOG_SHARED_KEY or it is ignored. Uploads go
// over plain HTTP by design (TLS costs this chip more heap than it has), so
// without a signature anyone on the path could raise the door's radio at will,
// or worse, retune it.
#ifndef REMOTE_CONFIG
#define REMOTE_CONFIG 1
#endif

// Most commands accepted from one reply. A queue rather than immediate
// application because the reply arrives on the WiFi task and the door is owned
// by the control task — see invariant 9.
#ifndef REMOTE_CMD_QUEUE_DEPTH
#define REMOTE_CMD_QUEUE_DEPTH 12
#endif

// How long to wait before a restart a remote command asked for. Long enough
// for the acknowledgement to be uploaded first — the ack lives in RAM, so
// rebooting immediately would lose it and the server could not tell "applied"
// from "never arrived".
#ifndef REMOTE_RESTART_DELAY_MS
#define REMOTE_RESTART_DELAY_MS 20000
#endif

// Longest single command line accepted. A MAC list is the long one.
#ifndef REMOTE_CMD_MAX_LEN
#define REMOTE_CMD_MAX_LEN 128
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

// ---------------------------------------------------------------------------
// Scheduled lockout
// ---------------------------------------------------------------------------
//
// Windows in which the beacon may not open the door — a night lockout, most
// obviously. Gates opening only, exactly like the manual lock. See schedule.h
// for the four rules that shape it, the first of which is that an unset clock
// disables every window rather than applying them to a guess.

// How many windows can be stored. Four covers "weeknights", "weekends" and a
// couple of exceptions; each costs 6 bytes of NVS and a comparison per tick.
#ifndef SCHEDULE_MAX_WINDOWS
#define SCHEDULE_MAX_WINDOWS 4
#endif

// Minutes east of UTC, because the door keeps UTC and windows are wall time.
// -480 is US Pacific standard, -420 daylight. There is NO DST handling: adjust
// this twice a year, or leave an hour of slack at each end of your windows.
#ifndef SCHEDULE_UTC_OFFSET_MIN
#define SCHEDULE_UTC_OFFSET_MIN 0
#endif

// ---------------------------------------------------------------------------
// Vibration sensor
// ---------------------------------------------------------------------------
//
// Answers "did the door start moving?" within about a second, where a limit
// switch cannot answer "did it arrive?" until the whole travel has elapsed.
// See vibration.h. Disabled by default; mount it ON THE DOOR, never on the
// controller board, or it will hear the relay instead of the door.

#ifndef PIN_VIBRATION
#define PIN_VIBRATION -1
#endif

// Most SW-420/801S modules idle HIGH and pull LOW on movement, but this varies
// by manufacturer and is not reliably printed on the board. Either setting
// works — the sensor is read as EDGES, not levels — so this only decides
// whether the internal pull-up is enabled.
#ifndef VIBRATION_ACTIVE_LOW
#define VIBRATION_ACTIVE_LOW 1
#endif

// How long after a relay pulse to ignore the sensor. The relay's own click and
// the structure ringing from it are not the door moving, and on a sensor
// mounted anywhere near the board they are the loudest thing it will ever hear.
#ifndef VIBRATION_BLANK_MS
#define VIBRATION_BLANK_MS 400
#endif

// Edges needed before a travel counts as "the door moved". More than one, so a
// single spurious reading — wind, a bird landing on it, a passing lorry — is
// not mistaken for a door that worked.
#ifndef VIBRATION_MIN_PULSES
#define VIBRATION_MIN_PULSES 3
#endif

// The count that means the door is genuinely IN MOTION, as opposed to the
// count above, which only means something happened.
//
// Two different questions needing two different bars, because the cost of
// being wrong points in opposite directions:
//
//   * After the actuating press, "did it start?" wants a LOW bar. A false
//     negative there sends another press, and a press mid-travel reads as STOP
//     on this hardware — so missing real motion is the expensive mistake.
//   * After a WAKE press, "did that one press already move it?" wants a HIGH
//     bar. A false positive there suppresses the actuating press entirely, so
//     the door never moves and the attempt ends up reported as a stall.
//
// 50 is reached in about 25 ms of real travel, which emits roughly 2,000
// edges per second. A relay's contacts still ringing past the blanking window
// do not get anywhere near it.
#ifndef VIBRATION_MOVING_PULSES
#define VIBRATION_MOVING_PULSES 50
#endif

// ---------------------------------------------------------------------------
// Maintenance mode — a bounded window in which the beacon cannot move the door
// ---------------------------------------------------------------------------
//
// Proximity thresholds are only meaningful for the door where it is MOUNTED:
// the ESP32's antenna is a PCB trace and it is directional, so the same beacon
// at the same distance reads several dB apart depending on which way the board
// faces. Calibrating in place means standing at the door with the collar, which
// is exactly what makes the door actuate — so a window is needed in which it
// listens but does not act. See maintenance.h.

// How long a maintenance window lasts when no duration is given. Long enough to
// walk the boundary a few times and read the numbers back.
#ifndef MAINT_DEFAULT_MS
#define MAINT_DEFAULT_MS 3600000UL   // 60 minutes
#endif

// Floor and ceiling on a requested window. The ceiling is the safety property:
// a door that cannot let an animal in must not stay that way indefinitely, and
// every window ends by itself. Raising this is raising how long the door can be
// left unable to do its job with nobody watching.
#ifndef MAINT_MIN_MS
#define MAINT_MIN_MS 60000UL         // 1 minute
#endif
#ifndef MAINT_MAX_MS
#define MAINT_MAX_MS 14400000UL      // 4 hours
#endif

// How often the accumulated RSSI distribution is uploaded while a window is
// open, so it can be read from a browser rather than a cable.
#ifndef MAINT_REPORT_MS
#define MAINT_REPORT_MS 30000UL
#endif

// ---------------------------------------------------------------------------
// Network console
// ---------------------------------------------------------------------------
//
// The console, reachable over WiFi during a maintenance window. It only listens
// while that window is open, and it is the same console the UART offers — so it
// can open the door. See console.h for why it is bound to the window.

#ifndef CONSOLE_PORT
#define CONSOLE_PORT 23
#endif

// Required before any byte reaches the console. Empty DISABLES the network
// console entirely, which is the default: this firmware must not ship a way
// onto your door that works out of the box. Set it in secrets.h.
#ifndef CONSOLE_PASSWORD
#define CONSOLE_PASSWORD ""
#endif

// How long a connected client has to produce that password before it is
// dropped, so a half-open connection cannot hold the single client slot.
#ifndef CONSOLE_AUTH_TIMEOUT_MS
#define CONSOLE_AUTH_TIMEOUT_MS 15000UL
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

// How many times to press the door's button per actuation, and how long to
// leave between presses.
//
// 1 is the default and the right answer for a controller that reliably acts on
// a single press. Some do not: a press is occasionally swallowed — by the
// controller's own debouncing, by a marginal contact, or by firmware that was
// busy — and the door simply does not move.
//
// If YOUR door sometimes needs its physical button pressed twice, this is the
// same problem and 2 will paper over it.
//
// BUT READ THIS FIRST. The door has no position feedback: it cannot tell
// whether the first press worked. So a repeat is sent blind, and if the first
// press DID take, the second arrives while the door is moving. Many controllers
// read a press mid-travel as STOP — in which case two presses turn "sometimes
// does not move" into "sometimes stops halfway", which is worse, because a door
// parked halfway is neither open nor shut and the firmware believes it is
// whichever it last commanded.
//
// So: test it. Watch a dozen cycles before trusting it, and be specific about
// which failure you are trading for which. The real fix is a position sensor,
// which is the only thing that lets the door know whether it needs to try
// again; this is a stopgap for the wait.
#ifndef RELAY_PULSE_COUNT
#define RELAY_PULSE_COUNT 1
#endif

// Gap between repeated presses. Long enough that the controller sees two
// distinct presses rather than one long one; short enough to stay well inside
// the actuation lockout. Only used when RELAY_PULSE_COUNT > 1.
#ifndef RELAY_PULSE_GAP_MS
#define RELAY_PULSE_GAP_MS 1000
#endif

// How long YOUR door takes to travel, in ms. 0 means "not measured".
//
// This is the number the whole verification path is built on, so it is worth
// being exact about what it does:
//
//   * it is the deadline for arrival. With limit switches fitted, a travel that
//     does not reach its end within this (plus TRAVEL_GRACE_MS) is a STALL —
//     logged, sounded, and for a close, reversed. See petdoor/actuator.h.
//   * it drives the announcement. For this long the status LED shows "moving"
//     and the annunciator ticks.
//   * with no switches fitted it is ONLY the announcement, and the "arrived"
//     chime is a stopwatch expiring. It will chime at a door stuck halfway.
//
// At 0 the firmware does not verify and does not announce, because it has not
// been told how long to wait. That is the default, and it is deliberately not
// a guess: a travel time too short reads every good travel as a stall, and
// with a close that means reversing a door that was closing perfectly well.
//
// OPEN AND CLOSE ARE SEPARATE, and on a mounted door they are not equal.
// Gravity assists one direction and opposes the other: measured flat, the
// reference door took 12,180 ms to open and 12,704 ms to close, and mounted
// upright those two diverge further. Measure both.
//
// Measure them in service rather than with a stopwatch: `s` reports the
// duration of the last verified travel in each direction, and `calibrate`
// times both and adopts the result. See docs/TUNING.md.
#ifndef DOOR_TRAVEL_MS
#define DOOR_TRAVEL_MS 0
#endif

// Per-direction overrides. Both default to DOOR_TRAVEL_MS, so a build that
// only knows one number still works and a build that knows two says so.
#ifndef DOOR_TRAVEL_OPEN_MS
#define DOOR_TRAVEL_OPEN_MS DOOR_TRAVEL_MS
#endif

#ifndef DOOR_TRAVEL_CLOSE_MS
#define DOOR_TRAVEL_CLOSE_MS DOOR_TRAVEL_MS
#endif

// How much longer than the measured travel to wait before calling it a stall.
// A door is slower in January, slower with a bird leaning on it, and slower
// as the mechanism wears. This is the margin that keeps those from reading as
// failures — and keeping it as a separate number means the travel time stays
// an honest measurement rather than a measurement with padding baked in.
#ifndef TRAVEL_GRACE_MS
#define TRAVEL_GRACE_MS 3000
#endif

// The deadline for arrival when limit switches ARE fitted but the travel time
// is not yet known. With a switch at the destination the door does not need a
// travel time to know it has arrived — the switch says so — so it waits, and
// the duration it waited becomes the measurement. This only bounds how long
// that wait may be before it is called a stall.
//
// It is generous on purpose: being wrong in this direction costs a slow stall
// report on a door nobody has calibrated, and being wrong in the other
// direction reports a perfectly good travel as a failure.
#ifndef ARRIVAL_WAIT_MAX_MS
#define ARRIVAL_WAIT_MAX_MS 60000
#endif

// ---- waking the vendor controller (see docs/REQUIREMENTS.md F1) ------------
//
// The ESP32 does not drive the motor. It closes a relay across a button on a
// commercial door controller, and that controller SLEEPS: the first press
// after a long idle is always swallowed, and the second works. Confirmed 4 out
// of 4 after 180 s idle, while six consecutive presses seconds apart all
// worked.
//
// A coop door is idle for hours between uses, so nearly every real actuation
// is a cold one — which is why this is not an edge case to tolerate but the
// normal path to design for.
//
// The fix is a WAKE PRESS: when the controller is believed asleep, press once
// to wake it, then press again to actuate. What makes that safe rather than a
// second way to break things is that the two presses are not sent blind. A
// press that lands on an awake controller MOVES THE DOOR, and a second press
// mid-travel reads as STOP on this hardware — the exact failure the wake press
// would otherwise introduce. So after the wake press the firmware watches for
// WAKE_PROBE_MS, and if the door started moving it does not press again.
//
// WAKE_IDLE_MS is how long since the last press before the controller is
// believed asleep. Below it, one press; above it, wake then press.
#ifndef WAKE_IDLE_MS
#define WAKE_IDLE_MS 30000
#endif

// How long to watch after a wake press before deciding it did nothing.
// Motion onset measured at ~1300 ms after a press, so this must be comfortably
// past that or a press that DID take would be followed by one that stops the
// door. It is also dead time added to every cold actuation, so it should not be
// much longer than it needs to be.
#ifndef WAKE_PROBE_MS
#define WAKE_PROBE_MS 1800
#endif

// How long to wait for the door to start moving after the actuating press.
// Only meaningful with a vibration sensor or a limit switch at the starting
// end — with neither, nothing can observe a start and this is unused.
#ifndef MOTION_ONSET_MS
#define MOTION_ONSET_MS 2500
#endif

// A press that demonstrably moved nothing may be retried straight away: the
// door is still where it was, nothing is trapped, and the failure is
// distinguishable from a stall. This bounds how many times.
#ifndef SWALLOW_RETRY_LIMIT
#define SWALLOW_RETRY_LIMIT 2
#endif

// ---- when a close fails ----------------------------------------------------
//
// A door that stops partway while CLOSING is the one case this project exists
// to prevent, because that is exactly when something may be under it. So a
// stalled close fails OPEN: the firmware reverses its own command rather than
// pressing again.
//
// Retrying the close is then delayed by MINUTES, not seconds — long enough
// that whatever blocked it has moved or been noticed — and limited to
// CLOSE_RETRY_LIMIT attempts. After that the door stays open and says so, on
// the console, in the log, in the status line and on the LED. An open door is
// an inconvenience; a door that keeps driving onto an obstruction is not.
#ifndef CLOSE_RETRY_DELAY_MS
#define CLOSE_RETRY_DELAY_MS 300000
#endif

#ifndef CLOSE_RETRY_LIMIT
#define CLOSE_RETRY_LIMIT 3
#endif

// How long to leave a direction alone after an attempt in it achieved nothing.
//
// Without this the door loops. Nothing moved, so nothing was committed, so the
// condition that asked for the travel is still true, so the control task asks
// again on its next tick — ten times a second, pressing a relay each time. The
// internal retry (SWALLOW_RETRY_LIMIT) is the fast one and has already been
// spent by the time this applies; this is the slow one, for "that did not work
// and doing it all again immediately will not work either".
//
// Per direction, so a close that achieved nothing never delays the open that
// follows it. Opening is the safe direction and is never held back by this.
#ifndef FAILED_ATTEMPT_COOLDOWN_MS
#define FAILED_ATTEMPT_COOLDOWN_MS 30000
#endif

// The floor between immediate "the door moved" reports to the server.
//
// A completed travel changes the one thing a dashboard exists to show, so it is
// uploaded at once rather than waiting for the door to be idle. That overrides
// the idle gate deliberately — see wifi_logger.h for why that gate exists — and
// a forced flush bypasses the upload interval ENTIRELY, so without a floor a
// door flapping at the threshold would put the radio up every half minute for
// as long as it lasted, which is precisely the starvation the gate prevents.
//
// A minute is longer than any realistic open/close cycle (a close needs the
// full exit dwell of confirmed absence first), so in normal use this never
// bites. When it does bite, nothing is lost: the state still goes out with the
// next ordinary upload.
#ifndef DOOR_REPORT_MIN_MS
#define DOOR_REPORT_MIN_MS 60000
#endif

// ---- travel-time calibration (see docs/REQUIREMENTS.md R18) ----------------
//
// `calibrate` times a travel in each direction and adopts the result. It
// begins with a QUIET PERIOD during which nothing is commanded and no limit
// switch may change, because the vendor controller has modes of its own and
// has been observed driving the door with nothing commanding it. A measurement
// taken while that is happening is not a measurement of anything.
//
// 90 s because that is the quiet period that recorded zero movement once those
// modes were disabled. Shorter risks certifying a door that simply had not got
// round to moving yet.
#ifndef CAL_QUIET_MS
#define CAL_QUIET_MS 90000
#endif

// ---- position sensors ------------------------------------------------------
//
// Two normally-open limit switches telling the door where it ACTUALLY is. A
// reed switch and a magnet is the right pick for a coop: sealed glass, nothing
// exposed to weather or to a curious beak.
//
//   GPIO PIN_SENSOR_OPEN   ──[ reed switch ]── GND    closed when fully OPEN
//   GPIO PIN_SENSOR_CLOSED ──[ reed switch ]── GND    closed when fully CLOSED
//
// -1 disables that end, and both default to -1 because most builds have none.
//
// What they buy you: the door stops guessing. Its reported state becomes the
// measured one, the "arrived" chime fires on arrival instead of on a stopwatch,
// and a press the controller swallowed becomes visible instead of being
// indistinguishable from one it obeyed.
//
// What they deliberately do NOT do is drive the motor. The proximity logic
// still decides every actuation and the interlock still gates it; these only
// correct what the firmware believes. A sensor that can command a door has a
// much worse failure mode than one that cannot. See petdoor/position.h.
#ifndef PIN_SENSOR_OPEN
#define PIN_SENSOR_OPEN -1
#endif

#ifndef PIN_SENSOR_CLOSED
#define PIN_SENSOR_CLOSED -1
#endif

// 1 = switch shorts the pin to GND and the pin idles high on an internal
// pull-up. This is the default and what you want almost always: a broken wire
// or a lost magnet then reads as "not at that end", never as a false arrival.
//
// 0 = your wiring drives the pin HIGH at that end and supplies its own
// pull-down. The ESP32 cannot pull down and up at once, so the internal
// pull-up is dropped in that mode and the external resistor is not optional.
#ifndef SENSOR_ACTIVE_LOW
#define SENSOR_ACTIVE_LOW 1
#endif

// A reed switch chatters as the magnet passes, and a door settling on its stop
// can bounce it several times. Without this the door would announce three
// arrivals for one. 50 ms is far longer than the bounce and far shorter than
// any real travel.
#ifndef SENSOR_DEBOUNCE_MS
#define SENSOR_DEBOUNCE_MS 50
#endif

// ---- the annunciator -------------------------------------------------------
//
// A buzzer that says "I heard you, wait" while the door travels and "that
// should be it" when DOOR_TRAVEL_MS is up. Fifteen seconds of silence between
// a relay click and a door that has moved is indistinguishable from a relay
// click that went nowhere, which is most of what makes this door frustrating
// to stand next to.
//
// -1 disables it, and that is the default: this firmware must not start
// driving an arbitrary GPIO on the assumption something harmless is attached.
// Set it to the pin your buzzer is on. If your board has one but does not say
// where, `buzzer <pin>` then `beep` on the console tries a candidate in
// seconds, and the value is saved on the device — you are not reflashing to
// guess. See docs/WIRING.md#the-annunciator.
//
// This is NOT speech. A buzzer plays patterns; a spoken "wait" needs a DAC, an
// amplifier and a speaker, which is a different build. See petdoor/chime.h.
#ifndef PIN_BUZZER
#define PIN_BUZZER -1
#endif

// 0 = ACTIVE buzzer: has its own oscillator, sounds on DC, one fixed pitch.
// 1 = PASSIVE buzzer: a bare transducer that needs a square wave, and plays
//     whatever pitch it is given.
// Guessing wrong is harmless — an active buzzer driven as passive still makes
// noise, it just ignores the tune. `beep` tells you which you have: three even
// beeps on both, but only a passive one plays a RISING pair when the door
// finishes.
#ifndef BUZZER_PASSIVE
#define BUZZER_PASSIVE 0
#endif

// The same active-low trap as the relays, and for the same reason: plenty of
// boards drive their buzzer through a transistor that sounds when the pin is
// pulled to GND. Symptom of getting it wrong is a buzzer that screams
// continuously from boot and goes quiet only during a chime. Only meaningful
// for an ACTIVE buzzer; the passive path drives a waveform either way.
#ifndef BUZZER_ACTIVE_LOW
#define BUZZER_ACTIVE_LOW 0
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
