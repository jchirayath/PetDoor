// PetDoor — BLE-beacon proximity control for a chicken coop / pet door.
//
// An ESP32 scans continuously for a Bluetooth Low Energy beacon. When the
// beacon has been convincingly nearby for a moment, it pulses the OPEN relay;
// when the beacon has been convincingly gone for a while, it pulses the CLOSE
// relay.
//
// Quick start
//   1. Flash as-is. With no beacon configured it boots into discovery mode and
//      prints every BLE device it can hear.
//   2. Find your beacon in that list, copy its MAC.
//   3. Put the MAC in secrets.h (see secrets.example.h), reflash.
//   4. Type `c` in the serial monitor and walk around to pick your thresholds.
//
// Serial monitor: 115200 baud. Type `h` for the command list.
//
// Full documentation is in docs/. Read docs/SAFETY.md before connecting this
// to a motor that moves a door an animal walks through.

#include <Arduino.h>
#include <Preferences.h>
#include <errno.h>
#include <esp_system.h>
#include <stdlib.h>

#if defined(__has_include)
#if __has_include(<soc/rtc_cntl_reg.h>)
#include <soc/rtc_cntl_reg.h>
#endif
#endif

// Software-triggered download mode needs an RTC "force download boot" flag.
// The ESP32-S3/C3/C6 have one; the ORIGINAL ESP32 (WROOM) does not, so on that
// chip the only way in is holding IO0 while EN is pulsed — or wiring DTR/RTS so
// the uploader can do it for you.
#ifdef RTC_CNTL_FORCE_DOWNLOAD_BOOT
#define PETDOOR_CAN_REBOOT_TO_FLASH 1
#else
#define PETDOOR_CAN_REBOOT_TO_FLASH 0
#endif

#include "actuator.h"
#include "ble_scanner.h"
#include "chime.h"
#include "config.h"
#include "console.h"
#include "door.h"
#include "eventlog.h"
#include "maintenance.h"
#include "position.h"
#include "vibration.h"
#include "proximity.h"
#include "schedule.h"
#include "wifi_logger.h"

namespace {

ProximityTracker g_tracker;
DoorController g_door;

// Announcement bookkeeping. The travel itself is owned by the actuator; these
// are only about what the buzzer and the console have already said.
bool g_lockRefusedAnnounced = false;
bool g_gaveUpAnnounced = false;
DoorState g_lastObserved = DOOR_UNKNOWN;
bool g_sensorFaultAnnounced = false;
Actuator::CalState g_lastCalState = Actuator::CAL_OFF;

// Why a LOG_REFUSED entry exists, for the reasons that are not an
// ActuationResult. Kept well clear of that enum so adding a refusal to it can
// never silently re-label history on the dashboard.
constexpr uint8_t kRefusedBySchedule = 100;

// Defined further down, beside the rest of the maintenance-window handling,
// but needed by the console key dispatch above it.
bool applyScheduleLine(const char *line, String &result);
void printScheduleMenu();
bool beginMaintenance(uint32_t nowMs, uint32_t durationMs, String &message);
void endMaintenance(String &message);
void publishStatusLines();
void announceMovement(ActuationSource src, DoorState target);

bool g_calibrate = false;
uint32_t g_lastDiscoverDumpMs = 0;
uint32_t g_lastCalibrateMs = 0;
PresenceState g_lastReportedPresence = PRESENCE_ABSENT;
bool g_lastReportedFix = false;        // no fix at boot
bool g_lastReportedScanHealthy = true; // radio assumed good until proven otherwise

// The console is otherwise single-keystroke. Editing a MAC needs a whole line,
// so `m` switches into a line-buffered mode until Enter is pressed.
TaskHandle_t g_controlTaskHandle = nullptr;
uint32_t g_bootCount = 0;

// Why the ESP32 last restarted. BROWNOUT is the one that matters: it means the
// supply sagged, which is the usual cause of a coop controller that "randomly
// reboots" — almost always a thin USB cable or a supply shared with the relay
// coils.
const char *resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external reset pin";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "CRASH (panic)";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply sagged)";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "unknown";
  }
}

// Counted in NVS so it survives the reboot it is counting.
void recordBoot() {
  Preferences prefs;
  if (!prefs.begin("petdoor", /*readOnly=*/false)) return;
  g_bootCount = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", g_bootCount);
  prefs.end();
}

// `!` is a two-step command: it stops the firmware controlling the door until
// something is flashed, so a single stray keystroke must not trigger it.
bool g_downloadModeArmed = false;

// Reboots straight into the ROM's UART download mode, so a board with no
// DTR/RTS auto-reset wiring can be flashed without the IO0/EN button dance.
//
// Sets the RTC "force download boot" flag and resets: the ROM then comes up in
// download mode instead of running the app. The flag lives in the RTC domain,
// so it survives the software reset — and a power cycle clears it, which is
// the way back out if you change your mind.
void rebootToDownloadMode() {
#if PETDOOR_CAN_REBOOT_TO_FLASH
  Con.println(F("[sys] rebooting into UART download mode."));
  Con.println(F("[sys]   * the door is NOT controlled until you flash"));
  Con.println(F("[sys]   * quit your terminal first, then upload"));
  Con.println(F("[sys]   * power-cycle the board to cancel"));
  Con.flush();
  delay(400);
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
  esp_restart();
#else
  Con.println(F("[sys] This chip (original ESP32) has no software"));
  Con.println(F("[sys] download-mode flag — only the S3/C3/C6 do."));
  Con.println(F("[sys] To flash: hold IO0, tap EN, release IO0."));
  Con.println(F("[sys] To avoid that for good, wire the adapter's"));
  Con.println(F("[sys] DTR->IO0 and RTS->EN so uploads reset the board."));
#endif
}

enum EntryMode { ENTRY_NONE, ENTRY_MAC, ENTRY_THRESH, ENTRY_TIMING, ENTRY_FILTER,
                 ENTRY_SCHEDULE };
EntryMode g_entry = ENTRY_NONE;
char g_macLine[192];
uint16_t g_macLineLen = 0;
bool g_thresholdsStored = false;
bool g_timingStored = false;
bool g_filterStored = false;
bool g_fastFilterStored = false;

// Deadline for the manual-open hold, 0 when inactive. Not persisted: automatic
// control must always be what a reboot comes back to.
uint32_t g_manualHoldUntilMs = 0;

// Deadline for a restart a remote command asked for, 0 when none is pending.
// Deferred so the acknowledgement reaches the server before the reboot takes
// it with it. Same signed-delta comparison as everywhere else in this file.
uint32_t g_restartAtMs = 0;

// When set, the beacon may no longer open the door.
//
// WHAT IT DOES NOT DO, and each of these is deliberate:
//
//   It does not close a door that is already open. Locking sets a rule about
//   FUTURE opens; it does not slam a door an animal may be standing in. The
//   normal close path still runs when the beacon leaves, so a locked door
//   settles shut on its own and then stays shut.
//
//   It does not block closing. Closing is the safe direction and is never
//   gated on anything.
//
//   It does not block YOU. `door open`, from the console or from the server,
//   still works while locked — the lock is about the collar, not the owner.
//   That is the escape hatch: if the animal is shut out, you can let it in
//   without first unlocking and losing the state you wanted.
//
// It IS persisted, because a lock that a power cut silently clears is not a
// lock. The cost is that it survives a reboot you did not intend, which is why
// every status line says so in capitals.
bool g_locked = false;

// Set by a `scan` command: upload the discovery table with the next flush.
bool g_scanRequested = false;

// A bounded, self-expiring LED test. updateLed() rewrites the pin on every
// tick, so a command that simply lit the LED would be dark again 100 ms later
// — the test has to live inside the pattern engine rather than beside it.
//
// Three deliberate flashes, mirroring the buzzer's three beeps, because it
// answers the same question: is this peripheral on the pin I think it is. The
// low duty cycle keeps it clearly apart from the even 1 Hz / 2 Hz / 5 Hz fault
// blinks, and it expires by itself — a test that could be left running would
// be a status light that lies.
bool g_ledTestActive = false;
uint32_t g_ledTestStartMs = 0;
constexpr uint32_t kLedTestPeriodMs = 800;
constexpr uint32_t kLedTestOnMs = 200;
constexpr uint32_t kLedTestMs = 3 * kLedTestPeriodMs;

void startLedTest(uint32_t nowMs) {
  g_ledTestActive = true;
  g_ledTestStartMs = nowMs;
}

