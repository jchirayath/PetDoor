// Host tests for the two sensor-health decisions in petdoor/sensor_verdict.h.
//
// WHY THESE EXIST
//
// Both decisions fail silently, in both directions, and neither failure shows up
// in a compile or a crash:
//
//   raised wrongly  — the annunciator sounds every SENSOR_FAULT_BEEP_MS for a
//                     sensor that works. An alarm nobody can stand gets
//                     unplugged, and that loses every future message with it.
//   never raised    — the door keeps deciding on evidence that stopped
//                     arriving, and nothing says so until the next actuation.
//
// The clear path for SF_VIBRATION_NOISY is the specific thing that drove this
// file. It was added after the fault was found to be the only one with no way
// back, and it could not be demonstrated on hardware without first deliberately
// latching a fault — which the same change had just made harder to do. A live
// door is a bad instrument for a decision table; this is a good one.

#include <cstdio>
#include <cstdint>

#include "../../petdoor/sensor_verdict.h"

static int g_fails = 0;
static int g_checks = 0;

static void check(bool cond, const char *what) {
  ++g_checks;
  if (!cond) {
    ++g_fails;
    std::printf("  FAIL  %s\n", what);
  }
}

static const char *name(SensorVerdict v) {
  return v == SV_RAISE ? "RAISE" : v == SV_CLEAR ? "CLEAR" : "NOTHING";
}

static void eq(SensorVerdict got, SensorVerdict want, const char *what) {
  ++g_checks;
  if (got != want) {
    ++g_fails;
    std::printf("  FAIL  %s — wanted %s, got %s\n", what, name(want), name(got));
  }
}

// ---------------------------------------------------------------------------
// judgeIdleNoise
// ---------------------------------------------------------------------------
//
// Signature: edges, threshold, endChanged, runs, runRanAway, faultLatched
static void test_idle_noise_raises_only_on_real_chatter() {
  // Plenty of edges, nothing to excuse them: that is a chattering sensor.
  eq(judgeIdleNoise(500, 200, false, 0, false, false, false), SV_RAISE,
     "edges over the threshold with no stand-down raises");

  // Exactly at the threshold counts — the comparison is >=, and an off-by-one
  // here would make the knob mean something other than what the docs say.
  eq(judgeIdleNoise(200, 200, false, 0, false, false, false), SV_RAISE,
     "edges exactly at the threshold raises");
  eq(judgeIdleNoise(199, 200, false, 0, false, false, false), SV_NOTHING,
     "one edge below the threshold does not raise");

  // Ambient. The reference door reads 0 at rest.
  eq(judgeIdleNoise(0, 200, false, 0, false, false, false), SV_NOTHING,
     "a silent window raises nothing");
}

static void test_a_moving_door_is_never_called_a_broken_sensor() {
  // A switch confirmed the door changed ends. Those edges were a DOOR. Blaming
  // the sensor would blame the one part that reported the truth. This is the
  // false positive that fired on the reference door at 15:50 with 9,655 edges.
  eq(judgeIdleNoise(9655, 200, true, 0, false, false, false), SV_NOTHING,
     "a confirmed change of ends excuses any number of edges");

  // No switch saw it — an unplugged reed, or a build with none — but the
  // movement arrived in discrete runs, which is a door being handled rather
  // than a sensor chattering continuously.
  eq(judgeIdleNoise(9655, 200, false, 1, false, false, false), SV_NOTHING,
     "one discrete run of movement excuses the edges with no switch involved");
  eq(judgeIdleNoise(40000, 200, false, 3, false, false, false), SV_NOTHING,
     "several runs likewise");
}

static void test_a_run_that_never_ends_is_chatter_not_a_door() {
  // The loophole that must stay shut: if a stuck sensor's endless "run" bought
  // a stand-down, a permanently-stuck sensor would excuse itself forever and
  // the fault could never be raised at all.
  eq(judgeIdleNoise(40000, 200, false, 1, true, false, false), SV_RAISE,
     "a run past VIBRATION_RUN_MAX_MS does NOT excuse the edges");
  // Belt and braces: a runaway run alongside ordinary ones still raises.
  eq(judgeIdleNoise(40000, 200, false, 4, true, false, false), SV_RAISE,
     "a runaway run is not laundered by other runs in the same window");
}

