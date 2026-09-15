#include "ble_scanner.h"

#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <Preferences.h>
#include <ctype.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

namespace BleScanner {
namespace {

struct SeenDevice {
  char mac[18];
  String name;
  String mfgHex;
  String svc;
  int rssi;
  int8_t measuredPower;  // from an iBeacon frame; 0 when the frame carried none
  DeviceClass cls;
  uint32_t lastSeenMs;
  uint32_t lastTableWriteMs;
  bool isTarget;
  bool used;
};

constexpr size_t kSampleQueueDepth = 32;

BLEScan *g_scan = nullptr;
QueueHandle_t g_sampleQueue = nullptr;
SemaphoreHandle_t g_tableMutex = nullptr;

SeenDevice g_table[DEVICE_TABLE_SIZE];

volatile uint32_t g_lastAdvMs = 0;
volatile uint32_t g_advCount = 0;
volatile uint32_t g_droppedSamples = 0;
volatile bool g_targetSeen = false;

EddystoneTlm g_targetTlm = {};
volatile uint32_t g_targetTlmMs = 0;
volatile bool g_haveTargetTlm = false;
volatile bool g_discover = false;

bool g_matchUuid = false;

// Fixed char arrays rather than String: this list is walked in the BLE
// callback on every advertisement, and strcmp allocates nothing.
char g_targetMacs[BEACON_MAX_MACS][18] = {};
uint8_t g_targetMacCount = 0;

uint8_t g_targetUuid[16] = {0};

uint32_t g_lastScanRestartMs = 0;
uint32_t g_scanRestarts = 0;

// NVS. The namespace is capped at 15 characters by the Preferences API.
constexpr char kNvsNamespace[] = "petdoor";
constexpr char kNvsMacsKey[] = "macs";

bool g_macsFromNvs = false;

// A MAC is exactly 12 hex digits and 5 colons. Deliberately strict: a typo
// stored in NVS would silently stop the door working until someone thought to
// re-read the banner.
bool looksLikeMac(const char *s) {
  int digits = 0, colons = 0;
  for (const char *c = s; *c; c++) {
    if (isxdigit(static_cast<unsigned char>(*c))) {
      digits++;
    } else if (*c == ':') {
      colons++;
    } else {
      return false;
    }
  }
  return digits == 12 && colons == 5;
}

bool placeholderOrEmpty(const char *s) {
  if (s == nullptr || s[0] == '\0') return true;
  // Treat an all-zero MAC or UUID as "not configured".
  for (const char *c = s; *c; c++) {
    if (*c != '0' && *c != ':' && *c != '-' && *c != ' ') return false;
  }
  return true;
}

// Splits a comma / semicolon / space separated MAC list into g_targetMacs,
// lower-casing as it goes. Placeholder entries (all zeros) are skipped, so the
// commented-out example in secrets.example.h never counts as a real target.
void parseMacList(const char *list) {
  if (list == nullptr) return;

  char buf[18];
  uint8_t len = 0;
  bool overflow = false;

  for (const char *c = list;; c++) {
    const bool sep = (*c == ',' || *c == ';' || *c == ' ' || *c == '\t');

    if (*c != '\0' && !sep) {
      if (len < sizeof(buf) - 1) buf[len++] = static_cast<char>(tolower(*c));
      continue;
    }

    if (len > 0) {
      buf[len] = '\0';
      if (!placeholderOrEmpty(buf)) {
        if (g_targetMacCount < BEACON_MAX_MACS) {
          strncpy(g_targetMacs[g_targetMacCount], buf, sizeof(g_targetMacs[0]) - 1);
          g_targetMacs[g_targetMacCount][sizeof(g_targetMacs[0]) - 1] = '\0';
          g_targetMacCount++;
        } else {
          overflow = true;
        }
      }
      len = 0;
    }

    if (*c == '\0') break;
  }

  if (overflow) {
    Serial.printf("[ble] BEACON_MAC lists more than BEACON_MAX_MACS (%d); extras ignored\r\n",
                  BEACON_MAX_MACS);
  }
}

bool matchesTarget(const String &macLower, bool haveIb, const IBeaconData &ib) {
  if (g_targetMacCount == 0 && !g_matchUuid) return false;

  if (g_targetMacCount > 0) {
    bool hit = false;
    for (uint8_t i = 0; i < g_targetMacCount; i++) {
      if (strcmp(macLower.c_str(), g_targetMacs[i]) == 0) {
        hit = true;
        break;
      }
    }
    if (!hit) return false;
  }

  if (g_matchUuid) {
    if (!haveIb) return false;
    if (memcmp(ib.uuid, g_targetUuid, 16) != 0) return false;
    if (BEACON_MAJOR >= 0 && ib.major != static_cast<uint16_t>(BEACON_MAJOR)) return false;
    if (BEACON_MINOR >= 0 && ib.minor != static_cast<uint16_t>(BEACON_MINOR)) return false;
  }
  return true;
}

void updateTable(const String &macLower, BLEAdvertisedDevice &device, const String &mfg,
                 bool isTarget, int8_t measuredPower, uint32_t nowMs) {
  if (g_tableMutex == nullptr) return;
  if (xSemaphoreTake(g_tableMutex, 0) != pdTRUE) return;  // never block the BLE task

  int slot = -1;
  int oldest = -1;
  uint32_t oldestMs = UINT32_MAX;

  for (int i = 0; i < DEVICE_TABLE_SIZE; i++) {
    if (!g_table[i].used) {
      if (slot < 0) slot = i;
      continue;
    }
    if (strncmp(g_table[i].mac, macLower.c_str(), sizeof(g_table[i].mac)) == 0) {
      slot = i;
      // Throttle: refreshing every field on every advertisement would churn
      // the heap hard in a busy RF environment.
      if ((nowMs - g_table[i].lastTableWriteMs) < TABLE_MIN_UPDATE_MS) {
        g_table[i].rssi = device.getRSSI();
        if (measuredPower != 0) g_table[i].measuredPower = measuredPower;
        g_table[i].lastSeenMs = nowMs;
        xSemaphoreGive(g_tableMutex);
        return;
      }
      break;
    }
    if (g_table[i].lastSeenMs < oldestMs) {
      oldestMs = g_table[i].lastSeenMs;
      oldest = i;
    }
  }

  if (slot < 0) slot = (oldest >= 0) ? oldest : 0;  // evict least recently seen

  SeenDevice &e = g_table[slot];
  strncpy(e.mac, macLower.c_str(), sizeof(e.mac) - 1);
  e.mac[sizeof(e.mac) - 1] = '\0';
  e.name = device.haveName() ? device.getName() : String();

  e.mfgHex = mfg.length()
                 ? bytesToHex(reinterpret_cast<const uint8_t *>(mfg.c_str()), mfg.length())
                 : String();

  String svc;
  if (device.haveServiceUUID()) svc += device.getServiceUUID().toString();
  if (device.haveServiceData()) {
    if (svc.length()) svc += ',';
    svc += device.getServiceDataUUID().toString();
  }
  svc.toLowerCase();
  e.svc = svc;

  e.rssi = device.getRSSI();
  e.measuredPower = measuredPower;
  e.cls = classifyDevice(e.name, e.mfgHex, e.svc);
  e.lastSeenMs = nowMs;
  e.lastTableWriteMs = nowMs;
  e.isTarget = isTarget;
  e.used = true;

  xSemaphoreGive(g_tableMutex);
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice device) override {
    const uint32_t now = millis();
    g_lastAdvMs = now;
    g_advCount++;

    String mac = device.getAddress().toString();
    mac.toLowerCase();

    String mfg;
    if (device.haveManufacturerData()) mfg = device.getManufacturerData();

    IBeaconData ib = {};
    const bool haveIb = mfg.length() ? parseIBeacon(mfg, ib) : false;

    const bool isTarget = matchesTarget(mac, haveIb, ib);
    if (isTarget) {
      g_targetSeen = true;

      // Eddystone beacons interleave TLM frames with their UID/URL frames, so
      // this lands only on some advertisements. Cheap: a length check and a
      // handful of byte reads, safe for the BLE callback.
      if (device.haveServiceData()) {
        const String sd = device.getServiceData();
        EddystoneTlm tlm;
        if (sd.length() && parseEddystoneTlm(sd, tlm)) {
          g_targetTlm = tlm;
          g_targetTlmMs = now;
          g_haveTargetTlm = true;
        }
      }
      BleSample sample;
      sample.rssi = device.getRSSI();
      sample.measuredPower = haveIb ? ib.measuredPower : 0;
      sample.atMs = now;
      if (g_sampleQueue != nullptr && xQueueSend(g_sampleQueue, &sample, 0) != pdTRUE) {
        // Queue full means the control task is behind. Dropping the newest
        // sample is harmless; the filter only needs a steady stream, not
        // every single packet.
        g_droppedSamples++;
      }
    }

    if (g_discover) {
      updateTable(mac, device, mfg, isTarget, haveIb ? ib.measuredPower : 0, now);
    }
  }
};

ScanCallbacks g_callbacks;

void applyScanSettings() {
  g_scan->setActiveScan(SCAN_ACTIVE != 0);
  g_scan->setInterval(SCAN_INTERVAL_MS);
  g_scan->setWindow(SCAN_WINDOW_MS);

  // THE important line. The second argument is `wantDuplicates`, and it
  // defaults to false. Left at the default, BLEScan drops every repeat
  // advertisement from a device it has already reported ("Ignoring %s, already
  // seen it") and enables the controller's duplicate filter as well — so an
  // infinite scan yields exactly one callback per device, forever. Passing
  // true is what turns this into a real RSSI sample stream.
  g_scan->setAdvertisedDeviceCallbacks(&g_callbacks, true);
}

}  // namespace

