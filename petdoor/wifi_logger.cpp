#include "wifi_logger.h"

#include <ArduinoOTA.h>
#include <HTTPClient.h>
#if LOG_ALLOW_TLS
#include <WiFiClientSecure.h>
#endif
#include <WiFi.h>
#include <mbedtls/md.h>
#include <time.h>

#include "eventlog.h"

namespace WifiLogger {
namespace {

volatile bool g_otaRequested = false;
volatile bool g_otaOpen = false;
uint32_t g_otaOpenedMs = 0;
bool g_otaBegun = false;

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

// HMAC-SHA256 of `body` under `key`, lower-case hex. SHA256 is in mbedTLS
// already (WPA2 needs it), so this costs microseconds and no extra flash.
String signBody(const String &body, const char *key, const String &timestamp) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr) return String();

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, info, 1) != 0) {
    mbedtls_md_free(&ctx);
    return String();
  }
  mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const unsigned char *>(key), strlen(key));
  // Sign timestamp + body, not body alone: without the timestamp inside the
  // signature an eavesdropper could replay an old batch verbatim.
  mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char *>(timestamp.c_str()),
                         timestamp.length());
  mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char *>("\n"), 1);
  mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char *>(body.c_str()),
                         body.length());
  unsigned char out[32];
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);

  static const char hexd[] = "0123456789abcdef";
  String hex;
  hex.reserve(64);
  for (int i = 0; i < 32; i++) {
    hex += hexd[out[i] >> 4];
    hex += hexd[out[i] & 0x0F];
  }
  return hex;
}

int g_lastHttpCode = 0;

bool post(const String &body) {
  if (LOG_ENDPOINT_URL[0] == '\0') return false;

  // Plain HTTP where we can, TLS where the endpoint demands it.
  //
  // HTTP is preferred and is what a LAN endpoint should use: the upload is
  // already authenticated by HMAC, and a TLS handshake costs 1-3 s of radio and
  // ~40 KB of heap. But an endpoint on the public internet is usually
  // HTTPS-only — it will answer plain HTTP with a redirect the door cannot
  // usefully follow — and there the events would otherwise cross the internet
  // in clear. Encryption is worth the handshake in that case, because uploads
  // only happen while the animal is away.
  //
  // Certificates are not verified. The door sends and never acts on a reply, so
  // an impostor server gains nothing it could not get by listening; validating
  // would mean shipping and rotating a CA bundle on a device with no clock at
  // boot. This protects against interception, not impersonation.
  const bool wantsTls = String(LOG_ENDPOINT_URL).startsWith("https:");

  WiFiClient plain;
  HTTPClient http;
  bool began;
#if LOG_ALLOW_TLS
  WiFiClientSecure secure;
  if (wantsTls) secure.setInsecure();
  began = wantsTls ? http.begin(secure, LOG_ENDPOINT_URL)
                   : http.begin(plain, LOG_ENDPOINT_URL);
#else
  if (wantsTls) {
    // Fail loudly rather than silently posting nothing: an https endpoint with
    // TLS compiled out can never work, and the reason is not obvious.
    Serial.println(F("[wifi] LOG_ENDPOINT_URL is https but LOG_ALLOW_TLS is 0."));
    Serial.println(F("[wifi] Use an http:// endpoint, or read the note in config.h."));
    g_lastHttpCode = -2;
    return false;
  }
  began = http.begin(plain, LOG_ENDPOINT_URL);
#endif
  if (!began) {
    g_lastHttpCode = -1;
    return false;
  }
  http.setTimeout(8000);
  http.addHeader("Content-Type", "text/csv");
  http.addHeader("X-PetDoor-Id", LOG_DEVICE_ID);

  const String ts = String(static_cast<unsigned long>(time(nullptr)));
  http.addHeader("X-PetDoor-Timestamp", ts);

  if (LOG_SHARED_KEY[0] != '\0') {
    const String sig = signBody(body, LOG_SHARED_KEY, ts);
    if (sig.length()) http.addHeader("X-PetDoor-Signature", sig);
  }

  const int code = http.POST(const_cast<uint8_t *>(
                                 reinterpret_cast<const uint8_t *>(body.c_str())),
                             body.length());
  g_lastHttpCode = code;
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
    if (LOG_ENDPOINT_URL[0] == '\0') {
      Serial.println(F("[wifi] no LOG_ENDPOINT_URL set — logging locally only"));
    } else if (g_lastHttpCode == 401 || g_lastHttpCode == 403) {
      Serial.printf("[wifi] upload REJECTED (HTTP %d) — the server did not accept "
                    "our signature. Check LOG_SHARED_KEY matches.\r\n", g_lastHttpCode);
    } else if (g_lastHttpCode > 0) {
      Serial.printf("[wifi] upload failed, HTTP %d\r\n", g_lastHttpCode);
    } else {
      Serial.println(F("[wifi] upload failed — endpoint unreachable"));
    }
    // Deliberately NOT recorded in the event log: a failure entry would be a
    // new event, which would arm another upload, which could fail again.
    Serial.println(F("[wifi] events are kept locally and will be retried"));
  }
  Serial.println(F("[wifi] radio down"));
  g_busy = false;
}

