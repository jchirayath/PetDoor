// actuator.h — one actuation attempt, owned end to end.
//
// This module exists because of a single fact discovered on real hardware: the
// ESP32 does not drive the motor. It closes a relay across a button on a
// commercial door controller, and that controller has modes, timers and a
// sleep of its own. Four measured failure modes follow from it, and no two of
// them can be handled in the same place:
//
//   F1  The controller SLEEPS. The first press after a long idle is always
//       swallowed; the second works. A coop door is idle for hours, so nearly
//       every real actuation is a cold one.
//   F2  The controller MOVES THE DOOR BY ITSELF. It has been watched leaving
//       the open limit ~15 s after arriving, with nothing commanding it.
//   F3  A repeat press MID-TRAVEL reads as STOP, and parks the door halfway.
//       F1 demands repeat presses; F3 says exactly when they are forbidden.
//   F4  "The press failed" and "the door was already there" are identical
//       without position feedback. Both produce no movement.
//
// F1 and F3 together are why this is a state machine rather than a flag. The
// answer to a sleeping controller is a WAKE PRESS — press once to wake it,
// then press again to actuate — and the answer to F3 is that the second press
// must not be sent if the first one already started the door moving. That
// requires looking at the door between two presses, which means an attempt
// has a middle, which means something has to own it.
//
//     IDLE ──request──▶ WAKE? ──▶ PRESS ──▶ AWAIT_START ──▶ AWAIT_ARRIVAL
//                         │                      │                 │
//                         │ it moved             │ nothing moved   │ timeout
//                         └──────────────▶       ▼                 ▼
//                                            retry (R12)        STALLED
//
// WHAT THIS DOES NOT DO
//
// It does not decide WHETHER the door should move. The proximity pipeline, the
// lock, the schedule and the maintenance window all still make that decision,
// upstream, exactly as before; this module is handed a direction and is
// responsible only for getting the door there and for being honest about
// whether it did. The one exception is written down and bounded: a close that
// stalls is reversed, because a door stopped partway while closing is the one
// case this project exists to prevent.
//
// It also does not let a sensor command the motor. A limit switch here can
// only end a wait sooner or turn a success into a stall. The reversal above is
// the firmware undoing a command of its OWN, which is a different thing from a
// switch driving a door — and the distinction is worth keeping, because a stuck
// switch that can command a door is a much worse failure than one that cannot.
//
// OWNERSHIP. Only the control task calls into this, and this is the only caller
// of DoorController::press(). That chain is what makes "the door has one owner"
// a checkable property rather than an aspiration.

#pragma once

#include <Arduino.h>

#include "chime.h"
#include "config.h"
#include "door.h"
#include "eventlog.h"