bool begin() {
  static_assert(SCAN_WINDOW_MS <= SCAN_INTERVAL_MS,
                "SCAN_WINDOW_MS must be <= SCAN_INTERVAL_MS.");

  // NVS wins over the compile-time default, so a beacon can be changed from
  // the serial console without rebuilding.
  {
    Preferences prefs;
    String stored;
    if (prefs.begin(kNvsNamespace, /*readOnly=*/true)) {
      stored = prefs.getString(kNvsMacsKey, "");
      prefs.end();
    }
    if (stored.length() > 0) {
      parseMacList(stored.c_str());
      g_macsFromNvs = g_targetMacCount > 0;
    }
    if (g_targetMacCount == 0) {
      g_macsFromNvs = false;
      parseMacList(BEACON_MAC);
    }
  }
  if (!placeholderOrEmpty(BEACON_UUID)) {
    if (uuidFromString(BEACON_UUID, g_targetUuid)) {
      g_matchUuid = true;
    } else {
      Serial.println(F("[ble] BEACON_UUID is not a valid 128-bit UUID; ignoring it."));
    }
  }

  // With nothing to look for, the only useful thing this firmware can do is
  // help you find the beacon.
  g_discover = !isConfigured();

  g_sampleQueue = xQueueCreate(kSampleQueueDepth, sizeof(BleSample));
  g_tableMutex = xSemaphoreCreateMutex();
  if (g_sampleQueue == nullptr || g_tableMutex == nullptr) {
    Serial.println(F("[ble] failed to allocate queue/mutex"));
    return false;
  }

  BLEDevice::init("PetDoor");
  g_scan = BLEDevice::getScan();
  if (g_scan == nullptr) {
    Serial.println(F("[ble] getScan() returned null"));
    return false;
  }

  applyScanSettings();

  // duration 0 == scan until told otherwise.
  if (!g_scan->start(0, nullptr, false)) {
    Serial.println(F("[ble] scan failed to start"));
    return false;
  }

  g_lastAdvMs = millis();
  g_lastScanRestartMs = millis();
  return true;
}

