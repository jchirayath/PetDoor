#include "wifi_logger.h"

#if PETDOOR_ENABLE_WIFI

#include <ArduinoOTA.h>
#include <esp_ota_ops.h>
#include <HTTPClient.h>
#if LOG_ALLOW_TLS
#include <WiFiClientSecure.h>
#endif
#include <WiFi.h>
#include <mbedtls/md.h>
#include <time.h>

#include "eventlog.h"

// Tell the Arduino core NOT to decide at boot whether a freshly flashed image
// is good. Left alone it confirms every image before this firmware runs a
// single line, which makes the bootloader's rollback unreachable — and an
// image that boots but cannot join WiFi is then permanent.
//
// Returning true defers that judgement to confirmImage() below.
#if OTA_REQUIRE_CONFIRM
extern "C" bool verifyRollbackLater() {
  return true;
}
#endif

namespace WifiLogger {

uint32_t g_bootCount = 0;

namespace {

volatile bool g_otaRequested = false;

#if REMOTE_CONFIG
// Commands arrive on the WiFi task and are applied by the control task, which
// is the only owner of the door and the tracker (invariant 9). This queue is
// the handover, exactly as BLE samples cross the other way.
QueueHandle_t g_cmdQueue = nullptr;
// Reported on the NEXT upload, so the server can see what became of what it
// sent rather than assuming delivery meant application.
char g_ackText[96] = {0};

// A fresh random value sent with every upload and folded into the signature we
// expect on the reply. Without it the HMAC proves only that the SERVER once
// said these bytes — not that it said them just now, to this request.
//
// That distinction matters because uploads are plain HTTP by design. Anyone on
// the path can record a reply carrying `door open` or `ota` and play it back at
// a moment of their choosing; the signature would still be valid. Binding the
// reply to a value the attacker cannot predict and the door has just invented
// makes an old reply verify against the wrong material and fail.
//
// Note the asymmetry this repairs: the upload direction was already protected,
// because the server rejects timestamps outside its skew window. The reply
// direction had no equivalent.
char g_nonce[33] = {0};
#endif

// Telemetry, not remote control: these ride every upload so a door can be
// understood from the server even in a build that refuses to take orders.
// Keep them OUTSIDE the REMOTE_CONFIG guard.
//
// Filled by the control task, which owns the tracker and the door; the WiFi
// task only sends the string it was handed.
char g_statusLine[224] = {0};

// Seeded from config.h, then overridable at runtime and from NVS.
uint32_t g_settleMs = WIFI_IDLE_SETTLE_MS;
uint32_t g_minUploadMs = WIFI_MIN_UPLOAD_INTERVAL_MS;
uint32_t g_heartbeatMs = WIFI_HEARTBEAT_MS;
// Bigger than the status line because it carries every tunable at once.
char g_configLine[320] = {0};

// A pending discovery-table dump. A String rather than a fixed buffer because
// it is several KB and exists only between a `scan` request and the next flush.
String g_scanPayload;

#if OTA_REQUIRE_CONFIRM
bool g_imageConfirmed = false;

// Confirm the running image if the bootloader is still waiting to hear that it
// works. Called only once an upload has SUCCEEDED, because reaching the server
// is precisely the property worth proving: an image that boots but cannot be
// managed is the one that must not be kept.
void confirmImage(const char *why) {
  if (g_imageConfirmed) return;
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running == nullptr || esp_ota_get_state_partition(running, &state) != ESP_OK) {
    g_imageConfirmed = true;          // not an OTA slot; nothing to confirm
    return;
  }
  if (state != ESP_OTA_IMG_PENDING_VERIFY) {
    g_imageConfirmed = true;          // already settled
    return;
  }
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    g_imageConfirmed = true;
    Serial.printf("[ota] image confirmed good (%s); rollback cancelled\r\n", why);
  } else {
    Serial.println(F("[ota] could not confirm image; it rolls back on next reboot"));
  }
}
#endif

#if REMOTE_CONFIG

void newNonce() {
  static const char hexd[] = "0123456789abcdef";
  for (int i = 0; i < 32; i += 8) {
    const uint32_t r = esp_random();   // hardware RNG; the radio is up here
    for (int b = 0; b < 8; b++) g_nonce[i + b] = hexd[(r >> (28 - 4 * b)) & 0xF];
  }
  g_nonce[32] = '\0';
}
#endif
volatile bool g_otaOpen = false;
uint32_t g_otaOpenedMs = 0;
bool g_otaBegun = false;
TaskHandle_t g_taskHandle = nullptr;

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

