#include "eventlog.h"

#include <Preferences.h>
#include <string.h>

namespace EventLog {
namespace {

constexpr char kNvs[] = "petdoor";
constexpr char kBlobKey[] = "evlog";
constexpr char kHeadKey[] = "evhead";
constexpr char kCountKey[] = "evcount";

LogEntry g_ring[EVENT_LOG_CAPACITY];
uint16_t g_head = 0;   // next slot to write
uint16_t g_count = 0;  // entries in use, <= capacity
uint16_t g_bootNum = 0;

// Wall clock is held as an offset so it stays correct as millis() advances.
// 0 means "we have never been told the time".
uint32_t g_epochAtBootSec = 0;

void persist() {
  Preferences prefs;
  if (!prefs.begin(kNvs, /*readOnly=*/false)) return;
  prefs.putBytes(kBlobKey, g_ring, sizeof(g_ring));
  prefs.putUShort(kHeadKey, g_head);
  prefs.putUShort(kCountKey, g_count);
  prefs.end();
}

}  // namespace

void begin(uint16_t bootNum) {
  g_bootNum = bootNum;
  memset(g_ring, 0, sizeof(g_ring));

  Preferences prefs;
  if (!prefs.begin(kNvs, /*readOnly=*/true)) return;
  const size_t got = prefs.getBytes(kBlobKey, g_ring, sizeof(g_ring));
  g_head = prefs.getUShort(kHeadKey, 0);
  g_count = prefs.getUShort(kCountKey, 0);
  prefs.end();

  // A short or absent blob means this is the first boot with logging, or the
  // capacity changed between builds. Either way, start clean rather than
  // interpreting stale bytes as entries.
  if (got != sizeof(g_ring) || g_head >= EVENT_LOG_CAPACITY ||
      g_count > EVENT_LOG_CAPACITY) {
    memset(g_ring, 0, sizeof(g_ring));
    g_head = 0;
    g_count = 0;
  }
}

void setEpoch(uint32_t epochSecNow) {
  if (epochSecNow == 0) return;
  g_epochAtBootSec = epochSecNow - (millis() / 1000);
}

bool haveEpoch() { return g_epochAtBootSec != 0; }

uint32_t epochNow() {
  return haveEpoch() ? (g_epochAtBootSec + millis() / 1000) : 0;
}

void record(LogEventType type, uint8_t detail, int rssi) {
  LogEntry &e = g_ring[g_head];
  e.epochSec = epochNow();
  e.uptimeSec = millis() / 1000;
  e.bootNum = g_bootNum;
  e.type = static_cast<uint8_t>(type);
  e.detail = detail;
  e.rssi = static_cast<int16_t>(rssi);
  e.reserved = 0;

  g_head = static_cast<uint16_t>((g_head + 1) % EVENT_LOG_CAPACITY);
  if (g_count < EVENT_LOG_CAPACITY) g_count++;

  // Written through immediately: the events worth recording are exactly the
  // ones most likely to be followed by a power cut.
  persist();
}

uint16_t count() { return g_count; }
uint16_t capacity() { return EVENT_LOG_CAPACITY; }

bool get(uint16_t index, LogEntry &out) {
  if (index >= g_count) return false;
  // Oldest first: when the ring has wrapped, the oldest entry sits at head.
  const uint16_t start =
      (g_count == EVENT_LOG_CAPACITY) ? g_head : 0;
  out = g_ring[(start + index) % EVENT_LOG_CAPACITY];
  return true;
}

void clear() {
  memset(g_ring, 0, sizeof(g_ring));
  g_head = 0;
  g_count = 0;
  persist();
}

const char *typeName(uint8_t type) {
  switch (type) {
    case LOG_BOOT: return "BOOT";
    case LOG_OPEN: return "OPEN";
    case LOG_CLOSE: return "CLOSE";
    case LOG_REFUSED: return "REFUSED";
    case LOG_FIX_LOST: return "FIX_LOST";
    case LOG_FIX_GOT: return "FIX_GOT";
    default: return "?";
  }
}

namespace {
// Formats an epoch second as UTC. No timezone handling: the log is for working
// out what happened, and a fixed reference beats a wrong local time.
void formatUtc(uint32_t epoch, char *buf, size_t len) {
  if (epoch == 0) {
    snprintf(buf, len, "       (no clock)   ");
    return;
  }
  const time_t t = static_cast<time_t>(epoch);
  struct tm tmv;
  gmtime_r(&t, &tmv);
  snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02dZ", tmv.tm_year + 1900,
           tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}
}  // namespace

void dump(Stream &out) {
  out.printf("---- event log (%u/%u) ----\r\n", g_count, EVENT_LOG_CAPACITY);
  if (g_count == 0) {
    out.println(F("  (empty)"));
    return;
  }
  out.println(F("  when                  boot  uptime    event     detail"));
  LogEntry e;
  char when[28];
  for (uint16_t i = 0; i < g_count; i++) {
    if (!get(i, e)) break;
    formatUtc(e.epochSec, when, sizeof(when));
    out.printf("  %-21s #%-4u %6lus  %-9s", when, e.bootNum,
               static_cast<unsigned long>(e.uptimeSec), typeName(e.type));
    if (e.type == LOG_BOOT) {
      out.printf(" reset=%u", e.detail);
    } else if (e.type == LOG_REFUSED) {
      out.printf(" reason=%u", e.detail);
    } else if (e.type == LOG_OPEN || e.type == LOG_CLOSE) {
      out.printf(" %s", e.detail ? "manual" : "beacon");
    }
    if (e.rssi != 0) out.printf(" rssi=%d", e.rssi);
    out.println();
  }
  if (!haveEpoch()) {
    out.println(F("  (no wall clock set — times are uptime only)"));
  }
}

void dumpCsv(Stream &out) {
  out.println(F("epoch,uptime_s,boot,event,detail,rssi"));
  LogEntry e;
  for (uint16_t i = 0; i < g_count; i++) {
    if (!get(i, e)) break;
    out.printf("%lu,%lu,%u,%s,%u,%d\r\n", static_cast<unsigned long>(e.epochSec),
               static_cast<unsigned long>(e.uptimeSec), e.bootNum, typeName(e.type),
               e.detail, e.rssi);
  }
}

}  // namespace EventLog
