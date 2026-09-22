#include "position.h"

#include "chime.h"

namespace Position {
namespace {

int openPin_ = -1;
int closedPin_ = -1;
bool activeLow_ = true;

// Debounce state, one set per switch.
struct Debounced {
  bool stable = false;    // the level we believe
  bool candidate = false; // the level we have been seeing
  uint32_t sinceMs = 0;   // when we started seeing it
};
Debounced openSw_;
Debounced closedSw_;

// Reads one pin, applying polarity. "Made" means the switch is closed, i.e. the
// door is at that end.
bool rawMade(int pin) {
  if (pin < 0) return false;
  const int level = digitalRead(pin);
  return activeLow_ ? (level == LOW) : (level == HIGH);
}

void update(Debounced &d, int pin, uint32_t nowMs) {
  if (pin < 0) {
    d.stable = d.candidate = false;
    return;
  }
  const bool now = rawMade(pin);
  if (now != d.candidate) {
    d.candidate = now;
    d.sinceMs = nowMs;
    return;
  }
  if (d.candidate != d.stable &&
      static_cast<int32_t>(nowMs - d.sinceMs) >= static_cast<int32_t>(SENSOR_DEBOUNCE_MS)) {
    d.stable = d.candidate;
  }
}

void attach(int pin) {
  if (pin < 0) return;
  // INPUT_PULLUP for the active-low wiring this is built around: switch to
  // GND, pin idles high. An active-high installation supplies its own pull-down
  // and gets a plain INPUT, because the ESP32 cannot pull down and up at once.
  pinMode(pin, activeLow_ ? INPUT_PULLUP : INPUT);
}

}  // namespace

const char *pinProblem(int pin) {
  if (pin < 0) return nullptr;  // "not fitted" is always valid
  if (!digitalPinIsValid(pin)) return "no such GPIO on this chip";
#if CONFIG_IDF_TARGET_ESP32
  if (pin >= 6 && pin <= 11) return "GPIO 6-11 are the SPI flash; touching them crashes the chip";
  // 34-39 are input-only, which SOUNDS ideal for a switch and is the trap: they
  // have no internal pull-up, so a switch on one floats and reads as noise.
  if (pin >= 34 && pin <= 39) return "GPIO 34-39 have no internal pull-up; a switch on one floats";
#endif
  if (pin == PIN_RELAY_OPEN) return "that is the OPEN relay";
  if (pin == PIN_RELAY_CLOSE) return "that is the CLOSE relay";
  if (pin == PIN_STATUS_LED) return "that is the status LED";
  if (Chime::enabled() && pin == Chime::pin()) return "that is the buzzer";
#if CONFIG_IDF_TARGET_ESP32
  if (pin == 0 || pin == 2 || pin == 12 || pin == 15) {
    return "warning: strapping pin — a switch closed at power-on can stop the board booting";
  }
#endif
  return nullptr;
}

bool configure(int openPin, int closedPin, bool activeLow) {
  if (openPin >= 0 && closedPin >= 0 && openPin == closedPin) return false;
  for (int p : {openPin, closedPin}) {
    const char *why = pinProblem(p);
    if (why != nullptr && strncmp(why, "warning:", 8) != 0) return false;
  }
  openPin_ = openPin;
  closedPin_ = closedPin;
  activeLow_ = activeLow;
  attach(openPin_);
  attach(closedPin_);
  // Start from a clean slate rather than carrying a belief across a rewiring.
  openSw_ = Debounced();
  closedSw_ = Debounced();
  return true;
}

void begin(int openPin, int closedPin, bool activeLow) {
  openPin_ = closedPin_ = -1;
  configure(openPin, closedPin, activeLow);
}

void tick(uint32_t nowMs) {
  update(openSw_, openPin_, nowMs);
  update(closedSw_, closedPin_, nowMs);
}

bool enabled() { return openPin_ >= 0 || closedPin_ >= 0; }
int openPin() { return openPin_; }
int closedPin() { return closedPin_; }
bool activeLow() { return activeLow_; }
bool openMade() { return openSw_.stable; }
bool closedMade() { return closedSw_.stable; }

bool fault() { return openSw_.stable && closedSw_.stable; }

DoorState state() {
  if (!enabled()) return DOOR_UNKNOWN;
  if (fault()) return DOOR_UNKNOWN;     // contradictory; believe neither
  if (openSw_.stable) return DOOR_OPEN;
  if (closedSw_.stable) return DOOR_CLOSED;
  return DOOR_UNKNOWN;                  // mid-travel, or not at either end
}

}  // namespace Position
