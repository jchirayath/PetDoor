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
  uint32_t lastActuationMs() const { return lastActuationMs_; }
  bool hasActuated() const { return hasActuated_; }

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
  uint32_t directionGapMs() const { return directionGapMs_; }
  bool setDirectionGapMs(uint32_t ms) {
    if (ms < kMinDirectionGapMs) return false;
    directionGapMs_ = ms;
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

  uint32_t minIntervalMs_ = MIN_ACTUATION_INTERVAL_MS;
  uint32_t directionGapMs_ = DIRECTION_CHANGE_GAP_MS;
  uint32_t openCount_ = 0;
  uint32_t closeCount_ = 0;
  uint32_t lockedOutCount_ = 0;
  uint32_t bootGraceCount_ = 0;
};