// dumpTable() writes to a Stream because it was built for the console. This
// collects that output into a String instead, so the same rendering can be
// uploaded — one implementation, so the remote view can never drift from what
// the console shows.
class StringStream : public Stream {
 public:
  String text;
  size_t write(uint8_t c) override {
    text += static_cast<char>(c);
    return 1;
  }
  size_t write(const uint8_t *buf, size_t n) override {
    for (size_t i = 0; i < n; i++) text += static_cast<char>(buf[i]);
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// Signed delta, not `nowMs < g_manualHoldUntilMs` — the naive comparison ends
// the hold 49 days early or late around the millis() wrap. Same trap as
// ProximityTracker::sampleAgeMs(); see the comment there.
bool manualHoldActive(uint32_t nowMs) {
  if (g_manualHoldUntilMs == 0) return false;
  return static_cast<int32_t>(g_manualHoldUntilMs - nowMs) > 0;
}

uint32_t manualHoldRemainingMs(uint32_t nowMs) {
  if (!manualHoldActive(nowMs)) return 0;
  return static_cast<uint32_t>(static_cast<int32_t>(g_manualHoldUntilMs - nowMs));
}

String fmt1(float v) {
  if (v < 0.0f) return String("?");
  return String(v, 1);
}

void printBanner() {
  Con.println();
  Con.println(F("=================================================="));
  Con.println(F("  PetDoor — BLE proximity door controller"));
  if (PETDOOR_GIT[0] != '\0') Con.printf("  commit        : %s\r\n", PETDOOR_GIT);
  Con.printf("  v%s  (built %s, %s)\r\n", PETDOOR_VERSION, PETDOOR_BUILD,
                BleScanner::stackName());
  Con.println(F("=================================================="));
  Con.print(F("  target beacon : "));
  Con.println(BleScanner::describeTarget());
  Con.printf("  open / close  : GPIO %d / GPIO %d (active %s)\r\n", PIN_RELAY_OPEN,
                PIN_RELAY_CLOSE, RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
  Con.printf("  relay pulse   : %lu ms\r\n",
                static_cast<unsigned long>(g_door.pulseMs()));
  if (g_door.pulseCount() > 1) {
    Con.printf("  presses       : %u per actuation, %lu ms apart\r\n",
                  g_door.pulseCount(),
                  static_cast<unsigned long>(g_door.pulseGapMs()));
    // Worth saying out loud: this is a blind retry, and the failure it can
    // introduce looks nothing like the one it fixes.
    Con.println(F("  !! repeat presses are sent blind — the door cannot tell whether"));
    Con.println(F("  !! the first worked. If it did, the second may stop it mid-travel."));
  }
  {
    const uint32_t tOpen = Actuator::travelMs(DOOR_OPEN);
    const uint32_t tClose = Actuator::travelMs(DOOR_CLOSED);
    if (tOpen > 0 || tClose > 0) {
      Con.printf("  door travel   : open %lu ms, close %lu ms (%s)\r\n",
                    static_cast<unsigned long>(tOpen),
                    static_cast<unsigned long>(tClose),
                    Position::enabled() ? "verified by the limit switches"
                                        : "announced only — a stopwatch");
      // Warned about at boot rather than static_assert'ed, because both values
      // are runtime-adjustable and can be changed long after compiling.
      const uint32_t longest = tOpen > tClose ? tOpen : tClose;
      if (g_door.minIntervalMs() < longest) {
        Con.printf("  !! min interval (%lu ms) is SHORTER than door travel (%lu ms).\r\n",
                      static_cast<unsigned long>(g_door.minIntervalMs()),
                      static_cast<unsigned long>(longest));
        Con.println(F("  !! A reversing command can land mid-travel; most controllers"));
        Con.println(F("  !! read that as STOP, leaving the door parked half open."));
        Con.println(F("  !! Raise it with 'w', or set MIN_ACTUATION_INTERVAL_MS."));
      }
    } else if (Position::enabled()) {
      // Switches fitted and no travel time: the door can still tell that it
      // arrived, but it cannot tell how long it is allowed to take, so a stall
      // takes ARRIVAL_WAIT_MAX_MS to report instead of travel + grace.
      Con.println(F("  door travel   : NOT MEASURED — limit switches are fitted, so"));
      Con.println(F("  !! arrival is still verified, but a stall takes a full minute"));
      Con.println(F("  !! to report. Run 'calibrate' once to fix that."));
    }
    if (Actuator::wakeIdleMs() > 0) {
      Con.printf("  wake press    : after %lu s idle, a press to wake the controller\r\n",
                    static_cast<unsigned long>(Actuator::wakeIdleMs() / 1000UL));
    }
  }
  Con.printf("  status LED    : GPIO %d\r\n", PIN_STATUS_LED);
  if (Chime::enabled()) {
    Con.printf("  buzzer        : GPIO %d, %s, active %s\r\n", Chime::pin(),
                  Chime::passive() ? "passive (tones)" : "active (fixed pitch)",
                  Chime::activeLow() ? "LOW" : "HIGH");
  } else {
    Con.println(F("  buzzer        : none ('w' then 'buzzer <pin>' to add one)"));
  }
  if (Position::enabled()) {
    Con.printf("  limit switches: open GPIO %d, closed GPIO %d, active %s\r\n",
                  Position::openPin(), Position::closedPin(),
                  Position::activeLow() ? "LOW" : "HIGH");
    Con.printf("  door is really: %s\r\n",
                  DoorController::stateName(Position::state()));
  } else {
    Con.println(F("  limit switches: none — the door is OPEN LOOP and only knows"));
    Con.println(F("                  what it commanded ('w' then 'sensors' to add)"));
  }
  Con.printf("  thresholds    : open at >= %d dBm, close at <= %d dBm (%s)\r\n",
                g_tracker.enterDbm(), g_tracker.exitDbm(),
                g_thresholdsStored ? "saved on device" : "compiled in");
  Con.printf("  dwell         : open after %lu ms near, close after %lu ms far (%s)\r\n",
                static_cast<unsigned long>(g_tracker.enterConfirmMs()),
                static_cast<unsigned long>(g_tracker.exitConfirmMs()),
                g_timingStored ? "saved on device" : "compiled in");
  Con.printf("  scan          : %d ms window / %d ms interval, %s, duplicates ON\r\n",
                SCAN_WINDOW_MS, SCAN_INTERVAL_MS, SCAN_ACTIVE ? "active" : "passive");
  Con.printf("  boot          : #%lu, last reset: %s\r\n",
                static_cast<unsigned long>(g_bootCount), resetReasonName());
  Con.println(F("--------------------------------------------------"));

  if (esp_reset_reason() == ESP_RST_BROWNOUT) {
    Con.println();
    Con.println(F("  !! Last restart was a BROWNOUT — the supply sagged. !!"));
    Con.println(F("  Use a thicker USB cable, a stronger 5 V supply, or feed"));
    Con.println(F("  the relay coils from their own supply with a common ground."));
    Con.println();
  }

  if (!BleScanner::isConfigured()) {
    Con.println();
    Con.println(F("  !! NO BEACON CONFIGURED — the door will not be operated. !!"));
    Con.println(F("  Discovery mode is on. Find your beacon below, then set"));
    Con.println(F("  BEACON_MAC in secrets.h (copy secrets.example.h) and reflash."));
    Con.println(F("  A Minew beacon usually shows up as class Minew or iBeacon."));
    Con.println();
  }

  Con.println(F("  Type 'h' for commands."));
  Con.println();
}

void printHelp() {
  Con.println(F("commands:"));
  Con.println(F("  h  this help"));
  Con.println(F("  s  status"));
  Con.println(F("  d  toggle discovery mode (list every BLE device in range)"));
  Con.println(F("  c  toggle calibration stream (live RSSI + distance)"));
  Con.println(F("  r  reset the proximity filter"));
  Con.println(F("  l  show the event log (what the door actually did)"));
  Con.println(F("  u  upload the log now over WiFi (if configured)"));
  Con.println(F("  p  open a firmware update window (OTA, no buttons)"));
  Con.println(F("  m  edit the beacon MAC list (saved on the device)"));
  Con.println(F("  t  edit the open/close thresholds (saved on the device)"));
  Con.println(F("  n  edit the scheduled lockout windows (e.g. locked overnight)"));
  Con.println(F("  w  edit the dwell times / how fast it reacts"));
  Con.println(F("  f  edit the filter shape (median window, smoothing)"));
#if PETDOOR_CAN_REBOOT_TO_FLASH
  Con.println(F("  !  reboot into flash mode (no IO0/EN buttons needed)"));
#else
  Con.println(F("  !  flash-mode help (this chip needs the IO0/EN buttons)"));
#endif
#if ALLOW_MANUAL_SERIAL_CONTROL
  Con.println(F("  o  OPEN the door now, and hold it open (see 'O')"));
  Con.println(F("  x  CLOSE the door now, and clear any hold"));
  Con.println(F("  O  clear the manual hold, handing control back to the beacon"));
  Con.println(F("  C  calibrate the travel time, both directions (needs 'M' first)"));
  Con.println(F("  k  LOCK — the beacon may no longer open the door"));
  Con.println(F("  M  MAINTENANCE — the door listens but does not move, for a"));
  Con.println(F("     bounded window; opens the console over WiFi. 'M' again ends it."));
  Con.println(F("  K  unlock"));
#endif
  Con.println(F("status LED:"));
  Con.println(F("  solid        door OPEN"));
  Con.println(F("  brief blip   door CLOSED  (double blip = closed and LOCKED)"));
  Con.println(F("  near-solid   door MOVING"));
  Con.println(F("  off          position UNKNOWN — with switches fitted, that means"));
  Con.println(F("               genuinely between the two ends"));
  Con.println(F("  3 blips      GAVE UP closing: staying open until somebody helps"));
  Con.println(F("  1 Hz blink   no beacon configured / never heard"));
  Con.println(F("  2 Hz flash   beacon battery LOW"));
  Con.println(F("  5 Hz flutter radio unhealthy"));
  Con.println(F("buzzer (if fitted — 'w' to configure):"));
  Con.println(F("  WHO ASKED, by how many beeps. The long beep is last to open and"));
  Con.println(F("  first to close, so you also hear which way it is going:"));
  Con.println(F("    . -        2 beeps: the COLLAR arrived     (- . closing)"));
  Con.println(F("    . . -      3 beeps: the CONSOLE asked      (- . . closing)"));
  Con.println(F("    . . . -    4 beeps: the NETWORK asked      (- . . . closing)"));
  Con.println(F("  then, while it travels and when it finishes:"));
  Con.println(F("    tick..tick still moving"));
  Con.println(F("    rising pair ARRIVED (verified by a switch, or the timer expired)"));
  Con.println(F("    two long    it NEVER MOVED — the press was swallowed"));
  Con.println(F("    5 fast      it STALLED partway. A stalled close is REVERSED"));
  Con.println(F("    long . .    GAVE UP closing; staying open"));
  Con.println(F("    . pause .   the door moved and NOTHING commanded it"));
  Con.println(F("    one long    refused (locked, or a schedule window)"));
}

void printStatus(uint32_t nowMs) {
  Con.println(F("---- status ----"));
  Con.print(F("  target       : "));
  Con.println(BleScanner::describeTarget());
  Con.printf("  uptime       : %lu s\r\n", static_cast<unsigned long>(nowMs / 1000));
  Con.printf("  presence     : %s%s\r\n",
                g_tracker.isPresent() ? "PRESENT" : "ABSENT",
                g_tracker.hasFix(nowMs) ? "" : " (no fix)");
  Con.printf("  door         : %s\r\n", DoorController::stateName(g_door.state()));

  if (g_tracker.everSeen()) {
    Con.printf("  rssi         : %d dBm filtered (raw %d), ~%s m\r\n", g_tracker.filteredRssi(),
                  g_tracker.rawRssi(), fmt1(g_tracker.distanceM()).c_str());
    Con.printf("  last seen    : %lu ms ago, %lu samples\r\n",
                  static_cast<unsigned long>(g_tracker.sampleAgeMs(nowMs)),
                  static_cast<unsigned long>(g_tracker.totalSamples()));
  } else {
    Con.println(F("  rssi         : beacon has never been heard"));
  }

  const uint32_t enterFor = g_tracker.pendingEnterMs(nowMs);
  const uint32_t exitFor = g_tracker.pendingExitMs(nowMs);
  // Runtime dwell, not the compile-time macro: after a `w` command that
  // lengthens a dwell, the macro is smaller than the elapsed time and the
  // subtraction underflows to ~4.29e9.
  if (enterFor) {
    const uint32_t total = g_tracker.enterConfirmMs();
    Con.printf("  opening in   : %lu ms\r\n",
                  static_cast<unsigned long>(total > enterFor ? total - enterFor : 0));
  }
  if (exitFor) {
    const uint32_t total = g_tracker.exitConfirmMs();
    Con.printf("  closing in   : %lu ms\r\n",
                  static_cast<unsigned long>(total > exitFor ? total - exitFor : 0));
  }

  Con.printf("  radio        : %lu adverts, last %lu ms ago, %lu samples dropped\r\n",
                static_cast<unsigned long>(BleScanner::advCount()),
                static_cast<unsigned long>(BleScanner::advAgeMs(nowMs)),
                static_cast<unsigned long>(BleScanner::droppedSamples()));
  {
    EddystoneTlm tlm;
    uint32_t tlmAge = 0;
    if (BleScanner::targetTelemetry(tlm, tlmAge, nowMs)) {
      if (tlm.batteryMv > 0) {
        // A CR2032 is ~3000 mV fresh and considered flat around 2200 mV.
        const int pct = static_cast<int>(
            (static_cast<long>(tlm.batteryMv) - 2200) * 100 / (3000 - 2200));
        Con.printf("  beacon batt  : %u mV (~%d%%)%s\r\n", tlm.batteryMv,
                      pct < 0 ? 0 : (pct > 100 ? 100 : pct),
                      tlm.batteryMv < 2400 ? "  << REPLACE SOON" : "");
      } else {
        Con.println(F("  beacon batt  : not reported (0 mV = mains powered)"));
      }
      Con.printf("  beacon temp  : %.1f C, up %lu s, %lu adverts sent\r\n",
                    static_cast<double>(tlm.temperatureC),
                    static_cast<unsigned long>(tlm.uptimeSec),
                    static_cast<unsigned long>(tlm.advCount));
    }
  }

  if (g_tracker.minRssi() != 0) {
    Con.printf("  weakest heard: %d dBm  (%lu samples at/below -85 dBm)\r\n",
                  g_tracker.minRssi(), static_cast<unsigned long>(g_tracker.weakSamples()));
  }
  Con.printf("  worst gap    : %lu ms between samples%s\r\n",
                static_cast<unsigned long>(g_tracker.maxGapMs()),
                g_tracker.maxGapMs() > SAMPLE_MAX_AGE_MS ? "  << EXCEEDS SAMPLE_MAX_AGE_MS" : "");
  Con.printf("  relay pulse  : %lu ms\r\n",
                static_cast<unsigned long>(g_door.pulseMs()));
  if (Actuator::busy()) {
    Con.printf("  actuating    : %s %s — %lu ms elapsed, %lu ms before it counts as a stall\r\n",
                  Actuator::phaseName(Actuator::phase()),
                  DoorController::stateName(Actuator::target()),
                  static_cast<unsigned long>(Actuator::elapsedMs(nowMs)),
                  static_cast<unsigned long>(Actuator::deadlineRemainingMs(nowMs)));
  }
  {
    // The configured times and the measured ones, side by side, because that
    // comparison is the whole of travel-time tuning: a measurement that keeps
    // landing near the deadline is a door about to start reporting stalls.
    const uint32_t tOpen = Actuator::travelMs(DOOR_OPEN);
    const uint32_t tClose = Actuator::travelMs(DOOR_CLOSED);
    if (tOpen > 0 || tClose > 0) {
      Con.printf("  door travel  : open %lu ms, close %lu ms configured\r\n",
                    static_cast<unsigned long>(tOpen),
                    static_cast<unsigned long>(tClose));
    } else {
      Con.println(F("  door travel  : not measured ('calibrate', or 'w' then 'travel')"));
    }
    const uint32_t mOpen = Actuator::measuredMs(DOOR_OPEN);
    const uint32_t mClose = Actuator::measuredMs(DOOR_CLOSED);
    if (mOpen > 0 || mClose > 0) {
      Con.printf("  last verified: open %lu ms, close %lu ms (measured, this boot)\r\n",
                    static_cast<unsigned long>(mOpen),
                    static_cast<unsigned long>(mClose));
    }
  }
  if (Actuator::gaveUp()) {
    Con.printf("  !! GAVE UP   : %u close attempts stalled. The door is staying OPEN\r\n",
                  Actuator::closeAttempts());
    Con.println(F("  !!             until somebody clears whatever is in the way."));
    Con.println(F("  !!             'x' or 'o' from here clears this and tries again."));
  } else if (Actuator::retryWaitRemainingMs(nowMs) > 0) {
    Con.printf("  retrying in  : %lu s (attempt %u of %u after a stalled close)\r\n",
                  static_cast<unsigned long>(Actuator::retryWaitRemainingMs(nowMs) / 1000UL),
                  static_cast<unsigned>(Actuator::closeAttempts() + 1),
                  static_cast<unsigned>(Actuator::retryLimit()));
  }
  if (Actuator::calState() != Actuator::CAL_OFF) {
    const uint32_t left = Actuator::calRemainingMs(nowMs);
    Con.printf("  calibrating  : %s%s\r\n", Actuator::calMessage(),
                  left ? (String(" — ") + (left / 1000UL) + " s of quiet left").c_str() : "");
  }
  if (Position::enabled()) {
    if (Position::fault()) {
      Con.println(F("  position     : !! FAULT — both limit switches made at once"));
    } else {
      Con.printf("  position     : %s (measured)%s\r\n",
                    DoorController::stateName(Position::state()),
                    Position::state() == DOOR_UNKNOWN ? "  << between the two switches" : "");
    }
    Con.printf("  switches     : open GPIO %d %s, closed GPIO %d %s\r\n",
                  Position::openPin(), Position::openMade() ? "MADE" : "open",
                  Position::closedPin(), Position::closedMade() ? "MADE" : "open");
  } else {
    Con.println(F("  position     : not measured — no limit switches fitted"));
  }
  if (Chime::enabled()) {
    Con.printf("  buzzer       : GPIO %d, %s, active %s%s\r\n", Chime::pin(),
                  Chime::passive() ? "passive" : "active",
                  Chime::activeLow() ? "LOW" : "HIGH",
                  Chime::playing() != CHIME_NONE ? "  << sounding now" : "");
  }
  if (g_locked) {
    Con.println(F("  LOCKED       : the beacon cannot open this door ('K' to unlock)"));
  }
  Schedule::describe(Con, EventLog::haveEpoch(), EventLog::epochNow());
  if (Vibration::enabled()) {
    Con.printf("  vibration    : GPIO %d, %lu edges since boot%s\r\n",
               Vibration::pin(), static_cast<unsigned long>(Vibration::pulses()),
               Vibration::activeLow() ? " (pull-up on)" : "");
  } else {
    Con.println(F("  vibration    : no sensor ('w' then 'vibration <pin>' to add one)"));
  }
  if (Maintenance::active(millis())) {
    const uint32_t left = Maintenance::remainingMs(millis());
    Con.printf("  MAINTENANCE  : ON — the door will not move for another %lu min %lu s\r\n",
               static_cast<unsigned long>(left / 60000UL),
               static_cast<unsigned long>((left / 1000UL) % 60UL));
#if PETDOOR_ENABLE_WIFI
    // Only meaningful with WiFi compiled in; without it `Con` is the UART and
    // there is no network side to report on.
    Con.printf("  console      : %s\r\n",
               Con.networkAttached()
                   ? ("attached from " + Con.clientIp().toString()).c_str()
                   : NetConsole::listening() ? "listening, nobody attached" : "off");
#endif
    // The measurement itself. Percentiles rather than an average because the
    // tails are what a threshold has to separate: the strongest reading from
    // away and the weakest from at the door.
    Maintenance::Stats st;
    if (Maintenance::stats(st)) {
      Con.printf("  calibration  : n=%lu  min %d  p5 %d  median %d  p95 %d  max %d\r\n",
                 static_cast<unsigned long>(st.n), st.min, st.p5, st.median,
                 st.p95, st.max);
      Con.println(F("                 ('r' clears this after moving the collar)"));
    } else {
      Con.println(F("  calibration  : no samples yet"));
    }
  }
  {
    const uint32_t heldFor = manualHoldRemainingMs(millis());
    if (heldFor > 0) {
      Con.printf("  manual hold  : %lu s left — automatic CLOSING paused ('O' to clear)\r\n",
                    static_cast<unsigned long>(heldFor / 1000));
    }
  }
  Con.printf("  actuations   : %lu open, %lu close (%lu locked out, %lu in boot grace)\r\n",
                static_cast<unsigned long>(g_door.openCount()),
                static_cast<unsigned long>(g_door.closeCount()),
                static_cast<unsigned long>(g_door.lockedOutCount()),
                static_cast<unsigned long>(g_door.bootGraceCount()));
  Con.printf("  scan restarts: %lu\r\n", static_cast<unsigned long>(BleScanner::scanRestarts()));
  Con.printf("  firmware     : v%s (built %s)\r\n", PETDOOR_VERSION, PETDOOR_BUILD);
  Con.printf("  ble stack    : %s\r\n", BleScanner::stackName());
  Con.printf("  boot         : #%lu, last reset: %s\r\n",
                static_cast<unsigned long>(g_bootCount), resetReasonName());
  WifiLogger::printStatus(Serial);
  // Stack headroom, in bytes still unused at the worst moment so far. A task
  // sized much larger than its high-water mark is heap sitting idle; one
  // approaching zero is a crash waiting for the right input.
  if (g_controlTaskHandle) {
    Con.printf("  task stacks  : control %lu free of %d",
                  static_cast<unsigned long>(
                      uxTaskGetStackHighWaterMark(g_controlTaskHandle) * sizeof(StackType_t)),
                  CONTROL_TASK_STACK);
    const uint32_t up = WifiLogger::stackFreeBytes();
    if (up) Con.printf(", uploader %lu free of %d", static_cast<unsigned long>(up),
                          WIFI_TASK_STACK);
    Con.println();
  }
  Con.printf("  free heap    : %lu bytes (low-water %lu)\r\n",
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMinFreeHeap()));
  Con.println();
}

void printMacMenu() {
  Con.println();
  Con.println(F("---- beacon MAC list ----"));
  const String csv = BleScanner::targetMacsCsv();
  if (csv.length() == 0) {
    Con.println(F("  (none configured — the door will not be operated)"));
  } else {
    Con.printf("  in use (%s):\r\n",
                  BleScanner::targetMacsAreStored() ? "saved on device" : "compiled in");
    int idx = 1, from = 0;
    while (from < static_cast<int>(csv.length())) {
      int comma = csv.indexOf(',', from);
      if (comma < 0) comma = csv.length();
      String one = csv.substring(from, comma);
      one.trim();
      if (one.length()) Con.printf("    %d. %s\r\n", idx++, one.c_str());
      from = comma + 1;
    }
  }
  Con.println(F("  type one of:"));
  Con.println(F("    aa:bb:cc:dd:ee:ff,11:22:33:44:55:66   replace the whole list"));
  Con.println(F("    +aa:bb:cc:dd:ee:ff                    add one"));
  Con.println(F("    -2                                    remove entry 2"));
  Con.println(F("    clear                                 forget saved list"));
  Con.println(F("    q                                     cancel"));
  Con.println(F("  (live output is paused while this menu is open)"));
  Con.print(F("> "));
}

void saveAndRestart(const char *csv) {
  String err;
  if (!BleScanner::storeTargetMacs(csv, err)) {
    Con.printf("\r\n[mac] rejected: %s\r\n", err.c_str());
    Con.println(F("[mac] nothing was changed."));
    printMacMenu();
    return;
  }
  Con.printf("\r\n[mac] saved: %s\r\n", csv);
  Con.println(F("[mac] restarting to apply..."));
  Con.flush();
  delay(250);
  ESP.restart();
}

void processMacLine(char *line) {
  // trim both ends
  while (*line == ' ' || *line == '\t') line++;
  int n = strlen(line);
  while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';

  if (n == 0 || strcmp(line, "q") == 0) {
    Con.println(F("\r\n[mac] cancelled, nothing changed."));
    Con.println(F("[mac] live output resumed."));
    g_entry = ENTRY_NONE;
    return;
  }

  if (strcmp(line, "clear") == 0) {
    BleScanner::clearStoredTargetMacs();
    Con.println(F("\r\n[mac] saved list forgotten; reverting to the compiled-in default."));
    Con.println(F("[mac] restarting to apply..."));
    Con.flush();
    delay(250);
    ESP.restart();
  }

  if (line[0] == '+') {
    String csv = BleScanner::targetMacsCsv();
    if (csv.length()) csv += ",";
    csv += (line + 1);
    saveAndRestart(csv.c_str());
    return;
  }

  if (line[0] == '-') {
    const int target = atoi(line + 1);
    if (target < 1) {
      Con.println(F("\r\n[mac] give an entry number, e.g. -2"));
      printMacMenu();
      return;
    }
    const String csv = BleScanner::targetMacsCsv();
    String kept;
    int idx = 1, from = 0;
    while (from < static_cast<int>(csv.length())) {
      int comma = csv.indexOf(',', from);
      if (comma < 0) comma = csv.length();
      String one = csv.substring(from, comma);
      one.trim();
      if (one.length()) {
        if (idx != target) {
          if (kept.length()) kept += ",";
          kept += one;
        }
        idx++;
      }
      from = comma + 1;
    }
    if (target >= idx) {
      Con.printf("\r\n[mac] there is no entry %d\r\n", target);
      printMacMenu();
      return;
    }
    if (kept.length() == 0) {
      // Removing the last one means "use the compiled-in default" rather than
      // leaving a list that cannot be stored.
      BleScanner::clearStoredTargetMacs();
      Con.println(F("\r\n[mac] last entry removed; reverting to the compiled-in default."));
      Con.println(F("[mac] restarting to apply..."));
      Con.flush();
      delay(250);
      ESP.restart();
    }
    saveAndRestart(kept.c_str());
    return;
  }

  saveAndRestart(line);
}

float distanceForRssi(int rssi) {
  return estimateDistanceM(rssi, static_cast<int8_t>(BEACON_MEASURED_POWER_DBM),
                           PATH_LOSS_EXPONENT);
}

void printThresholdMenu() {
  Con.println();
  Con.println(F("---- proximity thresholds ----"));
  Con.printf("  open when  >= %4d dBm  (~%s m)\r\n", g_tracker.enterDbm(),
                fmt1(distanceForRssi(g_tracker.enterDbm())).c_str());
  Con.printf("  close when <= %4d dBm  (~%s m)\r\n", g_tracker.exitDbm(),
                fmt1(distanceForRssi(g_tracker.exitDbm())).c_str());
  Con.printf("  source     : %s\r\n", g_thresholdsStored ? "saved on device" : "compiled in");
  if (g_tracker.hasFix(millis())) {
    Con.printf("  beacon now : %d dBm  (~%s m)\r\n", g_tracker.filteredRssi(),
                  fmt1(g_tracker.distanceM()).c_str());
  } else {
    Con.println(F("  beacon now : no fix"));
  }
  Con.println(F("  type one of:"));
  Con.println(F("    here        use where the beacon is RIGHT NOW as the open point"));
  Con.println(F("    1m          open within 1 m, close beyond ~2.5x that"));
  Con.println(F("    1m,3m       open within 1 m, close beyond 3 m"));
  Con.println(F("    -59,-69     set directly in dBm (open,close)"));
  Con.println(F("    clear       forget saved values"));
  Con.println(F("    q           cancel"));
  Con.println(F("  (live output is paused while this menu is open)"));
  Con.print(F("> "));
}

void applyThresholds(int enterDbm, int exitDbm) {
  if (!g_tracker.setThresholds(enterDbm, exitDbm)) {
    Con.printf("\r\n[thr] rejected: open (%d) must be greater than close (%d).\r\n",
                  enterDbm, exitDbm);
    Con.println(F("[thr] the gap between them IS the hysteresis band."));
    printThresholdMenu();
    return;
  }
  BleScanner::storeThresholds(enterDbm, exitDbm);
  g_thresholdsStored = true;
  Con.printf("\r\n[thr] saved: open >= %d dBm (~%s m), close <= %d dBm (~%s m)\r\n",
                enterDbm, fmt1(distanceForRssi(enterDbm)).c_str(),
                exitDbm, fmt1(distanceForRssi(exitDbm)).c_str());
  Con.println(F("[thr] active immediately; no restart needed."));
  Con.println(F("[thr] live output resumed."));
  g_entry = ENTRY_NONE;
}

void processThresholdLine(char *line) {
  while (*line == ' ' || *line == '\t') line++;
  int n = strlen(line);
  while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';

  if (n == 0 || strcmp(line, "q") == 0) {
    Con.println(F("\r\n[thr] cancelled, nothing changed."));
    Con.println(F("[thr] live output resumed."));
    g_entry = ENTRY_NONE;
    return;
  }

  if (strcmp(line, "clear") == 0) {
    BleScanner::clearStoredThresholds();
    g_tracker.setThresholds(RSSI_ENTER_DBM, RSSI_EXIT_DBM);
    g_thresholdsStored = false;
    Con.println(F("\r\n[thr] reverted to the compiled-in defaults."));
    g_entry = ENTRY_NONE;
    return;
  }

  // "here" — calibrate from where the beacon is sitting. This is the most
  // reliable option, because it needs no path-loss model at all: it uses the
  // signal actually observed at the spot you care about.
  if (strcmp(line, "here") == 0) {
    if (!g_tracker.hasFix(millis())) {
      Con.println(F("\r\n[thr] no fix — the beacon is not being heard right now."));
      printThresholdMenu();
      return;
    }
    const int measured = g_tracker.filteredRssi();
    // A few dB of margin below the measured value, per docs/TUNING.md: the
    // reading was taken in good conditions, and rain, orientation or a body in
    // the way all cost a few dB.
    applyThresholds(measured - 3, measured - 13);
    return;
  }

  // "1m" or "1m,3m"
  if (strchr(line, 'm') != nullptr) {
    float openM = atof(line);
    const char *comma = strchr(line, ',');
    float closeM = comma ? atof(comma + 1) : (openM * 2.5f);
    if (openM <= 0.0f || closeM <= openM) {
      Con.println(F("\r\n[thr] need 0 < open < close, e.g. 1m,3m"));
      printThresholdMenu();
      return;
    }
    const int8_t ref = static_cast<int8_t>(BEACON_MEASURED_POWER_DBM);
    applyThresholds(rssiAtDistanceM(openM, ref, PATH_LOSS_EXPONENT),
                    rssiAtDistanceM(closeM, ref, PATH_LOSS_EXPONENT));
    return;
  }

  // "-59,-69"
  const char *comma = strchr(line, ',');
  if (comma == nullptr) {
    Con.println(F("\r\n[thr] give two values, e.g. -59,-69 or 1m,3m"));
    printThresholdMenu();
    return;
  }
  applyThresholds(atoi(line), atoi(comma + 1));
}

// Console numbers must be parsed defensively: atol("-1") cast to uint32_t is
// 4294967295, which passes any "greater than zero" check and would then be
// written to NVS and reloaded on every boot.
bool parseBoundedMs(const char *text, uint32_t minMs, uint32_t maxMs, uint32_t &out) {
  if (text == nullptr) return false;
  char *end = nullptr;
  errno = 0;
  const long long v = strtoll(text, &end, 10);
  if (end == text || errno == ERANGE) return false;
  if (v < static_cast<long long>(minMs) || v > static_cast<long long>(maxMs)) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

// A door that takes longer than two minutes to move is not a pet door, and an
// absurd value here would leave the LED and buzzer announcing "moving" for the
// rest of the afternoon. Persisted, so the ceiling has to hold across reboots.
constexpr uint32_t kMaxTravelMs = 120000;

// Both of these are shared by the console and the remote command channel, so
// that `travel 15000` typed at the door and `travel 15000` queued on the server
// take exactly the same path. Two parsers for one setting is how they drift.
// "<ms>" sets both directions; "<openMs> <closeMs>" sets them separately.
//
// One argument still works because that is what every door that has ever been
// configured was told, and because a door lying flat really does travel at
// much the same speed either way. Two exist because a door mounted upright
// does not: gravity assists the close and opposes the open.
bool applyTravelMs(const char *arg, String &msg) {
  uint32_t openMs = 0, closeMs = 0;
  unsigned long a = 0, b = 0;
  const int n = (arg != nullptr) ? sscanf(arg, "%lu %lu", &a, &b) : 0;
  if (n == 1) {
    openMs = closeMs = static_cast<uint32_t>(a);
  } else if (n == 2) {
    openMs = static_cast<uint32_t>(a);
    closeMs = static_cast<uint32_t>(b);
  } else {
    msg = F("travel needs <ms>, or <open ms> <close ms>  (0 = do not announce)");
    return false;
  }
  if ((openMs != 0 && (openMs < 1000 || openMs > kMaxTravelMs)) ||
      (closeMs != 0 && (closeMs < 1000 || closeMs > kMaxTravelMs))) {
    msg = String("travel rejected: 0, or 1000-") + kMaxTravelMs +
          " ms (0 = do not announce or verify)";
    return false;
  }
  if (!Actuator::setTravelMs(openMs, closeMs)) {
    msg = F("travel rejected");
    return false;
  }
  BleScanner::storeTravelPair(openMs, closeMs);
  g_timingStored = true;

  if (openMs == 0 && closeMs == 0) {
    msg = F("travel 0: the door no longer announces or verifies its travels");
    // Leaving the buzzer mid-pattern here would strand a tone on the pin.
    Chime::stop();
    return true;
  }
  msg = String("travel: open ") + openMs + " ms, close " + closeMs + " ms";
  const uint32_t longest = openMs > closeMs ? openMs : closeMs;
  if (g_door.minIntervalMs() < longest) {
    msg += "; WARNING min interval (";
    msg += g_door.minIntervalMs();
    msg += " ms) is shorter, so a reversal can land mid-travel";
  }
  if (!Position::enabled()) {
    msg += "; with no limit switches this is a stopwatch, not a measurement";
  }
  return true;
}

// "<ms>" — how long an idle vendor controller is assumed to need a wake press.
bool applyWakeSpec(const char *arg, String &msg) {
  if (arg == nullptr) {
    msg = F("wake needs <ms>, or 0 to never send a wake press");
    return false;
  }
  const uint32_t ms = strtoul(arg, nullptr, 10);
  if (!Actuator::setWakeIdleMs(ms)) {
    msg = F("wake rejected: 0 (off), or 5000-3600000 ms");
    return false;
  }
  BleScanner::storeActuation(Actuator::wakeIdleMs(), Actuator::retryDelayMs(),
                             Actuator::retryLimit());
  g_timingStored = true;
  if (ms == 0) {
    msg = F("wake OFF — one press per actuation. If your controller sleeps, "
            "expect the first press after an idle hour to be swallowed");
    return true;
  }
  msg = String("wake: a press after ") + ms +
        " ms of idle is preceded by a wake press";
  return true;
}

// "<minutes> <attempts>" — what happens after a close that stalled.
bool applyRetrySpec(const char *args, String &msg) {
  unsigned long mins = 0, tries = 0;
  if (args == nullptr || sscanf(args, "%lu %lu", &mins, &tries) != 2) {
    msg = F("retry needs <minutes> <attempts>, e.g. 5 3");
    return false;
  }
  if (!Actuator::setRetryPolicy(static_cast<uint32_t>(mins) * 60000UL,
                                static_cast<uint8_t>(tries))) {
    msg = F("retry rejected: 1-1440 minutes, 1-10 attempts");
    return false;
  }
  BleScanner::storeActuation(Actuator::wakeIdleMs(), Actuator::retryDelayMs(),
                             Actuator::retryLimit());
  g_timingStored = true;
  msg = String("retry: wait ") + mins + " min between failed closes, " + tries +
        " attempts, then stay open";
  return true;
}

// "<settle ms> <minimum interval ms> <heartbeat ms>". Heartbeat 0 disables it.
bool applyUploadTiming(const char *args, String &msg) {
  unsigned long st = 0, mi = 0, hb = 0;
  if (args == nullptr || sscanf(args, "%lu %lu %lu", &st, &mi, &hb) != 3) {
    msg = F("upload needs three values: <settle ms> <interval ms> <heartbeat ms>");
    return false;
  }
  if (st < WifiLogger::kMinSettleMs || st > WifiLogger::kMaxSettleMs) {
    msg = String("upload: settle must be ") + WifiLogger::kMinSettleMs + "-" +
          WifiLogger::kMaxSettleMs + " ms";
    return false;
  }
  // The floor here is the one that matters. WiFi and BLE share one antenna, so
  // every upload is time taken from listening for the collar; a few seconds
  // would keep the radio up continuously and starve the door's actual job.
  if (mi < WifiLogger::kMinUploadIntervalMs || mi > WifiLogger::kMaxUploadIntervalMs) {
    msg = String("upload: interval must be ") + WifiLogger::kMinUploadIntervalMs + "-" +
          WifiLogger::kMaxUploadIntervalMs + " ms — below that the radio never rests";
    return false;
  }
  if (hb != 0 && (hb < WifiLogger::kMinHeartbeatMs || hb > WifiLogger::kMaxHeartbeatMs)) {
    msg = String("upload: heartbeat must be 0 (off) or ") + WifiLogger::kMinHeartbeatMs +
          "-" + WifiLogger::kMaxHeartbeatMs + " ms";
    return false;
  }
  if (mi <= st) {
    msg = F("upload: the interval must exceed the settle time, or the gate never opens");
    return false;
  }
  WifiLogger::setUploadTiming(st, mi, hb);
  BleScanner::storeUpload(st, mi, hb);
  g_timingStored = true;
  msg = String("upload: settle ") + st + " ms, interval " + mi + " ms, heartbeat " +
        (hb ? String(hb) + " ms" : String("off"));
  if (hb == 0) {
    msg += "; WARNING with no heartbeat a door whose animal stays in goes silent "
           "and collects no commands";
  }
  return true;
}

// "off" | "<openPin> <closedPin> [low|high]". Either pin may be -1 for "that
// end has no switch", which is how you fit one before the other.
bool applySensorSpec(const char *args, String &msg) {
  char buf[64];
  strncpy(buf, args ? args : "", sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char *save = nullptr;
  char *tok = strtok_r(buf, " \t", &save);
  if (tok == nullptr) {
    msg = F("sensors needs: <open pin> <closed pin>, or 'off'");
    return false;
  }

  int openPin = -1, closedPin = -1;
  bool low = Position::activeLow();

  if (strcmp(tok, "off") == 0 || strcmp(tok, "none") == 0) {
    if (strtok_r(nullptr, " \t", &save) != nullptr) {
      msg = F("'sensors off' takes nothing else");
      return false;
    }
  } else {
    auto pin = [](const char *t, int &out) {
      char *end = nullptr;
      errno = 0;
      const long v = strtol(t, &end, 10);
      if (end == t || *end != '\0' || errno == ERANGE || v < -1 || v > 48) return false;
      out = static_cast<int>(v);
      return true;
    };
    const char *second = strtok_r(nullptr, " \t", &save);
    if (second == nullptr) {
      msg = F("sensors needs BOTH pins: <open> <closed>. Use -1 for an end with no switch");
      return false;
    }
    if (!pin(tok, openPin) || !pin(second, closedPin)) {
      msg = F("sensors: give GPIO numbers, or -1 for an end with no switch");
      return false;
    }
    for (char *t = strtok_r(nullptr, " \t", &save); t != nullptr;
         t = strtok_r(nullptr, " \t", &save)) {
      if (strcmp(t, "low") == 0)       low = true;
      else if (strcmp(t, "high") == 0) low = false;
      else {
        msg = String("sensors: unknown option '") + t + "' (low|high)";
        return false;
      }
    }
    if (openPin >= 0 && openPin == closedPin) {
      msg = F("sensors: the two switches cannot share one pin");
      return false;
    }
  }

  if (!Position::configure(openPin, closedPin, low)) {
    const char *a = Position::pinProblem(openPin);
    const char *b = Position::pinProblem(closedPin);
    msg = String("sensors rejected: ") + (a ? a : (b ? b : "unusable pin"));
    return false;
  }
  BleScanner::storeSensors(openPin, closedPin, low);

  if (!Position::enabled()) {
    msg = F("limit switches off — the door is open loop again");
    return true;
  }
  msg = String("sensors: open GPIO ") + Position::openPin() +
        ", closed GPIO " + Position::closedPin() +
        ", active " + (low ? "LOW" : "HIGH");
  return true;
}

// "off" | "<pin> [active|passive] [low|high]". Unspecified options keep their
// current value, so `buzzer 27` then `buzzer 27 passive` is a legal way to
// change your mind about one thing without restating the others.
bool applyBuzzerSpec(const char *args, String &msg) {
  char buf[64];
  strncpy(buf, args ? args : "", sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  // strtok_r, not strtok: the remote dispatcher is itself mid-strtok when it
  // calls this, and clobbering its state would eat the rest of the command.
  char *save = nullptr;
  char *tok = strtok_r(buf, " \t", &save);
  if (tok == nullptr) {
    msg = F("buzzer needs a GPIO number, or 'off'");
    return false;
  }

  int pin = -1;
  bool passive = Chime::passive();
  bool low = Chime::activeLow();

  if (strcmp(tok, "off") == 0 || strcmp(tok, "none") == 0) {
    pin = -1;
  } else {
    char *end = nullptr;
    errno = 0;
    const long v = strtol(tok, &end, 10);
    // Loose bound only — which numbers actually exist is the chip's business,
    // and Chime::pinProblem() below answers that per target.
    if (end == tok || *end != '\0' || errno == ERANGE || v < -1 || v > 48) {
      msg = F("buzzer: give a GPIO number, or 'off'");
      return false;
    }
    pin = static_cast<int>(v);
  }

  for (char *t = strtok_r(nullptr, " \t", &save); t != nullptr;
       t = strtok_r(nullptr, " \t", &save)) {
    if (strcmp(t, "passive") == 0)      passive = true;
    else if (strcmp(t, "active") == 0)  passive = false;
    else if (strcmp(t, "low") == 0)     low = true;
    else if (strcmp(t, "high") == 0)    low = false;
    else {
      msg = String("buzzer: unknown option '") + t + "' (passive|active|low|high)";
      return false;
    }
  }

  if (!Chime::configure(pin, passive, low)) {
    const char *why = Chime::pinProblem(pin);
    msg = String("buzzer rejected: ") + (why ? why : "unusable pin");
    return false;
  }
  BleScanner::storeChime(pin, passive, low);

  if (pin < 0) {
    msg = F("buzzer disabled");
    return true;
  }
  msg = String("buzzer GPIO ") + pin + ", " + (passive ? "passive" : "active") +
        ", active " + (low ? "LOW" : "HIGH");
  const char *warn = Chime::pinProblem(pin);
  if (warn != nullptr) {
    msg += " (";
    msg += warn;
    msg += ")";
  }
  return true;
}

// Shared by the `w` menu and the remote verb, like the buzzer and sensor specs
// beside it. It was remote-only until the console's own status line
// ("'w' then 'vibration <pin>' to add one") turned out to name a command the
// `w` menu did not implement — the one place a user is told to look was the one
// place it could not be set.
bool applyVibrationSpec(const char *args, String &msg) {
  if (args == nullptr) {
    msg = F("vibration needs a GPIO number, or 'off'");
    return false;
  }
  char buf[48];
  snprintf(buf, sizeof(buf), "%s", args);
  // strtok_r for the same reason as applyBuzzerSpec: the remote dispatcher is
  // itself mid-strtok when it calls this.
  char *save = nullptr;
  char *tok = strtok_r(buf, " \t", &save);
  if (tok == nullptr) {
    msg = F("vibration needs a GPIO number, or 'off'");
    return false;
  }
  if (strcmp(tok, "off") == 0 || strcmp(tok, "none") == 0) {
    Vibration::configure(-1, false);
    BleScanner::storeVibration(-1, false);
    msg = F("vibration sensor disabled");
    return true;
  }
  const int pin = atoi(tok);
  const char *problem = Vibration::pinProblem(pin);
  if (!Vibration::configure(pin, VIBRATION_ACTIVE_LOW != 0)) {
    msg = String("vibration rejected: ") + (problem != nullptr ? problem : "unusable pin");
    return false;
  }
  BleScanner::storeVibration(pin, VIBRATION_ACTIVE_LOW != 0);
  msg = String("vibration sensor on GPIO ") + pin;
  // A warning, not a refusal: configure() took the pin, so say what is odd
  // about it and let the edge count settle the argument.
  if (problem != nullptr) {
    msg += " (warning: ";
    msg += problem;
    msg += ")";
  }
  return true;
}

void printTimingMenu() {
  Con.println();
  Con.println(F("---- dwell / timing ----"));
  Con.printf("  open after   : %5lu ms near\r\n",
                static_cast<unsigned long>(g_tracker.enterConfirmMs()));
  Con.printf("  close after  : %5lu ms far\r\n",
                static_cast<unsigned long>(g_tracker.exitConfirmMs()));
  Con.printf("  min interval : %5lu ms between actuations (CLOSING only)\r\n",
                static_cast<unsigned long>(g_door.minIntervalMs()));
  Con.printf("  interlock gap: %5lu ms before any relay fires\r\n",
                static_cast<unsigned long>(g_door.directionGapMs()));
  Con.printf("  relay pulse  : %5lu ms held closed (the \"button press\")\r\n",
                static_cast<unsigned long>(g_door.pulseMs()));
  if (g_door.pulseCount() > 1) {
    Con.printf("  presses      : %u per actuation, %lu ms apart\r\n",
                  g_door.pulseCount(),
                  static_cast<unsigned long>(g_door.pulseGapMs()));
  }
  Con.printf("  calls in     : quiet %lu ms, min gap %lu ms, heartbeat %lu min\r\n",
                static_cast<unsigned long>(WifiLogger::settleMs()),
                static_cast<unsigned long>(WifiLogger::minIntervalMs()),
                static_cast<unsigned long>(WifiLogger::heartbeatMs() / 60000));
  Con.printf("  door travel  : open %5lu ms, close %5lu ms (0 = do not announce)\r\n",
                static_cast<unsigned long>(Actuator::travelMs(DOOR_OPEN)),
                static_cast<unsigned long>(Actuator::travelMs(DOOR_CLOSED)));
  Con.printf("  wake press   : %5lu ms of idle before one is sent (0 = never)\r\n",
                static_cast<unsigned long>(Actuator::wakeIdleMs()));
  Con.printf("  failed close : retry after %lu min, %u attempts, then stay open\r\n",
                static_cast<unsigned long>(Actuator::retryDelayMs() / 60000UL),
                Actuator::retryLimit());
  if (Chime::enabled()) {
    Con.printf("  buzzer       : GPIO %d, %s, active %s\r\n", Chime::pin(),
                  Chime::passive() ? "passive" : "active",
                  Chime::activeLow() ? "LOW" : "HIGH");
  } else {
    Con.println(F("  buzzer       : none"));
  }
  if (Position::enabled()) {
    Con.printf("  sensors      : open GPIO %d, closed GPIO %d, active %s — reads %s\r\n",
                  Position::openPin(), Position::closedPin(),
                  Position::activeLow() ? "LOW" : "HIGH",
                  Position::fault() ? "FAULT: both made"
                                    : DoorController::stateName(Position::state()));
  } else {
    Con.println(F("  sensors      : none (open loop)"));
  }
  Con.printf("  source       : %s\r\n", g_timingStored ? "saved on device" : "compiled in");
  Con.println();
  Con.printf("  MEASURED worst gap between samples: %lu ms\r\n",
                static_cast<unsigned long>(g_tracker.maxGapMs()));
  Con.println(F("  A close dwell at or below that WILL close on a routine"));
  Con.println(F("  radio dropout, with the beacon still present."));
  Con.println(F("  type one of:"));
  Con.println(F("    1000,3000,2000   open ms, close ms, min interval ms"));
  Con.println(F("    fast             1000 / 3000 / 2000  (responsive)"));
  Con.println(F("    safe             1500 / 15000 / 5000 (the default)"));
  Con.println(F("    gap 250          relay interlock dead time, ms (min 100)"));
  Con.println(F("    pulse 200        how long the relay stays closed, ms (50-10000)"));
  Con.println(F("    presses 2 1000   press N times per actuation, ms apart (N 1-3)"));
  Con.println(F("      for a controller that sometimes swallows a press. Blind retry:"));
  Con.println(F("      if the first press DID take, the second may stop it mid-travel"));
  Con.println(F("      raise this if the relay clicks but the door does not move:"));
  Con.println(F("      many controllers debounce their button and ignore a short tap"));
  Con.println(F("    travel 12200 12700   how long YOUR door takes: <open> <close> ms"));
  Con.println(F("      one value sets both. With limit switches this is the deadline"));
  Con.println(F("      for arrival, and a travel that misses it is a STALL. Without"));
  Con.println(F("      them it is only a stopwatch, and will chime at a stuck door"));
  Con.println(F("    calibrate        time both directions and adopt the result"));
  Con.println(F("      needs both switches and a maintenance window. Starts with a"));
  Con.println(F("      quiet period: if the door moves on its own, it says so"));
  Con.println(F("    wake 30000       idle time after which a wake press is sent, ms"));
  Con.println(F("      a sleeping controller swallows the first press. 0 = off"));
  Con.println(F("    retry 5 3        after a stalled CLOSE: wait <min>, <n> attempts"));
  Con.println(F("      then stay open and say so. A stalled close always fails OPEN"));
  Con.println(F("    buzzer 27        which GPIO the buzzer is on ('buzzer off' = none)"));
  Con.println(F("    buzzer 27 passive      a bare transducer that needs a tone, not DC"));
  Con.println(F("    buzzer 27 active low   one that sounds when pulled to GND"));
  Con.println(F("    beep             three beeps now — find the pin by trying it"));
  Con.println(F("    led              three flashes now — the same trick for the LED"));
  Con.println(F("    upload 60000 300000 1800000   how often the door calls in:"));
  Con.println(F("      <quiet before uploading> <minimum gap> <heartbeat>, all ms."));
  Con.println(F("      Lower the middle one for faster commands, at the cost of"));
  Con.println(F("      radio time the BLE scan would otherwise have. 0 heartbeat = off"));
  Con.println(F("    sensors 32 25    limit switch pins: <open> <closed> ('sensors off')"));
  Con.println(F("    sensors 32 25 low   same, for switches that pull the pin to GND"));
  Con.println(F("    vibration 33     which GPIO the vibration sensor's DO is on"));
  Con.println(F("      answers 'did it START moving', seconds before a limit"));
  Con.println(F("      switch can answer 'did it arrive' ('vibration off' = none)"));
  Con.println(F("      the door stops guessing where it is. Without them it only"));
  Con.println(F("      knows what it COMMANDED, which is why the chime is a timer"));
  Con.println(F("    clear            forget saved values"));
  Con.println(F("    q                cancel"));
  Con.println(F("  (live output is paused while this menu is open)"));
  Con.print(F("> "));
}

void applyTiming(uint32_t enterMs, uint32_t exitMs, uint32_t lockoutMs) {
  if (enterMs == 0 || exitMs == 0 || lockoutMs == 0) {
    Con.println(F("\r\n[dwell] all three must be greater than zero."));
    printTimingMenu();
    return;
  }
  if (lockoutMs >= exitMs) {
    Con.printf("\r\n[dwell] rejected: min interval (%lu) must be BELOW the close "
                  "dwell (%lu),\r\n",
                  static_cast<unsigned long>(lockoutMs), static_cast<unsigned long>(exitMs));
    Con.println(F("[dwell] otherwise the actuation lockout delays closing."));
    printTimingMenu();
    return;
  }

  g_tracker.setDwell(enterMs, exitMs);
  g_door.setMinIntervalMs(lockoutMs);
  BleScanner::storeTiming(enterMs, exitMs, lockoutMs);
  g_timingStored = true;

  Con.printf("\r\n[dwell] saved: open after %lu ms, close after %lu ms, "
                "min interval %lu ms\r\n",
                static_cast<unsigned long>(enterMs), static_cast<unsigned long>(exitMs),
                static_cast<unsigned long>(lockoutMs));

  const uint32_t gap = g_tracker.maxGapMs();
  if (gap && exitMs <= gap) {
    Con.printf("[dwell] !! WARNING: close dwell (%lu ms) is at or below the worst\r\n",
                  static_cast<unsigned long>(exitMs));
    Con.printf("[dwell]    measured gap (%lu ms). A routine radio dropout will now\r\n",
                  static_cast<unsigned long>(gap));
    Con.println(F("[dwell]    close the door with the beacon still present."));
  }
  Con.println(F("[dwell] active immediately; no restart needed."));
  g_entry = ENTRY_NONE;
}

void processTimingLine(char *line) {
  while (*line == ' ' || *line == '\t') line++;
  int n = strlen(line);
  while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';

  if (n == 0 || strcmp(line, "q") == 0) {
    Con.println(F("\r\n[dwell] cancelled, nothing changed."));
    g_entry = ENTRY_NONE;
    return;
  }
  if (strcmp(line, "clear") == 0) {
    BleScanner::clearStoredTiming();
    g_tracker.setDwell(ENTER_CONFIRM_MS, EXIT_CONFIRM_MS);
    g_door.setMinIntervalMs(MIN_ACTUATION_INTERVAL_MS);
    g_door.setDirectionGapMs(DIRECTION_CHANGE_GAP_MS);
    g_door.setPulseMs(RELAY_PULSE_MS);
    g_door.setPulseTrain(RELAY_PULSE_COUNT, RELAY_PULSE_GAP_MS);
    Actuator::setTravelMs(DOOR_TRAVEL_OPEN_MS, DOOR_TRAVEL_CLOSE_MS);
    BleScanner::storeTravelPair(DOOR_TRAVEL_OPEN_MS, DOOR_TRAVEL_CLOSE_MS);
    Actuator::setWakeIdleMs(WAKE_IDLE_MS);
    Actuator::setRetryPolicy(CLOSE_RETRY_DELAY_MS, CLOSE_RETRY_LIMIT);
    BleScanner::storeActuation(WAKE_IDLE_MS, CLOSE_RETRY_DELAY_MS, CLOSE_RETRY_LIMIT);
    g_timingStored = false;
    Con.println(F("\r\n[dwell] reverted to the compiled-in defaults."));
    Con.println(F("[dwell] the buzzer pin is kept — that is wiring, not tuning."));
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "gap ", 4) == 0) {
    uint32_t ms = 0;
    if (!parseBoundedMs(line + 4, DoorController::kMinDirectionGapMs,
                        DoorController::kMaxDirectionGapMs, ms) ||
        !g_door.setDirectionGapMs(ms)) {
      Con.printf("\r\n[dwell] rejected: interlock gap must be %lu-%lu ms.\r\n",
                    static_cast<unsigned long>(DoorController::kMinDirectionGapMs),
                    static_cast<unsigned long>(DoorController::kMaxDirectionGapMs));
      Con.println(F("[dwell] both relays energised at once shorts the motor."));
      printTimingMenu();
      return;
    }
    BleScanner::storeTiming(g_tracker.enterConfirmMs(), g_tracker.exitConfirmMs(),
                            g_door.minIntervalMs());
    BleScanner::storeDirectionGap(ms);
    g_timingStored = true;
    Con.printf("\r\n[dwell] interlock gap now %lu ms (saved on device).\r\n",
                  static_cast<unsigned long>(ms));
    g_entry = ENTRY_NONE;
    return;
  }
  if (strcmp(line, "beep") == 0) {
    if (!Chime::enabled()) {
      Con.println(F("\r\n[dwell] no buzzer configured. 'buzzer <pin>' first."));
      printTimingMenu();
      return;
    }
    Chime::play(CHIME_TEST);
    Con.printf("\r\n[dwell] three beeps on GPIO %d. Silence means the wrong pin,\r\n",
                  Chime::pin());
    Con.println(F("[dwell] or the wrong kind: try 'buzzer <pin> passive', then this again."));
    g_entry = ENTRY_NONE;
    return;
  }
  if (strcmp(line, "led") == 0) {
    startLedTest(millis());
    Con.printf("\r\n[dwell] three flashes on GPIO %d. Nothing at all means the\r\n",
                  PIN_STATUS_LED);
    Con.println(F("[dwell] wrong pin; lit solid and then dark means it is reversed."));
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "travel", 6) == 0 && (line[6] == ' ' || line[6] == '\0')) {
    String msg;
    if (!applyTravelMs(line[6] ? line + 7 : nullptr, msg)) {
      Con.printf("\r\n[dwell] %s\r\n", msg.c_str());
      printTimingMenu();
      return;
    }
    Con.printf("\r\n[dwell] %s (saved on device).\r\n", msg.c_str());
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "upload", 6) == 0 && (line[6] == ' ' || line[6] == '\0')) {
    String msg;
    if (!applyUploadTiming(line[6] ? line + 7 : nullptr, msg)) {
      Con.printf("\r\n[dwell] %s\r\n", msg.c_str());
      printTimingMenu();
      return;
    }
    Con.printf("\r\n[dwell] %s (saved on device).\r\n", msg.c_str());
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "sensors", 7) == 0 && (line[7] == ' ' || line[7] == '\0')) {
    String msg;
    if (!applySensorSpec(line[7] ? line + 8 : nullptr, msg)) {
      Con.printf("\r\n[dwell] %s\r\n", msg.c_str());
      printTimingMenu();
      return;
    }
    Con.printf("\r\n[dwell] %s (saved on device).\r\n", msg.c_str());
    if (Position::enabled()) {
      Con.printf("[dwell] reading right now: %s\r\n",
                    DoorController::stateName(Position::state()));
      Con.println(F("[dwell] move the door by hand and press 's' to watch it change."));
    }
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "vibration", 9) == 0 && (line[9] == ' ' || line[9] == '\0')) {
    String msg;
    if (!applyVibrationSpec(line[9] ? line + 10 : nullptr, msg)) {
      Con.printf("\r\n[dwell] %s\r\n", msg.c_str());
      printTimingMenu();
      return;
    }
    Con.printf("\r\n[dwell] %s.\r\n", msg.c_str());
    Con.println(F("[dwell] tap the sensor, then press 's' — the edge count must climb."));
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "buzzer", 6) == 0 && (line[6] == ' ' || line[6] == '\0')) {
    String msg;
    if (!applyBuzzerSpec(line[6] ? line + 7 : nullptr, msg)) {
      Con.printf("\r\n[dwell] %s\r\n", msg.c_str());
      printTimingMenu();
      return;
    }
    Con.printf("\r\n[dwell] %s (saved on device).\r\n", msg.c_str());
    if (Chime::enabled()) {
      Chime::play(CHIME_TEST);
      Con.println(F("[dwell] beeping now — if you hear nothing, it is the wrong pin."));
    }
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "presses ", 8) == 0) {
    int n = 0; unsigned long g = 0;
    if (sscanf(line + 8, "%d %lu", &n, &g) < 1) {
      Con.println(F("\r\n[dwell] give: presses <count> [gap ms], e.g. presses 2 1000"));
      printTimingMenu();
      return;
    }
    if (g == 0) g = g_door.pulseGapMs();
    if (n < 1 || n > DoorController::kMaxPulseCount ||
        !g_door.setPulseTrain(static_cast<uint8_t>(n), g)) {
      Con.printf("\r\n[dwell] rejected: count 1-%u, gap %lu-%lu ms.\r\n",
                    DoorController::kMaxPulseCount,
                    static_cast<unsigned long>(DoorController::kMinPulseGapMs),
                    static_cast<unsigned long>(DoorController::kMaxPulseGapMs));
      printTimingMenu();
      return;
    }
    BleScanner::storePulseTrain(g_door.pulseCount(), g_door.pulseGapMs());
    g_timingStored = true;
    Con.printf("\r\n[dwell] %u press(es) per actuation, %lu ms apart (saved).\r\n",
                  g_door.pulseCount(), static_cast<unsigned long>(g_door.pulseGapMs()));
    if (g_door.pulseCount() > 1) {
      Con.println(F("[dwell] blind retry: the door cannot tell whether the first"));
      Con.println(F("[dwell] press worked. Watch a dozen cycles before trusting it."));
    }
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "pulse ", 6) == 0) {
    uint32_t ms = 0;
    if (!parseBoundedMs(line + 6, DoorController::kMinPulseMs, DoorController::kMaxPulseMs,
                        ms) ||
        !g_door.setPulseMs(ms)) {
      Con.printf("\r\n[dwell] rejected: relay pulse must be %lu-%lu ms.\r\n",
                    static_cast<unsigned long>(DoorController::kMinPulseMs),
                    static_cast<unsigned long>(DoorController::kMaxPulseMs));
      printTimingMenu();
      return;
    }
    BleScanner::storePulseMs(ms);
    g_timingStored = true;
    Con.printf("\r\n[dwell] relay pulse now %lu ms (saved on device).\r\n",
                  static_cast<unsigned long>(ms));
    Con.println(F("[dwell] test it with 'o' and 'x' before trusting it to the door."));
    if (ms >= 2000) {
      Con.printf("[dwell] note: the control task is blocked for the whole %lu ms — no\r\n",
                    static_cast<unsigned long>(ms));
      Con.println(F("[dwell] samples drained and no reversing mid-travel. See WIRING.md."));
    }
    g_entry = ENTRY_NONE;
    return;
  }
  if (strcmp(line, "fast") == 0) {
    applyTiming(1000, 3000, 2000);
    return;
  }
  if (strcmp(line, "safe") == 0) {
    applyTiming(ENTER_CONFIRM_MS, EXIT_CONFIRM_MS, MIN_ACTUATION_INTERVAL_MS);
    return;
  }

  const char *c1 = strchr(line, ',');
  if (c1 == nullptr) {
    Con.println(F("\r\n[dwell] give three values, e.g. 1000,3000,2000"));
    printTimingMenu();
    return;
  }
  const char *c2 = strchr(c1 + 1, ',');
  if (c2 == nullptr) {
    Con.println(F("\r\n[dwell] give three values, e.g. 1000,3000,2000"));
    printTimingMenu();
    return;
  }
  // One hour is far beyond anything sensible and still leaves no room for a
  // sign-flipped value to become a ~49-day dwell.
  constexpr uint32_t kMaxDwellMs = 3600000;
  uint32_t en = 0, ex = 0, lk = 0;
  if (!parseBoundedMs(line, 1, kMaxDwellMs, en) ||
      !parseBoundedMs(c1 + 1, 1, kMaxDwellMs, ex) ||
      !parseBoundedMs(c2 + 1, 1, kMaxDwellMs, lk)) {
    Con.printf("\r\n[dwell] rejected: each value must be 1-%lu ms.\r\n",
                  static_cast<unsigned long>(kMaxDwellMs));
    printTimingMenu();
    return;
  }
  applyTiming(en, ex, lk);
}

