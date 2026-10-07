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
  // Travel time elapsed and NO limit switch was reached: the door did not get
  // where it was sent. Only ever recorded with sensors fitted, because without
  // them there is nothing to notice it with. detail = the DoorState we had
  // commanded, so the log says which direction failed.
  LOG_STALLED = 6,
  // detail: 1 = window opened, 0 = it expired on its own, 2 = ended by hand.
  // Worth a log line because for its duration the door deliberately ignores
  // the beacon, and somebody reading back "why did nothing happen at 4pm"
  // needs to see that stated rather than infer it from an absence.
  LOG_MAINT = 7,
  // The network console. detail: 0 = WRONG PASSWORD, 1 = attached,
  // 2 = refused (a session was already open), 3 = gave no password in time.
  // `reserved` carries the last octet of the client's address — not an audit
  // trail, but enough to tell "me, from my laptop" from "something else on the
  // network". Recorded because this port can open the door, and a console that
  // only reports attempts down a serial cable reports them to nobody: the
  // whole point of it is that nobody is holding a cable.
  LOG_CONSOLE = 8,
  // The relay fired and the vibration sensor heard nothing for the whole
  // travel: the door never started. Distinct from STALLED, which means it
  // started and did not arrive — different causes, different fixes. detail =
  // the state the door believed it was moving to.
  LOG_NO_MOVEMENT = 9,
  // A limit switch reported the door at an end that nothing commanded it to go
  // to, with no travel in flight. A hand, the wind, or the door's own vendor
  // controller acting on a mode of its own — all three happen, and none of
  // them was visible to this firmware before the switches were fitted. The
  // controller has been observed leaving the open limit by itself ~15 s after
  // arriving, twice, which is the kind of thing you can only chase if the door
  // writes it down. detail = the DoorState observed, so the log says which way
  // it went.
  LOG_UNCOMMANDED = 10,
  // A press was repeated because the previous one demonstrably moved nothing.
  // detail = which attempt this was (1 = the first retry). Safe to retry
  // immediately — nothing moved, so nothing is trapped — but worth recording,
  // because a door that needs a retry every time is a door whose pulse width
  // or wake threshold is wrong.
  LOG_RETRY = 11,
  // Close attempts are exhausted and the door is staying OPEN deliberately.
  // detail = how many attempts were made. The single most important entry in
  // this log: it means the door has stopped trying to protect the coop and is
  // waiting for a person. Everything else here is history; this is a request.
  LOG_GAVE_UP = 12,
  // A wake press was sent before an actuation, because the vendor controller
  // had been idle long enough to be asleep. detail = 1 if the wake press alone
  // moved the door (so no second press was sent), 0 if the actuating press
  // followed it. Recorded because the ratio between those two is the only
  // evidence available for whether WAKE_IDLE_MS is set anywhere near right.
  LOG_WAKE = 13,
  // The beacon's own battery crossed BEACON_LOW_BATTERY_MV, as reported in its
  // Eddystone-TLM frames. detail = 1 when it went low, 0 when it recovered;
  // `reserved` carries the millivolts.
  //
  // Worth its own event because of what a flat beacon means here: the door
  // refuses to act until it has heard the collar once since boot (invariant 5),
  // so a battery that dies overnight does not shut the door — it stops the door
  // working at all, silently, and the animal is outside. This is the warning
  // that gives you days rather than a surprise.
  //
  // Latched with a recovery margin, so a cell sitting on the threshold produces
  // one entry and not a stream of them.
  LOG_BEACON_LOW = 14,
  // A SENSOR disagreed with the other one badly enough to be called broken.
  // detail = SensorFault below; `reserved` carries the evidence (a vibration
  // count, or 0 where the fault is binary).
  //
  // Distinct from STALLED and NO_MOVE, which say the DOOR did not do what it
  // was told. This says the thing watching the door is lying, which is worse:
  // a stall is visible, and a dead sensor is not.
  LOG_SENSOR_FAULT = 15,
};

