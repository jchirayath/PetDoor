#include "console.h"

#if PETDOOR_ENABLE_WIFI

#include <WiFi.h>

#include "eventlog.h"

ConsoleStream Con;

namespace {

WiFiServer g_server(CONSOLE_PORT);
WiFiClient g_client;
bool g_listening = false;

// A connected client is not a trusted one. Until the password arrives nothing
// it sends reaches the console and nothing the console says reaches it.
bool g_authed = false;
uint32_t g_connectedAtMs = 0;
char g_pwBuf[64];
uint8_t g_pwLen = 0;

// Telnet clients open with IAC negotiation (0xFF + command + option). The
// console would read those as keystrokes, so they are consumed here. `nc`
// sends none of this; `telnet` sends several.
uint8_t g_iacSkip = 0;

// The last octet of a client's address. All a 16-bit field can hold, and
// enough to distinguish "my laptop again" from "something new on the network".
int16_t lastOctet(const WiFiClient &c) {
  const IPAddress ip = const_cast<WiFiClient &>(c).remoteIP();
  return static_cast<int16_t>(ip[3]);
}

void dropClient(const char *why) {
  if (g_client) {
    if (g_authed && why != nullptr) {
      g_client.printf("\r\n*** %s ***\r\n", why);
      g_client.flush();
    }
    g_client.stop();
  }
  g_authed = false;
  g_pwLen = 0;
  g_iacSkip = 0;
}

}  // namespace

void ConsoleStream::begin(unsigned long baud) { Serial.begin(baud); }

size_t ConsoleStream::write(uint8_t c) {
  const size_t n = Serial.write(c);
  // Only an authenticated client sees output. Writing to a dead socket is
  // cheap to guard and expensive to get wrong — a blocked write here would
  // stall the control task, which owns the door.
  if (g_authed && g_client && g_client.connected()) g_client.write(c);
  return n;
}

size_t ConsoleStream::write(const uint8_t *buf, size_t size) {
  const size_t n = Serial.write(buf, size);
  if (g_authed && g_client && g_client.connected()) g_client.write(buf, size);
  return n;
}

int ConsoleStream::available() {
  if (Serial.available()) return Serial.available();
  if (g_authed && g_client && g_client.available()) return g_client.available();
  return 0;
}

int ConsoleStream::read() {
  if (Serial.available()) return Serial.read();
  if (g_authed && g_client && g_client.available()) return g_client.read();
  return -1;
}

int ConsoleStream::peek() {
  if (Serial.available()) return Serial.peek();
  if (g_authed && g_client && g_client.available()) return g_client.peek();
  return -1;
}

void ConsoleStream::flush() {
  Serial.flush();
  if (g_authed && g_client && g_client.connected()) g_client.flush();
}

bool ConsoleStream::networkAttached() const {
  return g_authed && g_client && g_client.connected();
}

IPAddress ConsoleStream::clientIp() const {
  return g_client ? g_client.remoteIP() : IPAddress();
}