namespace Actuator {

// Where an attempt currently is. These describe a travel IN FLIGHT and nothing
// else — the retry delay and the gave-up state are policy that outlives any
// one attempt, and are kept separately for exactly that reason.
enum Phase : uint8_t {
  PH_IDLE,
  PH_WAKE_WAIT,      // wake press sent; watching to see whether it moved the door
  PH_AWAIT_START,    // actuating press sent; waiting for motion
  PH_AWAIT_ARRIVAL,  // it is moving (or we cannot tell); waiting for the far end
};

// How an attempt ended. The whole point of the rebuild is that these are five
// different things rather than one "done".
enum Outcome : uint8_t {
  OUT_NONE = 0,
  // A limit switch at the destination reported the door arrived. The only
  // outcome that is a measurement.
  OUT_ARRIVED,
  // The travel time elapsed and nothing could confirm it. This is what every
  // actuation used to be: an assumption, and the honest name for it.
  OUT_ASSUMED,
  // No travel time is configured and no switch is fitted at the destination,
  // so there is nothing to wait for and nothing to announce.
  OUT_UNTIMED,
  // Press after press and the door demonstrably never moved. Nothing is
  // trapped — the door is still where it was — and the belief is left alone.
  OUT_NO_MOVE,
  // It started and never arrived. Something is in the way, or the mechanism
  // jammed. A stalled CLOSE is reversed.
  OUT_STALLED,
};

// Everything the caller needs to announce and record one attempt. Handed over
// once, through consumeResult(), so the sound and the log entry are produced
// in one place by the code that owns the console and the event log.
struct Result {
  Outcome outcome;
  DoorState target;
  ActuationSource source;
  uint16_t flags;       // LogActuationFlags
  uint32_t measuredMs;  // press to arrival, 0 unless the outcome is OUT_ARRIVED
  uint8_t presses;      // how many button presses this attempt cost
  // Every edge the vibration sensor produced during the attempt, INCLUDING the
  // ones the blanking window discards. Deliberately raw: the blanked edges are
  // the relay's own click, which is exactly the signal that proves the sensor
  // is still electrically there. A connected-but-deaf sensor still hears the
  // relay; a disconnected one reports literally nothing, and nothing is the one
  // reading that cannot be explained by a door that simply did not move.
  uint32_t vibrationRaw;
  uint8_t attempt;      // which close attempt it was, 0 for an open
  bool gaveUp;          // this stall exhausted the attempts; staying open
};

// `door` must outlive the module. Seeds the travel times and the wake and
// retry policy from NVS where they have been stored, otherwise from config.h.
void begin(DoorController *door);

// Ask for the door to go somewhere.
//
// `force` means a person or the network asked rather than the collar. It
// overrides "already there" and the actuation lockout — both of which exist to
// stop the BEACON thrashing the motor, and neither of which should stand
// between someone at the console and their own door. It does not override the
// boot grace, and it does not override the interlock.
//
// Returns ACT_DONE when an attempt has begun. A repeat request in the same
// direction while one is in flight is ACT_BUSY (F3); the opposite direction is
// a reversal and always supersedes, because "open while closing" must never be
// the request that gets refused.
ActuationResult request(DoorState target, ActuationSource src, uint32_t nowMs,
                        bool force);

// Advances the attempt. Must be called every control tick: every wait in here
// is a deadline rather than a delay(), which is what keeps a twelve-second
// travel from blocking the door's own decision-making.
void tick(uint32_t nowMs);

// Takes the result of the attempt that just finished, once. Returns false when
// there is nothing new.
bool consumeResult(Result &out);

// A log entry the machine wants written, popped the same way. Kept out of
// consumeResult() because several can occur within one attempt — a wake press
// and two retries — and they are worth recording individually: the ratio of
// wake presses that worked to wake presses that did nothing is the only
// evidence there is for whether WAKE_IDLE_MS is set anywhere near right.
struct Note {
  LogEventType type;
  uint8_t detail;
  // Whether this belongs in the event log, or only on the console.
  //
  // Not everything worth saying is worth storing. A wake press happens before
  // nearly every real actuation, because the door is idle for hours between
  // uses — recording each one would double the log's volume with its least
  // interesting entry, and that it happened at all is already carried by
  // LOGF_WOKE on the actuation's own entry.
  bool record;
};
bool popNote(Note &out);

bool busy();
Phase phase();
const char *phaseName(Phase p);
const char *outcomeName(Outcome o);

// Where the in-flight travel is going, and how long it has been going there.
DoorState target();
uint32_t elapsedMs(uint32_t nowMs);

// How long the current travel is allowed to take before it counts as a stall.
// 0 when nothing is in flight or nothing is being timed.
uint32_t deadlineRemainingMs(uint32_t nowMs);

// ---- travel time, per direction (R17) -------------------------------------
//
// Separate per direction because on a mounted door they are not equal: gravity
// assists one and opposes the other. 0 means "not measured", which disables
// verification and announcement for that direction rather than guessing.
uint32_t travelMs(DoorState dir);
bool setTravelMs(uint32_t openMs, uint32_t closeMs);

// The duration of the last VERIFIED travel in each direction, which is a
// measurement rather than a setting. 0 until one happens. This is what makes
// travel time measurable in service without a stopwatch: fit the switches,
// use the door, read `s`.
uint32_t measuredMs(DoorState dir);

// ---- the wake press (R1) --------------------------------------------------
uint32_t wakeIdleMs();
bool setWakeIdleMs(uint32_t ms);           // 0 disables the wake press
bool wakeNeeded(uint32_t nowMs);           // is the controller believed asleep?

// ---- what happens after a failed close (R10, R11) ------------------------
uint32_t retryDelayMs();
uint8_t retryLimit();
bool setRetryPolicy(uint32_t delayMs, uint8_t limit);

uint8_t closeAttempts();
uint32_t retryWaitRemainingMs(uint32_t nowMs);

// True when close attempts are exhausted and the door is deliberately staying
// open. It stays true until a person intervenes or until the door is observed
// genuinely closed — a door that quietly resumed trying would defeat the point
// of a limit.
bool gaveUp();
void clearGaveUp();

// ---- travel-time calibration (R17, R18) ----------------------------------
//
// Times one travel in each direction and adopts the result. It begins with a
// quiet period in which nothing is commanded and no switch may change, because
// of F2: the vendor controller moves the door by itself, and a travel time
// measured while that is happening is not a measurement of anything.
enum CalState : uint8_t { CAL_OFF, CAL_QUIET, CAL_RUN_A, CAL_RUN_B, CAL_DONE, CAL_FAILED };

bool startCalibration(uint32_t nowMs, bool maintenanceActive, String &msg);
void abortCalibration(const char *why);
CalState calState();
bool calibrating();

// Progress and the verdict, for the console and the status line. The verdict
// outlives the run so it can be read back after it finishes.
const char *calMessage();
uint32_t calRemainingMs(uint32_t nowMs);

// The tune announcing a movement, by who asked and which way. Lives here
// rather than in chime.h so that the chime module keeps depending on nothing
// but config.h, and so the mapping from "who asked" to "what it sounds like"
// sits beside the code that knows who asked.
ChimeTune moveTune(ActuationSource src, DoorState target);

}  // namespace Actuator
