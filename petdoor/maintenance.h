// maintenance.h — a bounded window in which the door holds still so it can be
// measured.
//
// The problem this solves: proximity thresholds are only meaningful for the
// door WHERE IT IS MOUNTED. The ESP32's antenna is a trace on the PCB and it is
// directional; the same beacon at the same distance reads several dB apart
// depending on which way the board faces and what metal is near it. Numbers
// gathered with the unit on a bench describe the bench. But calibrating in
// place means standing at the door with the collar in your hand, which is
// exactly the condition that makes the door actuate — so measuring it changes
// it, and the relay cycles the whole time you are trying to read a number.
//
// Maintenance mode is the answer: for a bounded period the door stops acting on
// the beacon entirely, while continuing to listen to it and accumulate
// statistics. You can then stand at the door, walk the boundary, and read the
// distribution off a web page, with nothing moving.
//
// THREE PROPERTIES THAT ARE NOT NEGOTIABLE, each of them a safety property:
//
//   1. IT EXPIRES BY ITSELF. This is a deadline, never a flag. A door left
//      inert because somebody forgot, or because the network died before the
//      "off" arrived, is a door that cannot let an animal in overnight. The
//      window ends on its own and the door resumes guarding itself. This is the
//      same reasoning that already times out the OTA window.
//
//   2. IT BLOCKS BOTH DIRECTIONS. The existing lock deliberately gates only
//      opening — a locked door still closes when the collar leaves. That is
//      wrong here: the calibrator is holding the collar AT the door, and a door
//      that closes on them mid-measurement is both useless and unsafe. See
//      driveDoor() in petdoor.ino.
//
//   3. IT DOES NOT SURVIVE A REBOOT. Nothing here is written to NVS. A brownout
//      or a power blip must bring the door back up guarding the animal, not
//      sitting inert with nobody aware of it. Losing a calibration session to a
//      reset is a minor annoyance; losing the door is not.
//
// Manual control is deliberately still live: `o` and `x` on the console, and
// `door open` / `door close` from the server, all work. Maintenance mode stops
// the BEACON commanding the door; it does not take the door away from you.

#pragma once

#include <Arduino.h>

#include "config.h"

namespace Maintenance {

// Percentiles of the RSSI seen since the accumulator was last reset.
//
// Percentiles rather than a mean because the mean is what misled us: a beacon
// sitting a hair off the close threshold has a perfectly reasonable-looking
// average and still never accumulates an unbroken stretch far enough to act on.
// What sets a usable threshold is the TAILS — the strongest reading from away
// and the weakest from at the door — because those are what the door must
// separate. p5/p95 rather than min/max because a single reflected or garbled
// advertisement should not define the band; min and max are reported too, so
// the size of that discard is visible rather than assumed.
struct Stats {
  uint32_t n;
  int16_t min, p5, median, p95, max;
  int16_t meanX10;  // tenths of a dBm, to avoid float in the log path
};

// Windows are bounded at both ends: too short and you cannot walk the boundary,
// too long and the "it expires by itself" guarantee stops being meaningful.
// Returns false and sets `problem` if the duration is outside that range.
bool start(uint32_t nowMs, uint32_t durationMs, const char *&problem);

// Ends the window early. Safe to call when inactive.
void stop();

bool active(uint32_t nowMs);
uint32_t remainingMs(uint32_t nowMs);

// True exactly once, on the tick when the window lapsed on its own, so the
// caller can log it and sound it. An expiry that happened silently would leave
// somebody wondering why the door started moving again.
bool consumeExpired(uint32_t nowMs);

// Feed every RSSI sample. Ignored unless a window is open, so the caller does
// not have to check first.
void addSample(uint32_t nowMs, int rssi);

// Start the distribution over — after moving the collar to a new position.
void resetStats();

// False when nothing has been sampled yet.
bool stats(Stats &out);

// Formats the accumulated distribution for upload or for the console.
String summary(uint32_t nowMs);

}  // namespace Maintenance
