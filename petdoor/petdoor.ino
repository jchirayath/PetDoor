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

#include "ble_scanner.h"
#include "chime.h"
#include "config.h"
#include "door.h"
#include "eventlog.h"
#include "position.h"
#include "proximity.h"
#include "wifi_logger.h"

namespace {

ProximityTracker g_tracker;
DoorController g_door;

// How long the door is believed to take to travel, and whether it is believed
// to be travelling right now. Open-loop: this is a stopwatch started by an
// actuation, not a position sensor. See DOOR_TRAVEL_MS in config.h.
uint32_t g_travelMs = DOOR_TRAVEL_MS;
bool g_travelling = false;
bool g_lockRefusedAnnounced = false;
bool g_travelVerified = false;      // did a switch confirm the last travel?
bool g_workingPending = false;      // travel started; waiting for an ack to finish
DoorState g_lastObserved = DOOR_UNKNOWN;
bool g_sensorFaultAnnounced = false;

// Is the door believed to be moving?
//
// OPEN LOOP. This is a stopwatch started by the last actuation, not a position
// sensor: it reports "moving" for g_travelMs after a relay pulse whether or not
// anything actually moved, and it will report "arrived" for a door jammed
// halfway. It exists to turn fifteen seconds of silence into fifteen seconds of
// visible and audible "yes, I heard you" — nothing more. A limit switch is the
// only thing that could make this a measurement; see docs/SAFETY.md.
bool doorTravelling(uint32_t nowMs) {
  if (g_travelMs == 0 || !g_door.hasActuated()) return false;
  return (nowMs - g_door.lastActuationMs()) < g_travelMs;
}


bool g_calibrate = false;
uint32_t g_lastDiscoverDumpMs = 0;
uint32_t g_lastCalibrateMs = 0;
DoorState g_lastReportedDoorState = DOOR_UNKNOWN;
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
  Serial.println(F("[sys] rebooting into UART download mode."));
  Serial.println(F("[sys]   * the door is NOT controlled until you flash"));
  Serial.println(F("[sys]   * quit your terminal first, then upload"));
  Serial.println(F("[sys]   * power-cycle the board to cancel"));
  Serial.flush();
  delay(400);
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
  esp_restart();
#else
  Serial.println(F("[sys] This chip (original ESP32) has no software"));
  Serial.println(F("[sys] download-mode flag — only the S3/C3/C6 do."));
  Serial.println(F("[sys] To flash: hold IO0, tap EN, release IO0."));
  Serial.println(F("[sys] To avoid that for good, wire the adapter's"));
  Serial.println(F("[sys] DTR->IO0 and RTS->EN so uploads reset the board."));
#endif
}

enum EntryMode { ENTRY_NONE, ENTRY_MAC, ENTRY_THRESH, ENTRY_TIMING, ENTRY_FILTER };
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
  Serial.println();
  Serial.println(F("=================================================="));
  Serial.println(F("  PetDoor — BLE proximity door controller"));
  Serial.printf("  v%s  (built %s, %s)\r\n", PETDOOR_VERSION, PETDOOR_BUILD,
                BleScanner::stackName());
  Serial.println(F("=================================================="));
  Serial.print(F("  target beacon : "));
  Serial.println(BleScanner::describeTarget());
  Serial.printf("  open / close  : GPIO %d / GPIO %d (active %s)\r\n", PIN_RELAY_OPEN,
                PIN_RELAY_CLOSE, RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
  Serial.printf("  relay pulse   : %lu ms\r\n",
                static_cast<unsigned long>(g_door.pulseMs()));
  if (g_door.pulseCount() > 1) {
    Serial.printf("  presses       : %u per actuation, %lu ms apart\r\n",
                  g_door.pulseCount(),
                  static_cast<unsigned long>(g_door.pulseGapMs()));
    // Worth saying out loud: this is a blind retry, and the failure it can
    // introduce looks nothing like the one it fixes.
    Serial.println(F("  !! repeat presses are sent blind — the door cannot tell whether"));
    Serial.println(F("  !! the first worked. If it did, the second may stop it mid-travel."));
  }
  if (g_travelMs > 0) {
    Serial.printf("  door travel   : %lu ms (announced on the LED and buzzer)\r\n",
                  static_cast<unsigned long>(g_travelMs));
    // Warned about at boot rather than static_assert'ed, because both values
    // are runtime-adjustable and can be changed long after compiling.
    if (g_door.minIntervalMs() < g_travelMs) {
      Serial.printf("  !! min interval (%lu ms) is SHORTER than door travel (%lu ms).\r\n",
                    static_cast<unsigned long>(g_door.minIntervalMs()),
                    static_cast<unsigned long>(g_travelMs));
      Serial.println(F("  !! A reversing command can land mid-travel; most controllers"));
      Serial.println(F("  !! read that as STOP, leaving the door parked half open."));
      Serial.println(F("  !! Raise it with 'w', or set MIN_ACTUATION_INTERVAL_MS."));
    }
  }
  Serial.printf("  status LED    : GPIO %d\r\n", PIN_STATUS_LED);
  if (Chime::enabled()) {
    Serial.printf("  buzzer        : GPIO %d, %s, active %s\r\n", Chime::pin(),
                  Chime::passive() ? "passive (tones)" : "active (fixed pitch)",
                  Chime::activeLow() ? "LOW" : "HIGH");
  } else {
    Serial.println(F("  buzzer        : none ('w' then 'buzzer <pin>' to add one)"));
  }
  if (Position::enabled()) {
    Serial.printf("  limit switches: open GPIO %d, closed GPIO %d, active %s\r\n",
                  Position::openPin(), Position::closedPin(),
                  Position::activeLow() ? "LOW" : "HIGH");
    Serial.printf("  door is really: %s\r\n",
                  DoorController::stateName(Position::state()));
  } else {
    Serial.println(F("  limit switches: none — the door is OPEN LOOP and only knows"));
    Serial.println(F("                  what it commanded ('w' then 'sensors' to add)"));
  }
  Serial.printf("  thresholds    : open at >= %d dBm, close at <= %d dBm (%s)\r\n",
                g_tracker.enterDbm(), g_tracker.exitDbm(),
                g_thresholdsStored ? "saved on device" : "compiled in");
  Serial.printf("  dwell         : open after %lu ms near, close after %lu ms far (%s)\r\n",
                static_cast<unsigned long>(g_tracker.enterConfirmMs()),
                static_cast<unsigned long>(g_tracker.exitConfirmMs()),
                g_timingStored ? "saved on device" : "compiled in");
  Serial.printf("  scan          : %d ms window / %d ms interval, %s, duplicates ON\r\n",
                SCAN_WINDOW_MS, SCAN_INTERVAL_MS, SCAN_ACTIVE ? "active" : "passive");
  Serial.printf("  boot          : #%lu, last reset: %s\r\n",
                static_cast<unsigned long>(g_bootCount), resetReasonName());
  Serial.println(F("--------------------------------------------------"));

  if (esp_reset_reason() == ESP_RST_BROWNOUT) {
    Serial.println();
    Serial.println(F("  !! Last restart was a BROWNOUT — the supply sagged. !!"));
    Serial.println(F("  Use a thicker USB cable, a stronger 5 V supply, or feed"));
    Serial.println(F("  the relay coils from their own supply with a common ground."));
    Serial.println();
  }

  if (!BleScanner::isConfigured()) {
    Serial.println();
    Serial.println(F("  !! NO BEACON CONFIGURED — the door will not be operated. !!"));
    Serial.println(F("  Discovery mode is on. Find your beacon below, then set"));
    Serial.println(F("  BEACON_MAC in secrets.h (copy secrets.example.h) and reflash."));
    Serial.println(F("  A Minew beacon usually shows up as class Minew or iBeacon."));
    Serial.println();
  }

  Serial.println(F("  Type 'h' for commands."));
  Serial.println();
}

