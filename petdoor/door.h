// door.h — relay control for the door motor.
//
// Two momentary outputs: one pulses OPEN, one pulses CLOSE. The controller
// enforces the safety rules that matter when the load is a motor attached to a
// door an animal walks through:
//
//   * the two relays are never energised at once, and reversing direction
//     always waits out DIRECTION_CHANGE_GAP_MS;
//   * no actuation within MIN_ACTUATION_INTERVAL_MS of the previous one;
//   * repeating a command the door is already executing does nothing;
//   * the door is never driven closed during the post-boot grace window.
//
// WHAT THIS CLASS IS NOT RESPONSIBLE FOR: sequencing an attempt. It presses a
// button and it gates whether a press is allowed; it does not decide whether a
// wake press is needed first, does not wait for the door to arrive, and does
// not retry. That belongs to one owner — see petdoor/actuator.h — because the
// rules are about a whole attempt rather than about a pulse, and spreading
// them across the control task, the chime logic and the position module is how
// the door came to be commandable mid-travel.
//
// Without limit switches the firmware still knows only what it commanded, not
// where the door is. See docs/SAFETY.md.

#pragma once

#include <Arduino.h>

#include "config.h"

enum DoorState {
  DOOR_UNKNOWN,  // since boot, we have not commanded anything
  DOOR_OPEN,
  DOOR_CLOSED,
};

// What caused an actuation. Recorded in the event log so "the door opened" can
// be told apart from "I opened the door" weeks later.
enum ActuationSource : uint8_t {
  SRC_BEACON = 0,  // the proximity logic decided
  SRC_MANUAL = 1,  // someone typed o or x at the console
  // Split out from SRC_MANUAL because the two answer different questions
  // weeks later. "I was standing at the door" and "something on the network
  // asked" look identical in a log that calls both manual, and only one of
  // them is worth investigating when a door opened at 3 a.m. It also earns a
  // different sound: see ChimeTune.
  SRC_REMOTE = 2,
  // The firmware reversing a close of its own that stalled. Not a person and
  // not the collar — the door protecting whatever it failed to close onto.
  // Always worth finding in a log, and always worth hearing.
  SRC_FAILSAFE = 3,
};

enum ActuationResult {
  ACT_DONE,             // relay pulsed
  ACT_ALREADY,          // already in that state, nothing to do
  ACT_LOCKED_OUT,       // too soon after the previous actuation
  ACT_BOOT_GRACE,       // refusing to close during the boot grace window
  // A travel in this direction is already in flight. A second press in the
  // same direction mid-travel reads as STOP on real controllers, which parks
  // the door halfway — so it is refused rather than queued.
  ACT_BUSY,
  // A close stalled and the retry delay has not elapsed. Minutes, by design.
  ACT_RETRY_WAIT,
  // Close attempts are exhausted. The door is staying open deliberately.
  ACT_GAVE_UP,
};

class DoorController {
 public:
  void begin();

  // ---- the three steps of an actuation, kept separate on purpose ----------
  //
  // They are separate because the thing between them takes seconds and must
  // not be spent inside this class: a wake press has to be followed by a look
  // at whether the door moved, and an arrival has to be waited for. Fusing
  // them back into one requestOpen() would mean either blocking the control
  // task for a whole travel or going back to assuming.
  //
  // Only Actuator calls press() and commit(). Keeping that to one caller is
  // what makes "the door has one owner" checkable rather than aspirational.

  // May a travel to `target` start now? Gating only — nothing moves.
  // `force` is a human or the network asking rather than the collar: it
  // overrides "already there" and the actuation lockout, which exist to stop
  // the beacon thrashing the motor, and never overrides the boot grace.
  ActuationResult check(DoorState target, uint32_t nowMs, bool force);

  // One interlocked press of the button for `direction`. Blocks for the pulse
  // width and no longer. Changes NOTHING about what the door believes: a press
  // is not an arrival, and on this hardware it is not even reliably a
  // movement.
  void press(DoorState direction);

  // Record that a travel to `target` has been committed, for `src`. This is
  // what moves the believed state and the counters.
  void commit(DoorState target, ActuationSource src, uint32_t nowMs);