bool isConfigured() { return g_targetMacCount > 0 || g_matchUuid; }

String describeTarget() {
  if (!isConfigured()) return String("<none configured>");
  String out;
  if (g_targetMacCount == 1) {
    out += "MAC ";
    out += g_targetMacs[0];
  } else if (g_targetMacCount > 1) {
    out += "any of ";
    for (uint8_t i = 0; i < g_targetMacCount; i++) {
      if (i) out += ", ";
      out += g_targetMacs[i];
    }
  }
  if (g_matchUuid) {
    if (out.length()) out += " AND ";
    out += "iBeacon " + uuidToString(g_targetUuid);
    if (BEACON_MAJOR >= 0) out += " major=" + String(BEACON_MAJOR);
    if (BEACON_MINOR >= 0) out += " minor=" + String(BEACON_MINOR);
  }
  return out;
}

String targetMacsCsv() {
  String out;
  for (uint8_t i = 0; i < g_targetMacCount; i++) {
    if (i) out += ", ";
    out += g_targetMacs[i];
  }
  return out;
}

bool targetMacsAreStored() { return g_macsFromNvs; }

bool storeTargetMacs(const char *csv, String &error) {
  if (csv == nullptr) {
    error = "empty";
    return false;
  }

  // Validate into a scratch list first, so a bad entry cannot leave a
  // half-written list in NVS.
  char scratch[BEACON_MAX_MACS][18] = {};
  uint8_t count = 0;

  char buf[32];
  uint8_t len = 0;
  for (const char *c = csv;; c++) {
    const bool sep = (*c == ',' || *c == ';' || *c == ' ' || *c == '\t');
    if (*c != '\0' && !sep) {
      if (len < sizeof(buf) - 1) buf[len++] = static_cast<char>(tolower(*c));
      continue;
    }
    if (len > 0) {
      buf[len] = '\0';
      if (!looksLikeMac(buf)) {
        error = String("not a MAC address: ") + buf;
        return false;
      }
      if (count >= BEACON_MAX_MACS) {
        error = String("more than BEACON_MAX_MACS (") + BEACON_MAX_MACS + ") addresses";
        return false;
      }
      strncpy(scratch[count], buf, sizeof(scratch[0]) - 1);
      scratch[count][sizeof(scratch[0]) - 1] = '\0';
      count++;
      len = 0;
    }
    if (*c == '\0') break;
  }

  if (count == 0) {
    error = "no addresses given";
    return false;
  }

  String normalised;
  for (uint8_t i = 0; i < count; i++) {
    if (i) normalised += ",";
    normalised += scratch[i];
  }

  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
    error = "could not open NVS";
    return false;
  }
  const size_t written = prefs.putString(kNvsMacsKey, normalised);
  prefs.end();

  if (written == 0) {
    error = "NVS write failed";
    return false;
  }
  return true;
}