static void test_a_hand_move_in_flight_is_not_a_noisy_sensor() {
  // THE REGRESSION THIS EXISTS FOR. Measured on the reference door: a door
  // being moved by hand produced 12,642 edges in one idle window and raised
  // SF_VIBRATION_NOISY one second before a limit switch confirmed the door had
  // moved. Mid-move the door has left one end and not reached the other, so
  // `endChanged` is still false, and the run has not concluded, so `runs` is
  // still 0 — every stand-down the old signature had was blind to it.
  eq(judgeIdleNoise(12642, 200, false, 0, false, true, false), SV_NOTHING,
     "a hand-move still in flight is not a noisy sensor");

  // The same window with the run finished and the far end reached is already
  // covered by endChanged, but pin it so the two cannot drift apart.
  eq(judgeIdleNoise(12642, 200, true, 1, false, false, false), SV_NOTHING,
     "the same move, once concluded, still stands down");

  // AND THE OTHER DIRECTION, which is the whole reason this is a table and not
  // an if. A sensor stuck firing also holds a run "active"; if that bought a
  // stand-down the fault would become unraisable and a door would go on
  // deciding on evidence that stopped meaning anything. The ran-away flag is
  // what separates the two, so an in-flight run that has ALREADY blown past
  // VIBRATION_RUN_MAX_MS must still raise.
  eq(judgeIdleNoise(40000, 200, false, 0, true, true, false), SV_RAISE,
     "an in-flight run that has run away is still chatter");

  // A latched fault must not be cleared by a hand-move either: the window was
  // not quiet, it was busy, which says nothing about the sensor.
  eq(judgeIdleNoise(0, 200, false, 0, false, true, true), SV_NOTHING,
     "a hand-move does not clear a latched fault");
}

// ---------------------------------------------------------------------------
// judgeReedMissed — strikes are per END
// ---------------------------------------------------------------------------

static void test_a_failing_close_reed_can_actually_latch() {
  // THE SEQUENCE THAT COULD NEVER TRIP. Invariant 14 reverses a stalled close,
  // and that reversal ARRIVES on the open switch. With one counter for the
  // whole door, reset on any arrival, the count went 1 -> 0 -> 1 -> 0 and
  // SF_REED_MISSED was unreachable for a failing CLOSED reed whenever the OPEN
  // reed worked — which, because a stalled close always fails open, is the
  // normal case. Measured on the reference door 2026-10-10: three stalled
  // closes, the door gave up and stayed open, and nothing ever named the
  // closed switch.
  const uint8_t kOpen = 0, kClosed = 1;
  ReedMissedState st = {{0, 0}};

  // Close stalls with the door plainly moving: one strike against CLOSED.
  eq(judgeReedMissed(st, false, true, kClosed, 9000, 200, 2), SV_NOTHING,
     "one stalled close is not yet a fault");
  // The fail-open reversal arrives on the OPEN switch. That clears OPEN only.
  eq(judgeReedMissed(st, true, false, kOpen, 9000, 200, 2), SV_CLEAR,
     "the reversal arriving clears the OPEN end");
  check(st.strikes[kClosed] == 1,
        "and must NOT wipe what is known about the CLOSED end");
  // Second stalled close: now it trips.
  eq(judgeReedMissed(st, false, true, kClosed, 9000, 200, 2), SV_RAISE,
     "the second stalled close raises the fault it could never reach before");
}

static void test_an_obstruction_is_not_a_sensor_fault() {
  // A stall with little movement means the door STOPPED. STALLED already
  // reports that, and faulting the sensor would blame the wrong part — the
  // door is the thing that did not move.
  ReedMissedState st = {{0, 0}};
  eq(judgeReedMissed(st, false, true, 1, 10, 200, 2), SV_NOTHING,
     "a stall with no movement is an obstruction, not a reed");
  eq(judgeReedMissed(st, false, true, 1, 199, 200, 2), SV_NOTHING,
     "just below the moving threshold still is not");
  check(st.strikes[1] == 0, "and banks no strike against the switch");
}

static void test_arriving_clears_only_the_end_that_arrived() {
  ReedMissedState st = {{0, 0}};
  // Bank a strike at each end.
  eq(judgeReedMissed(st, false, true, 0, 9000, 200, 3), SV_NOTHING, "open strike 1");
  eq(judgeReedMissed(st, false, true, 1, 9000, 200, 3), SV_NOTHING, "closed strike 1");
  eq(judgeReedMissed(st, true, false, 0, 9000, 200, 3), SV_CLEAR, "open arrives");
  check(st.strikes[0] == 0, "the open end is cleared");
  check(st.strikes[1] == 1, "the closed end is untouched — the bug, in one check");
  // An out-of-range end index must do nothing rather than scribble.
  eq(judgeReedMissed(st, false, true, 7, 9000, 200, 3), SV_NOTHING,
     "an impossible end index is ignored");
  check(st.strikes[0] == 0 && st.strikes[1] == 1, "and changes nothing");
}