void printFilterMenu() {
  Con.println();
  Con.println(F("---- filter shape (how fast RSSI is tracked) ----"));
  Con.println(F("  two filters run over the same samples:"));
  Con.printf("  CLOSE window  : %u samples (max %u)\r\n", g_tracker.windowSize(),
                kMaxMedianWindow);
  Con.printf("  CLOSE alpha   : %s  (higher = faster, noisier)\r\n",
                String(g_tracker.alpha(), 2).c_str());
  Con.printf("  OPEN  window  : %u samples\r\n", g_tracker.fastWindowSize());
  Con.printf("  OPEN  alpha   : %s\r\n", String(g_tracker.fastAlpha(), 2).c_str());
  Con.printf("  source        : %s / %s\r\n",
                g_filterStored ? "saved" : "compiled",
                g_fastFilterStored ? "saved" : "compiled");

  // Show what this actually costs in time, using the measured sample rate.
  const uint32_t gap = g_tracker.maxGapMs();
  Con.printf("  worst sample gap measured: %lu ms\r\n", static_cast<unsigned long>(gap));
  Con.println(F("  lag is roughly (window/2 + 1/alpha) x the sample interval."));
  Con.println(F("  the CLOSE pair no longer costs open latency, so prefer a"));
  Con.println(F("  LONG window here and tune the OPEN pair for speed."));
  Con.println(F("  type one of:"));
  Con.println(F("    fast        window 3, alpha 0.7  (twitchy close; rarely needed now)"));
  Con.println(F("    default     window 7, alpha 0.35 (the shipped shape)"));
  Con.println(F("    smooth      window 11, alpha 0.2 (noisy RF)"));
  Con.println(F("    3,0.7       window,alpha directly (window odd, 1..15)"));
  Con.println(F("    open 1,0.9  set the OPEN pair (window odd, 1..15)"));
  Con.println(F("    open same   make OPEN match CLOSE (old single-filter behaviour)"));
  Con.println(F("    clear       forget saved values (both pairs)"));
  Con.println(F("    q           cancel"));
  Con.println(F("  (live output is paused while this menu is open)"));
  Con.print(F("> "));
}