// Which sensor, and how it failed. Each is diagnosed by cross-checking against
// the other sensor, so these name a part rather than a symptom.
enum SensorFault : uint8_t {
  // Both limit switches report the door at their end at once. Physically
  // impossible: a shorted wire, a stuck switch, or a stray magnet.
  SF_REEDS_CONTRADICT = 1,
  // Travels the reeds VERIFIED produced no vibration. The door demonstrably
  // moved, so the sensor is deaf, miswired, or has fallen off the door.
  SF_VIBRATION_SILENT = 2,
  // Vibration accumulating while the door stands still. Either the
  // sensitivity screw is too far in, or it is mounted somewhere that feels the
  // world rather than the door.
  SF_VIBRATION_NOISY = 3,
  // The door ran a full travel's worth of vibration and never arrived. The
  // switch at that end is not making — a lost magnet or a broken wire — as
  // opposed to an obstruction, which stops the vibration too.
  SF_REED_MISSED = 4,
  // The sensor produced NOT ONE EDGE across a whole actuation — relay pulses
  // included. Unplugged, or its signal wire is broken.
  //
  // The only sensor fault diagnosable WITHOUT the other sensor, which is what
  // makes it worth its own code: every other entry here needs something working
  // to be checked against. A connected-but-deaf sensor still registers the
  // relay's own click conducted through the structure — around a hundred edges
  // a pulse on the reference door — so a flat zero is a missing signal path,
  // not a stationary door.
  //
  // Found by unplugging one: it had been completely invisible, reading
  // identically to a door that did not move.
  SF_VIBRATION_DEAD = 5,
  // The door is believed to be sitting at an end, nothing is moving, and the
  // switch FITTED at that end is not made.
  //
  // The quiet failure every other check here misses. SF_REEDS_CONTRADICT needs
  // both switches made at once; SF_REED_MISSED needs a commanded travel to run
  // its full duration and not arrive. A connector shaken loose, or a magnet
  // drifted a few millimetres, produces neither — the door just sits there with
  // a switch that says nothing, and nothing complains until the next actuation.
  SF_REED_LOST = 6,
};

// Flags packed into a LOG_OPEN / LOG_CLOSE entry's `reserved` field. The entry
// already spends `detail` on the ActuationSource, and these say how much the
// door actually knows about what it just did — which is the difference between
// a log you can trust and a log of intentions.
enum LogActuationFlags : uint16_t {
  // A limit switch confirmed arrival. Without this bit the entry records that
  // the door was COMMANDED there, nothing more.
  LOGF_VERIFIED = 0x0001,
  // A wake press preceded it.
  LOGF_WOKE = 0x0002,
  // At least one press had to be repeated.
  LOGF_RETRIED = 0x0004,
  // This travel was the firmware reversing a close of its own that stalled.
  LOGF_FAILSAFE = 0x0008,
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

// As above, but also sets the entry's spare field. Used by the console to
// carry which address connected.
void record(LogEventType type, uint8_t detail, int rssi, int16_t reserved);

// Wall clock. Until this is called, entries carry epochSec = 0.
void setEpoch(uint32_t epochSecNow);
bool haveEpoch();
uint32_t epochNow();

uint16_t count();
uint16_t capacity();

// The upload wire format, in ONE place. It used to be written out twice —
// once by dumpCsv() for the console and once by the uploader — and the two
// drifted the moment a column was added: the door logged the new field, showed
// it locally, and quietly kept uploading the old six. Anything that serialises
// an entry must go through these.
const char *csvHeader();
size_t formatCsvRow(const LogEntry &e, char *out, size_t n);

// Oldest-first iteration. Returns false once `index` runs past the end.
bool get(uint16_t index, LogEntry &out);

void clear();

// Human-readable dump, and a CSV form meant for batch upload.
void dump(Stream &out);
void dumpCsv(Stream &out);

const char *typeName(uint8_t type);

}  // namespace EventLog