void printHelp() {
  Serial.println(F("commands:"));
  Serial.println(F("  h  this help"));
  Serial.println(F("  s  status"));
  Serial.println(F("  d  toggle discovery mode (list every BLE device in range)"));
  Serial.println(F("  c  toggle calibration stream (live RSSI + distance)"));
  Serial.println(F("  r  reset the proximity filter"));
  Serial.println(F("  l  show the event log (what the door actually did)"));
  Serial.println(F("  u  upload the log now over WiFi (if configured)"));
  Serial.println(F("  p  open a firmware update window (OTA, no buttons)"));
  Serial.println(F("  m  edit the beacon MAC list (saved on the device)"));
  Serial.println(F("  t  edit the open/close thresholds (saved on the device)"));
  Serial.println(F("  w  edit the dwell times / how fast it reacts"));
  Serial.println(F("  f  edit the filter shape (median window, smoothing)"));
#if PETDOOR_CAN_REBOOT_TO_FLASH
  Serial.println(F("  !  reboot into flash mode (no IO0/EN buttons needed)"));
#else
  Serial.println(F("  !  flash-mode help (this chip needs the IO0/EN buttons)"));
#endif
#if ALLOW_MANUAL_SERIAL_CONTROL
  Serial.println(F("  o  pulse the OPEN relay now, and hold it open (see 'O')"));
  Serial.println(F("  x  pulse the CLOSE relay now, and clear any hold"));
  Serial.println(F("  O  clear the manual hold, handing control back to the beacon"));
  Serial.println(F("  k  LOCK — the beacon may no longer open the door"));
  Serial.println(F("  K  unlock"));
#endif
  Serial.println(F("status LED:"));
  Serial.println(F("  solid        door last OPENED"));
  Serial.println(F("  brief blip   door last CLOSED"));
  Serial.println(F("  near-solid   door MOVING (for the configured travel time)"));
  Serial.println(F("  1 Hz blink   no beacon configured / never heard"));
  Serial.println(F("  2 Hz flash   beacon battery LOW"));
  Serial.println(F("  5 Hz flutter radio unhealthy"));
  Serial.println(F("buzzer (if fitted — 'w' to configure):"));
  Serial.println(F("  tick .. tick door MOVING"));
  Serial.println(F("  rising pair  travel time is up"));
  Serial.println(F("  low buzz     refused: this door is LOCKED"));
}

void printStatus(uint32_t nowMs) {
  Serial.println(F("---- status ----"));
  Serial.print(F("  target       : "));
  Serial.println(BleScanner::describeTarget());
  Serial.printf("  uptime       : %lu s\r\n", static_cast<unsigned long>(nowMs / 1000));
  Serial.printf("  presence     : %s%s\r\n",
                g_tracker.isPresent() ? "PRESENT" : "ABSENT",
                g_tracker.hasFix(nowMs) ? "" : " (no fix)");
  Serial.printf("  door         : %s\r\n", DoorController::stateName(g_door.state()));

  if (g_tracker.everSeen()) {
    Serial.printf("  rssi         : %d dBm filtered (raw %d), ~%s m\r\n", g_tracker.filteredRssi(),
                  g_tracker.rawRssi(), fmt1(g_tracker.distanceM()).c_str());
    Serial.printf("  last seen    : %lu ms ago, %lu samples\r\n",
                  static_cast<unsigned long>(g_tracker.sampleAgeMs(nowMs)),
                  static_cast<unsigned long>(g_tracker.totalSamples()));
  } else {
    Serial.println(F("  rssi         : beacon has never been heard"));
  }

  const uint32_t enterFor = g_tracker.pendingEnterMs(nowMs);
  const uint32_t exitFor = g_tracker.pendingExitMs(nowMs);
  // Runtime dwell, not the compile-time macro: after a `w` command that
  // lengthens a dwell, the macro is smaller than the elapsed time and the
  // subtraction underflows to ~4.29e9.
  if (enterFor) {
    const uint32_t total = g_tracker.enterConfirmMs();
    Serial.printf("  opening in   : %lu ms\r\n",
                  static_cast<unsigned long>(total > enterFor ? total - enterFor : 0));
  }
  if (exitFor) {
    const uint32_t total = g_tracker.exitConfirmMs();
    Serial.printf("  closing in   : %lu ms\r\n",
                  static_cast<unsigned long>(total > exitFor ? total - exitFor : 0));
  }

  Serial.printf("  radio        : %lu adverts, last %lu ms ago, %lu samples dropped\r\n",
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
        Serial.printf("  beacon batt  : %u mV (~%d%%)%s\r\n", tlm.batteryMv,
                      pct < 0 ? 0 : (pct > 100 ? 100 : pct),
                      tlm.batteryMv < 2400 ? "  << REPLACE SOON" : "");
      } else {
        Serial.println(F("  beacon batt  : not reported (0 mV = mains powered)"));
      }
      Serial.printf("  beacon temp  : %.1f C, up %lu s, %lu adverts sent\r\n",
                    static_cast<double>(tlm.temperatureC),
                    static_cast<unsigned long>(tlm.uptimeSec),
                    static_cast<unsigned long>(tlm.advCount));
    }
  }

  if (g_tracker.minRssi() != 0) {
    Serial.printf("  weakest heard: %d dBm  (%lu samples at/below -85 dBm)\r\n",
                  g_tracker.minRssi(), static_cast<unsigned long>(g_tracker.weakSamples()));
  }
  Serial.printf("  worst gap    : %lu ms between samples%s\r\n",
                static_cast<unsigned long>(g_tracker.maxGapMs()),
                g_tracker.maxGapMs() > SAMPLE_MAX_AGE_MS ? "  << EXCEEDS SAMPLE_MAX_AGE_MS" : "");
  Serial.printf("  relay pulse  : %lu ms\r\n",
                static_cast<unsigned long>(g_door.pulseMs()));
  if (g_travelMs > 0) {
    if (doorTravelling(nowMs)) {
      Serial.printf("  door travel  : MOVING, %lu ms left of %lu (open loop — a timer)\r\n",
                    static_cast<unsigned long>(g_travelMs -
                                               (nowMs - g_door.lastActuationMs())),
                    static_cast<unsigned long>(g_travelMs));
    } else {
      Serial.printf("  door travel  : %lu ms announced after each actuation\r\n",
                    static_cast<unsigned long>(g_travelMs));
    }
  }
  if (Position::enabled()) {
    if (Position::fault()) {
      Serial.println(F("  position     : !! FAULT — both limit switches made at once"));
    } else {
      Serial.printf("  position     : %s (measured)%s\r\n",
                    DoorController::stateName(Position::state()),
                    Position::state() == DOOR_UNKNOWN ? "  << between the two switches" : "");
    }
    Serial.printf("  switches     : open GPIO %d %s, closed GPIO %d %s\r\n",
                  Position::openPin(), Position::openMade() ? "MADE" : "open",
                  Position::closedPin(), Position::closedMade() ? "MADE" : "open");
  } else {
    Serial.println(F("  position     : not measured — no limit switches fitted"));
  }
  if (Chime::enabled()) {
    Serial.printf("  buzzer       : GPIO %d, %s, active %s%s\r\n", Chime::pin(),
                  Chime::passive() ? "passive" : "active",
                  Chime::activeLow() ? "LOW" : "HIGH",
                  Chime::playing() != CHIME_NONE ? "  << sounding now" : "");
  }
  if (g_locked) {
    Serial.println(F("  LOCKED       : the beacon cannot open this door ('K' to unlock)"));
  }
  {
    const uint32_t heldFor = manualHoldRemainingMs(millis());
    if (heldFor > 0) {
      Serial.printf("  manual hold  : %lu s left — automatic CLOSING paused ('O' to clear)\r\n",
                    static_cast<unsigned long>(heldFor / 1000));
    }
  }
  Serial.printf("  actuations   : %lu open, %lu close (%lu locked out, %lu in boot grace)\r\n",
                static_cast<unsigned long>(g_door.openCount()),
                static_cast<unsigned long>(g_door.closeCount()),
                static_cast<unsigned long>(g_door.lockedOutCount()),
                static_cast<unsigned long>(g_door.bootGraceCount()));
  Serial.printf("  scan restarts: %lu\r\n", static_cast<unsigned long>(BleScanner::scanRestarts()));
  Serial.printf("  firmware     : v%s (built %s)\r\n", PETDOOR_VERSION, PETDOOR_BUILD);
  Serial.printf("  ble stack    : %s\r\n", BleScanner::stackName());
  Serial.printf("  boot         : #%lu, last reset: %s\r\n",
                static_cast<unsigned long>(g_bootCount), resetReasonName());
  WifiLogger::printStatus(Serial);
  // Stack headroom, in bytes still unused at the worst moment so far. A task
  // sized much larger than its high-water mark is heap sitting idle; one
  // approaching zero is a crash waiting for the right input.
  if (g_controlTaskHandle) {
    Serial.printf("  task stacks  : control %lu free of %d",
                  static_cast<unsigned long>(
                      uxTaskGetStackHighWaterMark(g_controlTaskHandle) * sizeof(StackType_t)),
                  CONTROL_TASK_STACK);
    const uint32_t up = WifiLogger::stackFreeBytes();
    if (up) Serial.printf(", uploader %lu free of %d", static_cast<unsigned long>(up),
                          WIFI_TASK_STACK);
    Serial.println();
  }
  Serial.printf("  free heap    : %lu bytes (low-water %lu)\r\n",
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMinFreeHeap()));
  Serial.println();
}

