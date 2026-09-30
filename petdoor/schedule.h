// schedule.h — time windows in which the beacon may not open the door.
//
// The use this exists for: the animals are in for the night, and the door
// should stay shut until morning whatever the collar says. Without it the only
// options are remembering to `lock` every evening and `unlock` every morning,
// or leaving the door free to open at 3 a.m. for whatever is wearing the
// collar — which, at 3 a.m., may be a fox that found it.
//
// A window is (start, end, days). It gates OPENING ONLY, exactly like the
// manual lock, and for the same reason: a door that refuses to close is
// dangerous in a way that a door which refuses to open is not. See driveDoor().
//
// FOUR THINGS THAT DECIDE THE DESIGN, all of them about being wrong safely:
//
//   1. NO CLOCK MEANS NO SCHEDULE. Every window is inert until NTP has
//      answered. A door that guesses the time can lock an animal out at noon
//      because it believes it is midnight, and it would do so silently. Until
//      the clock is real this module reports "not locked" and says why.
//
//   2. IT CANNOT KEEP A DOOR SHUT AGAINST AN ANIMAL THAT IS ALREADY OUTSIDE.
//      That is not a bug to be fixed — it is the whole point of a night
//      lockout, and it is also the thing that will shut a straggler out in the
//      cold. Anyone turning this on is making that trade deliberately, which
//      is why the console makes you read it.
//
//   3. THE MANUAL LOCK STILL WINS. `lock` is absolute and unconditional; a
//      schedule is an additional reason to refuse, never a reason to permit.
//      Nothing here can unlock a door somebody locked by hand.
//
//   4. LOCAL TIME IS AN OFFSET, NOT A TIMEZONE. The door keeps UTC. A fixed
//      offset in minutes turns that into wall time. There is no DST handling
//      and there deliberately isn't: a zoneinfo database on a microcontroller
//      is a large amount of machinery whose failure mode is a door that locks
//      an hour early twice a year. Shift the offset yourself, or set windows
//      with an hour of slack at each end.

#pragma once

#include <Arduino.h>

#include "config.h"

namespace Schedule {

struct Window {
  uint16_t startMin;  // minutes since local midnight, 0..1439
  uint16_t endMin;    // exclusive. LESS than startMin means it crosses midnight
  uint8_t days;       // bit 0 = Sunday ... bit 6 = Saturday. The day the window
                      // STARTS on, so a 22:00->06:00 Monday window runs into
                      // Tuesday morning.
  uint8_t enabled;
};

// Loads the stored windows and offset. Safe before anything else.
void begin();

uint8_t count();
uint8_t capacity();
bool get(uint8_t index, Window &out);

// `problem` is set to a human-readable reason when this returns false.
bool add(uint16_t startMin, uint16_t endMin, uint8_t days, const char *&problem);
bool removeAt(uint8_t index);
void clear();

// Minutes east of UTC (e.g. -420 for US Pacific daylight time).
void setUtcOffsetMinutes(int16_t minutes);
int16_t utcOffsetMinutes();

// THE decision. `haveClock` false always yields false — see note 1 above.
// When it returns true, *which receives the index of the window responsible,
// so the console and the log can say which one rather than just "scheduled".
bool lockedNow(bool haveClock, uint32_t epochSec, uint8_t *which = nullptr);

// Seconds until the active window ends, or until the next one starts. 0 when
// nothing is scheduled or the clock is unknown. Display only — the decision is
// always lockedNow().
uint32_t secondsUntilChange(bool haveClock, uint32_t epochSec);

void describe(Stream &out, bool haveClock, uint32_t epochSec);

// "22:00-06:00 Mon-Fri" into a window. Returns false with `problem` set.
bool parseSpec(const char *spec, Window &out, const char *&problem);

}  // namespace Schedule