#if REMOTE_CONFIG
// Split `body` on newlines and queue each non-empty, non-comment line.
void enqueueCommands(const String &body) {
  if (g_cmdQueue == nullptr) return;
  int start = 0;
  int queued = 0, dropped = 0;
  while (start < static_cast<int>(body.length())) {
    int nl = body.indexOf('\n', start);
    if (nl < 0) nl = body.length();
    String line = body.substring(start, nl);
    line.trim();
    start = nl + 1;
    if (!line.length() || line.startsWith("#")) continue;
    if (line.length() >= REMOTE_CMD_MAX_LEN) {
      dropped++;
      continue;
    }
    char buf[REMOTE_CMD_MAX_LEN] = {0};
    strncpy(buf, line.c_str(), sizeof(buf) - 1);
    if (xQueueSend(g_cmdQueue, buf, 0) == pdTRUE) queued++;
    else dropped++;
  }
  if (queued || dropped) {
    Serial.printf("[cmd] %d command(s) from the server%s\r\n", queued,
                  dropped ? ", some dropped (queue full or line too long)" : "");
  }
}

// The reply is only obeyed if it carries a valid signature over its own
// timestamp and body, under the same key the upload was signed with.
//
// Without this the channel would be an open instruction to any device on the
// path between door and server: uploads are plain HTTP on purpose, because a
// TLS handshake costs this chip more heap than it has.
bool responseTrusted(const String &body, const String &ts, const String &sig) {
  if (g_nonce[0] == '\0') {
    // No outstanding nonce means no upload is waiting on a reply, so anything
    // arriving now is unsolicited by definition.
    Serial.println(F("[cmd] reply ignored: no request outstanding"));
    return false;
  }
  if (LOG_SHARED_KEY[0] == '\0') {
    // No key means no way to tell the server from anyone else. Log uploads can
    // survive that; taking orders cannot.
    Serial.println(F("[cmd] reply ignored: no LOG_SHARED_KEY, so it cannot be verified"));
    return false;
  }
  if (!sig.length()) {
    Serial.println(F("[cmd] reply ignored: unsigned"));
    return false;
  }
  // Signed material is  ts + "\n" + nonce + "\n" + body.  signBody() puts a
  // newline after its `timestamp` argument, so passing ts+"\n"+nonce yields
  // exactly that, and matches what the server computes.
  const String expect = signBody(body, LOG_SHARED_KEY, ts + "\n" + String(g_nonce));
  if (!expect.length()) return false;
  // Length-constant compare, so a wrong signature cannot be narrowed down by
  // timing how long the rejection took.
  String got = sig;
  got.trim();
  got.toLowerCase();
  if (got.length() != expect.length()) {
    Serial.println(F("[cmd] reply ignored: signature malformed"));
    return false;
  }
  uint8_t diff = 0;
  for (size_t i = 0; i < expect.length(); i++) diff |= expect[i] ^ got[i];
  if (diff) {
    Serial.println(F("[cmd] reply ignored: BAD SIGNATURE — the server's key differs, or"));
    Serial.println(F("[cmd] something on the path is trying to reconfigure this door"));
    return false;
  }
  return true;
}
#endif  // REMOTE_CONFIG

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
  // Reported so a server collecting from several doors can tell which build
  // and which boot an upload came from — the first thing you want after an
  // update, and the thing that makes a reboot loop visible from the server.
  http.addHeader("X-PetDoor-Version", PETDOOR_VERSION);
  http.addHeader("X-PetDoor-Build", PETDOOR_BUILD);
  http.addHeader("X-PetDoor-Boot", String(g_bootCount));
  // Where to push an update to. The door used to print this to the serial
  // console only, which is no use at all once it is on a wall: you could open
  // an OTA window and still have nowhere to aim. The server sees the proxy's
  // address, never the door's, so the door has to say.
  http.addHeader("X-PetDoor-IP", WiFi.localIP().toString());
  // A snapshot of what `s` shows on the console, so the door can be tuned by
  // someone who cannot reach it. Compact on purpose: this rides every upload.
  if (g_statusLine[0] != '\0') http.addHeader("X-PetDoor-Status", g_statusLine);
  // The settings behind that snapshot, so the dashboard can show what each
  // value currently IS rather than offering blank boxes.
  if (g_configLine[0] != '\0') http.addHeader("X-PetDoor-Config", g_configLine);

  const String ts = String(static_cast<unsigned long>(time(nullptr)));
  http.addHeader("X-PetDoor-Timestamp", ts);

  if (LOG_SHARED_KEY[0] != '\0') {
    const String sig = signBody(body, LOG_SHARED_KEY, ts);
    if (sig.length()) http.addHeader("X-PetDoor-Signature", sig);
  }