void printMacMenu() {
  Serial.println();
  Serial.println(F("---- beacon MAC list ----"));
  const String csv = BleScanner::targetMacsCsv();
  if (csv.length() == 0) {
    Serial.println(F("  (none configured — the door will not be operated)"));
  } else {
    Serial.printf("  in use (%s):\r\n",
                  BleScanner::targetMacsAreStored() ? "saved on device" : "compiled in");
    int idx = 1, from = 0;
    while (from < static_cast<int>(csv.length())) {
      int comma = csv.indexOf(',', from);
      if (comma < 0) comma = csv.length();
      String one = csv.substring(from, comma);
      one.trim();
      if (one.length()) Serial.printf("    %d. %s\r\n", idx++, one.c_str());
      from = comma + 1;
    }
  }
  Serial.println(F("  type one of:"));
  Serial.println(F("    aa:bb:cc:dd:ee:ff,11:22:33:44:55:66   replace the whole list"));
  Serial.println(F("    +aa:bb:cc:dd:ee:ff                    add one"));
  Serial.println(F("    -2                                    remove entry 2"));
  Serial.println(F("    clear                                 forget saved list"));
  Serial.println(F("    q                                     cancel"));
  Serial.println(F("  (live output is paused while this menu is open)"));
  Serial.print(F("> "));
}

void saveAndRestart(const char *csv) {
  String err;
  if (!BleScanner::storeTargetMacs(csv, err)) {
    Serial.printf("\r\n[mac] rejected: %s\r\n", err.c_str());
    Serial.println(F("[mac] nothing was changed."));
    printMacMenu();
    return;
  }
  Serial.printf("\r\n[mac] saved: %s\r\n", csv);
  Serial.println(F("[mac] restarting to apply..."));
  Serial.flush();
  delay(250);
  ESP.restart();
}