void applyFilter(int win, float alpha) {
  // setFilter() may drag the fast pair along to keep the open path from
  // becoming the slower of the two. Note what it was so the change can be
  // reported and persisted rather than silently forgotten at the next boot.
  const uint8_t prevFastWin = g_tracker.fastWindowSize();
  const float prevFastAlpha = g_tracker.fastAlpha();

  // Check the int before narrowing: 257 truncates to 1, which setFilter would
  // accept as a valid odd window — silently disabling the filter while the
  // "near-unfiltered" warning below tested the untruncated value and stayed
  // quiet.
  if (win < 1 || win > kMaxMedianWindow ||
      !g_tracker.setFilter(static_cast<uint8_t>(win), alpha)) {
    Con.printf("\r\n[filt] rejected: window must be ODD and 1..%u, alpha 0<a<=1\r\n",
                  kMaxMedianWindow);
    printFilterMenu();
    return;
  }
  BleScanner::storeFilter(static_cast<uint8_t>(win), alpha);
  g_filterStored = true;
  // Report what is actually in force, not what was typed.
  Con.printf("\r\n[filt] saved: window %u, alpha %s\r\n", g_tracker.windowSize(),
                String(g_tracker.alpha(), 2).c_str());
  if (g_tracker.windowSize() <= 1 || g_tracker.alpha() >= 0.99f) {
    Con.println(F("[filt] !! near-unfiltered. A single RSSI spike can now move"));
    Con.println(F("[filt]    the door. This is the failure the project fixes."));
  }
  if (g_tracker.fastWindowSize() != prevFastWin || g_tracker.fastAlpha() != prevFastAlpha) {
    // Persist it, or the clamp would be undone by the stored value at the next
    // boot and the invariant would hold only until a power cut.
    BleScanner::storeFastFilter(g_tracker.fastWindowSize(), g_tracker.fastAlpha());
    g_fastFilterStored = true;
    Con.printf("[filt] OPEN pair pulled to %u, %s to stay ahead of CLOSE.\r\n",
                  g_tracker.fastWindowSize(), String(g_tracker.fastAlpha(), 2).c_str());
  }
  Con.println(F("[filt] filter reset; it will re-acquire in a second or two."));
  g_entry = ENTRY_NONE;
}

void applyFastFilter(int win, float alpha) {
  // setFastFilter() changes nothing when it refuses, so there is no rollback to
  // do here — it rejects both a malformed pair and one that would make the open
  // path slower than the close path.
  //
  // Same narrowing guard as applyFilter(): check the int before the cast, or
  // 257 would truncate to a valid-looking 1.
  if (win < 1 || win > kMaxMedianWindow ||
      !g_tracker.setFastFilter(static_cast<uint8_t>(win), alpha)) {
    Con.printf("\r\n[filt] rejected: window must be ODD and 1..%u, alpha 0<a<=1,\r\n",
                  kMaxMedianWindow);
    Con.printf("[filt] and must not be slower than CLOSE (window <= %u, alpha >= %s).\r\n",
                  g_tracker.windowSize(), String(g_tracker.alpha(), 2).c_str());
    printFilterMenu();
    return;
  }
  BleScanner::storeFastFilter(static_cast<uint8_t>(win), alpha);
  g_fastFilterStored = true;
  Con.printf("\r\n[filt] saved: OPEN window %u, alpha %s\r\n", g_tracker.fastWindowSize(),
                String(g_tracker.fastAlpha(), 2).c_str());
  Con.println(F("[filt] the close path is unchanged; only open latency moved."));
  g_entry = ENTRY_NONE;
}

void processFilterLine(char *line) {
  while (*line == ' ' || *line == '\t') line++;
  int n = strlen(line);
  while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';

  if (n == 0 || strcmp(line, "q") == 0) {
    Con.println(F("\r\n[filt] cancelled, nothing changed."));
    g_entry = ENTRY_NONE;
    return;
  }
  if (strcmp(line, "clear") == 0) {
    BleScanner::clearStoredFilter();
    BleScanner::clearStoredFastFilter();
    g_tracker.setFilter(RSSI_MEDIAN_WINDOW, RSSI_EWMA_ALPHA);
    g_tracker.setFastFilter(RSSI_FAST_WINDOW, RSSI_FAST_ALPHA);
    g_filterStored = false;
    g_fastFilterStored = false;
    Con.println(F("\r\n[filt] reverted to the compiled-in defaults (both pairs)."));
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "open", 4) == 0 && (line[4] == ' ' || line[4] == '\0')) {
    const char *arg = line + 4;
    while (*arg == ' ' || *arg == '\t') arg++;
    if (strcmp(arg, "same") == 0) {
      // Collapse back to one filter: both decisions read the same numbers,
      // which is exactly how the tracker behaved before the split.
      applyFastFilter(g_tracker.windowSize(), g_tracker.alpha());
      return;
    }
    const char *c = strchr(arg, ',');
    if (c == nullptr) {
      Con.println(F("\r\n[filt] give: open window,alpha e.g. open 1,0.9"));
      printFilterMenu();
      return;
    }
    applyFastFilter(atoi(arg), atof(c + 1));
    return;
  }
  if (strcmp(line, "fast") == 0) { applyFilter(3, 0.7f); return; }
  if (strcmp(line, "default") == 0) { applyFilter(RSSI_MEDIAN_WINDOW, RSSI_EWMA_ALPHA); return; }
  if (strcmp(line, "smooth") == 0) { applyFilter(11, 0.2f); return; }

  const char *comma = strchr(line, ',');
  if (comma == nullptr) {
    Con.println(F("\r\n[filt] give window,alpha e.g. 3,0.7"));
    printFilterMenu();
    return;
  }
  applyFilter(atoi(line), static_cast<float>(atof(comma + 1)));
}

void handleMacEntryChar(int c) {
  if (c == '\r' || c == '\n') {
    if (g_macLineLen == 0) {
      // A bare Enter (or the \n of a \r\n pair) cancels rather than submits.
      Con.println(F("\r\n[mac] cancelled, nothing changed."));
      g_entry = ENTRY_NONE;
      return;
    }
    g_macLine[g_macLineLen] = '\0';
    g_macLineLen = 0;
    if (g_entry == ENTRY_THRESH) {
      processThresholdLine(g_macLine);
    } else if (g_entry == ENTRY_TIMING) {
      processTimingLine(g_macLine);
    } else if (g_entry == ENTRY_FILTER) {
      processFilterLine(g_macLine);
    } else if (g_entry == ENTRY_SCHEDULE) {
      String msg;
      applyScheduleLine(g_macLine, msg);
      Con.printf("[sched] %s\r\n", msg.c_str());
      if (strcasecmp(g_macLine, "q") == 0) {
        g_entry = ENTRY_NONE;
        Con.println(F("[sched] done."));
      } else {
        printScheduleMenu();
      }
    } else {
      processMacLine(g_macLine);
    }
    return;
  }

  if (c == 0x08 || c == 0x7F) {  // backspace / delete
    if (g_macLineLen > 0) {
      g_macLineLen--;
      Con.print(F("\b \b"));
    }
    return;
  }

  if (c >= 0x20 && g_macLineLen < sizeof(g_macLine) - 1) {
    g_macLine[g_macLineLen++] = static_cast<char>(c);
    Con.write(static_cast<char>(c));  // echo, so typing is visible
  }
}

