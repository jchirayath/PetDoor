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
                                    bool runRanAway, bool faultLatched) {
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
