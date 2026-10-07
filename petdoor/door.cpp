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

  // One press, or several for a controller that sometimes swallows one. The
  // interlock above runs once: it guards against the OPPOSITE relay being
  // energised, and repeats of the same relay cannot violate that.
  //
  // This is the ONE place the control task blocks for a relay, and it blocks
  // for a single press width. Everything that used to be sequenced with more
  // delay() calls — waiting to see whether a wake press took, waiting for the
  // door to arrive, waiting out a retry — is now a deadline the actuator
  // checks each tick. Blocking only for the press itself also means the relay
  // cannot be left asserted by a control task that wedges somewhere else: the
  // release is on the far side of a delay, not of a state machine.
  for (uint8_t i = 0; i < pulseCount_; i++) {
    if (i) delay(pulseGapMs_);
    digitalWrite(pin, RELAY_ASSERT);
    delay(pulseMs_);
    digitalWrite(pin, RELAY_RELEASE);
  }
}

bool DoorController::lockedOut(uint32_t nowMs) const {
  if (!hasActuated_) return false;
  return (nowMs - lastActuationMs_) < minIntervalMs_;
}

ActuationResult DoorController::check(DoorState target, uint32_t nowMs, bool force) {
  // Never close on an unknown door right after boot. If the ESP32 rebooted
  // while an animal was in the doorway, slamming the door is the one outcome
  // worth engineering against — so this is checked before `force`, and a
  // person at the console cannot override it either. They have a hand on the
  // door; thirty seconds is not a hardship.
  if (target == DOOR_CLOSED && nowMs < BOOT_GRACE_MS) {
    bootGraceCount_++;
    return ACT_BOOT_GRACE;
  }

  if (force) return ACT_DONE;

  if (state_ == target) return ACT_ALREADY;

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
  if (target == DOOR_OPEN) return ACT_DONE;

  if (lockedOut(nowMs)) {
    lockedOutCount_++;
    return ACT_LOCKED_OUT;
  }
  return ACT_DONE;
}

void DoorController::press(DoorState direction) {
  pulse(direction == DOOR_OPEN ? PIN_RELAY_OPEN : PIN_RELAY_CLOSE);
  lastPressMs_ = millis();
  hasPressed_ = true;
  pressCount_++;
}

void DoorController::commit(DoorState target, ActuationSource src, uint32_t nowMs) {
  (void)nowMs;  // millis() below, not the tick's nowMs: the press just blocked
  if (target == DOOR_OPEN) {
    openCount_++;
  } else {
    closeCount_++;
  }
  lastSource_ = src;
  state_ = target;
  // Taken after the press rather than from the tick that decided on it: the
  // press blocks for its pulse width, and a travel timed from before it would
  // be short by exactly that much.
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
    case ACT_BUSY: return "already-travelling";
    case ACT_RETRY_WAIT: return "waiting-to-retry";
    case ACT_GAVE_UP: return "gave-up-staying-open";
    default: return "?";
  }
}