  DoorState state() const { return state_; }

  // Tell the controller where the door ACTUALLY is, from a limit switch.
  //
  // Correction only, never command: this can change what the controller
  // believes, and nothing else. No relay is pulsed here, no lockout is
  // touched, no interlock is bypassed.
  //
  // It matters because "already in that state" is how requestOpen() and
  // requestClose() decide to do nothing. If someone shoves the door by hand,
  // or the controller swallowed a press, the belief is stale and the door
  // sits there refusing to correct itself. With a switch fitted, reality wins
  // and the next request actuates as it should.
  //
  // DOOR_UNKNOWN is ignored rather than stored: mid-travel is the normal
  // reading between the two switches, and forgetting the last known position
  // every time the door passes between them would be worse than useless.
  // Returns true if the belief actually changed, so the caller can log it.
  bool observePosition(DoorState observed) {
    if (observed == DOOR_UNKNOWN || observed == state_) return false;
    state_ = observed;
    // hasActuated_ and lastActuationMs_ are deliberately NOT touched. They mean
    // "we pulsed a relay", and lockedOut() is built on them: setting them from
    // a sensor reading would start the actuation lockout at boot, with
    // lastActuationMs_ still 0, and refuse the first close for no reason.
    // Nothing moved because of us. Only the belief changed.
    return true;
  }
  // Admit that the door's position is no longer known.
  //
  // Called when a travel stalled: the door is somewhere between the two ends
  // and claiming either would be a lie. The lie that matters is "closed",
  // because that is the one that makes the next close request a no-op — so the
  // door says UNKNOWN instead and the next request actuates.
  //
  // Deliberately narrower than observePosition(), which refuses to store
  // DOOR_UNKNOWN: there, UNKNOWN is the normal mid-travel reading from a pair
  // of switches and forgetting the last position every time the door passed
  // between them would be worse than useless. Here it is a conclusion.
  void setBeliefUnknown() { state_ = DOOR_UNKNOWN; }

  uint32_t lastActuationMs() const { return lastActuationMs_; }
  bool hasActuated() const { return hasActuated_; }

  // When a relay last closed, for ANY reason — a wake press, a retry, or a
  // real actuation. This is the clock the wake decision runs on, and it is
  // deliberately not lastActuationMs(): the question "is the controller
  // awake?" is answered by when it last saw a button, not by when the door
  // last agreed to go somewhere.
  uint32_t lastPressMs() const { return lastPressMs_; }
  bool hasPressed() const { return hasPressed_; }
  uint32_t pressCount() const { return pressCount_; }

  // Why the door last moved. Meaningless before the first actuation.
  ActuationSource lastSource() const { return lastSource_; }

  // Lifetime-of-boot counters. Opens/closes are motor wear; the refusals tell
  // you *why* the door is not moving when you expected it to.
  uint32_t openCount() const { return openCount_; }
  uint32_t closeCount() const { return closeCount_; }
  uint32_t lockedOutCount() const { return lockedOutCount_; }
  uint32_t bootGraceCount() const { return bootGraceCount_; }

  // Minimum gap between actuations. Protects the motor from thrash; must stay
  // below the exit dwell or it delays closing.
  uint32_t minIntervalMs() const { return minIntervalMs_; }
  void setMinIntervalMs(uint32_t ms) { minIntervalMs_ = ms; }

  // Dead time with the opposite relay released before this one is asserted.
  // This is the motor interlock — both relays energised at once is a short
  // across the direction contacts. A mechanical relay releases in roughly
  // 5-15 ms, so the 250 ms default already leaves well over an order of
  // magnitude of margin, and the 100 ms floor below still leaves plenty.
  static constexpr uint32_t kMinDirectionGapMs = 100;
  // Ceiling as well as a floor. pulse() sits in delay() for this long with the
  // control task blocked, so an absurd value (a negative console entry cast to
  // unsigned, say) would wedge the door and the console together — and it is
  // persisted, so a power cycle would not recover it.
  static constexpr uint32_t kMaxDirectionGapMs = 5000;
  uint32_t directionGapMs() const { return directionGapMs_; }
  bool setDirectionGapMs(uint32_t ms) {
    if (ms < kMinDirectionGapMs || ms > kMaxDirectionGapMs) return false;
    directionGapMs_ = ms;
    return true;
  }