void handleSerial(uint32_t nowMs) {
  while (Con.available() > 0) {
    const int c = Con.read();

    if (g_entry != ENTRY_NONE) {
      handleMacEntryChar(c);
      continue;
    }

    if (g_downloadModeArmed) {
      g_downloadModeArmed = false;
      if (c == 'y' || c == 'Y') {
        Con.println(F("y"));
        rebootToDownloadMode();
      } else {
        Con.println(F("\r\n[sys] cancelled."));
      }
      continue;
    }

    switch (c) {
      case 'h':
      case '?':
        printHelp();
        break;
      case 's':
        printStatus(nowMs);
        break;
      case 'd':
        BleScanner::setDiscoverEnabled(!BleScanner::discoverEnabled());
        Con.printf("[cmd] discovery %s\r\n", BleScanner::discoverEnabled() ? "ON" : "OFF");
        break;
      case 'c':
        g_calibrate = !g_calibrate;
        Con.printf("[cmd] calibration stream %s\r\n", g_calibrate ? "ON" : "OFF");
        if (g_calibrate) {
          Con.print(F("  tracking: "));
          Con.println(BleScanner::describeTarget());
          Con.println(F("  Walk to where the door should open and note the filtered value."));
          Con.println(F("  Set RSSI_ENTER_DBM a little below it, RSSI_EXIT_DBM 8-12 dBm lower."));
        }
        break;
      case 'r':
        g_tracker.reset();
        // Also start the calibration distribution over, so the workflow at a
        // mounted door is: stand at a position, 'r', wait, 's'. Without this
        // the previous position's readings stay mixed into the percentiles and
        // quietly widen every band.
        if (Maintenance::active(nowMs)) {
          Maintenance::resetStats();
          Con.println(F("[cmd] calibration distribution cleared for this position"));
        }
        Con.println(F("[cmd] proximity filter reset"));
        Con.println(F("[cmd] range stats cleared — walk the beacon out and"));
        Con.println(F("[cmd] back, then press 's' to see weakest RSSI and"));
        Con.println(F("[cmd] worst sample gap."));
        break;
      case 'l':
        EventLog::dump(Serial);
        break;
      case 'u':
        WifiLogger::requestFlushNow();
        break;
      case 'p':
        WifiLogger::beginOtaWindow(g_tracker.isPresent());
        break;
      case 'P':
        WifiLogger::closeOtaWindow();
        break;
      case 'L':
        EventLog::dumpCsv(Serial);
        break;
      case 'm':
        g_entry = ENTRY_MAC;
        g_macLineLen = 0;
        printMacMenu();
        break;
      case 't':
        g_entry = ENTRY_THRESH;
        g_macLineLen = 0;
        printThresholdMenu();
        break;
      case 'w':
        g_entry = ENTRY_TIMING;
        g_macLineLen = 0;
        printTimingMenu();
        break;
      case 'f':
        g_entry = ENTRY_FILTER;
        g_macLineLen = 0;
        printFilterMenu();
        break;
      case '!':
#if !PETDOOR_CAN_REBOOT_TO_FLASH
        // Nothing to confirm — just explain and stay put.
        rebootToDownloadMode();
        break;
#endif
        g_downloadModeArmed = true;
        g_calibrate = false;  // otherwise [cal] lines scroll the prompt away
        Con.println(F("[sys] reboot into flash mode?"));
        Con.println(F("[sys] the door will NOT be controlled until you flash."));
        Con.print(F("[sys] press 'y' to confirm, anything else cancels: "));
        break;
#if ALLOW_MANUAL_SERIAL_CONTROL
      case 'o': {
        Con.println(F("[cmd] forcing OPEN"));
        const ActuationResult r =
            Actuator::request(DOOR_OPEN, SRC_MANUAL, nowMs, /*force=*/true);
        if (r != ACT_DONE) {
          Con.printf("[cmd] refused: %s\r\n", DoorController::resultName(r));
          Chime::play(CHIME_REFUSED);
          break;
        }
        announceMovement(SRC_MANUAL, DOOR_OPEN);
        if (MANUAL_HOLD_MS > 0) {
          g_manualHoldUntilMs = millis() + MANUAL_HOLD_MS;
          if (g_manualHoldUntilMs == 0) g_manualHoldUntilMs = 1;  // 0 means "off"
          Con.printf("[cmd] holding open for %lu s — automatic closing is paused.\r\n",
                        static_cast<unsigned long>(MANUAL_HOLD_MS / 1000));
          Con.println(F("[cmd] 'x' closes now, 'O' hands control back immediately."));
        }
        break;
      }
      case 'x': {
        Con.println(F("[cmd] forcing CLOSE"));
        // Clearing the hold here is what makes `x` mean "I am done" rather than
        // "close it, and then let the hold quietly keep it from closing again".
        g_manualHoldUntilMs = 0;
        // Forced, so this also clears a gave-up state: somebody is standing at
        // the door asking for a close, and they can see what the firmware
        // cannot. They get the full attempt budget back.
        const ActuationResult r =
            Actuator::request(DOOR_CLOSED, SRC_MANUAL, nowMs, /*force=*/true);
        if (r != ACT_DONE) {
          Con.printf("[cmd] refused: %s\r\n", DoorController::resultName(r));
          if (r == ACT_BOOT_GRACE) {
            Con.println(F("[cmd] the boot grace is deliberate and cannot be forced:"));
            Con.println(F("[cmd] a door that just rebooted does not know whether"));
            Con.println(F("[cmd] something is standing in it."));
          }
          Chime::play(CHIME_REFUSED);
          break;
        }
        announceMovement(SRC_MANUAL, DOOR_CLOSED);
        break;
      }
      case 'C': {
        String msg;
        const bool ok = Actuator::startCalibration(nowMs, Maintenance::active(nowMs), msg);
        Con.printf("[cal] %s\r\n", msg.c_str());
        if (!ok) Chime::play(CHIME_REFUSED);
        break;
      }
      case 'n':
        g_entry = ENTRY_SCHEDULE;
        g_macLineLen = 0;
        printScheduleMenu();
        break;
      case 'M': {
        String msg;
        if (Maintenance::active(nowMs)) {
          endMaintenance(msg);
        } else {
          // No argument from a single keypress, so the default window. The
          // remote `maint <minutes>` form takes one.
          beginMaintenance(nowMs, MAINT_DEFAULT_MS, msg);
        }
        Con.printf("[cmd] %s\r\n", msg.c_str());
        break;
      }
      case 'k':
        g_locked = true;
        BleScanner::storeLock(true);
        Con.println(F("[cmd] LOCKED — the beacon can no longer open this door"));
        Con.println(F("[cmd] 'o' still opens it by hand; 'K' unlocks."));
        break;
      case 'K':
        g_locked = false;
        BleScanner::storeLock(false);
        Con.println(F("[cmd] unlocked — the beacon controls the door again"));
        break;
      case 'O':
        if (g_manualHoldUntilMs != 0) {
          g_manualHoldUntilMs = 0;
          Con.println(F("[cmd] manual hold cleared; automatic control resumed."));
        } else {
          Con.println(F("[cmd] no manual hold was active."));
        }
        break;
#endif
      default:
        break;  // ignore newlines and anything else
    }
  }
}

// One LED, several states, in priority order — most urgent wins.
//
//   5 Hz flutter   radio is unhealthy (no adverts from any device)
//   3 quick blips  GAVE UP closing — the door is staying open on purpose
//   2 Hz flash     beacon battery is low
//   1 Hz blink     no beacon configured, or never heard since boot
//   near-solid     the door is travelling
//   solid          door OPEN
//   brief blip     door CLOSED (a double blip if it is also locked)
//   off            position unknown
//
// With limit switches fitted the last three show the MEASURED position rather
// than the commanded one, which is the whole point of fitting them: the LED
// stops reporting what the door was told and starts reporting where it is. A
// door stopped partway then reads as "unknown" instead of as whichever end it
// was last sent to.
//
// The gave-up blips sit above the battery flash deliberately. A flat beacon
// battery is a job for the weekend; a door that has stopped closing is
// tonight's.
//
// Door state is shown here rather than by pulsing a relay: a relay's indicator
// is driven by its coil, so "flashing" one would energise the motor and
// physically move the door. See docs/SAFETY.md.
bool beaconBatteryLow(uint32_t nowMs) {
#if BEACON_LOW_BATTERY_MV > 0
  EddystoneTlm tlm;
  uint32_t age = 0;
  if (!BleScanner::targetTelemetry(tlm, age, nowMs)) return false;
  if (tlm.batteryMv == 0) return false;  // mains powered, not a fault
  return tlm.batteryMv <= BEACON_LOW_BATTERY_MV;
#else
  (void)nowMs;
  return false;
#endif
}

// Drives the annunciator from the travel stopwatch: the working pattern for as
// long as the door is believed to be moving, the done chime on the edge where
// that expires. Edge-triggered, so a silent buzzer costs one comparison a tick.
// Read the limit switches, and let reality correct what the door believes.
//
// This never actuates. It only ever changes the controller's idea of where the
// door is, which matters because "already in that state" is how an actuation
// request decides to do nothing: a stale belief leaves the door refusing to
// correct itself after a swallowed press or a shove by hand.
void updatePosition(uint32_t nowMs) {
  if (!Position::enabled()) return;
  Position::tick(nowMs);

  if (Position::fault()) {
    if (!g_sensorFaultAnnounced) {
      g_sensorFaultAnnounced = true;
      Con.println(F("[pos] !! BOTH limit switches are made at once."));
      Con.println(F("[pos] !! That cannot happen on a working door — suspect a"));
      Con.println(F("[pos] !! shorted wire, a stuck switch, or a stray magnet."));
      Con.println(F("[pos] !! Position is being ignored until it clears."));
    }
    return;
  }
  g_sensorFaultAnnounced = false;

  const DoorState seen = Position::state();
  if (seen == g_lastObserved) return;
  const DoorState previously = g_lastObserved;
  g_lastObserved = seen;
  if (seen == DOOR_UNKNOWN) return;   // the normal reading between the switches

  // Arrival during a travel is the actuator's business — it is watching for
  // exactly this and will resolve the attempt itself. Nothing to do here.
  if (Actuator::busy()) return;

  // THE DOOR MOVED AND NOTHING COMMANDED IT.
  //
  // This is not a hypothetical. The vendor controller has modes of its own and
  // was watched leaving the open limit about fifteen seconds after arriving,
  // twice, with nothing driving it. A hand on the door and a gust of wind do
  // it too. None of it was visible to this firmware before the switches were
  // fitted, which is why a door that "randomly" changed state was unfalsifiable.
  //
  // `previously == DOOR_UNKNOWN` excludes the honest case: the first reading
  // after boot, where the door is not moving, we simply had not looked yet.
  const bool uncommanded = previously != DOOR_UNKNOWN;

  if (g_door.observePosition(seen)) {
    if (uncommanded) {
      Con.printf("[pos] !! the door moved to %s and NOTHING commanded it.\r\n",
                    DoorController::stateName(seen));
      Con.println(F("[pos] !! A hand, the wind, or a mode on the door's own"));
      Con.println(F("[pos] !! controller. If this repeats, that controller still"));
      Con.println(F("[pos] !! has an automatic mode enabled — see docs/COOP-CONVERSION.md."));
      EventLog::record(LOG_UNCOMMANDED, static_cast<uint8_t>(seen),
                       g_tracker.filteredRssi());
      Chime::play(CHIME_UNCOMMANDED);
#if PETDOOR_ENABLE_WIFI
      // Worth a radio burst of its own. A door moving by itself is the kind of
      // thing you want to find in the dashboard the same evening, not at the
      // next heartbeat half an hour later.
      publishStatusLines();
      WifiLogger::requestFlushNow();
#endif
    } else {
      Con.printf("[pos] switch says %s — correcting what the door believed\r\n",
                    DoorController::stateName(seen));
    }
  }

  // Reality says the door is shut, so whatever made the firmware give up on
  // closing it is no longer true. Clearing this from an OBSERVATION rather than
  // from a command is the one case where a switch is allowed to change policy
  // — and it only ever relaxes a refusal, never causes a movement.
  if (seen == DOOR_CLOSED && Actuator::gaveUp()) {
    Actuator::clearGaveUp();
    Con.println(F("[pos] the door is closed after all — resuming normal control"));
  }
}

// Rebuild the two lines the next upload carries: what the door can see, and
// what it is set to.
//
// Pulled out of the control loop's five-second timer so it can also be called
// the moment a remote command changes something. Otherwise the upload that
// follows a command carries a snapshot taken BEFORE it was applied, and the
// dashboard shows the old value while insisting the command succeeded.
void publishStatusLines() {
#if PETDOOR_ENABLE_WIFI
  char line[352];
  snprintf(line, sizeof(line),
           "rssi=%d raw=%d dist=%s present=%d door=%s locked=%d presses=%u "
           "gap=%lu samples=%lu adv=%lu weak=%lu heap=%lu up=%lu maint=%lu ip=%s real=%s "
           "act=%s gaveup=%d attempt=%u retry=%lu mopen=%lu mclose=%lu cal=%s",
           g_tracker.filteredRssi(), g_tracker.rawRssi(),
           fmt1(g_tracker.distanceM()).c_str(),
           g_tracker.isPresent() ? 1 : 0,
           DoorController::stateName(g_door.state()), g_locked ? 1 : 0,
           g_door.pulseCount(),
           static_cast<unsigned long>(g_tracker.maxGapMs()),
           static_cast<unsigned long>(g_tracker.totalSamples()),
           static_cast<unsigned long>(BleScanner::advCount()),
           static_cast<unsigned long>(g_tracker.weakSamples()),
           static_cast<unsigned long>(ESP.getFreeHeap()),
           static_cast<unsigned long>(millis() / 1000),
           // Seconds left in the maintenance window, 0 when there is none. The
           // dashboard needs this to say the door is deliberately not moving —
           // otherwise a window looks exactly like a door that has died.
           static_cast<unsigned long>(Maintenance::remainingMs(millis()) / 1000),
           // The door's own address, so the dashboard can tell you where to
           // point a console during a maintenance window. You cannot look it
           // up on the door itself without already being on the door.
           WifiLogger::localIp().c_str(),
           // What the switches SAY, as opposed to what we commanded. "none"
           // when no switches are fitted; "?" is the normal mid-travel
           // reading with them.
           !Position::enabled() ? "none"
               : Position::fault() ? "FAULT"
               : Position::state() == DOOR_OPEN ? "OPEN"
               : Position::state() == DOOR_CLOSED ? "CLOSED" : "?",
           // What the actuation path is doing, and what it has concluded.
           // Without these the dashboard cannot tell a door that is staying
           // open on purpose from one that has died — which are the two
           // states it most needs to distinguish.
           Actuator::busy() ? Actuator::phaseName(Actuator::phase()) : "idle",
           Actuator::gaveUp() ? 1 : 0,
           static_cast<unsigned>(Actuator::closeAttempts()),
           static_cast<unsigned long>(Actuator::retryWaitRemainingMs(millis()) / 1000UL),
           // Measured travel times, which is how travel time gets tuned from a
           // browser: compare them against the configured pair below.
           static_cast<unsigned long>(Actuator::measuredMs(DOOR_OPEN)),
           static_cast<unsigned long>(Actuator::measuredMs(DOOR_CLOSED)),
           Actuator::calState() == Actuator::CAL_OFF ? "-"
               : Actuator::calibrating() ? "running"
               : Actuator::calState() == Actuator::CAL_DONE ? "done" : "failed");
  WifiLogger::setStatusLine(line);

  // Every tunable the remote channel can change, so the dashboard's
  // settings form can show what each one IS rather than a blank box. Built
  // here for the same reason the status line is: this task owns all of it.
  // Windows, compactly: HHMM-HHMM/<day mask in hex>, comma separated. The
  // dashboard cannot offer to edit a schedule it cannot see, and the door is
  // the only thing that knows what it has stored.
  char sched[96] = "";
  for (uint8_t i = 0; i < Schedule::count(); i++) {
    Schedule::Window w;
    if (!Schedule::get(i, w)) break;
    char one[20];
    snprintf(one, sizeof(one), "%s%02u%02u-%02u%02u/%02x", i ? "," : "",
             w.startMin / 60, w.startMin % 60, w.endMin / 60, w.endMin % 60,
             w.days & 0x7F);
    strncat(sched, one, sizeof(sched) - strlen(sched) - 1);
  }

  char cfg[576];
  snprintf(cfg, sizeof(cfg),
           "enter=%d exit=%d dopen=%lu dclose=%lu dmin=%lu pulse=%lu "
           "pcount=%u pgap=%lu igap=%lu travel=%lu fwin=%u falpha=%s "
           "owin=%u oalpha=%s bpin=%d bpassive=%d blow=%d "
           "sopen=%d sshut=%d slow=%d "
           "upsettle=%lu upmin=%lu upbeat=%lu "
           "vib=%d tz=%d topen=%lu tclose=%lu wake=%lu retrymin=%lu retrymax=%u "
           "sched=%s",
           g_tracker.enterDbm(), g_tracker.exitDbm(),
           static_cast<unsigned long>(g_tracker.enterConfirmMs()),
           static_cast<unsigned long>(g_tracker.exitConfirmMs()),
           static_cast<unsigned long>(g_door.minIntervalMs()),
           static_cast<unsigned long>(g_door.pulseMs()),
           g_door.pulseCount(),
           static_cast<unsigned long>(g_door.pulseGapMs()),
           static_cast<unsigned long>(g_door.directionGapMs()),
           // Kept as the open-direction value so a dashboard that only knows
           // about one travel time still shows a real number rather than a
           // blank. topen/tclose below are the pair it should prefer.
           static_cast<unsigned long>(Actuator::travelMs(DOOR_OPEN)),
           g_tracker.windowSize(), String(g_tracker.alpha(), 2).c_str(),
           g_tracker.fastWindowSize(), String(g_tracker.fastAlpha(), 2).c_str(),
           Chime::pin(), Chime::passive() ? 1 : 0, Chime::activeLow() ? 1 : 0,
           Position::openPin(), Position::closedPin(),
           Position::activeLow() ? 1 : 0,
           static_cast<unsigned long>(WifiLogger::settleMs()),
           static_cast<unsigned long>(WifiLogger::minIntervalMs()),
           static_cast<unsigned long>(WifiLogger::heartbeatMs()),
           Vibration::pin(), Schedule::utcOffsetMinutes(),
           static_cast<unsigned long>(Actuator::travelMs(DOOR_OPEN)),
           static_cast<unsigned long>(Actuator::travelMs(DOOR_CLOSED)),
           static_cast<unsigned long>(Actuator::wakeIdleMs()),
           static_cast<unsigned long>(Actuator::retryDelayMs() / 60000UL),
           static_cast<unsigned>(Actuator::retryLimit()),
           sched[0] ? sched : "-");
  WifiLogger::setConfigLine(cfg);
#endif
}

// Announce a movement the moment it is commanded, naming who asked for it.
//
// Played here rather than when the travel ENDS because this is the half-second
// that answers the question the door is otherwise silent about for twelve
// seconds: "did it hear me?". The outcome gets its own sound later.
void announceMovement(ActuationSource src, DoorState target) {
  const ChimeTune tune = Actuator::moveTune(src, target);
  if (tune != CHIME_NONE) Chime::play(tune);
  // updateChime() starts the travel tick once this has finished saying itself.
  // It does not need telling: "a travel is in flight and the buzzer is idle" is
  // the whole condition, which also covers the travels nothing announces —
  // a calibration run, and the reversal after a stalled close.
}