void processMacLine(char *line) {
  // trim both ends
  while (*line == ' ' || *line == '\t') line++;
  int n = strlen(line);
  while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';

  if (n == 0 || strcmp(line, "q") == 0) {
    Serial.println(F("\r\n[mac] cancelled, nothing changed."));
    Serial.println(F("[mac] live output resumed."));
    g_entry = ENTRY_NONE;
    return;
  }

  if (strcmp(line, "clear") == 0) {
    BleScanner::clearStoredTargetMacs();
    Serial.println(F("\r\n[mac] saved list forgotten; reverting to the compiled-in default."));
    Serial.println(F("[mac] restarting to apply..."));
    Serial.flush();
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
      Serial.println(F("\r\n[mac] give an entry number, e.g. -2"));
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
      Serial.printf("\r\n[mac] there is no entry %d\r\n", target);
      printMacMenu();
      return;
    }
    if (kept.length() == 0) {
      // Removing the last one means "use the compiled-in default" rather than
      // leaving a list that cannot be stored.
      BleScanner::clearStoredTargetMacs();
      Serial.println(F("\r\n[mac] last entry removed; reverting to the compiled-in default."));
      Serial.println(F("[mac] restarting to apply..."));
      Serial.flush();
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
  Serial.println();
  Serial.println(F("---- proximity thresholds ----"));
  Serial.printf("  open when  >= %4d dBm  (~%s m)\r\n", g_tracker.enterDbm(),
                fmt1(distanceForRssi(g_tracker.enterDbm())).c_str());
  Serial.printf("  close when <= %4d dBm  (~%s m)\r\n", g_tracker.exitDbm(),
                fmt1(distanceForRssi(g_tracker.exitDbm())).c_str());
  Serial.printf("  source     : %s\r\n", g_thresholdsStored ? "saved on device" : "compiled in");
  if (g_tracker.hasFix(millis())) {
    Serial.printf("  beacon now : %d dBm  (~%s m)\r\n", g_tracker.filteredRssi(),
                  fmt1(g_tracker.distanceM()).c_str());
  } else {
    Serial.println(F("  beacon now : no fix"));
  }
  Serial.println(F("  type one of:"));
  Serial.println(F("    here        use where the beacon is RIGHT NOW as the open point"));
  Serial.println(F("    1m          open within 1 m, close beyond ~2.5x that"));
  Serial.println(F("    1m,3m       open within 1 m, close beyond 3 m"));
  Serial.println(F("    -59,-69     set directly in dBm (open,close)"));
  Serial.println(F("    clear       forget saved values"));
  Serial.println(F("    q           cancel"));
  Serial.println(F("  (live output is paused while this menu is open)"));
  Serial.print(F("> "));
}

void applyThresholds(int enterDbm, int exitDbm) {
  if (!g_tracker.setThresholds(enterDbm, exitDbm)) {
    Serial.printf("\r\n[thr] rejected: open (%d) must be greater than close (%d).\r\n",
                  enterDbm, exitDbm);
    Serial.println(F("[thr] the gap between them IS the hysteresis band."));
    printThresholdMenu();
    return;
  }
  BleScanner::storeThresholds(enterDbm, exitDbm);
  g_thresholdsStored = true;
  Serial.printf("\r\n[thr] saved: open >= %d dBm (~%s m), close <= %d dBm (~%s m)\r\n",
                enterDbm, fmt1(distanceForRssi(enterDbm)).c_str(),
                exitDbm, fmt1(distanceForRssi(exitDbm)).c_str());
  Serial.println(F("[thr] active immediately; no restart needed."));
  Serial.println(F("[thr] live output resumed."));
  g_entry = ENTRY_NONE;
}

void processThresholdLine(char *line) {
  while (*line == ' ' || *line == '\t') line++;
  int n = strlen(line);
  while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';

  if (n == 0 || strcmp(line, "q") == 0) {
    Serial.println(F("\r\n[thr] cancelled, nothing changed."));
    Serial.println(F("[thr] live output resumed."));
    g_entry = ENTRY_NONE;
    return;
  }

  if (strcmp(line, "clear") == 0) {
    BleScanner::clearStoredThresholds();
    g_tracker.setThresholds(RSSI_ENTER_DBM, RSSI_EXIT_DBM);
    g_thresholdsStored = false;
    Serial.println(F("\r\n[thr] reverted to the compiled-in defaults."));
    g_entry = ENTRY_NONE;
    return;
  }

  // "here" — calibrate from where the beacon is sitting. This is the most
  // reliable option, because it needs no path-loss model at all: it uses the
  // signal actually observed at the spot you care about.
  if (strcmp(line, "here") == 0) {
    if (!g_tracker.hasFix(millis())) {
      Serial.println(F("\r\n[thr] no fix — the beacon is not being heard right now."));
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
      Serial.println(F("\r\n[thr] need 0 < open < close, e.g. 1m,3m"));
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
    Serial.println(F("\r\n[thr] give two values, e.g. -59,-69 or 1m,3m"));
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
bool applyTravelMs(const char *arg, String &msg) {
  uint32_t ms = 0;
  if (!parseBoundedMs(arg, 0, kMaxTravelMs, ms)) {
    msg = String("travel rejected: 0-") + kMaxTravelMs + " ms (0 = do not announce)";
    return false;
  }
  g_travelMs = ms;
  BleScanner::storeTravelMs(ms);
  g_timingStored = true;
  if (ms == 0) {
    msg = F("travel 0: the door no longer announces that it is moving");
    // Leaving the buzzer mid-pattern here would strand a tone on the pin.
    g_travelling = false;
    Chime::stop();
    return true;
  }
  msg = String("travel ") + ms + " ms";
  if (g_door.minIntervalMs() < ms) {
    msg += "; WARNING min interval (";
    msg += g_door.minIntervalMs();
    msg += " ms) is shorter, so a reversal can land mid-travel";
  }
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

void printTimingMenu() {
  Serial.println();
  Serial.println(F("---- dwell / timing ----"));
  Serial.printf("  open after   : %5lu ms near\r\n",
                static_cast<unsigned long>(g_tracker.enterConfirmMs()));
  Serial.printf("  close after  : %5lu ms far\r\n",
                static_cast<unsigned long>(g_tracker.exitConfirmMs()));
  Serial.printf("  min interval : %5lu ms between actuations (CLOSING only)\r\n",
                static_cast<unsigned long>(g_door.minIntervalMs()));
  Serial.printf("  interlock gap: %5lu ms before any relay fires\r\n",
                static_cast<unsigned long>(g_door.directionGapMs()));
  Serial.printf("  relay pulse  : %5lu ms held closed (the \"button press\")\r\n",
                static_cast<unsigned long>(g_door.pulseMs()));
  if (g_door.pulseCount() > 1) {
    Serial.printf("  presses      : %u per actuation, %lu ms apart\r\n",
                  g_door.pulseCount(),
                  static_cast<unsigned long>(g_door.pulseGapMs()));
  }
  Serial.printf("  calls in     : quiet %lu ms, min gap %lu ms, heartbeat %lu min\r\n",
                static_cast<unsigned long>(WifiLogger::settleMs()),
                static_cast<unsigned long>(WifiLogger::minIntervalMs()),
                static_cast<unsigned long>(WifiLogger::heartbeatMs() / 60000));
  Serial.printf("  door travel  : %5lu ms (0 = do not announce)\r\n",
                static_cast<unsigned long>(g_travelMs));
  if (Chime::enabled()) {
    Serial.printf("  buzzer       : GPIO %d, %s, active %s\r\n", Chime::pin(),
                  Chime::passive() ? "passive" : "active",
                  Chime::activeLow() ? "LOW" : "HIGH");
  } else {
    Serial.println(F("  buzzer       : none"));
  }
  if (Position::enabled()) {
    Serial.printf("  sensors      : open GPIO %d, closed GPIO %d, active %s — reads %s\r\n",
                  Position::openPin(), Position::closedPin(),
                  Position::activeLow() ? "LOW" : "HIGH",
                  Position::fault() ? "FAULT: both made"
                                    : DoorController::stateName(Position::state()));
  } else {
    Serial.println(F("  sensors      : none (open loop)"));
  }
  Serial.printf("  source       : %s\r\n", g_timingStored ? "saved on device" : "compiled in");
  Serial.println();
  Serial.printf("  MEASURED worst gap between samples: %lu ms\r\n",
                static_cast<unsigned long>(g_tracker.maxGapMs()));
  Serial.println(F("  A close dwell at or below that WILL close on a routine"));
  Serial.println(F("  radio dropout, with the beacon still present."));
  Serial.println(F("  type one of:"));
  Serial.println(F("    1000,3000,2000   open ms, close ms, min interval ms"));
  Serial.println(F("    fast             1000 / 3000 / 2000  (responsive)"));
  Serial.println(F("    safe             1500 / 15000 / 5000 (the default)"));
  Serial.println(F("    gap 250          relay interlock dead time, ms (min 100)"));
  Serial.println(F("    pulse 200        how long the relay stays closed, ms (50-10000)"));
  Serial.println(F("    presses 2 1000   press N times per actuation, ms apart (N 1-3)"));
  Serial.println(F("      for a controller that sometimes swallows a press. Blind retry:"));
  Serial.println(F("      if the first press DID take, the second may stop it mid-travel"));
  Serial.println(F("      raise this if the relay clicks but the door does not move:"));
  Serial.println(F("      many controllers debounce their button and ignore a short tap"));
  Serial.println(F("    travel 15000     how long YOUR door takes to move, ms (0 = off)"));
  Serial.println(F("      the LED goes near-solid and the buzzer ticks for this long"));
  Serial.println(F("      after every actuation, then chimes. A STOPWATCH, not a sensor:"));
  Serial.println(F("      it will chime cheerfully at a door stuck halfway"));
  Serial.println(F("    buzzer 27        which GPIO the buzzer is on ('buzzer off' = none)"));
  Serial.println(F("    buzzer 27 passive      a bare transducer that needs a tone, not DC"));
  Serial.println(F("    buzzer 27 active low   one that sounds when pulled to GND"));
  Serial.println(F("    beep             three beeps now — find the pin by trying it"));
  Serial.println(F("    upload 60000 300000 1800000   how often the door calls in:"));
  Serial.println(F("      <quiet before uploading> <minimum gap> <heartbeat>, all ms."));
  Serial.println(F("      Lower the middle one for faster commands, at the cost of"));
  Serial.println(F("      radio time the BLE scan would otherwise have. 0 heartbeat = off"));
  Serial.println(F("    sensors 32 33    limit switch pins: <open> <closed> ('sensors off')"));
  Serial.println(F("    sensors 32 33 low   same, for switches that pull the pin to GND"));
  Serial.println(F("      the door stops guessing where it is. Without them it only"));
  Serial.println(F("      knows what it COMMANDED, which is why the chime is a timer"));
  Serial.println(F("    clear            forget saved values"));
  Serial.println(F("    q                cancel"));
  Serial.println(F("  (live output is paused while this menu is open)"));
  Serial.print(F("> "));
}

void applyTiming(uint32_t enterMs, uint32_t exitMs, uint32_t lockoutMs) {
  if (enterMs == 0 || exitMs == 0 || lockoutMs == 0) {
    Serial.println(F("\r\n[dwell] all three must be greater than zero."));
    printTimingMenu();
    return;
  }
  if (lockoutMs >= exitMs) {
    Serial.printf("\r\n[dwell] rejected: min interval (%lu) must be BELOW the close "
                  "dwell (%lu),\r\n",
                  static_cast<unsigned long>(lockoutMs), static_cast<unsigned long>(exitMs));
    Serial.println(F("[dwell] otherwise the actuation lockout delays closing."));
    printTimingMenu();
    return;
  }

  g_tracker.setDwell(enterMs, exitMs);
  g_door.setMinIntervalMs(lockoutMs);
  BleScanner::storeTiming(enterMs, exitMs, lockoutMs);
  g_timingStored = true;

  Serial.printf("\r\n[dwell] saved: open after %lu ms, close after %lu ms, "
                "min interval %lu ms\r\n",
                static_cast<unsigned long>(enterMs), static_cast<unsigned long>(exitMs),
                static_cast<unsigned long>(lockoutMs));

  const uint32_t gap = g_tracker.maxGapMs();
  if (gap && exitMs <= gap) {
    Serial.printf("[dwell] !! WARNING: close dwell (%lu ms) is at or below the worst\r\n",
                  static_cast<unsigned long>(exitMs));
    Serial.printf("[dwell]    measured gap (%lu ms). A routine radio dropout will now\r\n",
                  static_cast<unsigned long>(gap));
    Serial.println(F("[dwell]    close the door with the beacon still present."));
  }
  Serial.println(F("[dwell] active immediately; no restart needed."));
  g_entry = ENTRY_NONE;
}

void processTimingLine(char *line) {
  while (*line == ' ' || *line == '\t') line++;
  int n = strlen(line);
  while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';

  if (n == 0 || strcmp(line, "q") == 0) {
    Serial.println(F("\r\n[dwell] cancelled, nothing changed."));
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
    g_travelMs = DOOR_TRAVEL_MS;
    g_timingStored = false;
    Serial.println(F("\r\n[dwell] reverted to the compiled-in defaults."));
    Serial.println(F("[dwell] the buzzer pin is kept — that is wiring, not tuning."));
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "gap ", 4) == 0) {
    uint32_t ms = 0;
    if (!parseBoundedMs(line + 4, DoorController::kMinDirectionGapMs,
                        DoorController::kMaxDirectionGapMs, ms) ||
        !g_door.setDirectionGapMs(ms)) {
      Serial.printf("\r\n[dwell] rejected: interlock gap must be %lu-%lu ms.\r\n",
                    static_cast<unsigned long>(DoorController::kMinDirectionGapMs),
                    static_cast<unsigned long>(DoorController::kMaxDirectionGapMs));
      Serial.println(F("[dwell] both relays energised at once shorts the motor."));
      printTimingMenu();
      return;
    }
    BleScanner::storeTiming(g_tracker.enterConfirmMs(), g_tracker.exitConfirmMs(),
                            g_door.minIntervalMs());
    BleScanner::storeDirectionGap(ms);
    g_timingStored = true;
    Serial.printf("\r\n[dwell] interlock gap now %lu ms (saved on device).\r\n",
                  static_cast<unsigned long>(ms));
    g_entry = ENTRY_NONE;
    return;
  }
  if (strcmp(line, "beep") == 0) {
    if (!Chime::enabled()) {
      Serial.println(F("\r\n[dwell] no buzzer configured. 'buzzer <pin>' first."));
      printTimingMenu();
      return;
    }
    Chime::play(CHIME_TEST);
    Serial.printf("\r\n[dwell] three beeps on GPIO %d. Silence means the wrong pin,\r\n",
                  Chime::pin());
    Serial.println(F("[dwell] or the wrong kind: try 'buzzer <pin> passive', then this again."));
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "travel", 6) == 0 && (line[6] == ' ' || line[6] == '\0')) {
    String msg;
    if (!applyTravelMs(line[6] ? line + 7 : nullptr, msg)) {
      Serial.printf("\r\n[dwell] %s\r\n", msg.c_str());
      printTimingMenu();
      return;
    }
    Serial.printf("\r\n[dwell] %s (saved on device).\r\n", msg.c_str());
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "upload", 6) == 0 && (line[6] == ' ' || line[6] == '\0')) {
    String msg;
    if (!applyUploadTiming(line[6] ? line + 7 : nullptr, msg)) {
      Serial.printf("\r\n[dwell] %s\r\n", msg.c_str());
      printTimingMenu();
      return;
    }
    Serial.printf("\r\n[dwell] %s (saved on device).\r\n", msg.c_str());
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "sensors", 7) == 0 && (line[7] == ' ' || line[7] == '\0')) {
    String msg;
    if (!applySensorSpec(line[7] ? line + 8 : nullptr, msg)) {
      Serial.printf("\r\n[dwell] %s\r\n", msg.c_str());
      printTimingMenu();
      return;
    }
    Serial.printf("\r\n[dwell] %s (saved on device).\r\n", msg.c_str());
    if (Position::enabled()) {
      Serial.printf("[dwell] reading right now: %s\r\n",
                    DoorController::stateName(Position::state()));
      Serial.println(F("[dwell] move the door by hand and press 's' to watch it change."));
    }
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "buzzer", 6) == 0 && (line[6] == ' ' || line[6] == '\0')) {
    String msg;
    if (!applyBuzzerSpec(line[6] ? line + 7 : nullptr, msg)) {
      Serial.printf("\r\n[dwell] %s\r\n", msg.c_str());
      printTimingMenu();
      return;
    }
    Serial.printf("\r\n[dwell] %s (saved on device).\r\n", msg.c_str());
    if (Chime::enabled()) {
      Chime::play(CHIME_TEST);
      Serial.println(F("[dwell] beeping now — if you hear nothing, it is the wrong pin."));
    }
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "presses ", 8) == 0) {
    int n = 0; unsigned long g = 0;
    if (sscanf(line + 8, "%d %lu", &n, &g) < 1) {
      Serial.println(F("\r\n[dwell] give: presses <count> [gap ms], e.g. presses 2 1000"));
      printTimingMenu();
      return;
    }
    if (g == 0) g = g_door.pulseGapMs();
    if (n < 1 || n > DoorController::kMaxPulseCount ||
        !g_door.setPulseTrain(static_cast<uint8_t>(n), g)) {
      Serial.printf("\r\n[dwell] rejected: count 1-%u, gap %lu-%lu ms.\r\n",
                    DoorController::kMaxPulseCount,
                    static_cast<unsigned long>(DoorController::kMinPulseGapMs),
                    static_cast<unsigned long>(DoorController::kMaxPulseGapMs));
      printTimingMenu();
      return;
    }
    BleScanner::storePulseTrain(g_door.pulseCount(), g_door.pulseGapMs());
    g_timingStored = true;
    Serial.printf("\r\n[dwell] %u press(es) per actuation, %lu ms apart (saved).\r\n",
                  g_door.pulseCount(), static_cast<unsigned long>(g_door.pulseGapMs()));
    if (g_door.pulseCount() > 1) {
      Serial.println(F("[dwell] blind retry: the door cannot tell whether the first"));
      Serial.println(F("[dwell] press worked. Watch a dozen cycles before trusting it."));
    }
    g_entry = ENTRY_NONE;
    return;
  }
  if (strncmp(line, "pulse ", 6) == 0) {
    uint32_t ms = 0;
    if (!parseBoundedMs(line + 6, DoorController::kMinPulseMs, DoorController::kMaxPulseMs,
                        ms) ||
        !g_door.setPulseMs(ms)) {
      Serial.printf("\r\n[dwell] rejected: relay pulse must be %lu-%lu ms.\r\n",
                    static_cast<unsigned long>(DoorController::kMinPulseMs),
                    static_cast<unsigned long>(DoorController::kMaxPulseMs));
      printTimingMenu();
      return;
    }
    BleScanner::storePulseMs(ms);
    g_timingStored = true;
    Serial.printf("\r\n[dwell] relay pulse now %lu ms (saved on device).\r\n",
                  static_cast<unsigned long>(ms));
    Serial.println(F("[dwell] test it with 'o' and 'x' before trusting it to the door."));
    if (ms >= 2000) {
      Serial.printf("[dwell] note: the control task is blocked for the whole %lu ms — no\r\n",
                    static_cast<unsigned long>(ms));
      Serial.println(F("[dwell] samples drained and no reversing mid-travel. See WIRING.md."));
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
    Serial.println(F("\r\n[dwell] give three values, e.g. 1000,3000,2000"));
    printTimingMenu();
    return;
  }
  const char *c2 = strchr(c1 + 1, ',');
  if (c2 == nullptr) {
    Serial.println(F("\r\n[dwell] give three values, e.g. 1000,3000,2000"));
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
    Serial.printf("\r\n[dwell] rejected: each value must be 1-%lu ms.\r\n",
                  static_cast<unsigned long>(kMaxDwellMs));
    printTimingMenu();
    return;
  }
  applyTiming(en, ex, lk);
}