bool loadStoredThresholds(int &enterDbm, int &exitDbm) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/true)) return false;
  const int e = prefs.getInt("enter", 0);
  const int x = prefs.getInt("exit", 0);
  prefs.end();
  if (e == 0 || x == 0 || e <= x) return false;  // unset or nonsensical
  enterDbm = e;
  exitDbm = x;
  return true;
}

void storeThresholds(int enterDbm, int exitDbm) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) return;
  prefs.putInt("enter", enterDbm);
  prefs.putInt("exit", exitDbm);
  prefs.end();
}

void clearStoredThresholds() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) return;
  prefs.remove("enter");
  prefs.remove("exit");
  prefs.end();
}

bool loadStoredTiming(uint32_t &enterMs, uint32_t &exitMs, uint32_t &lockoutMs) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/true)) return false;
  const uint32_t e = prefs.getUInt("dwellEnter", 0);
  const uint32_t x = prefs.getUInt("dwellExit", 0);
  const uint32_t l = prefs.getUInt("lockout", 0);
  prefs.end();
  if (e == 0 || x == 0) return false;
  enterMs = e;
  exitMs = x;
  lockoutMs = l ? l : lockoutMs;
  return true;
}

void storeTiming(uint32_t enterMs, uint32_t exitMs, uint32_t lockoutMs) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) return;
  prefs.putUInt("dwellEnter", enterMs);
  prefs.putUInt("dwellExit", exitMs);
  prefs.putUInt("lockout", lockoutMs);
  prefs.end();
}

uint32_t loadStoredDirectionGap() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/true)) return 0;
  const uint32_t g = prefs.getUInt("dirGap", 0);
  prefs.end();
  return g;
}

void storeDirectionGap(uint32_t ms) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) return;
  prefs.putUInt("dirGap", ms);
  prefs.end();
}

void clearStoredTiming() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) return;
  prefs.remove("dwellEnter");
  prefs.remove("dwellExit");
  prefs.remove("lockout");
  prefs.remove("dirGap");
  prefs.end();
}

bool loadStoredFilter(uint8_t &windowSize, float &alpha) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/true)) return false;
  const uint32_t w = prefs.getUInt("filtWin", 0);
  const float a = prefs.getFloat("filtAlpha", 0.0f);
  prefs.end();
  if (w == 0 || a <= 0.0f) return false;
  windowSize = static_cast<uint8_t>(w);
  alpha = a;
  return true;
}

void storeFilter(uint8_t windowSize, float alpha) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) return;
  prefs.putUInt("filtWin", windowSize);
  prefs.putFloat("filtAlpha", alpha);
  prefs.end();
}

void clearStoredFilter() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) return;
  prefs.remove("filtWin");
  prefs.remove("filtAlpha");
  prefs.end();
}

void clearStoredTargetMacs() {
  Preferences prefs;
  if (prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
    prefs.remove(kNvsMacsKey);
    prefs.end();
  }
}

bool popSample(BleSample &out) {
  if (g_sampleQueue == nullptr) return false;
  return xQueueReceive(g_sampleQueue, &out, 0) == pdTRUE;
}