// Announce and record how an attempt ended.
//
// One place, for both halves. These used to be spread between the chime logic
// (which said "arrived" on a timer) and the position module (which said
// "stalled" separately), and the result was a door that could announce both
// for the same travel.
void reportOutcome(const Actuator::Result &r) {
  const char *where = DoorController::stateName(r.target);

  switch (r.outcome) {
    case Actuator::OUT_ARRIVED:
      Con.printf("[door] %s — ARRIVED, verified by the limit switch in %lu ms\r\n",
                    where, static_cast<unsigned long>(r.measuredMs));
      {
        // A travel landing within a second of its deadline is a door about to
        // start reporting stalls for no reason — in January, or with a bird
        // leaning on it. Worth saying before it happens.
        const uint32_t configured = Actuator::travelMs(r.target);
        if (configured != 0 && r.measuredMs + 1000 > configured + TRAVEL_GRACE_MS) {
          Con.printf("[door] !! that is within a second of the %lu ms deadline — "
                        "raise it with 'w' then 'travel', or run 'calibrate'\r\n",
                        static_cast<unsigned long>(configured + TRAVEL_GRACE_MS));
        }
      }
      Chime::play(CHIME_DONE);
      break;

    case Actuator::OUT_ASSUMED:
      // The honest wording. Nothing measured this; the stopwatch ran out.
      Con.printf("[door] %s — travel time elapsed (assumed; no limit switch at that end)\r\n",
                    where);
      Chime::play(CHIME_DONE);
      break;

    case Actuator::OUT_UNTIMED:
      // No travel time and nothing to verify with, so there is nothing to
      // announce. Silence here is correct: a chime would be a claim.
      break;

    case Actuator::OUT_NO_MOVE:
      Con.printf("[door] !! %u presses and the door NEVER MOVED.\r\n", r.presses);
      Con.println(F("[door] !! Nothing was felt for the whole wait: the controller"));
      Con.println(F("[door] !! swallowed every press, or the door is jammed solid."));
      Con.println(F("[door] !! NOT the same as failing to arrive — nothing moved, so"));
      Con.println(F("[door] !! nothing is trapped, and the door is still where it was."));
      Con.println(F("[door] !! Try a longer 'pulse' first: 500 ms is swallowed by some"));
      Con.println(F("[door] !! controllers and 1000 ms is not."));
      EventLog::record(LOG_NO_MOVEMENT, static_cast<uint8_t>(r.target),
                       g_tracker.filteredRssi());
      Chime::play(CHIME_NO_MOVE);
      break;

    case Actuator::OUT_STALLED:
      Con.printf("[door] !! travelling %s STALLED — it started and never arrived.\r\n",
                    where);
      Con.println(F("[door] !! An obstruction, a jam, or a controller that stopped"));
      Con.println(F("[door] !! partway. The door's position is now UNKNOWN."));
      EventLog::record(LOG_STALLED, static_cast<uint8_t>(r.target),
                       g_tracker.filteredRssi(), static_cast<int16_t>(r.flags));
      if (r.target == DOOR_CLOSED) {
        // The one place the firmware reverses a command of its own. Said out
        // loud because it is the most important thing this door ever does.
        Con.println(F("[door] !! It was CLOSING, so it is being reopened. A door"));
        Con.println(F("[door] !! stopped partway shut is exactly when something"));
        Con.println(F("[door] !! may be under it."));
        if (r.gaveUp) {
          Con.printf("[door] !! %u close attempts have now stalled. GIVING UP: the door\r\n",
                        r.attempt);
          Con.println(F("[door] !! will stay OPEN until somebody clears the way and"));
          Con.println(F("[door] !! presses 'x', or sends `door close`."));
          EventLog::record(LOG_GAVE_UP, r.attempt, g_tracker.filteredRssi());
        } else {
          Con.printf("[door] !! next attempt in %lu min (attempt %u of %u).\r\n",
                        static_cast<unsigned long>(Actuator::retryDelayMs() / 60000UL),
                        static_cast<unsigned>(r.attempt + 1),
                        static_cast<unsigned>(Actuator::retryLimit()));
        }
      }
      Chime::play(r.gaveUp ? CHIME_GAVE_UP : CHIME_STALLED);
      break;

    default:
      break;
  }

  // The door's own record of having gone somewhere. Only for outcomes where it
  // actually believes it got there — OUT_NO_MOVE and OUT_STALLED have their own
  // entries above, and recording an OPEN for a travel that failed is exactly
  // the lie this rebuild exists to stop telling.
  const bool committed = (r.outcome == Actuator::OUT_ARRIVED ||
                          r.outcome == Actuator::OUT_ASSUMED ||
                          r.outcome == Actuator::OUT_UNTIMED);
  if (committed) {
    EventLog::record(r.target == DOOR_OPEN ? LOG_OPEN : LOG_CLOSE,
                     static_cast<uint8_t>(r.source), g_tracker.filteredRssi(),
                     static_cast<int16_t>(r.flags));
  }

#if PETDOOR_ENABLE_WIFI
  if (r.outcome == Actuator::OUT_NO_MOVE || r.outcome == Actuator::OUT_STALLED) {
    // A failure goes out unconditionally. These are rare, and they are the
    // entries somebody will be reading in a hurry.
    publishStatusLines();
    WifiLogger::requestFlushNow();
  } else if (committed) {
    // A COMPLETED TRAVEL also goes out at once, because "is the door open?" is
    // the one question a dashboard exists to answer, and the old behaviour
    // answered it worst exactly when it mattered: an open door is not idle, so
    // the status could sit unreported until the half-hour heartbeat while the
    // page showed the door as it had been before it moved.
    //
    // This overrides the idle gate on purpose. wifi_logger.h argues against
    // uploading per event, and that argument is sound — the radio is shared and
    // events happen when the animal is AT THE DOOR. Two things make this one
    // exception affordable:
    //
    //   * it lands at the END of a travel. Presence was settled long before;
    //     for an open the animal is already through the doorway, and for a
    //     close it has been gone for the whole exit dwell. A lost sample here
    //     can at worst delay noticing a DEPARTURE, which has the exit dwell to
    //     absorb it, and `!near` cancels a pending close regardless.
    //   * it is floored at DOOR_REPORT_MIN_MS. A forced flush ignores the
    //     upload interval completely, so without that floor a door flapping at
    //     the threshold would hold the radio up indefinitely.
    static uint32_t lastReportMs = 0;
    static bool reportedOnce = false;
    const uint32_t nowMs = millis();
    if (!reportedOnce || (nowMs - lastReportMs) >= DOOR_REPORT_MIN_MS) {
      reportedOnce = true;
      lastReportMs = nowMs;
      publishStatusLines();
      WifiLogger::requestFlushNow();
    }
  }
#endif
}

// Writes down the things the actuator noticed along the way: a wake press, and
// a press it had to repeat. Separate from the outcome because several can
// happen within one attempt.
void drainActuatorNotes() {
  Actuator::Note n;
  while (Actuator::popNote(n)) {
    switch (n.type) {
      case LOG_WAKE:
        if (n.detail == 1) {
          Con.println(F("[door] the wake press moved the door by itself — not pressing again"));
        } else {
          Con.println(F("[door] wake press sent (the controller had been idle)"));
        }
        break;
      case LOG_RETRY:
        Con.printf("[door] nothing moved; pressing again (retry %u)\r\n", n.detail);
        break;
      default:
        break;
    }
    if (n.record) EventLog::record(n.type, n.detail, g_tracker.filteredRssi());
  }
}

// Says how a calibration run ended.
//
// Without this the verdict was only reachable by pressing `s`: you asked for
// it, the door moved twice, and then nothing told you the answer. Edge
// triggered on the state, so it is said once.
//
// No chime: the travels have already sounded their own outcomes, and an
// acknowledgement played on top would cut off the arrival or the stall that
// actually carries the information.
void reportCalibration() {
  const Actuator::CalState cs = Actuator::calState();
  if (cs == g_lastCalState) return;
  g_lastCalState = cs;

  if (cs == Actuator::CAL_DONE) {
    Con.printf("[cal] %s\r\n", Actuator::calMessage());
    Con.println(F("[cal] a travel now has these plus TRAVEL_GRACE_MS to arrive"));
    Con.println(F("[cal] before it counts as a stall. These are reed-to-reed"));
    Con.println(F("[cal] times, so they are shorter than a stopwatch at the"));
    Con.println(F("[cal] physical limits — which is the number the deadline wants."));
#if PETDOOR_ENABLE_WIFI
    // The configuration changed, so the dashboard's settings form is stale
    // until this goes out.
    publishStatusLines();
    WifiLogger::requestFlushNow();
#endif
  } else if (cs == Actuator::CAL_FAILED) {
    Con.printf("[cal] FAILED: %s\r\n", Actuator::calMessage());
    Con.println(F("[cal] nothing was adopted; the previous travel times stand."));
  }
}

void updateChime(uint32_t nowMs) {
  // Two rules, and nothing else: starting and ending a travel is the actuator's
  // business, and the outcome tune is reportOutcome()'s.
  //
  // Gated on the buzzer being idle rather than on a flag, which is what keeps
  // the travel tick from cutting off the half-second that said who asked for
  // the movement — and what makes it cover the travels nothing announces.
  if (Chime::enabled() && Actuator::busy() && Chime::playing() == CHIME_NONE) {
    Chime::play(CHIME_WORKING);
  }
  // A travel that has ended must not leave the tick looping. The outcome tune
  // normally replaces it, but OUT_UNTIMED deliberately plays nothing.
  if (!Actuator::busy() && Chime::playing() == CHIME_WORKING) Chime::stop();
  Chime::tick(nowMs);
}

void updateLed(uint32_t nowMs, bool scanHealthy) {
  // The test overrides every pattern below, faults included: you asked to see
  // the LED, so you see the LED. Unsigned arithmetic keeps the elapsed
  // comparison correct across the millis() rollover.
  if (g_ledTestActive) {
    const uint32_t elapsed = nowMs - g_ledTestStartMs;
    if (elapsed < kLedTestMs) {
      digitalWrite(PIN_STATUS_LED,
                   (elapsed % kLedTestPeriodMs) < kLedTestOnMs ? HIGH : LOW);
      return;
    }
    g_ledTestActive = false;
  }

  bool on;
  if (!scanHealthy) {
    on = (nowMs / 100) % 2 == 0;  // 5 Hz: radio unhealthy
  } else if (Actuator::gaveUp()) {
    // Three quick blips, repeating. Deliberately not another even blink —
    // 5 Hz, 2 Hz and 1 Hz are spoken for by the faults around it — and
    // deliberately above the beacon-battery flash in priority: a flat battery
    // is a job for the weekend, a door that has stopped closing is tonight's.
    const uint32_t t = nowMs % 2000;
    on = (t < 120) || (t > 240 && t < 360) || (t > 480 && t < 600);
  } else if (beaconBatteryLow(nowMs)) {
    on = (nowMs / 250) % 2 == 0;  // 2 Hz: replace the beacon battery
  } else if (!BleScanner::isConfigured() || !BleScanner::targetEverSeen()) {
    on = (nowMs / 500) % 2 == 0;  // 1 Hz: nothing to track yet
  } else if (Actuator::busy()) {
    // Lit, with a heartbeat gap: the door is moving. "Nearly solid" is
    // recognisable across a yard and cannot be mistaken for a fault.
    on = (nowMs % 600) > 120;
  } else {
    // MEASURED position where there is one, commanded position otherwise.
    //
    // This is the difference the switches buy, shown on the one indicator
    // somebody can see from the coop: with them fitted the LED stops reporting
    // what the door was told and starts reporting where it is. A door sitting
    // between the two switches — shoved by hand, or stopped partway — reads as
    // neither open nor shut rather than as whichever was last commanded.
    const bool measured = Position::enabled() && !Position::fault();
    const DoorState shown = measured ? Position::state() : g_door.state();
    switch (shown) {
      case DOOR_OPEN:
        on = true;                      // solid
        break;
      case DOOR_CLOSED:
        // Locked reads as a DOUBLE blip, so a glance tells you whether the
        // door is merely shut or shut against the collar.
        on = g_locked
                 ? ((nowMs % 2000) < 150 ||
                    ((nowMs % 2000) > 300 && (nowMs % 2000) < 450))
                 : (nowMs % 2000) < 200;
        break;
      default:
        // Off means "I do not know", which with switches fitted is a real and
        // useful answer: the door is somewhere in between.
        on = false;
        break;
    }
  }
  digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW);
}

// Opens a maintenance window and, with it, the network console. Shared by the
// console key and the remote command so the two cannot drift apart.
bool beginMaintenance(uint32_t nowMs, uint32_t durationMs, String &message) {
  const char *problem = nullptr;
  if (!Maintenance::start(nowMs, durationMs, problem)) {
    message = problem != nullptr ? problem : "maintenance window refused";
    return false;
  }
#if PETDOOR_ENABLE_WIFI
  // Ask for the radio, but do NOT start the console here. holdRadio() only
  // sets a flag — the uploader task is what actually associates, moments from
  // now. Calling WiFiServer::begin() against a network stack that is still
  // down panics the chip, which is precisely how boot #504 happened. The
  // listener is started from serviceMaintenance() once WiFi is really up.
  WifiLogger::holdRadio(true);
#endif
  EventLog::record(LOG_MAINT, 1, g_tracker.filteredRssi());
  Chime::play(CHIME_ACK_SET);
  char buf[160];
  snprintf(buf, sizeof(buf),
           "maintenance ON for %lu min — the beacon cannot move the door until "
           "it expires%s",
           static_cast<unsigned long>(Maintenance::remainingMs(nowMs) / 60000UL),
           // The console comes up a moment later, with the radio.
           PETDOOR_ENABLE_WIFI ? "; console opening" : "");
  message = buf;
  return true;
}

void endMaintenance(String &message) {
  Maintenance::stop();
  NetConsole::stop();
#if PETDOOR_ENABLE_WIFI
  WifiLogger::holdRadio(false);
#endif
  EventLog::record(LOG_MAINT, 2, g_tracker.filteredRssi());
  Chime::play(CHIME_ACK_SET);
  message = "maintenance OFF — the beacon controls the door again";
}

// One implementation of the schedule commands, shared by the console editor and
// the remote verb so the two cannot drift apart.
void printScheduleMenu() {
  Con.println(F("\r\n---- scheduled lockout ----"));
  Schedule::describe(Con, EventLog::haveEpoch(), EventLog::epochNow());
  Con.println(F("  type one of:"));
  Con.println(F("    add 22:00-06:00               every night"));
  Con.println(F("    add 22:00-06:00 Mon-Fri       weeknights only"));
  Con.println(F("    add 13:00-14:00 Sat,Sun       any window, any days"));
  Con.println(F("    del <n>                       remove one, by its number"));
  Con.println(F("    del 22:00-06:00               remove one, by its times"));
  Con.println(F("      numbers RENUMBER as you delete, so deleting several by"));
  Con.println(F("      number only works highest first. Deleting by times does not"));
  Con.println(F("    clear                         remove all"));
  Con.println(F("    tz -420                       minutes east of UTC"));
  Con.println(F("    q                             done"));
  Con.println(F("  A window stops the BEACON opening the door. It never stops the"));
  Con.println(F("  door closing, and it cannot let an animal back in — decide with"));
  Con.println(F("  that in mind."));
}

bool applyScheduleLine(const char *line, String &result) {
  while (*line == ' ') line++;
  char buf[96];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char *verb = strtok(buf, " \t");
  if (verb == nullptr || strcasecmp(verb, "list") == 0 || strcasecmp(verb, "q") == 0) {
    result = String(Schedule::count()) + " window(s) stored";
    return true;
  }
  if (strcasecmp(verb, "clear") == 0) {
    Schedule::clear();
    result = "all windows removed — the beacon may open the door at any hour";
    return true;
  }
  if (strcasecmp(verb, "tz") == 0) {
    const char *a = strtok(nullptr, " \t");
    if (a == nullptr) { result = "tz takes minutes east of UTC, e.g. -420"; return false; }
    const long m = strtol(a, nullptr, 10);
    if (m < -840 || m > 840) { result = "offset must be -840..+840 minutes"; return false; }
    Schedule::setUtcOffsetMinutes(static_cast<int16_t>(m));
    char msg[96];
    snprintf(msg, sizeof(msg), "local time is now UTC%+ld:%02ld", m / 60, labs(m % 60));
    result = msg;
    return true;
  }
  if (strcasecmp(verb, "del") == 0 || strcasecmp(verb, "rm") == 0) {
    // Rest of the line, not one token, so a window can be named in full.
    char *rest = strtok(nullptr, "");
    if (rest == nullptr) {
      result = "del takes a window number from `list`, or the window itself "
               "(e.g. `del 22:00-06:00`)";
      return false;
    }
    while (*rest == ' ') rest++;

    bool numeric = (*rest != '\0');
    for (const char *c = rest; *c != '\0'; c++) {
      if (*c < '0' || *c > '9') { numeric = false; break; }
    }

    if (numeric) {
      const long n = strtol(rest, nullptr, 10);
      if (!Schedule::removeAt(static_cast<uint8_t>(n))) {
        // Spell out the renumbering, because this is the failure people
        // actually hit: deleting TWO windows by number in one go. Removing a
        // window shifts the rest down, so `del 0` then `del 1` deletes one and
        // then fails — and in a remote batch the failure tone is all you hear.
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "no window [%ld] — there %s %u. Removing a window RENUMBERS "
                 "the rest, so deleting several by number only works highest "
                 "first. `del 22:00-06:00` deletes by the window itself and "
                 "does not care about order; `clear` removes all of them.",
                 n, Schedule::count() == 1 ? "is" : "are",
                 static_cast<unsigned>(Schedule::count()));
        result = msg;
        return false;
      }
      result = "window removed";
      return true;
    }

    // By specification instead of by position. Order-independent, which is the
    // whole point: several of these in one batch all mean what they say.
    Schedule::Window want;
    const char *problem = nullptr;
    if (!Schedule::parseSpec(rest, want, problem)) {
      result = problem != nullptr ? problem : "could not read that window";
      return false;
    }
    for (uint8_t i = 0; i < Schedule::count(); i++) {
      Schedule::Window w;
      if (!Schedule::get(i, w)) break;
      // Matched on the TIMES only. Days are deliberately not compared: `list`
      // shows them, and someone deleting "22:00-06:00" means that window
      // whether or not they retyped its day mask correctly.
      if (w.startMin == want.startMin && w.endMin == want.endMin) {
        Schedule::removeAt(i);
        result = "window removed";
        return true;
      }
    }
    result = "no window with those times — `list` shows what is stored";
    return false;
  }
  if (strcasecmp(verb, "add") == 0) {
    char *rest = strtok(nullptr, "");
    if (rest == nullptr) { result = "add takes HH:MM-HH:MM [days]"; return false; }
    while (*rest == ' ') rest++;
    Schedule::Window w;
    const char *problem = nullptr;
    if (!Schedule::parseSpec(rest, w, problem)) {
      result = problem != nullptr ? problem : "could not read that window";
      return false;
    }
    if (!Schedule::add(w.startMin, w.endMin, w.days, problem)) {
      result = problem != nullptr ? problem : "window refused";
      return false;
    }
    // Said on every add rather than buried in a doc. This is the consequence
    // people do not think through until it happens to a straggler.
    result = "window added — NOTE: while it is in force the door will not open "
             "for the collar, including for an animal caught outside";
    return true;
  }
  result = "schedule takes add / del / list / clear / tz";
  return false;
}