void printFilterMenu() {
  Serial.println();
  Serial.println(F("---- filter shape (how fast RSSI is tracked) ----"));
  Serial.println(F("  two filters run over the same samples:"));
  Serial.printf("  CLOSE window  : %u samples (max %u)\r\n", g_tracker.windowSize(),
                kMaxMedianWindow);
  Serial.printf("  CLOSE alpha   : %s  (higher = faster, noisier)\r\n",
                String(g_tracker.alpha(), 2).c_str());
  Serial.printf("  OPEN  window  : %u samples\r\n", g_tracker.fastWindowSize());
  Serial.printf("  OPEN  alpha   : %s\r\n", String(g_tracker.fastAlpha(), 2).c_str());
  Serial.printf("  source        : %s / %s\r\n",
                g_filterStored ? "saved" : "compiled",
                g_fastFilterStored ? "saved" : "compiled");

  // Show what this actually costs in time, using the measured sample rate.
  const uint32_t gap = g_tracker.maxGapMs();
  Serial.printf("  worst sample gap measured: %lu ms\r\n", static_cast<unsigned long>(gap));
  Serial.println(F("  lag is roughly (window/2 + 1/alpha) x the sample interval."));
  Serial.println(F("  the CLOSE pair no longer costs open latency, so prefer a"));
  Serial.println(F("  LONG window here and tune the OPEN pair for speed."));
  Serial.println(F("  type one of:"));
  Serial.println(F("    fast        window 3, alpha 0.7  (twitchy close; rarely needed now)"));
  Serial.println(F("    default     window 7, alpha 0.35 (the shipped shape)"));
  Serial.println(F("    smooth      window 11, alpha 0.2 (noisy RF)"));
  Serial.println(F("    3,0.7       window,alpha directly (window odd, 1..15)"));
  Serial.println(F("    open 1,0.9  set the OPEN pair (window odd, 1..15)"));
  Serial.println(F("    open same   make OPEN match CLOSE (old single-filter behaviour)"));
  Serial.println(F("    clear       forget saved values (both pairs)"));
  Serial.println(F("    q           cancel"));
  Serial.println(F("  (live output is paused while this menu is open)"));
  Serial.print(F("> "));
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
    Serial.printf("\r\n[filt] rejected: window must be ODD and 1..%u, alpha 0<a<=1\r\n",
                  kMaxMedianWindow);
    printFilterMenu();
    return;
  }
  BleScanner::storeFilter(static_cast<uint8_t>(win), alpha);
  g_filterStored = true;
  // Report what is actually in force, not what was typed.
  Serial.printf("\r\n[filt] saved: window %u, alpha %s\r\n", g_tracker.windowSize(),
                String(g_tracker.alpha(), 2).c_str());
  if (g_tracker.windowSize() <= 1 || g_tracker.alpha() >= 0.99f) {
    Serial.println(F("[filt] !! near-unfiltered. A single RSSI spike can now move"));
    Serial.println(F("[filt]    the door. This is the failure the project fixes."));
  }
  if (g_tracker.fastWindowSize() != prevFastWin || g_tracker.fastAlpha() != prevFastAlpha) {
    // Persist it, or the clamp would be undone by the stored value at the next
    // boot and the invariant would hold only until a power cut.
    BleScanner::storeFastFilter(g_tracker.fastWindowSize(), g_tracker.fastAlpha());
    g_fastFilterStored = true;
    Serial.printf("[filt] OPEN pair pulled to %u, %s to stay ahead of CLOSE.\r\n",
                  g_tracker.fastWindowSize(), String(g_tracker.fastAlpha(), 2).c_str());
  }
  Serial.println(F("[filt] filter reset; it will re-acquire in a second or two."));
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
    Serial.printf("\r\n[filt] rejected: window must be ODD and 1..%u, alpha 0<a<=1,\r\n",
                  kMaxMedianWindow);
    Serial.printf("[filt] and must not be slower than CLOSE (window <= %u, alpha >= %s).\r\n",
                  g_tracker.windowSize(), String(g_tracker.alpha(), 2).c_str());
    printFilterMenu();
    return;
  }
  BleScanner::storeFastFilter(static_cast<uint8_t>(win), alpha);
  g_fastFilterStored = true;
  Serial.printf("\r\n[filt] saved: OPEN window %u, alpha %s\r\n", g_tracker.fastWindowSize(),
                String(g_tracker.fastAlpha(), 2).c_str());
  Serial.println(F("[filt] the close path is unchanged; only open latency moved."));
  g_entry = ENTRY_NONE;
}