#if REMOTE_CONFIG
  // What became of the commands from last time. Sent now rather than straight
  // after applying them: by then the radio is down, and raising it again just
  // to acknowledge would cost more BLE time than the acknowledgement is worth.
  if (g_ackText[0] != '\0') http.addHeader("X-PetDoor-Ack", g_ackText);
  // Tells the server this build can accept commands, so it can warn about a
  // door running older firmware that will never collect what it queues.
  http.addHeader("X-PetDoor-Remote", "1");
  // A new one for every upload: a nonce reused is a nonce that can be replayed
  // against.
  newNonce();
  http.addHeader("X-PetDoor-Nonce", g_nonce);
  const char *wanted[] = {"X-PetDoor-Timestamp", "X-PetDoor-Signature"};
  http.collectHeaders(wanted, 2);
#endif

  const int code = http.POST(const_cast<uint8_t *>(
                                 reinterpret_cast<const uint8_t *>(body.c_str())),
                             body.length());
  g_lastHttpCode = code;

#if REMOTE_CONFIG
  if (code >= 200 && code < 300) {
    g_ackText[0] = '\0';            // delivered — stop repeating it
    const String reply = http.getString();
    if (reply.length() &&
        responseTrusted(reply, http.header("X-PetDoor-Timestamp"),
                        http.header("X-PetDoor-Signature"))) {
      enqueueCommands(reply);
    }
  }
  // Burn the nonce whatever happened. One upload may authorise at most one
  // reply; leaving it live would let a second, later reply reuse the binding.
  g_nonce[0] = '\0';
#endif

  http.end();
  return code >= 200 && code < 300;
}