bool waitForSample(BleSample &out, uint32_t timeoutMs) {
  if (g_sampleQueue == nullptr) {
    vTaskDelay(pdMS_TO_TICKS(timeoutMs));
    return false;
  }
  return xQueueReceive(g_sampleQueue, &out, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

bool serviceWatchdog(uint32_t nowMs) {
  if (g_scan == nullptr) return false;

  // Silence from every device at once means the stack is wedged, not that the
  // neighbourhood went quiet — there is essentially always some BLE traffic.
  const uint32_t silentFor = advAgeMs(nowMs);
  if (silentFor < SCAN_WATCHDOG_MS) return false;

  // Do not thrash if restarting is not helping.
  if ((nowMs - g_lastScanRestartMs) < SCAN_WATCHDOG_MS) return false;

  Serial.printf("[ble] no advertisements for %lums, restarting scan\r\n",
                static_cast<unsigned long>(silentFor));
  g_scan->stop();
  g_scan->clearResults();
  applyScanSettings();
  g_scan->start(0, nullptr, false);

  g_lastScanRestartMs = nowMs;
  g_lastAdvMs = nowMs;  // give it a fresh window before judging again
  g_scanRestarts++;
  return true;
}

uint32_t lastAdvMs() { return g_lastAdvMs; }

uint32_t advAgeMs(uint32_t nowMs) {
  // Read the volatile exactly once: the BLE task can move it between reads.
  const uint32_t last = g_lastAdvMs;
  return (nowMs > last) ? (nowMs - last) : 0;
}
uint32_t advCount() { return g_advCount; }
uint32_t droppedSamples() { return g_droppedSamples; }
uint32_t scanRestarts() { return g_scanRestarts; }
bool targetEverSeen() { return g_targetSeen; }

bool targetTelemetry(EddystoneTlm &out, uint32_t &ageMs, uint32_t nowMs) {
  if (!g_haveTargetTlm) return false;
  out = g_targetTlm;
  const uint32_t seen = g_targetTlmMs;
  ageMs = (nowMs > seen) ? (nowMs - seen) : 0;
  return true;
}

void setDiscoverEnabled(bool enabled) { g_discover = enabled; }
bool discoverEnabled() { return g_discover; }

void dumpTable(Stream &out, uint32_t nowMs) {
  if (g_tableMutex == nullptr) return;
  if (xSemaphoreTake(g_tableMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

  out.println(F("---- BLE devices in range ----"));
  out.println(F("  MAC                RSSI  Dist    Age     Class      Name / manufacturer data"));

  int shown = 0;
  for (int i = 0; i < DEVICE_TABLE_SIZE; i++) {
    const SeenDevice &e = g_table[i];
    if (!e.used) continue;
    shown++;

    // Clamped: e.lastSeenMs is stamped by the BLE callback task and can sit a
    // few ms ahead of the control task's nowMs. Unclamped, a device heard this
    // instant prints an age of ~2^32 ms.
    const uint32_t ageMs = (nowMs > e.lastSeenMs) ? (nowMs - e.lastSeenMs) : 0;

    // Same rule as the proximity tracker: prefer the beacon's own calibrated
    // power, fall back to the configured reference so non-iBeacon devices still
    // get an estimate. Indicative only — RSSI is not a rangefinder.
    const int8_t refPower =
        (e.measuredPower != 0) ? e.measuredPower
                               : static_cast<int8_t>(BEACON_MEASURED_POWER_DBM);
    const float dist = estimateDistanceM(e.rssi, refPower, PATH_LOSS_EXPONENT);

    char distBuf[8];
    if (dist < 0.0f) {
      snprintf(distBuf, sizeof(distBuf), "   ?");
    } else if (dist < 100.0f) {
      snprintf(distBuf, sizeof(distBuf), "%4.1f", static_cast<double>(dist));
    } else {
      snprintf(distBuf, sizeof(distBuf), " 99+");
    }

    out.printf("%s %-17s %4d  %sm  %5lums %-10s %s", e.isTarget ? "->" : "  ", e.mac, e.rssi,
               distBuf, static_cast<unsigned long>(ageMs), deviceClassName(e.cls),
               e.name.length() ? e.name.c_str() : "");

    if (e.mfgHex.length()) {
      out.print(F(" mfg="));
      out.print(e.mfgHex.substring(0, 40));
    }
    if (e.svc.length()) {
      out.print(F(" svc="));
      out.print(e.svc);
    }
    out.println();
  }

  if (shown == 0) out.println(F("  (nothing heard yet)"));
  out.printf("  %d device(s), %lu advertisements total\r\n", shown,
             static_cast<unsigned long>(g_advCount));
  xSemaphoreGive(g_tableMutex);
}

}  // namespace BleScanner
