// sensor_verdict.h — the sensor-health decisions, as pure functions.
//
// These were inline in the control task, and both of them fail SILENTLY. A
// sensor fault raised in error costs a buzzer sounding every quarter of an hour
// for a sensor that works; one never raised costs a door that keeps deciding on
// evidence that stopped arriving. Neither shows up as a crash, and neither is
// visible in a compile.
//
// So they live here instead: no Arduino, no config.h, no globals, nothing to
// mock. Every threshold arrives as an argument, which is what lets
// tests/host/test_sensor_verdict.cpp walk the whole decision table in a
// millisecond rather than waiting for a real door to misbehave. Same reason
// beacon.* is written as pure functions.
//
// Keep them pure. The moment one of these reads a global or a clock, the table
// stops being enumerable and the tests stop being worth anything.

#pragma once

#include <stdint.h>

enum SensorVerdict : uint8_t {
  SV_NOTHING = 0,  // leave the fault exactly as it is
  SV_RAISE,        // raise it
  SV_CLEAR,        // drop it
};

// ---------------------------------------------------------------------------
// Vibration accumulating while the door is supposed to be standing still
// ---------------------------------------------------------------------------
//
// `edges` is the count since the window opened and `threshold` the count that
// means "this is not ambient". The other three are what distinguish a BROKEN
// SENSOR from a MOVING DOOR, which is the whole difficulty: both produce
// thousands of edges with no travel in flight.
//
//   endChanged  — a switch confirmed the door is at a different end than when
//                 the window opened. It moved. Blaming the sensor would blame
//                 the one part that reported the truth.
//   runs        — discrete bursts of movement, each starting and stopping. A
//                 door being handled does this; a sensor with its sensitivity
//                 screw wound in chatters more or less continuously.
//   runRanAway  — one of those "runs" went on past any plausible travel. That
//                 one IS chatter, and it must not buy a stand-down, or a
//                 permanently-stuck sensor would excuse itself forever.
inline SensorVerdict judgeIdleNoise(uint32_t edges, uint32_t threshold,
                                    bool endChanged, uint8_t runs,
                                    bool runRanAway, bool runInProgress,
                                    bool faultLatched) {
  // A TRAVEL STILL IN FLIGHT, and the one case this decision used to get
  // backwards. The COMMANDED version never reaches here — `Actuator::busy()`
  // invalidates the window before it can open — but a door moved BY HAND has
  // no actuation to notice, and nothing stood down for it.
  //
  // The window could therefore expire in the middle of a hand-move: the door
  // has left one end and not yet reached the other, so `endChanged` is still
  // false and the run has not concluded, so `runs` is still 0 — while the
  // sensor is correctly reporting thousands of real edges. Measured on the
  // reference door: 12,642 edges raised SF_VIBRATION_NOISY one second before a
  // limit switch confirmed the door had in fact moved. The door had the
  // evidence and blamed the sensor for telling the truth.
  //
  // `!runRanAway` matters as much as the flag: a sensor stuck firing keeps a
  // run "active" too, and standing down for that would make the fault
  // unraisable — the exact silent failure in the other direction that this
  // decision table exists to keep walkable. A run past VIBRATION_RUN_MAX_MS is
  // abandoned and marked ran-away precisely so it still counts as noise.
  if (runInProgress && !runRanAway) return SV_NOTHING;

  // The door demonstrably moved. Says nothing about the sensor either way, so
  // neither raise nor clear — a latched fault keeps waiting for a quiet window.
  if (endChanged) return SV_NOTHING;
  if (runs > 0 && !runRanAway) return SV_NOTHING;

  if (edges >= threshold) return SV_RAISE;

  // A WHOLE WINDOW AT REST, BELOW THE THRESHOLD THE FAULT WAS RAISED ON.
  //
  // This is the clear that did not exist. SF_VIBRATION_NOISY was the only
  // sensor fault with no way back, and it is the one most likely to be raised
  // in error — so it latched until a power cycle, sounding the annunciator every
  // SENSOR_FAULT_BEEP_MS for a working sensor. An alarm nobody can stand gets
  // unplugged, which loses every future message with it.
  return faultLatched ? SV_CLEAR : SV_NOTHING;
}