namespace NetConsole {

void start() {
  if (g_listening) return;
  // No password, no console. Refused rather than started-and-unusable so the
  // reason appears in the log at the moment somebody tries to use it, instead
  // of looking like a network fault.
  if (strlen(CONSOLE_PASSWORD) == 0) {
    Serial.println(F("[console] not started: CONSOLE_PASSWORD is unset in secrets.h"));
    return;
  }
  g_server.begin();
  g_server.setNoDelay(true);
  g_listening = true;
  Serial.printf("[console] listening on port %d (window-bounded)\r\n", CONSOLE_PORT);
}

void stop() {
  if (!g_listening) return;
  dropClient("maintenance window closed");
  g_server.end();
  g_listening = false;
  Serial.println(F("[console] stopped listening"));
}

bool listening() { return g_listening; }

void tick(uint32_t nowMs) {
  if (!g_listening) return;

  // The address, announced once the radio has one. start() cannot print it:
  // the window opens before WiFi has associated, so at that moment localIP()
  // is 0.0.0.0. Without this you would have to already know the door's address
  // to reach the console that tells you the door's address.
  static bool announced = false;
  if (!announced && WiFi.status() == WL_CONNECTED) {
    announced = true;
    Serial.printf("[console] reachable at %s:%d\r\n",
                  WiFi.localIP().toString().c_str(), CONSOLE_PORT);
  }
  if (WiFi.status() != WL_CONNECTED) announced = false;

  // One client at a time. A second connection is refused rather than allowed
  // to displace the first: displacing would let anyone on the LAN knock an
  // authenticated operator off mid-calibration.
  if (WiFiClient incoming = g_server.available()) {
    if (g_client && g_client.connected()) {
      incoming.println(F("busy — another console is connected"));
      Serial.printf("[console] refused %s: already in session\r\n",
                    incoming.remoteIP().toString().c_str());
      EventLog::record(LOG_CONSOLE, 2, 0, static_cast<int16_t>(incoming.remoteIP()[3]));
      incoming.stop();
    } else {
      g_client = incoming;
      g_client.setNoDelay(true);
      g_authed = false;
      g_pwLen = 0;
      g_iacSkip = 0;
      g_connectedAtMs = nowMs;
      // The only thing an unauthenticated client is told. It reveals that
      // something is listening, which a port scan reveals anyway; it does not
      // reveal that this is a door.
      g_client.print(F("password: "));
    }
  }

  if (!g_client) return;
  if (!g_client.connected()) {
    dropClient(nullptr);
    return;
  }

  if (g_authed) return;  // authenticated input is drained by the console itself

  // Unauthenticated: a bounded time to produce a password, then dropped. This
  // stops a half-open connection from holding the single client slot for the
  // rest of the window.
  if (nowMs - g_connectedAtMs > CONSOLE_AUTH_TIMEOUT_MS) {
    g_client.println(F("\r\ntimeout"));
    Serial.printf("[console] %s gave no password in time\r\n",
                  g_client.remoteIP().toString().c_str());
    EventLog::record(LOG_CONSOLE, 3, 0, lastOctet(g_client));
    dropClient(nullptr);
    return;
  }

  while (g_client.available()) {
    const int ci = g_client.read();
    if (ci < 0) break;
    const uint8_t c = static_cast<uint8_t>(ci);

    if (g_iacSkip > 0) { g_iacSkip--; continue; }
    if (c == 0xFF) { g_iacSkip = 2; continue; }

    if (c == '\r') continue;
    if (c != '\n') {
      if (g_pwLen < sizeof(g_pwBuf) - 1) g_pwBuf[g_pwLen++] = static_cast<char>(c);
      continue;
    }

    g_pwBuf[g_pwLen] = '\0';
    const bool ok = (strlen(CONSOLE_PASSWORD) > 0) &&
                    (strcmp(g_pwBuf, CONSOLE_PASSWORD) == 0);
    // Wiped whether or not it matched: this buffer held a secret and lives in
    // static RAM for the life of the program.
    memset(g_pwBuf, 0, sizeof(g_pwBuf));
    g_pwLen = 0;

    if (!ok) {
      g_client.println(F("\r\nno."));
      Serial.printf("[console] rejected a client from %s\r\n",
                    g_client.remoteIP().toString().c_str());
      // The entry that matters: a wrong password against a port that can open
      // the door. It uploads with everything else, so it is visible from a
      // browser rather than only to somebody holding a serial cable.
      EventLog::record(LOG_CONSOLE, 0, 0, lastOctet(g_client));
      dropClient(nullptr);
      return;
    }

    g_authed = true;
    Serial.printf("[console] %s attached\r\n", g_client.remoteIP().toString().c_str());
    EventLog::record(LOG_CONSOLE, 1, 0, lastOctet(g_client));
    g_client.println(F("\r\nattached. 'h' for help. This session ends when the "
                       "maintenance window does."));
    return;
  }
}

}  // namespace NetConsole

#endif  // PETDOOR_ENABLE_WIFI