// Upload a discovery table. Same signing, same key, different Kind — so the
// server can file it separately without guessing from the body.
bool postScan(const String &text) {
  WiFiClient plain;
  HTTPClient http;
  if (!http.begin(plain, LOG_ENDPOINT_URL)) return false;
  http.setTimeout(8000);
  http.addHeader("Content-Type", "text/plain");
  http.addHeader("X-PetDoor-Id", LOG_DEVICE_ID);
  http.addHeader("X-PetDoor-Kind", "scan");
  const String ts = String(static_cast<unsigned long>(time(nullptr)));
  http.addHeader("X-PetDoor-Timestamp", ts);
  if (LOG_SHARED_KEY[0] != '\0') {
    const String sig = signBody(text, LOG_SHARED_KEY, ts);
    if (sig.length()) http.addHeader("X-PetDoor-Signature", sig);
  }
  const int code = http.POST(const_cast<uint8_t *>(
                                 reinterpret_cast<const uint8_t *>(text.c_str())),
                             text.length());
  http.end();
  if (code >= 200 && code < 300) {
    g_scanPayload = String();      // delivered; do not repeat it
    return true;
  }
  return false;
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

  // A discovery table, if one was asked for. Sent as its own POST rather than
  // mixed into the event body: it is not events, and the server's CSV parser
  // should not have to tell the difference.
  if (ok && g_scanPayload.length()) {
    if (postScan(g_scanPayload)) {
      Serial.printf("[cmd] uploaded discovery table (%u bytes)\r\n",
                    g_scanPayload.length());
    } else {
      Serial.println(F("[cmd] discovery table upload failed; will retry next flush"));
    }
  }

  radioDown();
  g_lastUploadMs = millis();
  g_haveUploaded = true;
  if (ok) {
    g_uploads++;
#if OTA_REQUIRE_CONFIRM
    // Reaching the server is the capability a new image has to demonstrate.
    confirmImage("upload succeeded");
#endif
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

void setBootCount(uint32_t n) { g_bootCount = n; }

void begin() {
#if REMOTE_CONFIG
  if (g_cmdQueue == nullptr) {
    g_cmdQueue = xQueueCreate(REMOTE_CMD_QUEUE_DEPTH, REMOTE_CMD_MAX_LEN);
  }
#endif
  if (!configured()) return;
  WiFi.mode(WIFI_OFF);  // explicit: nothing is radiating until we ask
  xTaskCreatePinnedToCore(uploaderTask, "petdoor-wifi", WIFI_TASK_STACK, nullptr, 1,
                          &g_taskHandle, 0);
}

void tick(uint32_t nowMs, bool idle) {
  if (!configured()) return;
  g_idleNow = idle;

  // A door that only speaks while the animal is out is a door that goes silent
  // for a whole rainy weekend — and a silent door collects no commands and
  // reports no status. So past WIFI_HEARTBEAT_MS, call in regardless.
  //
  // This deliberately costs BLE time at a moment the beacon may be present,
  // which is exactly what the idle gate exists to avoid. Half an hour between
  // bursts makes that a rounding error; losing the channel does not.
  const bool overdue = g_heartbeatMs > 0 && g_haveUploaded &&
                       (nowMs - g_lastUploadMs) >= g_heartbeatMs;

  if (!idle && !overdue) {
    g_idleValid = false;
    return;
  }
  if (overdue && !g_busy && !g_otaOpen) {
    g_idleValid = false;
    g_flushRequested = true;
    return;
  }
  if (!g_idleValid) {
    g_idleSinceMs = nowMs;
    g_idleValid = true;
    return;
  }
  if ((nowMs - g_idleSinceMs) < g_settleMs) return;
  if (g_haveUploaded && (nowMs - g_lastUploadMs) < g_minUploadMs) return;
  if (EventLog::count() == 0) return;   // nothing to say yet (first boot only)
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

void setUploadTiming(uint32_t settleMs, uint32_t minIntervalMs, uint32_t heartbeatMs) {
  g_settleMs = settleMs;
  g_minUploadMs = minIntervalMs;
  g_heartbeatMs = heartbeatMs;
}
uint32_t settleMs() { return g_settleMs; }
uint32_t minIntervalMs() { return g_minUploadMs; }
uint32_t heartbeatMs() { return g_heartbeatMs; }

bool busy() { return g_busy || g_otaOpen; }

uint32_t stackFreeBytes() {
  if (!g_taskHandle) return 0;
  return uxTaskGetStackHighWaterMark(g_taskHandle) * sizeof(StackType_t);
}

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

#if REMOTE_CONFIG
bool popCommand(char *out) {
  if (g_cmdQueue == nullptr) return false;
  return xQueueReceive(g_cmdQueue, out, 0) == pdTRUE;
}

void setAck(const char *text) {
  strncpy(g_ackText, text ? text : "", sizeof(g_ackText) - 1);
  g_ackText[sizeof(g_ackText) - 1] = '\0';
}
#endif

void queueScanUpload(const String &text) { g_scanPayload = text; }

void setStatusLine(const char *text) {
  strncpy(g_statusLine, text ? text : "", sizeof(g_statusLine) - 1);
  g_statusLine[sizeof(g_statusLine) - 1] = '\0';
}

void setConfigLine(const char *text) {
  strncpy(g_configLine, text ? text : "", sizeof(g_configLine) - 1);
  g_configLine[sizeof(g_configLine) - 1] = '\0';
}

bool imageConfirmed() {
#if OTA_REQUIRE_CONFIRM
  return g_imageConfirmed;
#else
  return true;
#endif
}

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

#else  // PETDOOR_ENABLE_WIFI

// Not compiled in. The API stays so the rest of the firmware needs no #ifdefs;
// every call is a no-op and the linker drops the network stack entirely.
namespace WifiLogger {
bool isEnabled() { return false; }
void begin() {}
void setBootCount(uint32_t) {}
void tick(uint32_t, bool) {}
void requestFlushNow() {
  Serial.println(F("[wifi] not compiled in (PETDOOR_ENABLE_WIFI is 0)"));
}
void setUploadTiming(uint32_t, uint32_t, uint32_t) {}
uint32_t settleMs() { return WIFI_IDLE_SETTLE_MS; }
uint32_t minIntervalMs() { return WIFI_MIN_UPLOAD_INTERVAL_MS; }
uint32_t heartbeatMs() { return WIFI_HEARTBEAT_MS; }
void setStatusLine(const char *) {}
void setConfigLine(const char *) {}
void queueScanUpload(const String &) {}
bool imageConfirmed() { return true; }
bool busy() { return false; }
uint32_t stackFreeBytes() { return 0; }
bool otaWindowOpen() { return false; }
void closeOtaWindow() {}
void beginOtaWindow(bool) {
  Serial.println(F("[ota] not compiled in — use the IO0/EN buttons"));
}
void printStatus(Stream &out) {
  out.println(F("  wifi         : not compiled in"));
}
}  // namespace WifiLogger

#endif  // PETDOOR_ENABLE_WIFI
