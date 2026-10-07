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
  eq(judgeIdleNoise(500, 200, false, 0, false, false), SV_RAISE,
     "edges over the threshold with no stand-down raises");

  // Exactly at the threshold counts — the comparison is >=, and an off-by-one
  // here would make the knob mean something other than what the docs say.
  eq(judgeIdleNoise(200, 200, false, 0, false, false), SV_RAISE,
     "edges exactly at the threshold raises");
  eq(judgeIdleNoise(199, 200, false, 0, false, false), SV_NOTHING,
     "one edge below the threshold does not raise");

  // Ambient. The reference door reads 0 at rest.
  eq(judgeIdleNoise(0, 200, false, 0, false, false), SV_NOTHING,
     "a silent window raises nothing");
}

static void test_a_moving_door_is_never_called_a_broken_sensor() {
  // A switch confirmed the door changed ends. Those edges were a DOOR. Blaming
  // the sensor would blame the one part that reported the truth. This is the
  // false positive that fired on the reference door at 15:50 with 9,655 edges.
  eq(judgeIdleNoise(9655, 200, true, 0, false, false), SV_NOTHING,
     "a confirmed change of ends excuses any number of edges");

  // No switch saw it — an unplugged reed, or a build with none — but the
  // movement arrived in discrete runs, which is a door being handled rather
  // than a sensor chattering continuously.
  eq(judgeIdleNoise(9655, 200, false, 1, false, false), SV_NOTHING,
     "one discrete run of movement excuses the edges with no switch involved");
  eq(judgeIdleNoise(40000, 200, false, 3, false, false), SV_NOTHING,
     "several runs likewise");
}

static void test_a_run_that_never_ends_is_chatter_not_a_door() {
  // The loophole that must stay shut: if a stuck sensor's endless "run" bought
  // a stand-down, a permanently-stuck sensor would excuse itself forever and
  // the fault could never be raised at all.
  eq(judgeIdleNoise(40000, 200, false, 1, true, false), SV_RAISE,
     "a run past VIBRATION_RUN_MAX_MS does NOT excuse the edges");
  // Belt and braces: a runaway run alongside ordinary ones still raises.
  eq(judgeIdleNoise(40000, 200, false, 4, true, false), SV_RAISE,
     "a runaway run is not laundered by other runs in the same window");
}

static void test_the_noise_fault_can_finally_clear() {
  // THE FIX. A full window at rest, below the threshold the fault was raised
  // on, with the fault latched: let it go. Before this, SF_VIBRATION_NOISY was
  // the only sensor fault with no way back and latched until a power cycle.
  eq(judgeIdleNoise(0, 200, false, 0, false, true), SV_CLEAR,
     "a quiet window clears a latched noise fault");
  eq(judgeIdleNoise(199, 200, false, 0, false, true), SV_CLEAR,
     "below-threshold, not merely zero, is enough to clear");

  // Still noisy: stay raised. Must not thrash between raise and clear.
  eq(judgeIdleNoise(500, 200, false, 0, false, true), SV_RAISE,
     "a still-noisy window keeps the fault, it does not clear it");

  // Nothing latched, nothing to clear. A CLEAR here would be a spurious
  // "recovered" line on the console and an extra radio burst every window.
  eq(judgeIdleNoise(0, 200, false, 0, false, false), SV_NOTHING,
     "a quiet window with no fault latched does nothing");

  // A door that moved says nothing about the sensor either way, so a latched
  // fault must keep waiting for a genuinely quiet window rather than being
  // cleared by movement.
  eq(judgeIdleNoise(9655, 200, true, 0, false, true), SV_NOTHING,
     "movement does not clear a latched fault; only a quiet window does");
  eq(judgeIdleNoise(9655, 200, false, 2, false, true), SV_NOTHING,
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
  test_the_noise_fault_can_finally_clear();
  test_a_switch_that_stops_making_is_noticed();
  test_a_single_switch_build_is_never_nagged();
  test_the_switch_coming_back_clears_it();
  std::printf("%s  %d checks, %d failed\n", g_fails ? "FAILED" : "ok    ",
              g_checks, g_fails);
  (void)check;
  return g_fails ? 1 : 0;
}