void processFilterLine(char *line) {
  while (*line == ' ' || *line == '\t') line++;
  int n = strlen(line);
  while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';

  if (n == 0 || strcmp(line, "q") == 0) {
    Serial.println(F("\r\n[filt] cancelled, nothing changed."));
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
    Serial.println(F("\r\n[filt] reverted to the compiled-in defaults (both pairs)."));
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
      Serial.println(F("\r\n[filt] give: open window,alpha e.g. open 1,0.9"));
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
    Serial.println(F("\r\n[filt] give window,alpha e.g. 3,0.7"));
    printFilterMenu();
    return;
  }
  applyFilter(atoi(line), static_cast<float>(atof(comma + 1)));
}

void handleMacEntryChar(int c) {
  if (c == '\r' || c == '\n') {
    if (g_macLineLen == 0) {
      // A bare Enter (or the \n of a \r\n pair) cancels rather than submits.
      Serial.println(F("\r\n[mac] cancelled, nothing changed."));
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
    } else {
      processMacLine(g_macLine);
    }
    return;
  }

  if (c == 0x08 || c == 0x7F) {  // backspace / delete
    if (g_macLineLen > 0) {
      g_macLineLen--;
      Serial.print(F("\b \b"));
    }
    return;
  }

  if (c >= 0x20 && g_macLineLen < sizeof(g_macLine) - 1) {
    g_macLine[g_macLineLen++] = static_cast<char>(c);
    Serial.write(static_cast<char>(c));  // echo, so typing is visible
  }
}