  // How long each relay is held closed — the length of the "button press" the
  // door controller sees. Runtime-adjustable because the right value is a
  // property of YOUR controller, not of this firmware: one that debounces its
  // button over 300 ms ignores the 200 ms default entirely, and the symptom is
  // a relay that clicks while the door does not move. Finding that by
  // reflashing one guess at a time is miserable.
  //
  // Floor: a mechanical relay needs ~5–15 ms just to pull in, so anything under
  // 50 ms risks the contacts never properly closing.
  // Ceiling: pulse() sits in delay() for this long with the control task
  // blocked — no samples drained, no presence re-evaluated, and the door cannot
  // be told to reverse mid-travel. 10 s is generous for a held-contact motor
  // and still short of wedging the console. See docs/WIRING.md.
  static constexpr uint32_t kMinPulseMs = 50;
  static constexpr uint32_t kMaxPulseMs = 10000;
  uint32_t pulseMs() const { return pulseMs_; }
  bool setPulseMs(uint32_t ms) {
    if (ms < kMinPulseMs || ms > kMaxPulseMs) return false;
    pulseMs_ = ms;
    return true;
  }

  // Repeat presses within a single press of the button. LEAVE THIS AT 1.
  //
  // It predates the wake press and was the old answer to a controller that
  // swallowed a press: press twice and hope. The wake press replaced it with a
  // version that looks at whether the door moved in between, which is strictly
  // better — and these two stack. Setting this to 2 means every press the
  // actuator issues becomes two, so a cold actuation sends four, and on this
  // hardware a press mid-travel reads as STOP.
  //
  // Kept because a controller may yet turn up that the wake logic cannot
  // handle and a blind double-press can. Capped at 3 for the same reason it
  // always was: every extra blind press is another chance to stop a moving
  // door. See RELAY_PULSE_COUNT in config.h.
  //
  // The gap has a floor because two presses closer together than a
  // controller's own debounce window are one press as far as it is concerned,
  // which would make the whole setting do nothing.
  static constexpr uint8_t kMaxPulseCount = 3;
  static constexpr uint32_t kMinPulseGapMs = 200;
  static constexpr uint32_t kMaxPulseGapMs = 5000;
  uint8_t pulseCount() const { return pulseCount_; }
  uint32_t pulseGapMs() const { return pulseGapMs_; }
  bool setPulseTrain(uint8_t count, uint32_t gapMs) {
    if (count < 1 || count > kMaxPulseCount) return false;
    if (count > 1 && (gapMs < kMinPulseGapMs || gapMs > kMaxPulseGapMs)) return false;
    pulseCount_ = count;
    pulseGapMs_ = gapMs;
    return true;
  }

  static const char *stateName(DoorState s);
  static const char *resultName(ActuationResult r);

 private:
  void pulse(uint8_t pin);
  bool lockedOut(uint32_t nowMs) const;

  DoorState state_ = DOOR_UNKNOWN;
  uint32_t lastActuationMs_ = 0;
  bool hasActuated_ = false;
  uint32_t lastPressMs_ = 0;
  bool hasPressed_ = false;
  uint32_t pressCount_ = 0;

  ActuationSource lastSource_ = SRC_BEACON;
  uint32_t minIntervalMs_ = MIN_ACTUATION_INTERVAL_MS;
  uint32_t directionGapMs_ = DIRECTION_CHANGE_GAP_MS;
  uint32_t pulseMs_ = RELAY_PULSE_MS;
  uint8_t pulseCount_ = RELAY_PULSE_COUNT;
  uint32_t pulseGapMs_ = RELAY_PULSE_GAP_MS;
  uint32_t openCount_ = 0;
  uint32_t closeCount_ = 0;
  uint32_t lockedOutCount_ = 0;
  uint32_t bootGraceCount_ = 0;
};