void startOta() {
  if (!g_otaBegun) {
    ArduinoOTA.setHostname(LOG_DEVICE_ID);
    if (OTA_PASSWORD[0] != '\0') ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
      Serial.println(F("\r\n[ota] receiving firmware — do not power off"));
    });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
      static int last = -1;
      const int pct = total ? (done * 100) / total : 0;
      if (pct / 10 != last / 10) {
        last = pct;
        Serial.printf("[ota] %d%%\r\n", pct);
      }
    });
    ArduinoOTA.onEnd([]() {
      Serial.println(F("[ota] written; rebooting into the new firmware"));
    });
    ArduinoOTA.onError([](ota_error_t e) {
      Serial.printf("[ota] failed (%u)%s\r\n", e,
                    e == OTA_AUTH_ERROR ? " — wrong password" : "");
    });
    ArduinoOTA.begin();
    g_otaBegun = true;
  }
  g_otaOpen = true;
  g_otaOpenedMs = millis();
  Serial.printf("[ota] window open for %lu s — the radio is up, so BLE sampling\r\n",
                static_cast<unsigned long>(OTA_WINDOW_MS / 1000));
  Serial.println(F("[ota] is degraded until it closes."));
  Serial.printf("[ota] push with:  arduino-cli upload --fqbn "
                "esp32:esp32:esp32:PartitionScheme=min_spiffs -p %s petdoor\r\n",
                WiFi.localIP().toString().c_str());
}

void stopOta(const char *why) {
  if (!g_otaOpen) return;
  g_otaOpen = false;
  radioDown();
  Serial.printf("[ota] window closed (%s); radio back to BLE\r\n", why);
}

void uploaderTask(void *) {
  for (;;) {
    if (g_otaRequested) {
      g_otaRequested = false;
      if (radioUp()) {
        startOta();
      } else {
        Serial.println(F("[ota] could not associate; window not opened"));
        radioDown();
      }
    }

    if (g_otaOpen) {
      ArduinoOTA.handle();
      if ((millis() - g_otaOpenedMs) > OTA_WINDOW_MS) stopOta("timed out");
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;  // an update window takes precedence over log uploads
    }

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

bool busy() { return g_busy || g_otaOpen; }

bool otaWindowOpen() { return g_otaOpen; }

void beginOtaWindow(bool petPresent) {
  if (!configured()) {
    Serial.println(F("[ota] no WIFI_SSID configured — use the IO0/EN buttons"));
    return;
  }
  if (OTA_PASSWORD[0] == '\0') {
    Serial.println(F("[ota] OTA_PASSWORD is not set, so OTA is disabled."));
    Serial.println(F("[ota] Without one, anyone on the network could reflash the door."));
    return;
  }
  if (petPresent) {
    Serial.println(F("[ota] refused: the beacon is present. An update reboots the"));
    Serial.println(F("[ota] door and shares the radio — wait until they are away."));
    return;
  }
  if (g_otaOpen) {
    Serial.println(F("[ota] window already open"));
    return;
  }
  g_otaRequested = true;
}

void closeOtaWindow() { stopOta("closed by request"); }

void printStatus(Stream &out) {
  if (!configured()) {
    out.println(F("  wifi         : not configured (no WIFI_SSID)"));
    return;
  }
  if (LOG_ENDPOINT_URL[0] == '\0') {
    out.println(F("  wifi         : configured, but no log endpoint — local only"));
    return;
  }
  out.printf("  log endpoint : %s%s%s\r\n", LOG_ENDPOINT_URL,
             LOG_SHARED_KEY[0] ? "  (signed)" : "  (unsigned)",
             String(LOG_ENDPOINT_URL).startsWith("https:") ? " (TLS)" : "");
  if (g_lastHttpCode) out.printf("  last response: HTTP %d\r\n", g_lastHttpCode);
  out.printf("  wifi         : %s, %lu uploads, %lu failures%s\r\n",
             g_busy ? "RADIO UP" : "idle (radio off)",
             static_cast<unsigned long>(g_uploads),
             static_cast<unsigned long>(g_failures),
             g_clockSynced ? ", clock synced" : ", clock not set");
}

}  // namespace WifiLogger