void handleSerial(uint32_t nowMs) {
  while (Serial.available() > 0) {
    const int c = Serial.read();

    if (g_entry != ENTRY_NONE) {
      handleMacEntryChar(c);
      continue;
    }

    if (g_downloadModeArmed) {
      g_downloadModeArmed = false;
      if (c == 'y' || c == 'Y') {
        Serial.println(F("y"));
        rebootToDownloadMode();
      } else {
        Serial.println(F("\r\n[sys] cancelled."));
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
        Serial.printf("[cmd] discovery %s\r\n", BleScanner::discoverEnabled() ? "ON" : "OFF");
        break;
      case 'c':
        g_calibrate = !g_calibrate;
        Serial.printf("[cmd] calibration stream %s\r\n", g_calibrate ? "ON" : "OFF");
        if (g_calibrate) {
          Serial.print(F("  tracking: "));
          Serial.println(BleScanner::describeTarget());
          Serial.println(F("  Walk to where the door should open and note the filtered value."));
          Serial.println(F("  Set RSSI_ENTER_DBM a little below it, RSSI_EXIT_DBM 8-12 dBm lower."));
        }
        break;
      case 'r':
        g_tracker.reset();
        Serial.println(F("[cmd] proximity filter reset"));
        Serial.println(F("[cmd] range stats cleared — walk the beacon out and"));
        Serial.println(F("[cmd] back, then press 's' to see weakest RSSI and"));
        Serial.println(F("[cmd] worst sample gap."));
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
        Serial.println(F("[sys] reboot into flash mode?"));
        Serial.println(F("[sys] the door will NOT be controlled until you flash."));
        Serial.print(F("[sys] press 'y' to confirm, anything else cancels: "));
        break;
#if ALLOW_MANUAL_SERIAL_CONTROL
      case 'o':
        Serial.println(F("[cmd] forcing OPEN"));
        g_door.forcePulseOpen();
        if (MANUAL_HOLD_MS > 0) {
          g_manualHoldUntilMs = millis() + MANUAL_HOLD_MS;
          if (g_manualHoldUntilMs == 0) g_manualHoldUntilMs = 1;  // 0 means "off"
          Serial.printf("[cmd] holding open for %lu s — automatic closing is paused.\r\n",
                        static_cast<unsigned long>(MANUAL_HOLD_MS / 1000));
          Serial.println(F("[cmd] 'x' closes now, 'O' hands control back immediately."));
        }
        break;
      case 'x':
        Serial.println(F("[cmd] forcing CLOSE"));
        // Clearing the hold here is what makes `x` mean "I am done" rather than
        // "close it, and then let the hold quietly keep it from closing again".
        g_manualHoldUntilMs = 0;
        g_door.forcePulseClose();
        break;
      case 'k':
        g_locked = true;
        BleScanner::storeLock(true);
        Serial.println(F("[cmd] LOCKED — the beacon can no longer open this door"));
        Serial.println(F("[cmd] 'o' still opens it by hand; 'K' unlocks."));
        break;
      case 'K':
        g_locked = false;
        BleScanner::storeLock(false);
        Serial.println(F("[cmd] unlocked — the beacon controls the door again"));
        break;
      case 'O':
        if (g_manualHoldUntilMs != 0) {
          g_manualHoldUntilMs = 0;
          Serial.println(F("[cmd] manual hold cleared; automatic control resumed."));
        } else {
          Serial.println(F("[cmd] no manual hold was active."));
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
//   2 Hz flash     beacon battery is low
//   1 Hz blink     no beacon configured, or never heard since boot
//   solid          door was last commanded OPEN
//   brief blip     door was last commanded CLOSED
//   off            nothing commanded since boot
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
      Serial.println(F("[pos] !! BOTH limit switches are made at once."));
      Serial.println(F("[pos] !! That cannot happen on a working door — suspect a"));
      Serial.println(F("[pos] !! shorted wire, a stuck switch, or a stray magnet."));
      Serial.println(F("[pos] !! Position is being ignored until it clears."));
    }
    return;
  }
  g_sensorFaultAnnounced = false;

  const DoorState seen = Position::state();
  if (seen != g_lastObserved) {
    g_lastObserved = seen;
    if (seen != DOOR_UNKNOWN) {
      // Arriving at an end is what confirms a travel actually completed.
      if (doorTravelling(nowMs)) g_travelVerified = true;
      if (g_door.observePosition(seen)) {
        Serial.printf("[pos] switch says %s — correcting what the door believed\r\n",
                      DoorController::stateName(seen));
      }
    }
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
  char line[224];
  snprintf(line, sizeof(line),
           "rssi=%d raw=%d dist=%s present=%d door=%s locked=%d presses=%u "
           "gap=%lu samples=%lu adv=%lu weak=%lu heap=%lu up=%lu real=%s",
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
           // What the switches SAY, as opposed to what we commanded. "none"
           // when no switches are fitted; "?" is the normal mid-travel
           // reading with them.
           !Position::enabled() ? "none"
               : Position::fault() ? "FAULT"
               : Position::state() == DOOR_OPEN ? "OPEN"
               : Position::state() == DOOR_CLOSED ? "CLOSED" : "?");
  WifiLogger::setStatusLine(line);

  // Every tunable the remote channel can change, so the dashboard's
  // settings form can show what each one IS rather than a blank box. Built
  // here for the same reason the status line is: this task owns all of it.
  char cfg[320];
  snprintf(cfg, sizeof(cfg),
           "enter=%d exit=%d dopen=%lu dclose=%lu dmin=%lu pulse=%lu "
           "pcount=%u pgap=%lu igap=%lu travel=%lu fwin=%u falpha=%s "
           "owin=%u oalpha=%s bpin=%d bpassive=%d blow=%d "
           "sopen=%d sshut=%d slow=%d "
           "upsettle=%lu upmin=%lu upbeat=%lu",
           g_tracker.enterDbm(), g_tracker.exitDbm(),
           static_cast<unsigned long>(g_tracker.enterConfirmMs()),
           static_cast<unsigned long>(g_tracker.exitConfirmMs()),
           static_cast<unsigned long>(g_door.minIntervalMs()),
           static_cast<unsigned long>(g_door.pulseMs()),
           g_door.pulseCount(),
           static_cast<unsigned long>(g_door.pulseGapMs()),
           static_cast<unsigned long>(g_door.directionGapMs()),
           static_cast<unsigned long>(g_travelMs),
           g_tracker.windowSize(), String(g_tracker.alpha(), 2).c_str(),
           g_tracker.fastWindowSize(), String(g_tracker.fastAlpha(), 2).c_str(),
           Chime::pin(), Chime::passive() ? 1 : 0, Chime::activeLow() ? 1 : 0,
           Position::openPin(), Position::closedPin(),
           Position::activeLow() ? 1 : 0,
           static_cast<unsigned long>(WifiLogger::settleMs()),
           static_cast<unsigned long>(WifiLogger::minIntervalMs()),
           static_cast<unsigned long>(WifiLogger::heartbeatMs()));
  WifiLogger::setConfigLine(cfg);
#endif
}

void updateChime(uint32_t nowMs) {
  const bool moving = doorTravelling(nowMs);
  if (moving != g_travelling) {
    const bool wasMoving = g_travelling;
    g_travelling = moving;
    if (moving) {
      g_travelVerified = false;   // a fresh travel has to earn its confirmation
      // Do not start the travel pattern on top of an acknowledgement. The ack
      // is the half-second that tells you the door HEARD you, and a `door
      // open` produces both in the same tick — starting the loop immediately
      // would swallow the one that carries the information.
      g_workingPending = true;
    } else if (wasMoving && g_travelMs != 0) {
      // With switches fitted, "arrived" is a measurement rather than a timer
      // running out — and a travel that ends with neither switch made is a
      // door that did NOT get there. Say so differently.
      if (Position::enabled() && !g_travelVerified) {
        Serial.println(F("[pos] !! travel time elapsed and NO limit switch was reached."));
        Serial.println(F("[pos] !! The door did not complete its travel: a swallowed"));
        Serial.println(F("[pos] !! button press, an obstruction, or a jam."));
        EventLog::record(LOG_STALLED, static_cast<uint8_t>(g_door.state()),
                         g_tracker.filteredRssi());
        Chime::play(CHIME_REFUSED);
        return;
      }
      // Only a stopwatch that actually ran out gets the done chime. Turning
      // announcements off mid-travel (`travel 0`) also clears g_travelling,
      // and announcing "arrived" because someone disabled announcements would
      // be a lie in the one direction that matters.
      g_workingPending = false;
      Chime::play(CHIME_DONE);
    } else {
      g_workingPending = false;
      Chime::stop();
    }
  }
  // Start the travel pattern once whatever was playing has had its say.
  if (g_workingPending && Chime::playing() == CHIME_NONE) {
    g_workingPending = false;
    if (g_travelling) Chime::play(CHIME_WORKING);
  }
  Chime::tick(nowMs);
}

void updateLed(uint32_t nowMs, bool scanHealthy) {
  bool on;
  if (!scanHealthy) {
    on = (nowMs / 100) % 2 == 0;  // 5 Hz: radio unhealthy
  } else if (beaconBatteryLow(nowMs)) {
    on = (nowMs / 250) % 2 == 0;  // 2 Hz: replace the beacon battery
  } else if (!BleScanner::isConfigured() || !BleScanner::targetEverSeen()) {
    on = (nowMs / 500) % 2 == 0;  // 1 Hz: nothing to track yet
  } else if (doorTravelling(nowMs)) {
    // Lit, with a heartbeat gap: the door is moving. Deliberately NOT another
    // even blink — 5 Hz, 2 Hz and 1 Hz are already spoken for by the three
    // faults above, and a fourth would be unreadable. "Nearly solid" is
    // recognisable across a yard and cannot be mistaken for a fault.
    on = (nowMs % 600) > 120;
  } else {
    switch (g_door.state()) {
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
        on = false;
        break;
    }
  }
  digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW);
}

void driveDoor(uint32_t nowMs) {
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
  if (g_locked && g_tracker.isPresent()) {
    // Say so, once per arrival. Someone standing at a locked door with the
    // collar in their hand cannot tell "locked" from "broken", and that is
    // exactly the moment they start taking the thing apart. Edge-triggered:
    // this condition holds for as long as the animal is there, and a buzzer
    // that repeated it would be an alarm.
    if (!g_lockRefusedAnnounced) {
      g_lockRefusedAnnounced = true;
      Chime::play(CHIME_REFUSED);
    }
    return;
  }
  g_lockRefusedAnnounced = false;

  // A manual hold suppresses automatic CLOSING only. Opening is never blocked:
  // if the beacon turns up mid-hold the door is already open, and a manual `x`
  // must never be able to keep the door shut against an animal walking up to
  // it. Holding open is the safe failure; holding closed is not.
  if (!g_tracker.isPresent() && manualHoldActive(nowMs)) return;

  const ActuationResult r = g_tracker.isPresent() ? g_door.requestOpen(nowMs)
                                                 : g_door.requestClose(nowMs);
  // ACT_ALREADY is the normal steady state and would swamp the log; the other
  // refusals are the ones that explain a door that did not move.
  if (r == ACT_LOCKED_OUT || r == ACT_BOOT_GRACE) {
    static ActuationResult lastLogged = ACT_DONE;
    if (r != lastLogged) {
      lastLogged = r;
      EventLog::record(LOG_REFUSED, static_cast<uint8_t>(r), g_tracker.filteredRssi());
    }
  }
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
      Serial.printf("[fix] acquired  (rssi %d dBm, ~%s m, %lu samples)\r\n",
                    g_tracker.filteredRssi(), fmt1(g_tracker.distanceM()).c_str(),
                    static_cast<unsigned long>(g_tracker.totalSamples()));
    } else {
      Serial.printf("[fix] lost  (last heard %lu ms ago) — counts as FAR\r\n",
                    static_cast<unsigned long>(g_tracker.sampleAgeMs(nowMs)));
    }
  }

  if (g_tracker.state() != g_lastReportedPresence) {
    g_lastReportedPresence = g_tracker.state();
    Serial.printf("[presence] %s  (rssi %d dBm, ~%s m, %lu samples)\r\n",
                  g_tracker.isPresent() ? "PRESENT" : "ABSENT", g_tracker.filteredRssi(),
                  fmt1(g_tracker.distanceM()).c_str(),
                  static_cast<unsigned long>(g_tracker.totalSamples()));
  }

  if (g_door.state() != g_lastReportedDoorState) {
    g_lastReportedDoorState = g_door.state();
    const bool manual = g_door.lastSource() == SRC_MANUAL;
    Serial.printf("[door] %s  (%s, rssi %d dBm, ~%s m)\r\n",
                  DoorController::stateName(g_door.state()),
                  manual ? "manual" : "beacon", g_tracker.filteredRssi(),
                  fmt1(g_tracker.distanceM()).c_str());
    EventLog::record(g_door.state() == DOOR_OPEN ? LOG_OPEN : LOG_CLOSE,
                     static_cast<uint8_t>(g_door.lastSource()),
                     g_tracker.filteredRssi());
  }

  if (scanHealthy != g_lastReportedScanHealthy) {
    g_lastReportedScanHealthy = scanHealthy;
    if (scanHealthy) {
      Serial.println(F("[radio] healthy — advertisements resumed"));
    } else {
      Serial.printf("[radio] UNHEALTHY — nothing heard from any device for %lu ms\r\n",
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
  if (strcmp(verb, "travel") == 0) {
    return applyTravelMs(arg(), result);
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
    if (strcmp(a, "open") == 0) {
      g_door.forcePulseOpen();
      if (MANUAL_HOLD_MS > 0) {
        g_manualHoldUntilMs = millis() + MANUAL_HOLD_MS;
        if (g_manualHoldUntilMs == 0) g_manualHoldUntilMs = 1;
      }
      result = "door opened (held)";
      return true;
    }
    if (strcmp(a, "close") == 0) {
      g_manualHoldUntilMs = 0;
      g_door.forcePulseClose();
      result = "door closed";
      return true;
    }
    if (strcmp(a, "auto") == 0) {
      g_manualHoldUntilMs = 0;
      result = "manual hold cleared";
      return true;
    }
    result = "door takes open, close or auto";
    return false;
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
    g_travelMs = DOOR_TRAVEL_MS;
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
// The point is to tell them apart by EAR, from the coop, without a screen. So
// the pairs mirror each other: open and close differ by direction, lock and
// unlock by register. Everything that merely changes a setting shares one short
// blip — the distinction that matters out there is "the door is about to move"
// versus "the door took a note".
ChimeTune ackTuneFor(const char *command) {
  if (command == nullptr) return CHIME_ACK_SET;
  if (strncmp(command, "door ", 5) == 0) {
    if (strstr(command, "open") != nullptr) return CHIME_ACK_OPEN;
    if (strstr(command, "close") != nullptr) return CHIME_ACK_CLOSE;
    return CHIME_ACK_SET;                       // `door auto` moves nothing
  }
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
    Serial.printf("[cmd] %s -> %s\r\n", line, result.c_str());
    if (ok) {
      applied++;
      // `beep` already sounds; a second tune on top would cut it off.
      if (strcmp(line, "beep") == 0) sounded = true;
      else ack = ackTuneFor(line);
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
      g_tracker.addSample(sample.rssi, sample.measuredPower, sample.atMs);
      while (BleScanner::popSample(sample)) {
        g_tracker.addSample(sample.rssi, sample.measuredPower, sample.atMs);
      }
    }

    const uint32_t now = millis();

    // 2. Re-evaluate presence. Must run even with no new samples — that is how
    //    a beacon that has gone silent gets noticed.
    g_tracker.update(now);

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
      Serial.println(F("[cmd] restarting to apply a new beacon list"));
      Serial.flush();
      delay(200);
      ESP.restart();
    }
#endif

    // 3. Keep the radio alive.
    BleScanner::serviceWatchdog(now);
    const bool scanHealthy = BleScanner::advAgeMs(now) < SCAN_WATCHDOG_MS;

    // 4. Act, then report. Reporting last means a state change is announced on
    //    the same tick it happens rather than one tick later.
    updatePosition(now);
    driveDoor(now);
    updateLed(now, scanHealthy);
    updateChime(now);
    if (g_entry == ENTRY_NONE) reportTransitions(now, scanHealthy);

    // 4b. Offer the uploader a window. "Idle" means the animal is not around
    //     and the door is shut, so sharing the antenna cannot cost us a
    //     detection that matters. See wifi_logger.h.
    const bool idle = !g_tracker.isPresent() && g_door.state() != DOOR_OPEN &&
                      g_entry == ENTRY_NONE && !WifiLogger::otaWindowOpen();
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
        Serial.printf("[cal] raw %4d  filtered %4d dBm  ~%s m  %s\r\n", g_tracker.rawRssi(),
                      g_tracker.filteredRssi(), fmt1(g_tracker.distanceM()).c_str(),
                      g_tracker.isPresent() ? "PRESENT" : "absent");
      } else {
        Serial.printf("[cal] no fix (last seen %lu ms ago)\r\n",
                      static_cast<unsigned long>(g_tracker.sampleAgeMs(now)));
      }
    }

    // No sleep here: the wait at the top of the loop is the pacing mechanism.
  }
}

}  // namespace

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);

  // Relays first: get the outputs into a known-safe state before anything else
  // can take time or fail.
  g_door.begin();
  g_tracker.begin();
  recordBoot();
  EventLog::begin(static_cast<uint16_t>(g_bootCount));
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

    uint32_t travel = DOOR_TRAVEL_MS;
    if (BleScanner::loadStoredTravelMs(travel)) {
      g_travelMs = travel;
      g_timingStored = true;
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
      Serial.printf("[chime] refusing GPIO %d: %s\r\n", bp,
                    Chime::pinProblem(bp) ? Chime::pinProblem(bp) : "invalid");
      Serial.println(F("[chime] annunciator disabled; 'w' then 'buzzer <pin>' to fix"));
      Chime::configure(-1, false, false);
    } else if (stored && Chime::enabled()) {
      const char *warn = Chime::pinProblem(Chime::pin());
      if (warn) Serial.printf("[chime] GPIO %d: %s\r\n", Chime::pin(), warn);
    }
  }

  {
    // The limit switches, after the buzzer so a saved pin that collides with
    // it is refused rather than silently fighting over the GPIO.
    int so = PIN_SENSOR_OPEN, sc = PIN_SENSOR_CLOSED;
    bool sl = SENSOR_ACTIVE_LOW != 0;
    BleScanner::loadStoredSensors(so, sc, sl);
    if (!Position::configure(so, sc, sl)) {
      Serial.printf("[pos] refusing GPIO %d/%d — check 'w' then 'sensors'\r\n", so, sc);
      Position::configure(-1, -1, true);
    }
  }

  // Said loudly because it survives a reboot by design, and a door that will
  // not open for the collar is exactly the thing someone needs to be told
  // about before they start wondering why the animal is outside.
  g_locked = BleScanner::loadStoredLock();
  if (g_locked) {
    Serial.println(F("  !! THIS DOOR IS LOCKED — the beacon cannot open it"));
    Serial.println(F("  !! 'K' unlocks, or queue `unlock` from the log server"));
  }

  if (!BleScanner::begin()) {
    Serial.println(F("[fatal] BLE scanner failed to start; rebooting in 5 s"));
    delay(5000);
    ESP.restart();
  }

  printBanner();

  WifiLogger::setBootCount(g_bootCount);
  WifiLogger::begin();
  xTaskCreatePinnedToCore(controlTask, "petdoor", CONTROL_TASK_STACK, nullptr, 1,
                          &g_controlTaskHandle, 1);
  Serial.println(F("[system] running"));
}

void loop() {
  // All work happens in controlTask, which owns the door and the filter.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
