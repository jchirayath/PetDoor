#include "wifi_logger.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <time.h>

#include "eventlog.h"

namespace WifiLogger {
namespace {

volatile bool g_flushRequested = false;
volatile bool g_busy = false;

uint32_t g_idleSinceMs = 0;
bool g_idleValid = false;
uint32_t g_lastUploadMs = 0;
bool g_haveUploaded = false;

uint32_t g_uploads = 0;
uint32_t g_failures = 0;
bool g_clockSynced = false;

// Set by the control loop so the uploader can bail out if the animal returns
// while the radio is busy.
volatile bool g_idleNow = false;

bool configured() {
  return WIFI_SSID[0] != '\0';
}

// Brings the radio up. Returns false on timeout so a missing access point
// cannot hold BLE hostage indefinitely.
bool radioUp() {
  WiFi.mode(WIFI_STA);
  // Modem sleep: lets the BLE controller have the airtime between beacons.
  WiFi.setSleep(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if ((millis() - started) > WIFI_CONNECT_TIMEOUT_MS) return false;
    if (!g_idleNow) return false;  // animal came back; abandon the attempt
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return true;
}

void radioDown() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
}

void syncClock() {
  configTime(0, 0, NTP_SERVER);  // UTC; the log stores epoch seconds
  const uint32_t started = millis();
  while ((millis() - started) < 5000) {
    const time_t now = time(nullptr);
    if (now > 1700000000) {  // plausibly past 2023, so NTP has answered
      EventLog::setEpoch(static_cast<uint32_t>(now));
      g_clockSynced = true;
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// Builds the CSV body in one String. The log is bounded at
// EVENT_LOG_CAPACITY * ~48 bytes, so this stays a few KB.
String buildBody() {
  String body;
  body.reserve(1024);
  body += F("epoch,uptime_s,boot,event,detail,rssi\n");
  LogEntry e;
  for (uint16_t i = 0; i < EventLog::count(); i++) {
    if (!EventLog::get(i, e)) break;
    body += String(e.epochSec);   body += ',';
    body += String(e.uptimeSec);  body += ',';
    body += String(e.bootNum);    body += ',';
    body += EventLog::typeName(e.type); body += ',';
    body += String(e.detail);     body += ',';
    body += String(e.rssi);       body += '\n';
  }
  return body;
}

bool post(const String &body) {
  if (LOG_ENDPOINT_URL[0] == '\0') return false;
  HTTPClient http;
  if (!http.begin(LOG_ENDPOINT_URL)) return false;
  http.setTimeout(8000);
  http.addHeader("Content-Type", "text/csv");
  const int code = http.POST(const_cast<uint8_t *>(
                                 reinterpret_cast<const uint8_t *>(body.c_str())),
                             body.length());
  http.end();
  return code >= 200 && code < 300;
}

void doFlush() {
  g_busy = true;
  Serial.println(F("[wifi] radio up — BLE sampling is degraded until this finishes"));

  if (!radioUp()) {
    Serial.println(F("[wifi] could not associate; giving the radio back to BLE"));
    radioDown();
    g_failures++;
    g_busy = false;
    return;
  }

  syncClock();
  const String body = buildBody();
  const bool ok = post(body);

  radioDown();
  g_lastUploadMs = millis();
  g_haveUploaded = true;
  if (ok) {
    g_uploads++;
    Serial.printf("[wifi] uploaded %u events%s\r\n", EventLog::count(),
                  g_clockSynced ? ", clock synced" : "");
  } else {
    g_failures++;
    Serial.println(F("[wifi] upload failed (endpoint unset or unreachable)"));
  }
  Serial.println(F("[wifi] radio down"));
  g_busy = false;
}

void uploaderTask(void *) {
  for (;;) {
    if (g_flushRequested) {
      g_flushRequested = false;
      doFlush();
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

}  // namespace

bool isEnabled() { return configured(); }

void begin() {
  if (!configured()) return;
  WiFi.mode(WIFI_OFF);  // explicit: nothing is radiating until we ask
  xTaskCreatePinnedToCore(uploaderTask, "petdoor-wifi", 6144, nullptr, 1, nullptr, 0);
}

void tick(uint32_t nowMs, bool idle) {
  if (!configured()) return;
  g_idleNow = idle;

  if (!idle) {
    g_idleValid = false;
    return;
  }
  if (!g_idleValid) {
    g_idleSinceMs = nowMs;
    g_idleValid = true;
    return;
  }
  if ((nowMs - g_idleSinceMs) < WIFI_IDLE_SETTLE_MS) return;
  if (g_haveUploaded && (nowMs - g_lastUploadMs) < WIFI_MIN_UPLOAD_INTERVAL_MS) return;
  if (EventLog::count() == 0) return;
  if (g_busy) return;

  g_idleValid = false;  // re-arm only after another settled idle period
  g_flushRequested = true;
}

void requestFlushNow() {
  if (!configured()) {
    Serial.println(F("[wifi] no WIFI_SSID configured; nothing to do"));
    return;
  }
  g_idleNow = true;  // manual request overrides the idle gate
  g_flushRequested = true;
  Serial.println(F("[wifi] flush requested"));
}

bool busy() { return g_busy; }

void printStatus(Stream &out) {
  if (!configured()) {
    out.println(F("  wifi         : not configured (no WIFI_SSID)"));
    return;
  }
  out.printf("  wifi         : %s, %lu uploads, %lu failures%s\r\n",
             g_busy ? "RADIO UP" : "idle (radio off)",
             static_cast<unsigned long>(g_uploads),
             static_cast<unsigned long>(g_failures),
             g_clockSynced ? ", clock synced" : ", clock not set");
}

}  // namespace WifiLogger