static void test_the_noise_fault_can_finally_clear() {
  // THE FIX. A full window at rest, below the threshold the fault was raised
  // on, with the fault latched: let it go. Before this, SF_VIBRATION_NOISY was
  // the only sensor fault with no way back and latched until a power cycle.
  eq(judgeIdleNoise(0, 200, false, 0, false, false, true), SV_CLEAR,
     "a quiet window clears a latched noise fault");
  eq(judgeIdleNoise(199, 200, false, 0, false, false, true), SV_CLEAR,
     "below-threshold, not merely zero, is enough to clear");

  // Still noisy: stay raised. Must not thrash between raise and clear.
  eq(judgeIdleNoise(500, 200, false, 0, false, false, true), SV_RAISE,
     "a still-noisy window keeps the fault, it does not clear it");

  // Nothing latched, nothing to clear. A CLEAR here would be a spurious
  // "recovered" line on the console and an extra radio burst every window.
  eq(judgeIdleNoise(0, 200, false, 0, false, false, false), SV_NOTHING,
     "a quiet window with no fault latched does nothing");

  // A door that moved says nothing about the sensor either way, so a latched
  // fault must keep waiting for a genuinely quiet window rather than being
  // cleared by movement.
  eq(judgeIdleNoise(9655, 200, true, 0, false, false, true), SV_NOTHING,
     "movement does not clear a latched fault; only a quiet window does");
  eq(judgeIdleNoise(9655, 200, false, 2, false, false, true), SV_NOTHING,
     "discrete runs do not clear a latched fault either");
}

// ---------------------------------------------------------------------------
// judgeReedAtRest
// ---------------------------------------------------------------------------
//
// Signature: fitted, made, disagreeForMs, limitMs, faultLatched
static void test_a_switch_that_stops_making_is_noticed() {
  const uint32_t LIMIT = 120000;

  // The gap this was written for: door believed at an end, at rest, switch
  // fitted there, not made, and it has been that way past the limit.
  eq(judgeReedAtRest(true, false, LIMIT, LIMIT, false), SV_RAISE,
     "a fitted switch not made for the full limit raises");
  eq(judgeReedAtRest(true, false, LIMIT * 10, LIMIT, false), SV_RAISE,
     "and stays raisable well past it");

  // Not yet. A reed opens for a moment as the door settles onto its stop, and
  // faulting on that would cry wolf on every single travel.
  eq(judgeReedAtRest(true, false, LIMIT - 1, LIMIT, false), SV_NOTHING,
     "one millisecond short of the limit does not raise");
  eq(judgeReedAtRest(true, false, 0, LIMIT, false), SV_NOTHING,
     "a switch that just opened does not raise");
}

static void test_a_single_switch_build_is_never_nagged() {
  const uint32_t LIMIT = 120000;
  // Position::fittedAt() is false for an end with no switch wired. A build with
  // only one reed is legal, and faulting about the end it cannot see would
  // punish a supported wiring — forever, since that end will never report.
  eq(judgeReedAtRest(false, false, LIMIT * 100, LIMIT, false), SV_NOTHING,
     "an end with NO switch fitted never raises, however long it says nothing");
  // And it must not be able to leave a fault stuck either.
  eq(judgeReedAtRest(false, false, LIMIT * 100, LIMIT, true), SV_NOTHING,
     "an unfitted end neither raises nor clears");
}

static void test_the_switch_coming_back_clears_it() {
  const uint32_t LIMIT = 120000;
  eq(judgeReedAtRest(true, true, 0, LIMIT, true), SV_CLEAR,
     "the switch making again clears a latched fault");
  eq(judgeReedAtRest(true, true, 0, LIMIT, false), SV_NOTHING,
     "a healthy switch with no fault latched does nothing");
  // Still broken: stay raised rather than thrashing.
  eq(judgeReedAtRest(true, false, LIMIT, LIMIT, true), SV_RAISE,
     "a still-broken switch keeps the fault");
}

int main() {
  std::printf("sensor_verdict.h — host unit tests\n");
  test_idle_noise_raises_only_on_real_chatter();
  test_a_moving_door_is_never_called_a_broken_sensor();
  test_a_run_that_never_ends_is_chatter_not_a_door();
  test_a_hand_move_in_flight_is_not_a_noisy_sensor();
  test_a_failing_close_reed_can_actually_latch();
  test_an_obstruction_is_not_a_sensor_fault();
  test_arriving_clears_only_the_end_that_arrived();
  test_the_noise_fault_can_finally_clear();
  test_a_switch_that_stops_making_is_noticed();
  test_a_single_switch_build_is_never_nagged();
  test_the_switch_coming_back_clears_it();
  std::printf("%s  %d checks, %d failed\n", g_fails ? "FAILED" : "ok    ",
              g_checks, g_fails);
  (void)check;
  return g_fails ? 1 : 0;
}
