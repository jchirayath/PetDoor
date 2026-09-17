// eventlog.h — a small persistent record of what the door actually did.
//
// Serial output is lost the moment nothing is attached, which is most of the
// time. This keeps the last N events in NVS so they survive a power cut and can
// be read back later — over the console today, and uploaded over WiFi once that
// exists.
//
// Only rare events are recorded: door actuations, refusals, boots. A busy day
// is a handful of entries, so flash wear is not a concern (NVS wear-levels, and
// this is single-digit writes per day against a 100k-cycle budget).
//
// Timestamps are recorded twice: uptime, which is always available, and wall
// clock, which is 0 until something tells the firmware what time it is. Reading
// the log is therefore useful even on a device that has never known the date.

#pragma once

#include <Arduino.h>

#include "config.h"

enum LogEventType : uint8_t {
  LOG_BOOT = 0,      // detail = esp_reset_reason()
  LOG_OPEN = 1,      // door commanded open
  LOG_CLOSE = 2,     // door commanded closed
  LOG_REFUSED = 3,   // detail = ActuationResult
  LOG_FIX_LOST = 4,  // beacon went stale — what usually precedes a close
  LOG_FIX_GOT = 5,
};

struct LogEntry {
  uint32_t epochSec;   // wall clock, 0 if unknown
  uint32_t uptimeSec;  // always valid
  uint16_t bootNum;
  uint8_t type;        // LogEventType
  uint8_t detail;      // type-specific
  int16_t rssi;        // filtered RSSI at the time, 0 if none
  int16_t reserved;
};  // 16 bytes

namespace EventLog {

// Loads the ring from NVS. Safe to call before anything else is recorded.
void begin(uint16_t bootNum);

// Appends one entry. Cheap; writes through to NVS immediately so a power cut
// straight after an actuation still records it.
void record(LogEventType type, uint8_t detail, int rssi);

// Wall clock. Until this is called, entries carry epochSec = 0.
void setEpoch(uint32_t epochSecNow);
bool haveEpoch();
uint32_t epochNow();

uint16_t count();
uint16_t capacity();

// Oldest-first iteration. Returns false once `index` runs past the end.
bool get(uint16_t index, LogEntry &out);

void clear();

// Human-readable dump, and a CSV form meant for batch upload.
void dump(Stream &out);
void dumpCsv(Stream &out);

const char *typeName(uint8_t type);

}  // namespace EventLog