// ---------------------------------------------------------------------------
// A switch that ought to be made, and is not
// ---------------------------------------------------------------------------
//
// The quiet failure the other checks all miss. SF_REEDS_CONTRADICT needs both
// switches made at once; SF_REED_MISSED needs a COMMANDED travel to run its
// full duration and not arrive. Neither fires for a door that is simply sitting
// at an end, at rest, with the switch at that end reporting nothing — which is
// what a connector shaken loose or a magnet drifted a few millimetres looks
// like. Until the next actuation, nothing complains, and the door goes on
// deciding as though the switch were still there.
//
// `fitted` matters because a single-switch build is legal: with no switch at
// that end there is nothing to expect, and faulting would punish a supported
// wiring. `disagreeForMs` is how long it has been like this — a reed can open
// for a moment as the door settles onto its stop, so this is a sustained
// condition, never an instantaneous one.
inline SensorVerdict judgeReedAtRest(bool fitted, bool made,
                                     uint32_t disagreeForMs, uint32_t limitMs,
                                     bool faultLatched) {
  if (!fitted) return SV_NOTHING;
  if (made) return faultLatched ? SV_CLEAR : SV_NOTHING;
  return disagreeForMs >= limitMs ? SV_RAISE : SV_NOTHING;
}

// ---------------------------------------------------------------------------
// A travel that RAN and never arrived — per END, which is the whole point
// ---------------------------------------------------------------------------
//
// A stall with plenty of movement means the door ran its travel and the switch
// at the far end never saw it: a lost magnet, a drifted gap, a broken wire. A
// stall with little movement means the door stopped — an obstruction — which
// `STALLED` already reports and which is not a sensor fault.
//
// THE STRIKES ARE PER END, and that is the bug this replaces. The counter used
// to be one number for the whole door, reset on ANY arrival with the reasoning
// "it arrived, so the reeds are fine too". But invariant 14 says a stalled
// CLOSE is reversed, and that reversal arrives on the OPEN switch — which says
// nothing whatsoever about the CLOSED one. So the sequence ran
//
//     close stalls -> 1,  reversal arrives -> 0,  close stalls -> 1,  ...
//
// and the count could never reach the threshold. `SF_REED_MISSED` was
// unreachable for a failing CLOSED reed whenever the OPEN reed worked, which —
// because a stalled close always fails open — is the normal case. Measured on
// the reference door 2026-10-10: three stalled closes, the door gave up and
// stayed open exactly as designed, and nothing ever named the closed switch.
// The message for it had been written and could not fire.
//
// `endIndex` is 0 or 1 — this header knows nothing of DoorState, by design.
struct ReedMissedState {
  uint8_t strikes[2];
};

inline SensorVerdict judgeReedMissed(ReedMissedState &st, bool arrived,
                                     bool stalled, uint8_t endIndex,
                                     uint32_t feltEdges,
                                     uint32_t movingThreshold, uint8_t limit) {
  if (endIndex > 1) return SV_NOTHING;

  if (arrived) {
    // Only THIS end is now known good. Clearing the other end's strikes is
    // what made the fault unreachable.
    st.strikes[endIndex] = 0;
    return SV_CLEAR;
  }
  if (!stalled) return SV_NOTHING;

  // The door stopped rather than ran: an obstruction, not a sensor. STALLED
  // already says so, and a sensor fault here would blame the wrong part.
  if (feltEdges < movingThreshold) return SV_NOTHING;

  if (st.strikes[endIndex] < 255) st.strikes[endIndex]++;
  return st.strikes[endIndex] >= limit ? SV_RAISE : SV_NOTHING;
}
