#include "door.h"

#if RELAY_ACTIVE_LOW
#define RELAY_ASSERT LOW
#define RELAY_RELEASE HIGH
#else
#define RELAY_ASSERT HIGH
#define RELAY_RELEASE LOW
#endif

static_assert(PIN_RELAY_OPEN != PIN_RELAY_CLOSE,
              "PIN_RELAY_OPEN and PIN_RELAY_CLOSE must be different pins.");

void DoorController::begin() {
  // Write the idle level BEFORE switching the pin to an output. Setting the
  // latch first means the pin never presents the asserted level, not even for
  // the microsecond between pinMode() and digitalWrite(). On a door motor that
  // glitch would be a real movement.
  digitalWrite(PIN_RELAY_OPEN, RELAY_RELEASE);
  digitalWrite(PIN_RELAY_CLOSE, RELAY_RELEASE);
  pinMode(PIN_RELAY_OPEN, OUTPUT);
  pinMode(PIN_RELAY_CLOSE, OUTPUT);
  digitalWrite(PIN_RELAY_OPEN, RELAY_RELEASE);
  digitalWrite(PIN_RELAY_CLOSE, RELAY_RELEASE);

  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  state_ = DOOR_UNKNOWN;
  hasActuated_ = false;
}

void DoorController::pulse(uint8_t pin) {
  // Interlock: force the opposite relay released and give it settling time
  // before asserting this one.
  const uint8_t other = (pin == PIN_RELAY_OPEN) ? PIN_RELAY_CLOSE : PIN_RELAY_OPEN;
  digitalWrite(other, RELAY_RELEASE);
  delay(directionGapMs_);

  digitalWrite(pin, RELAY_ASSERT);
  delay(pulseMs_);
  digitalWrite(pin, RELAY_RELEASE);
}

bool DoorController::lockedOut(uint32_t nowMs) const {
  if (!hasActuated_) return false;
  return (nowMs - lastActuationMs_) < minIntervalMs_;
}

ActuationResult DoorController::requestOpen(uint32_t nowMs) {
  if (state_ == DOOR_OPEN) return ACT_ALREADY;

  // Opening is deliberately NOT rate-limited.
  //
  // The lockout exists to stop the motor thrashing, but it can only do that by
  // delaying an actuation — and delaying an OPEN is the one direction that can
  // trap an animal outside a door it just watched close. Opening is the safe
  // direction, so it always goes through immediately.
  //
  // Thrash is still impossible: a close requires the full exit dwell of
  // confirmed absence first, so open/close pairs are inherently seconds apart,
  // and pulse() enforces DIRECTION_CHANGE_GAP_MS on top.
  (void)nowMs;

  pulse(PIN_RELAY_OPEN);
  openCount_++;
  lastSource_ = SRC_BEACON;
  state_ = DOOR_OPEN;
  lastActuationMs_ = millis();
  hasActuated_ = true;
  return ACT_DONE;
}

ActuationResult DoorController::requestClose(uint32_t nowMs) {
  if (state_ == DOOR_CLOSED) return ACT_ALREADY;

  // Never close on an unknown door right after boot. If the ESP32 rebooted
  // while an animal was in the doorway, slamming the door is the one outcome
  // worth engineering against.
  if (nowMs < BOOT_GRACE_MS) {
    bootGraceCount_++;
    return ACT_BOOT_GRACE;
  }

  if (lockedOut(nowMs)) {
    lockedOutCount_++;
    return ACT_LOCKED_OUT;
  }

  pulse(PIN_RELAY_CLOSE);
  closeCount_++;
  lastSource_ = SRC_BEACON;
  state_ = DOOR_CLOSED;
  lastActuationMs_ = millis();
  hasActuated_ = true;
  return ACT_DONE;
}

void DoorController::forcePulseOpen() {
  pulse(PIN_RELAY_OPEN);
  openCount_++;
  lastSource_ = SRC_MANUAL;
  state_ = DOOR_OPEN;
  lastActuationMs_ = millis();
  hasActuated_ = true;
}

void DoorController::forcePulseClose() {
  pulse(PIN_RELAY_CLOSE);
  closeCount_++;
  lastSource_ = SRC_MANUAL;
  state_ = DOOR_CLOSED;
  lastActuationMs_ = millis();
  hasActuated_ = true;
}

const char *DoorController::stateName(DoorState s) {
  switch (s) {
    case DOOR_OPEN: return "OPEN";
    case DOOR_CLOSED: return "CLOSED";
    default: return "UNKNOWN";
  }
}

const char *DoorController::resultName(ActuationResult r) {
  switch (r) {
    case ACT_DONE: return "done";
    case ACT_ALREADY: return "already";
    case ACT_LOCKED_OUT: return "locked-out";
    case ACT_BOOT_GRACE: return "boot-grace";
    default: return "?";
  }
}
