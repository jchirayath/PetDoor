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
// This is open-loop: without limit switches the firmware knows what it
// commanded, not where the door actually is. See docs/SAFETY.md.

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
};

enum ActuationResult {
  ACT_DONE,             // relay pulsed
  ACT_ALREADY,          // already in that state, nothing to do
  ACT_LOCKED_OUT,       // too soon after the previous actuation
  ACT_BOOT_GRACE,       // refusing to close during the boot grace window
};

class DoorController {
 public:
  void begin();

  ActuationResult requestOpen(uint32_t nowMs);
  ActuationResult requestClose(uint32_t nowMs);

  // Bypasses state and lockout checks but keeps the interlock. Serial commands
  // only, for bench-testing the wiring.
  void forcePulseOpen();
  void forcePulseClose();

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
  uint32_t lastActuationMs() const { return lastActuationMs_; }
  bool hasActuated() const { return hasActuated_; }

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

  // Repeat presses, for a controller that occasionally swallows one.
  //
  // Capped at 3 because this is a blind retry: with no position feedback the
  // door cannot know the first press worked, so every extra press is another
  // chance to hit a moving door with what its controller may read as STOP.
  // Two is a stopgap; three is already pushing it; more is not a fix, it is
  // noise. See RELAY_PULSE_COUNT in config.h.
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