// Runs the maintenance window: its expiry, the network console it carries, and
// the periodic upload of the distribution being measured.
void serviceMaintenance(uint32_t nowMs) {
  // Expiry is announced, never silent. The door starts obeying the beacon
  // again at this moment, and somebody standing at it with the collar needs to
  // know that before it moves rather than by watching it move.
  if (Maintenance::consumeExpired(nowMs)) {
    Con.println(F("[maint] window expired — the beacon controls the door again"));
    // Calibration depends on the window: it is what stops the beacon moving
    // the door mid-measurement. The window expires by itself, by design, so a
    // long run can outlive it — and a travel timed with the collar free to
    // interrupt it is not a measurement of anything.
    if (Actuator::calibrating()) {
      Actuator::abortCalibration("the maintenance window expired mid-run");
      Con.println(F("[cal] abandoned: the window it needed has expired"));
    }
    EventLog::record(LOG_MAINT, 0, g_tracker.filteredRssi());
    Chime::play(CHIME_DONE);
    NetConsole::stop();
#if PETDOOR_ENABLE_WIFI
    // The radio goes back to BLE with the window. Leaving it up would quietly
    // degrade BLE sampling forever after one calibration session.
    WifiLogger::holdRadio(false);
    publishStatusLines();
    WifiLogger::requestFlushNow();
#endif
  }

  if (!Maintenance::active(nowMs)) return;

#if PETDOOR_ENABLE_WIFI
  // Start listening only once there is a network to listen on, and stop again
  // if the link drops — a listening socket on a down stack is a crash, not an
  // inconvenience. Idempotent, so this is safe to evaluate every tick.
  if (WifiLogger::radioAssociated()) {
    NetConsole::start();
  } else if (NetConsole::listening()) {
    NetConsole::stop();
  }
#endif

  NetConsole::tick(nowMs);

#if PETDOOR_ENABLE_WIFI
  // Push the distribution periodically so it can be read from a browser. This
  // is the whole point of the window for anyone without a cable.
  static uint32_t lastReportMs = 0;
  if (nowMs - lastReportMs >= MAINT_REPORT_MS) {
    lastReportMs = nowMs;
    WifiLogger::queueScanUpload(Maintenance::summary(nowMs));
    WifiLogger::requestFlushNow();
  }
#endif
}

void driveDoor(uint32_t nowMs) {
  // Maintenance mode: the door listens but does not act. Checked before
  // everything else, and it blocks BOTH directions — unlike the lock below,
  // which deliberately still lets a locked door close when the beacon leaves.
  // That distinction is the point: somebody calibrating is standing at the door
  // holding the collar, and a door that shuts on them is both useless for
  // measurement and unsafe. The window expires by itself; see maintenance.h.
  if (Maintenance::active(nowMs)) return;

  // Never operate the door without a configured beacon.
  if (!BleScanner::isConfigured()) return;

  // Never operate the door until the beacon has been heard at least once since
  // boot. Otherwise a beacon with a flat battery would read as "absent" and the
  // door would close and stay shut.
  if (!BleScanner::targetEverSeen()) return;

  // The resulting state change is announced by reportTransitions(), which also
  // covers the manual `o` / `x` pulses. Reporting in one place keeps a single
  // line per change rather than one here and another there.
  // The lock: proximity may not open the door. Checked before anything else
  // because it is the strongest statement about what this door is allowed to
  // do — but note it gates only the OPEN path below. A locked door that is
  // open still closes normally when the beacon goes away.
  // A schedule is an ADDITIONAL reason to refuse, never a reason to permit:
  // `lock` set by hand stays absolute. An unset clock yields false here rather
  // than applying windows to a guessed time — see schedule.h.
  uint8_t schedWindow = 0;
  const bool schedLocked = Schedule::lockedNow(EventLog::haveEpoch(),
                                               EventLog::epochNow(), &schedWindow);
  if ((g_locked || schedLocked) && g_tracker.isPresent()) {
    // Say so, once per arrival. Someone standing at a locked door with the
    // collar in their hand cannot tell "locked" from "broken", and that is
    // exactly the moment they start taking the thing apart. Edge-triggered:
    // this condition holds for as long as the animal is there, and a buzzer
    // that repeated it would be an alarm.
    if (!g_lockRefusedAnnounced) {
      g_lockRefusedAnnounced = true;
      // A manual lock takes precedence in the ANNOUNCEMENT as well as in the
      // logic: it is the stronger statement about what this door may do, and
      // if somebody locked it by hand that is the fact they need back. The
      // schedule tone is therefore only for a window refusing on its own.
      Chime::play((schedLocked && !g_locked) ? CHIME_REFUSED_SCHEDULE
                                            : CHIME_REFUSED);
      if (schedLocked && !g_locked) {
        // Logged, unlike a manual lock, because nobody was there to decide it.
        // "The door refused at 3 a.m." is only explicable if the log says a
        // window did it. Edge-triggered: the condition holds for as long as the
        // animal stands there, and an entry per tick would fill the ring.
        Con.printf("[sched] refused: window [%u] is in force\r\n", schedWindow);
        // LOG_REFUSED's detail is normally an ActuationResult, and this is
        // deliberately outside that range: a schedule is not one of the
        // actuator's refusals. It used to be a bare 5, which this rebuild
        // turned into a collision — ACT_RETRY_WAIT is now 5, and a door
        // waiting out a stalled close would have been reported as having been
        // refused by a schedule window that does not exist.
        EventLog::record(LOG_REFUSED, kRefusedBySchedule, g_tracker.filteredRssi());
      }
    }
    return;
  }
  g_lockRefusedAnnounced = false;

  // A manual hold suppresses automatic CLOSING only. Opening is never blocked:
  // if the beacon turns up mid-hold the door is already open, and a manual `x`
  // must never be able to keep the door shut against an animal walking up to
  // it. Holding open is the safe failure; holding closed is not.
  if (!g_tracker.isPresent() && manualHoldActive(nowMs)) return;

  // Remembers the last refusal that was logged, so a condition holding for
  // minutes produces one entry rather than ten a second. Reset on success, or
  // the next genuine instance of the same refusal would be swallowed.
  static ActuationResult lastLogged = ACT_DONE;

  const DoorState want = g_tracker.isPresent() ? DOOR_OPEN : DOOR_CLOSED;
  const ActuationResult r = Actuator::request(want, SRC_BEACON, nowMs, /*force=*/false);
  if (r == ACT_DONE) {
    lastLogged = ACT_DONE;
    g_gaveUpAnnounced = false;
    Con.printf("[door] %s — the collar is %s (rssi %d dBm, ~%s m)\r\n",
                  want == DOOR_OPEN ? "opening" : "closing",
                  want == DOOR_OPEN ? "here" : "gone", g_tracker.filteredRssi(),
                  fmt1(g_tracker.distanceM()).c_str());
    announceMovement(SRC_BEACON, want);
    return;
  }

  // ACT_ALREADY and ACT_BUSY are the normal steady states and would swamp the
  // log; the rest are the refusals that explain a door that did not move.
  if (r == ACT_LOCKED_OUT || r == ACT_BOOT_GRACE || r == ACT_GAVE_UP ||
      r == ACT_RETRY_WAIT) {
    if (r != lastLogged) {
      lastLogged = r;
      EventLog::record(LOG_REFUSED, static_cast<uint8_t>(r), g_tracker.filteredRssi());
      // GAVE_UP and RETRY_WAIT are said out loud once as well. Both mean the
      // door is deliberately not closing, and somebody looking at an open door
      // at dusk deserves to be told that rather than left to guess.
      if (r == ACT_GAVE_UP && !g_gaveUpAnnounced) {
        g_gaveUpAnnounced = true;
        Con.println(F("[door] the beacon has gone and the door is NOT closing:"));
        Con.println(F("[door] close attempts are exhausted. 'x' tries again."));
      } else if (r == ACT_RETRY_WAIT) {
        Con.printf("[door] not closing yet — %lu s left of the retry delay\r\n",
                      static_cast<unsigned long>(
                          Actuator::retryWaitRemainingMs(nowMs) / 1000UL));
      }
    }
  }
  if (r != ACT_GAVE_UP) g_gaveUpAnnounced = false;
}

// Announces every change of state, so the console tells the story on its own
// without anyone having to poll with `s`.
void reportTransitions(uint32_t nowMs, bool scanHealthy) {
  // Signal acquired / lost. This underpins every presence decision: no fix
  // counts as FAR, so it is worth seeing directly rather than inferring it.
  const bool fix = g_tracker.hasFix(nowMs);
  if (fix != g_lastReportedFix) {
    g_lastReportedFix = fix;
    EventLog::record(fix ? LOG_FIX_GOT : LOG_FIX_LOST, 0, g_tracker.filteredRssi());
    if (fix) {
      Con.printf("[fix] acquired  (rssi %d dBm, ~%s m, %lu samples)\r\n",
                    g_tracker.filteredRssi(), fmt1(g_tracker.distanceM()).c_str(),
                    static_cast<unsigned long>(g_tracker.totalSamples()));
    } else {
      Con.printf("[fix] lost  (last heard %lu ms ago) — counts as FAR\r\n",
                    static_cast<unsigned long>(g_tracker.sampleAgeMs(nowMs)));
    }
  }

  if (g_tracker.state() != g_lastReportedPresence) {
    g_lastReportedPresence = g_tracker.state();
    Con.printf("[presence] %s  (rssi %d dBm, ~%s m, %lu samples)\r\n",
                  g_tracker.isPresent() ? "PRESENT" : "ABSENT", g_tracker.filteredRssi(),
                  fmt1(g_tracker.distanceM()).c_str(),
                  static_cast<unsigned long>(g_tracker.totalSamples()));
  }

  // Door state is NOT reported here any more. It is announced by
  // reportOutcome(), which knows how the travel actually ended — verified,
  // assumed, stalled or never started — and can therefore say something true.
  // Watching g_door.state() change could only ever report the commanded
  // state, and it reported it twice for a travel that failed: once as a
  // success here, once as a failure elsewhere.

  if (scanHealthy != g_lastReportedScanHealthy) {
    g_lastReportedScanHealthy = scanHealthy;
    if (scanHealthy) {
      Con.println(F("[radio] healthy — advertisements resumed"));
    } else {
      Con.printf("[radio] UNHEALTHY — nothing heard from any device for %lu ms\r\n",
                    static_cast<unsigned long>(BleScanner::advAgeMs(nowMs)));
    }
  }
}

#if REMOTE_CONFIG && PETDOOR_ENABLE_WIFI
// Apply one command sent back by the log server.
//
// Runs on controlTask, never on the WiFi task, because these touch the tracker
// and the door and those belong to one owner (invariant 9). Every setter used
// here is the SAME one the serial console calls, so the validation that
// protects a person at the keyboard protects the network path identically:
// thresholds that do not form a hysteresis band, a lockout longer than the
// close dwell, an open filter slower than the close filter — all still refused.
//
// Deliberately absent: anything touching WiFi, the endpoint, the shared key or
// the OTA password. Those are what this channel runs on; a command that broke
// one would strand the door with no way back. See REMOTE_CONFIG in config.h.
bool applyRemoteCommand(const char *line, String &result) {
  char buf[REMOTE_CMD_MAX_LEN];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char *verb = strtok(buf, " \t");
  if (verb == nullptr) return false;
  auto arg = []() { return strtok(nullptr, " \t"); };

  if (strcmp(verb, "ota") == 0) {
    WifiLogger::beginOtaWindow(g_tracker.isPresent());
    result = "ota window requested";
    return true;
  }
  if (strcmp(verb, "thresholds") == 0) {
    const char *a = arg(), *b = arg();
    if (!a || !b) { result = "thresholds needs <enter> <exit>"; return false; }
    if (!g_tracker.setThresholds(atoi(a), atoi(b))) {
      result = "thresholds rejected: enter must be above exit";
      return false;
    }
    BleScanner::storeThresholds(g_tracker.enterDbm(), g_tracker.exitDbm());
    g_thresholdsStored = true;
    result = String("thresholds ") + g_tracker.enterDbm() + "/" + g_tracker.exitDbm();
    return true;
  }
  if (strcmp(verb, "dwell") == 0) {
    const char *a = arg(), *b = arg(), *c = arg();
    if (!a || !b || !c) { result = "dwell needs <open> <close> <lockout>"; return false; }
    const uint32_t en = strtoul(a, nullptr, 10), ex = strtoul(b, nullptr, 10),
                   lk = strtoul(c, nullptr, 10);
    if (!en || !ex || !lk) { result = "dwell values must be non-zero"; return false; }
    if (lk >= ex) { result = "dwell rejected: lockout must be below the close dwell"; return false; }
    g_tracker.setDwell(en, ex);
    g_door.setMinIntervalMs(lk);
    BleScanner::storeTiming(en, ex, lk);
    g_timingStored = true;
    result = String("dwell ") + en + "/" + ex + "/" + lk;
    return true;
  }
  if (strcmp(verb, "gap") == 0) {
    const char *a = arg();
    if (!a || !g_door.setDirectionGapMs(strtoul(a, nullptr, 10))) {
      result = "gap rejected (100-5000 ms)";
      return false;
    }
    BleScanner::storeDirectionGap(g_door.directionGapMs());
    g_timingStored = true;
    result = String("gap ") + g_door.directionGapMs();
    return true;
  }
  if (strcmp(verb, "presses") == 0) {
    const char *a = arg(), *b = arg();
    if (!a) { result = "presses needs <count> [gap ms]"; return false; }
    const long n = atol(a);
    const uint32_t g = b ? strtoul(b, nullptr, 10) : g_door.pulseGapMs();
    if (n < 1 || n > DoorController::kMaxPulseCount ||
        !g_door.setPulseTrain(static_cast<uint8_t>(n), g)) {
      result = "presses rejected (count 1-3, gap 200-5000 ms)";
      return false;
    }
    BleScanner::storePulseTrain(g_door.pulseCount(), g_door.pulseGapMs());
    g_timingStored = true;
    result = String("presses ") + g_door.pulseCount() + " x, " +
             g_door.pulseGapMs() + " ms apart";
    return true;
  }
  if (strcmp(verb, "upload") == 0) {
    return applyUploadTiming(strtok(nullptr, ""), result);
  }
  if (strcmp(verb, "sensors") == 0) {
    return applySensorSpec(strtok(nullptr, ""), result);
  }
  if (strcmp(verb, "buzzer") == 0) {
    // Rest of the line: "27", "27 passive low", "off".
    return applyBuzzerSpec(strtok(nullptr, ""), result);
  }
  if (strcmp(verb, "beep") == 0) {
    // Remote, because "is the buzzer on the right pin" is exactly the question
    // you want to answer from indoors, with the door still fifty yards away.
    if (!Chime::enabled()) {
      result = "no buzzer configured; queue `buzzer <pin>` first";
      return false;
    }
    Chime::play(CHIME_TEST);
    result = String("beeping on GPIO ") + Chime::pin();
    return true;
  }
  if (strcmp(verb, "led") == 0) {
    // Remote for the same reason as `beep`: the pin you are checking is on a
    // door at the end of the garden, and the walk out to look at it is the
    // part worth removing.
    startLedTest(millis());
    result = String("flashing the status LED on GPIO ") + PIN_STATUS_LED;
    return true;
  }
  if (strcmp(verb, "pulse") == 0) {
    const char *a = arg();
    if (!a || !g_door.setPulseMs(strtoul(a, nullptr, 10))) {
      result = "pulse rejected (50-10000 ms)";
      return false;
    }
    BleScanner::storePulseMs(g_door.pulseMs());
    g_timingStored = true;
    result = String("pulse ") + g_door.pulseMs();
    return true;
  }
  if (strcmp(verb, "filter") == 0 || strcmp(verb, "openfilter") == 0) {
    const bool fast = (verb[0] == 'o');
    const char *a = arg(), *b = arg();
    if (!a || !b) { result = "filter needs <window> <alpha>"; return false; }
    const long w = atol(a);
    const float al = atof(b);
    if (w < 1 || w > kMaxMedianWindow) { result = "filter window out of range"; return false; }
    const bool ok = fast ? g_tracker.setFastFilter((uint8_t)w, al)
                         : g_tracker.setFilter((uint8_t)w, al);
    if (!ok) {
      result = fast ? "open filter rejected: must not be slower than the close filter"
                    : "filter rejected: window odd 3-15, alpha 0<a<=1";
      return false;
    }
    if (fast) {
      BleScanner::storeFastFilter(g_tracker.fastWindowSize(), g_tracker.fastAlpha());
      g_fastFilterStored = true;
      result = String("open filter ") + g_tracker.fastWindowSize() + "/" +
               String(g_tracker.fastAlpha(), 2);
    } else {
      BleScanner::storeFilter(g_tracker.windowSize(), g_tracker.alpha());
      g_filterStored = true;
      // setFilter() may drag the open pair along to keep it the faster of the
      // two; persist that too or a reboot would undo it.
      BleScanner::storeFastFilter(g_tracker.fastWindowSize(), g_tracker.fastAlpha());
      g_fastFilterStored = true;
      result = String("filter ") + g_tracker.windowSize() + "/" +
               String(g_tracker.alpha(), 2);
    }
    return true;
  }
  if (strcmp(verb, "macs") == 0) {
    const char *a = strtok(nullptr, "");     // rest of the line, commas and all
    if (!a) { result = "macs needs a comma-separated list"; return false; }
    String list(a);
    list.trim();
    String err;
    if (!BleScanner::storeTargetMacs(list.c_str(), err)) {
      result = String("macs rejected: ") + err;
      return false;
    }
    // A new list only takes effect after a restart: the BLE callback reads the
    // live list on every advertisement, so swapping it underneath is a race.
    //
    // The restart is DEFERRED rather than immediate so the acknowledgement can
    // be uploaded first — the ack lives in RAM, and rebooting now would lose
    // it, leaving the server unable to tell "applied" from "never arrived".
    g_restartAtMs = millis() + REMOTE_RESTART_DELAY_MS;
    if (g_restartAtMs == 0) g_restartAtMs = 1;
    WifiLogger::requestFlushNow();
    result = String("macs saved (") + list + "); restarting to apply";
    return true;
  }
  if (strcmp(verb, "door") == 0) {
    const char *a = arg();
    if (!a) { result = "door needs open or close"; return false; }
    if (strcmp(a, "auto") == 0) {
      g_manualHoldUntilMs = 0;
      result = "manual hold cleared";
      return true;
    }
    const bool wantOpen = (strcmp(a, "open") == 0);
    if (!wantOpen && strcmp(a, "close") != 0) {
      result = "door takes open, close or auto";
      return false;
    }
    const DoorState want = wantOpen ? DOOR_OPEN : DOOR_CLOSED;
    // SRC_REMOTE, not SRC_MANUAL. Weeks later "I was standing at the door" and
    // "something on the network asked" are different answers, and only one of
    // them is worth investigating. It also earns its own sound — four beeps
    // rather than three.
    const ActuationResult r =
        Actuator::request(want, SRC_REMOTE, millis(), /*force=*/true);
    if (r != ACT_DONE) {
      result = String("door refused: ") + DoorController::resultName(r);
      return false;
    }
    announceMovement(SRC_REMOTE, want);
    if (wantOpen) {
      if (MANUAL_HOLD_MS > 0) {
        g_manualHoldUntilMs = millis() + MANUAL_HOLD_MS;
        if (g_manualHoldUntilMs == 0) g_manualHoldUntilMs = 1;
      }
      // "opening", not "opened". The travel takes twelve seconds and the
      // outcome is reported separately when it resolves — claiming success
      // here is exactly what this rebuild stopped doing.
      result = "door opening (held); the outcome follows when it arrives";
    } else {
      g_manualHoldUntilMs = 0;
      result = "door closing; the outcome follows when it arrives";
    }
    return true;
  }
  if (strcmp(verb, "travel") == 0) {
    return applyTravelMs(strtok(nullptr, ""), result);
  }
  if (strcmp(verb, "wake") == 0) {
    return applyWakeSpec(arg(), result);
  }
  if (strcmp(verb, "retry") == 0) {
    const char *rest = strtok(nullptr, "");
    if (rest != nullptr && strcmp(rest, "clear") == 0) {
      // The way back from a door parked open by exhausted attempts, for
      // somebody who has just cleared the obstruction and is not at the door.
      Actuator::clearGaveUp();
      result = "close attempts reset — the door will try to close again";
      return true;
    }
    return applyRetrySpec(rest, result);
  }
  if (strcmp(verb, "calibrate") == 0) {
    String msg;
    const bool ok = Actuator::startCalibration(millis(), Maintenance::active(millis()), msg);
    result = msg;
    return ok;
  }
  if (strcmp(verb, "schedule") == 0) {
    char *rest = strtok(nullptr, "");
    String msg;
    const bool ok = applyScheduleLine(rest != nullptr ? rest : "list", msg);
    result = msg;
    return ok;
  }
  if (strcmp(verb, "vibration") == 0) {
    // Rest of the line: "33", or "off".
    return applyVibrationSpec(strtok(nullptr, ""), result);
  }
  if (strcmp(verb, "maint") == 0) {
    const char *a = arg();
    const bool wantOff = a != nullptr && (strcmp(a, "off") == 0 || strcmp(a, "0") == 0);
    String msg;
    if (wantOff) {
      endMaintenance(msg);
      result = msg;
      return true;
    }
    // A bare `maint` uses the default window; `maint <minutes>` names one.
    // Minutes rather than milliseconds because this is typed by a person
    // standing at a door, and a mistyped zero in milliseconds is a window
    // that ends before they have walked back.
    uint32_t ms = MAINT_DEFAULT_MS;
    if (a != nullptr && strcmp(a, "on") != 0) {
      const long mins = strtol(a, nullptr, 10);
      if (mins <= 0) { result = "maint takes <minutes>, 'on' or 'off'"; return false; }
      ms = static_cast<uint32_t>(mins) * 60000UL;
    }
    const bool ok = beginMaintenance(millis(), ms, msg);
    result = msg;
    return ok;
  }
  if (strcmp(verb, "lock") == 0 || strcmp(verb, "unlock") == 0) {
    const bool want = (verb[0] == 'l');
    g_locked = want;
    BleScanner::storeLock(want);
    if (want) {
      // Say what it means rather than just that it happened. Someone reading
      // this back on a dashboard days later needs to know an animal outside
      // cannot let itself in.
      result = "LOCKED — the beacon can no longer open this door; "
               "`door open` still can";
    } else {
      result = "unlocked — the beacon controls the door again";
    }
    return true;
  }
  if (strcmp(verb, "reboot") == 0) {
    // Deferred like the beacon-list restart, so the acknowledgement gets out
    // before the reboot takes it with it.
    g_restartAtMs = millis() + REMOTE_RESTART_DELAY_MS;
    if (g_restartAtMs == 0) g_restartAtMs = 1;
    WifiLogger::requestFlushNow();
    result = "rebooting shortly";
    return true;
  }
  if (strcmp(verb, "scan") == 0) {
    // Upload the discovery table. Without this a beacon can only be changed to
    // one whose address you already know — and the way you learn an address is
    // the discovery table, which until now only existed on the console.
    g_scanRequested = true;
    WifiLogger::requestFlushNow();
    result = "discovery table queued for upload";
    return true;
  }
  if (strcmp(verb, "resetstats") == 0) {
    g_tracker.reset();
    result = "proximity stats reset";
    return true;
  }
  if (strcmp(verb, "defaults") == 0) {
    // The way back from a bad remote setting, without a ladder.
    BleScanner::clearStoredThresholds();
    BleScanner::clearStoredTiming();
    BleScanner::clearStoredFilter();
    BleScanner::clearStoredFastFilter();
    g_tracker.setThresholds(RSSI_ENTER_DBM, RSSI_EXIT_DBM);
    g_tracker.setDwell(ENTER_CONFIRM_MS, EXIT_CONFIRM_MS);
    g_tracker.setFilter(RSSI_MEDIAN_WINDOW, RSSI_EWMA_ALPHA);
    g_tracker.setFastFilter(RSSI_FAST_WINDOW, RSSI_FAST_ALPHA);
    g_door.setMinIntervalMs(MIN_ACTUATION_INTERVAL_MS);
    g_door.setDirectionGapMs(DIRECTION_CHANGE_GAP_MS);
    g_door.setPulseMs(RELAY_PULSE_MS);
    g_door.setPulseTrain(RELAY_PULSE_COUNT, RELAY_PULSE_GAP_MS);
    Actuator::setTravelMs(DOOR_TRAVEL_OPEN_MS, DOOR_TRAVEL_CLOSE_MS);
    BleScanner::storeTravelPair(DOOR_TRAVEL_OPEN_MS, DOOR_TRAVEL_CLOSE_MS);
    Actuator::setWakeIdleMs(WAKE_IDLE_MS);
    Actuator::setRetryPolicy(CLOSE_RETRY_DELAY_MS, CLOSE_RETRY_LIMIT);
    BleScanner::storeActuation(WAKE_IDLE_MS, CLOSE_RETRY_DELAY_MS, CLOSE_RETRY_LIMIT);
    // Cleared too, because a door parked open by exhausted close attempts is
    // exactly the state somebody reaches for `defaults` from, and leaving it
    // set would make the command look like it had done nothing.
    Actuator::clearGaveUp();
    g_thresholdsStored = g_timingStored = g_filterStored = g_fastFilterStored = false;
    // Neither the lock nor the buzzer pin is cleared here. `defaults` is for undoing a
    // bad tuning change; silently unlocking a door as a side effect of that
    // would be a surprise in the one direction that matters.
    result = "reverted to compiled-in defaults (beacon MACs and lock kept)";
    return true;
  }
  result = String("unknown command: ") + verb;
  return false;
}

