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
#include "config.h"
#include "door.h"
#include "eventlog.h"
#include "proximity.h"
#include "wifi_logger.h"

namespace {

ProximityTracker g_tracker;
DoorController g_door;

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

String fmt1(float v) {
  if (v < 0.0f) return String("?");
  return String(v, 1);
}

void printBanner() {
  Serial.println();
  Serial.println(F("=================================================="));
  Serial.println(F("  PetDoor — BLE proximity door controller"));
  Serial.printf("  v%s  (built %s)\r\n", PETDOOR_VERSION, PETDOOR_BUILD);
  Serial.println(F("=================================================="));
  Serial.print(F("  target beacon : "));
  Serial.println(BleScanner::describeTarget());
  Serial.printf("  open / close  : GPIO %d / GPIO %d (active %s)\r\n", PIN_RELAY_OPEN,
                PIN_RELAY_CLOSE, RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
  Serial.printf("  status LED    : GPIO %d\r\n", PIN_STATUS_LED);
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
  Serial.println(F("  o  pulse the OPEN relay now (bypasses proximity logic)"));
  Serial.println(F("  x  pulse the CLOSE relay now (bypasses proximity logic)"));
#endif
  Serial.println(F("status LED:"));
  Serial.println(F("  solid        door last OPENED"));
  Serial.println(F("  brief blip   door last CLOSED"));
  Serial.println(F("  1 Hz blink   no beacon configured / never heard"));
  Serial.println(F("  2 Hz flash   beacon battery LOW"));
  Serial.println(F("  5 Hz flutter radio unhealthy"));
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
  Serial.printf("  actuations   : %lu open, %lu close (%lu locked out, %lu in boot grace)\r\n",
                static_cast<unsigned long>(g_door.openCount()),
                static_cast<unsigned long>(g_door.closeCount()),
                static_cast<unsigned long>(g_door.lockedOutCount()),
                static_cast<unsigned long>(g_door.bootGraceCount()));
  Serial.printf("  scan restarts: %lu\r\n", static_cast<unsigned long>(BleScanner::scanRestarts()));
  Serial.printf("  firmware     : v%s (built %s)\r\n", PETDOOR_VERSION, PETDOOR_BUILD);
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
    g_timingStored = false;
    Serial.println(F("\r\n[dwell] reverted to the compiled-in defaults."));
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
        break;
      case 'x':
        Serial.println(F("[cmd] forcing CLOSE"));
        g_door.forcePulseClose();
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

void updateLed(uint32_t nowMs, bool scanHealthy) {
  bool on;
  if (!scanHealthy) {
    on = (nowMs / 100) % 2 == 0;  // 5 Hz: radio unhealthy
  } else if (beaconBatteryLow(nowMs)) {
    on = (nowMs / 250) % 2 == 0;  // 2 Hz: replace the beacon battery
  } else if (!BleScanner::isConfigured() || !BleScanner::targetEverSeen()) {
    on = (nowMs / 500) % 2 == 0;  // 1 Hz: nothing to track yet
  } else {
    switch (g_door.state()) {
      case DOOR_OPEN:
        on = true;                      // solid
        break;
      case DOOR_CLOSED:
        on = (nowMs % 2000) < 200;      // brief blip every 2 s
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

    // 3. Keep the radio alive.
    BleScanner::serviceWatchdog(now);
    const bool scanHealthy = BleScanner::advAgeMs(now) < SCAN_WATCHDOG_MS;

    // 4. Act, then report. Reporting last means a state change is announced on
    //    the same tick it happens rather than one tick later.
    driveDoor(now);
    updateLed(now, scanHealthy);
    if (g_entry == ENTRY_NONE) reportTransitions(now, scanHealthy);

    // 4b. Offer the uploader a window. "Idle" means the animal is not around
    //     and the door is shut, so sharing the antenna cannot cost us a
    //     detection that matters. See wifi_logger.h.
    const bool idle = !g_tracker.isPresent() && g_door.state() != DOOR_OPEN &&
                      g_entry == ENTRY_NONE && !WifiLogger::otaWindowOpen();
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
    const uint32_t gap = BleScanner::loadStoredDirectionGap();
    if (gap) g_door.setDirectionGapMs(gap);   // floor enforced inside
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
