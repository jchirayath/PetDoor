#include "chime.h"

namespace Chime {
namespace {

// A pattern is a list of steps. freqHz 0 is silence; on an ACTIVE buzzer any
// non-zero frequency simply means "on", since the pitch is not ours to choose.
struct Step {
  uint16_t freqHz;
  uint16_t ms;
};

// "Wait." A short tick roughly once a second, for as long as the door is
// believed to be moving. Deliberately sparse: this plays for fifteen seconds
// at a time, several times a day, a few feet from a coop. Anything denser
// becomes an alarm, and an alarm nobody can stand gets unplugged.
const Step kWorking[] = {{2200, 120}, {0, 880}};

// "OK." Rising, because rising reads as finished and falling reads as failed
// in every appliance anyone owns. Two notes, a fifth apart.
const Step kDone[] = {{1568, 160}, {0, 60}, {2349, 280}};

// "No." Low and flat — the opposite shape to kDone, so the two can never be
// confused through a wall.
const Step kRefused[] = {{440, 500}};

// Three even beeps: long enough to hear, distinctive enough that you know the
// buzzer is responding to you and not to the door.
const Step kTest[] = {{2000, 150}, {0, 150}, {2000, 150}, {0, 150}, {2000, 150}};

struct Pattern {
  const Step *steps;
  uint8_t len;
  bool loops;
};

Pattern patternFor(ChimeTune t) {
  switch (t) {
    case CHIME_WORKING: return {kWorking, sizeof(kWorking) / sizeof(kWorking[0]), true};
    case CHIME_DONE:    return {kDone,    sizeof(kDone)    / sizeof(kDone[0]),    false};
    case CHIME_REFUSED: return {kRefused, sizeof(kRefused) / sizeof(kRefused[0]), false};
    case CHIME_TEST:    return {kTest,    sizeof(kTest)    / sizeof(kTest[0]),    false};
    default:            return {nullptr, 0, false};
  }
}

int pin_ = -1;
bool passive_ = false;
bool activeLow_ = false;
bool attached_ = false;

ChimeTune tune_ = CHIME_NONE;
Pattern pattern_ = {nullptr, 0, false};
uint8_t step_ = 0;
uint32_t stepUntilMs_ = 0;

// LEDC resolution for the passive path. 10 bits is what the core's own tone()
// uses; the duty below is half of it, which is the square wave a transducer
// wants.
constexpr uint8_t kLedcBits = 10;
constexpr uint32_t kLedcHalfDuty = (1u << kLedcBits) / 2;

void silence() {
  if (pin_ < 0) return;
  if (passive_) {
    if (attached_) ledcWrite(pin_, 0);
  } else {
    digitalWrite(pin_, activeLow_ ? HIGH : LOW);
  }
}

void sound(uint16_t freqHz) {
  if (pin_ < 0) return;
  if (freqHz == 0) {
    silence();
    return;
  }
  if (passive_) {
    if (!attached_) return;
    ledcWriteTone(pin_, freqHz);
    ledcWrite(pin_, kLedcHalfDuty);
  } else {
    digitalWrite(pin_, activeLow_ ? LOW : HIGH);
  }
}

void detach() {
  if (pin_ < 0) return;
  silence();
  if (passive_ && attached_) {
    ledcDetach(pin_);
    attached_ = false;
  }
}

// Takes the pin from whatever state it was in to idle-and-ready.
void attach() {
  if (pin_ < 0) return;
  if (passive_) {
    // 1 kHz is a placeholder; every sound() call sets the real frequency.
    attached_ = ledcAttach(pin_, 1000, kLedcBits);
    if (attached_) ledcWrite(pin_, 0);
  } else {
    pinMode(pin_, OUTPUT);
  }
  silence();
}

}  // namespace

const char *pinProblem(int pin) {
  if (pin < 0) return nullptr;  // "off" is always a valid answer

  // The first two checks are the chip's own opinion, via the core's macros, so
  // they stay correct on the S3/C3/C6 where the GPIO map is nothing like the
  // classic ESP32's. The flash and strapping pins below are not expressible
  // that way and are specific to the classic part.
  if (!digitalPinIsValid(pin)) return "no such GPIO on this chip";
  if (!digitalPinCanOutput(pin)) return "input-only GPIO — it cannot drive anything";
#if CONFIG_IDF_TARGET_ESP32
  if (pin >= 6 && pin <= 11) return "GPIO 6-11 are the SPI flash; touching them crashes the chip";
#endif

  if (pin == PIN_RELAY_OPEN) return "that is the OPEN relay — a beep would move the door";
  if (pin == PIN_RELAY_CLOSE) return "that is the CLOSE relay — a beep would move the door";
  if (pin == PIN_STATUS_LED) return "that is the status LED";

#if CONFIG_IDF_TARGET_ESP32
  if (pin == 0 || pin == 2 || pin == 12 || pin == 15) {
    return "warning: strapping pin — a buzzer's pull may stop the board booting";
  }
#endif
  return nullptr;
}

bool pinUsable(int pin) {
  const char *why = pinProblem(pin);
  // The strapping-pin message is advice, not a refusal: plenty of boards wire
  // a buzzer to GPIO 2 and boot perfectly well, and refusing it outright would
  // be this firmware overruling the user about their own hardware.
  return why == nullptr || strncmp(why, "warning:", 8) == 0;
}

void begin(int pin, bool passive, bool activeLow) {
  pin_ = -1;
  attached_ = false;
  configure(pin, passive, activeLow);
}

bool configure(int pin, bool passive, bool activeLow) {
  if (!pinUsable(pin)) return false;

  stop();
  detach();

  pin_ = pin;
  passive_ = passive;
  activeLow_ = activeLow;
  attach();
  return true;
}

int pin() { return pin_; }
bool passive() { return passive_; }
bool activeLow() { return activeLow_; }
bool enabled() { return pin_ >= 0; }
ChimeTune playing() { return tune_; }

const char *tuneName(ChimeTune t) {
  switch (t) {
    case CHIME_WORKING: return "working";
    case CHIME_DONE:    return "done";
    case CHIME_REFUSED: return "refused";
    case CHIME_TEST:    return "test";
    default:            return "silent";
  }
}

void play(ChimeTune tune) {
  if (pin_ < 0) {
    tune_ = CHIME_NONE;
    return;
  }
  const Pattern p = patternFor(tune);
  if (p.steps == nullptr || p.len == 0) {
    stop();
    return;
  }
  tune_ = tune;
  pattern_ = p;
  step_ = 0;
  sound(pattern_.steps[0].freqHz);
  stepUntilMs_ = millis() + pattern_.steps[0].ms;
}

void stop() {
  tune_ = CHIME_NONE;
  pattern_ = {nullptr, 0, false};
  step_ = 0;
  silence();
}

void tick(uint32_t nowMs) {
  if (tune_ == CHIME_NONE || pattern_.steps == nullptr || pin_ < 0) return;
  // Signed comparison so this survives the millis() rollover at 49 days, which
  // a door left running through a season will reach.
  if (static_cast<int32_t>(nowMs - stepUntilMs_) < 0) return;

  step_++;
  if (step_ >= pattern_.len) {
    if (!pattern_.loops) {
      stop();
      return;
    }
    step_ = 0;
  }
  sound(pattern_.steps[step_].freqHz);
  stepUntilMs_ = nowMs + pattern_.steps[step_].ms;
}

}  // namespace Chime