// Which acknowledgement a command earns.
//
// Only SETTINGS land here. A command that moves the door is announced by
// announceMovement() at the moment it is commanded, with the tune that says
// the network asked for it — so `door open` must NOT also get an ack, or the
// two would cut each other off.
//
// The distinction that matters from the coop is "the door is about to move"
// versus "the door took a note", so everything that takes a note shares one
// short blip and only the lock gets its own.
ChimeTune ackTuneFor(const char *command) {
  if (command == nullptr) return CHIME_ACK_SET;
  if (strncmp(command, "door ", 5) == 0) return CHIME_NONE;  // already announced
  if (strcmp(command, "lock") == 0) return CHIME_ACK_LOCK;
  if (strcmp(command, "unlock") == 0) return CHIME_ACK_UNLOCK;
  return CHIME_ACK_SET;
}

// Drain whatever the last upload brought back. Bounded per tick so a long
// batch cannot hold up the door.
void serviceRemoteCommands() {
  char line[REMOTE_CMD_MAX_LEN];
  int applied = 0, failed = 0;
  String last;
  ChimeTune ack = CHIME_NONE;
  bool sounded = false;
  for (int i = 0; i < 4 && WifiLogger::popCommand(line); i++) {
    String result;
    const bool ok = applyRemoteCommand(line, result);
    Con.printf("[cmd] %s -> %s\r\n", line, result.c_str());
    if (ok) {
      applied++;
      // `beep` already sounds; a second tune on top would cut it off.
      if (strcmp(line, "beep") == 0) sounded = true;
      else {
        const ChimeTune t = ackTuneFor(line);
        // CHIME_NONE means the command has already announced itself — a door
        // movement. Leaving `ack` alone keeps the batch's ack from replacing
        // the movement tune that is still playing.
        if (t != CHIME_NONE) ack = t;
        else sounded = true;
      }
    } else {
      failed++;
    }
    last = result;
  }

  // One sound per batch, not one per command: several commands arrive together
  // and a burst of overlapping tunes would say less than a single clear one.
  // A refusal always wins — "something did not take" is the thing you need to
  // know, and it is worth hearing over a confirmation of the rest.
  if (failed) Chime::play(CHIME_REFUSED);
  else if (ack != CHIME_NONE && !sounded) Chime::play(ack);
  if (applied || failed) {
    String ack = String(applied) + " applied";
    if (failed) ack += String(", ") + failed + " refused: " + last;
    WifiLogger::setAck(ack.c_str());

    // Report the RESULT straight away, rather than at the next natural upload.
    //
    // Two things were wrong without this. The snapshot is rebuilt on a
    // five-second timer, so an upload could carry a status taken before the
    // command was applied — the dashboard would say the command succeeded and
    // show the old value beside it. And worse: `door open` makes the door
    // non-idle, and the idle gate is what permits an upload at all, so opening
    // the door from the web stopped the door reporting until it closed again
    // or the half-hour heartbeat fired. The one action whose result you most
    // want to see was the one that silenced the channel.
    //
    // requestFlushNow() overrides the idle gate, which is exactly right here:
    // the radio burst is the point, and it is one burst per batch of commands,
    // not a new polling rate.
    publishStatusLines();
    WifiLogger::requestFlushNow();
  }
}
#endif  // REMOTE_CONFIG && PETDOOR_ENABLE_WIFI

void controlTask(void *) {
  for (;;) {
    // 1. Wait for a sample, or for the tick to expire — whichever comes first.
    //    Blocking here instead of sleeping a fixed CONTROL_TICK_MS is what makes
    //    the response immediate: the task wakes the moment the radio hears the
    //    beacon, rather than up to a full tick later. The timeout still fires
    //    when the beacon is silent, so a disappearing beacon is still noticed.
    BleSample sample;
    if (BleScanner::waitForSample(sample, CONTROL_TICK_MS)) {
      // The raw reading, not the filtered one: calibration is about what the
      // radio actually delivers at this mounting. Filtering is a choice made
      // afterwards, and folding it in here would hide the spikes that decide
      // where a threshold can safely go.
      //
      // Fed from BOTH paths. The drain loop below only has anything in it when
      // samples arrived faster than the tick, which at ~2 Hz is almost never —
      // so feeding only the loop meant the distribution stayed empty.
      g_tracker.addSample(sample.rssi, sample.measuredPower, sample.atMs);
      Maintenance::addSample(sample.atMs, sample.rssi);
      while (BleScanner::popSample(sample)) {
        g_tracker.addSample(sample.rssi, sample.measuredPower, sample.atMs);
        Maintenance::addSample(sample.atMs, sample.rssi);
      }
    }

    const uint32_t now = millis();

    // 2. Re-evaluate presence. Must run even with no new samples — that is how
    //    a beacon that has gone silent gets noticed.
    g_tracker.update(now);

    serviceMaintenance(now);

#if REMOTE_CONFIG && PETDOOR_ENABLE_WIFI
    // Anything the server sent back with the last upload. Applied here rather
    // than on the WiFi task because the door has one owner.
    serviceRemoteCommands();

    if (g_scanRequested) {
      g_scanRequested = false;
      StringStream out;
      BleScanner::dumpTable(out, now);
      WifiLogger::queueScanUpload(out.text);
    }

    // A remote change that needs a reboot (a new beacon list) asked for one.
    // Wait for the uploader to go quiet first so the acknowledgement gets out,
    // and never reboot with the door open on a present animal.
    if (g_restartAtMs != 0 && static_cast<int32_t>(now - g_restartAtMs) >= 0 &&
        !WifiLogger::busy()) {
      Con.println(F("[cmd] restarting to apply a new beacon list"));
      Con.flush();
      delay(200);
      ESP.restart();
    }
#endif

    // 3. Keep the radio alive.
    BleScanner::serviceWatchdog(now);
    const bool scanHealthy = BleScanner::advAgeMs(now) < SCAN_WATCHDOG_MS;

    // 4. Act, then report. Reporting last means a state change is announced on
    //    the same tick it happens rather than one tick later.
    //
    //    The order within this block matters. Vibration and the limit switches
    //    are sampled BEFORE the actuator ticks, because they are the evidence
    //    it decides on; the actuator's conclusions are drained immediately
    //    AFTER it, because resolve() keeps exactly one result and a second
    //    attempt starting in the next tick would overwrite an unread one.
    Vibration::tick(now);
    updatePosition(now);
    Actuator::tick(now);
    drainActuatorNotes();
    {
      Actuator::Result res;
      while (Actuator::consumeResult(res)) reportOutcome(res);
    }
    reportCalibration();
    driveDoor(now);
    updateLed(now, scanHealthy);
    updateChime(now);
    if (g_entry == ENTRY_NONE) reportTransitions(now, scanHealthy);

    // 4b. Offer the uploader a window. "Idle" means the animal is not around
    //     and the door is shut, so sharing the antenna cannot cost us a
    //     detection that matters. See wifi_logger.h.
    // Nothing can actuate during a maintenance window, so sharing the antenna
    // cannot cost a detection that matters — and the network console needs the
    // radio up for the whole window. Without this the ordinary rule would hand
    // BLE the antenna precisely when the collar is held at the door, which is
    // the one time calibration needs the link.
    // `!Actuator::busy()` keeps the radio off the air while a travel is being
    // verified. Association takes seconds of radio-intensive work, and a
    // vibration count or a limit-switch arrival missed because the WiFi task
    // had the antenna would turn a good travel into a reported stall — which
    // for a close means reversing a door that was closing perfectly well.
    const bool idle = (Maintenance::active(now) ||
                       (!g_tracker.isPresent() && g_door.state() != DOOR_OPEN &&
                        g_entry == ENTRY_NONE && !WifiLogger::otaWindowOpen())) &&
                      !Actuator::busy();
#if PETDOOR_ENABLE_WIFI
    static uint32_t lastStatusMs = 0;
    if (now - lastStatusMs >= 5000) {
      lastStatusMs = now;
      publishStatusLines();
    }
#endif
    WifiLogger::tick(now, idle);

    // 5. Diagnostics.
    handleSerial(now);

    // Periodic output is suppressed while the MAC editor is open — otherwise
    // the table scrolls the prompt off screen and you cannot see what you are
    // typing. Discovery keeps running; only the printing pauses.
    if (g_entry == ENTRY_NONE && BleScanner::discoverEnabled() &&
        (now - g_lastDiscoverDumpMs) >= DISCOVER_DUMP_INTERVAL_MS) {
      g_lastDiscoverDumpMs = now;
      BleScanner::dumpTable(Serial, now);
    }

    if (g_entry == ENTRY_NONE && g_calibrate && (now - g_lastCalibrateMs) >= CALIBRATE_INTERVAL_MS) {
      g_lastCalibrateMs = now;
      if (g_tracker.hasFix(now)) {
        Con.printf("[cal] raw %4d  filtered %4d dBm  ~%s m  %s\r\n", g_tracker.rawRssi(),
                      g_tracker.filteredRssi(), fmt1(g_tracker.distanceM()).c_str(),
                      g_tracker.isPresent() ? "PRESENT" : "absent");
      } else {
        Con.printf("[cal] no fix (last seen %lu ms ago)\r\n",
                      static_cast<unsigned long>(g_tracker.sampleAgeMs(now)));
      }
    }

    // No sleep here: the wait at the top of the loop is the pacing mechanism.
  }
}

}  // namespace

void setup() {
  Con.begin(SERIAL_BAUD);
  delay(200);

  // Relays first: get the outputs into a known-safe state before anything else
  // can take time or fail.
  g_door.begin();
  g_tracker.begin();
  // After g_door.begin(), which puts the relays in a known-safe state, and
  // before anything can ask for a travel.
  Actuator::begin(&g_door);
  recordBoot();
  EventLog::begin(static_cast<uint16_t>(g_bootCount));
  Schedule::begin();
  {
    // NVS wins over the compiled-in pin, like the buzzer and the switches: the
    // pin is a property of this board's wiring, and somebody who found it once
    // by trying should not have to find it again after a power cut.
    int vibPin = PIN_VIBRATION;
    bool vibLow = VIBRATION_ACTIVE_LOW != 0;
    BleScanner::loadStoredVibration(vibPin, vibLow);
    Vibration::begin(vibPin, vibLow);
  }
  EventLog::record(LOG_BOOT, static_cast<uint8_t>(esp_reset_reason()), 0);

  {
    int e = 0, x = 0;
    if (BleScanner::loadStoredThresholds(e, x) && g_tracker.setThresholds(e, x)) {
      g_thresholdsStored = true;
    }
  }
  {
    uint8_t w = RSSI_MEDIAN_WINDOW;
    float a = RSSI_EWMA_ALPHA;
    if (BleScanner::loadStoredFilter(w, a) && g_tracker.setFilter(w, a)) {
      g_filterStored = true;
    }
  }
  {
    // Loaded after the slow pair, because setFilter() calls reset() and would
    // otherwise wipe the fast filter's re-seeded value.
    uint8_t w = RSSI_FAST_WINDOW;
    float a = RSSI_FAST_ALPHA;
    if (BleScanner::loadStoredFastFilter(w, a) && g_tracker.setFastFilter(w, a)) {
      g_fastFilterStored = true;
    }
  }
  {
    uint32_t en = ENTER_CONFIRM_MS, ex = EXIT_CONFIRM_MS, lk = MIN_ACTUATION_INTERVAL_MS;
    if (BleScanner::loadStoredTiming(en, ex, lk) && lk < ex) {
      g_tracker.setDwell(en, ex);
      g_door.setMinIntervalMs(lk);
      g_timingStored = true;
    }
    const uint32_t savedPulse = BleScanner::loadStoredPulseMs();
    if (savedPulse != 0 && g_door.setPulseMs(savedPulse)) {
      g_timingStored = true;
    }
    uint8_t pn = RELAY_PULSE_COUNT;
    uint32_t pg = RELAY_PULSE_GAP_MS;
    if (BleScanner::loadStoredPulseTrain(pn, pg) && g_door.setPulseTrain(pn, pg)) {
      g_timingStored = true;
    }
    const uint32_t gap = BleScanner::loadStoredDirectionGap();
    if (gap) g_door.setDirectionGapMs(gap);   // floor enforced inside

    // Travel time, the wake threshold and the retry policy are all loaded by
    // Actuator::begin(), which is the only thing that uses them.
    {
      uint32_t t = 0;
      if (BleScanner::loadStoredTravelPair(t, t) ||
          BleScanner::loadStoredTravelMs(t)) {
        g_timingStored = true;
      }
      uint32_t w = 0, r = 0;
      uint8_t n = 0;
      if (BleScanner::loadStoredActuation(w, r, n)) g_timingStored = true;
    }

    uint32_t upSt = WIFI_IDLE_SETTLE_MS, upMi = WIFI_MIN_UPLOAD_INTERVAL_MS,
             upHb = WIFI_HEARTBEAT_MS;
    if (BleScanner::loadStoredUpload(upSt, upMi, upHb)) {
      WifiLogger::setUploadTiming(upSt, upMi, upHb);
      g_timingStored = true;
    }
  }

  {
    // The annunciator, after the relays so that a saved pin colliding with one
    // of them can be rejected against the pins the door has already claimed.
    int bp = PIN_BUZZER;
    bool bpassive = BUZZER_PASSIVE != 0;
    bool blow = BUZZER_ACTIVE_LOW != 0;
    const bool stored = BleScanner::loadStoredChime(bp, bpassive, blow);
    if (!Chime::configure(bp, bpassive, blow)) {
      Con.printf("[chime] refusing GPIO %d: %s\r\n", bp,
                    Chime::pinProblem(bp) ? Chime::pinProblem(bp) : "invalid");
      Con.println(F("[chime] annunciator disabled; 'w' then 'buzzer <pin>' to fix"));
      Chime::configure(-1, false, false);
    } else if (stored && Chime::enabled()) {
      const char *warn = Chime::pinProblem(Chime::pin());
      if (warn) Con.printf("[chime] GPIO %d: %s\r\n", Chime::pin(), warn);
    }
  }

  {
    // The limit switches, after the buzzer so a saved pin that collides with
    // it is refused rather than silently fighting over the GPIO.
    int so = PIN_SENSOR_OPEN, sc = PIN_SENSOR_CLOSED;
    bool sl = SENSOR_ACTIVE_LOW != 0;
    BleScanner::loadStoredSensors(so, sc, sl);
    if (!Position::configure(so, sc, sl)) {
      Con.printf("[pos] refusing GPIO %d/%d — check 'w' then 'sensors'\r\n", so, sc);
      Position::configure(-1, -1, true);
    }
  }

  // Said loudly because it survives a reboot by design, and a door that will
  // not open for the collar is exactly the thing someone needs to be told
  // about before they start wondering why the animal is outside.
  g_locked = BleScanner::loadStoredLock();
  if (g_locked) {
    Con.println(F("  !! THIS DOOR IS LOCKED — the beacon cannot open it"));
    Con.println(F("  !! 'K' unlocks, or queue `unlock` from the log server"));
  }

  if (!BleScanner::begin()) {
    Con.println(F("[fatal] BLE scanner failed to start; rebooting in 5 s"));
    delay(5000);
    ESP.restart();
  }

  printBanner();

  WifiLogger::setBootCount(g_bootCount);
  WifiLogger::begin();
  xTaskCreatePinnedToCore(controlTask, "petdoor", CONTROL_TASK_STACK, nullptr, 1,
                          &g_controlTaskHandle, 1);
  Con.println(F("[system] running"));
}

void loop() {
  // All work happens in controlTask, which owns the door and the filter.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
