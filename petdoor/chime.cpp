#include "chime.h"

#include "position.h"
#include "vibration.h"

namespace Chime {
namespace {

// A pattern is a list of steps. freqHz 0 is silence; on an ACTIVE buzzer any
// non-zero frequency simply means "on", since the pitch is not ours to choose.
struct Step {
  uint16_t freqHz;
  uint16_t ms;
};

// "Wait." A short tick roughly once a second, for as long as the door is
// believed to be moving. Deliberately sparse: this plays for twelve seconds
// at a time, several times a day, a few feet from a coop. Anything denser
// becomes an alarm, and an alarm nobody can stand gets unplugged.
const Step kWorking[] = {{2200, 120}, {0, 880}};

// ---- the movement announcements -------------------------------------------
//
// Count of beeps = who asked (2 collar, 3 console, 4 network); the long beep
// goes last to open and first to close. See chime.h for why it is counted
// rather than pitched.
//
// Every step is >= 120 ms because the control task ticks at 100 ms and that is
// the floor on how precisely a pattern can be timed. Shorter steps would be
// stretched to a tick and the short-vs-long contrast — the part that survives
// on an active buzzer — would blur into "some beeps" for all six.
constexpr uint16_t kShort = 150;   // a "dot"
constexpr uint16_t kLong = 400;    // a "dash"
constexpr uint16_t kGap = 120;     // between beeps of the same tune
constexpr uint16_t kLo = 1200;     // the dots' pitch, on a passive buzzer
constexpr uint16_t kHi = 1800;     // the dash's pitch: rising reads as opening

const Step kMoveBeaconOpen[] = {
    {kLo, kShort}, {0, kGap}, {kHi, kLong}};
const Step kMoveBeaconClose[] = {
    {kHi, kLong}, {0, kGap}, {kLo, kShort}};
const Step kMoveConsoleOpen[] = {
    {kLo, kShort}, {0, kGap}, {kLo, kShort}, {0, kGap}, {kHi, kLong}};
const Step kMoveConsoleClose[] = {
    {kHi, kLong}, {0, kGap}, {kLo, kShort}, {0, kGap}, {kLo, kShort}};
const Step kMoveRemoteOpen[] = {
    {kLo, kShort}, {0, kGap}, {kLo, kShort}, {0, kGap}, {kLo, kShort}, {0, kGap}, {kHi, kLong}};
const Step kMoveRemoteClose[] = {
    {kHi, kLong}, {0, kGap}, {kLo, kShort}, {0, kGap}, {kLo, kShort}, {0, kGap}, {kLo, kShort}};

// ---- the four endings -----------------------------------------------------

// "Arrived." Two equal medium beeps, rising a fifth. No movement tune has two
// equal beeps, which is what keeps this from being heard as one.
const Step kDone[] = {{1568, 250}, {0, 80}, {2349, 250}};

// "No." One long low beep — the opposite shape to everything above, so a
// refusal can never be mistaken for the door setting off.
const Step kRefused[] = {{440, 700}};

// "Not... now." A scheduled window is refusing to open the door. Two long low
// beeps with a deliberately wide gap — the same register as kRefused, because
// it means the same kind of thing, but unmistakably two events rather than one.
//
// The 400 ms gap is what carries it. kNoMove below is also two long low beeps
// and differs only in its gap being 150 ms, which sounds like a stutter where
// this sounds like two separate statements.
const Step kRefusedSchedule[] = {{440, 500}, {0, 400}, {440, 500}};

// "Nothing moved." Said twice, flatly and low. Twice rather than once because
// it is the ending that means "that press did not take" — which is worth
// distinguishing from a flat refusal standing right next to it.
const Step kNoMove[] = {{440, 500}, {0, 150}, {440, 500}};

// "It stopped partway." Five fast beeps: the most urgent pattern in the set,
// because this is the one ending that can mean something is under the door.
// The burst gap is 70 ms, well under every other tune's, so it reads as a
// clatter rather than as a count even on a one-pitch buzzer.
const Step kStalled[] = {{880, 180}, {0, 70}, {880, 180}, {0, 70}, {880, 180},
                         {0, 70},    {880, 180}, {0, 70}, {880, 180}};

// "I have given up closing it and it is staying open." One very long beep then
// two short — nothing else in the set starts with 800 ms. This plays once,
// not on a loop: the door reports this state continuously on its LED, in the
// log and in the status line, and a buzzer that repeated it all night would be
// an alarm, which gets unplugged, which loses the message entirely.
const Step kGaveUp[] = {{600, 800}, {0, 250}, {600, 200}, {0, 250}, {600, 200}};

// "The door moved and it was not me." Two short blips, far apart — the sound
// of the door asking a question rather than reporting a result.
const Step kUncommanded[] = {{1500, 150}, {0, 450}, {1500, 150}};

// "Something is wrong with me." Three short low blips with wide gaps — spaced
// so it reads as deliberate rather than urgent, because it repeats and anything
// urgent that repeats gets unplugged. Three distinguishes it from the two of
// kUncommanded, and the low register from that tune's high one.
const Step kSensorFault[] = {{440, 120}, {0, 350}, {440, 120}, {0, 350}, {440, 120}};

// Three even beeps: long enough to hear, distinctive enough that you know the
// buzzer is responding to you and not to the door.
const Step kTest[] = {{2000, 150}, {0, 150}, {2000, 150}, {0, 150}, {2000, 150}};

// The acknowledgements. Lock and unlock are a pair distinguished by LENGTH as
// well as pitch — two short against one medium — because the register they
// used to differ by is not ours to choose on an active buzzer.
const Step kAckLock[]   = {{700, 150}, {0, 130}, {700, 150}};
const Step kAckUnlock[] = {{2200, 350}};
const Step kAckSet[]    = {{2000, 150}};

struct Pattern {
  const Step *steps;
  uint8_t len;
  bool loops;
};

Pattern patternFor(ChimeTune t) {
#define PETDOOR_TUNE(steps, loops) \
  { (steps), sizeof(steps) / sizeof(Step), (loops) }
  switch (t) {
    case CHIME_WORKING:            return PETDOOR_TUNE(kWorking, true);
    case CHIME_MOVE_BEACON_OPEN:   return PETDOOR_TUNE(kMoveBeaconOpen, false);
    case CHIME_MOVE_BEACON_CLOSE:  return PETDOOR_TUNE(kMoveBeaconClose, false);
    case CHIME_MOVE_CONSOLE_OPEN:  return PETDOOR_TUNE(kMoveConsoleOpen, false);
    case CHIME_MOVE_CONSOLE_CLOSE: return PETDOOR_TUNE(kMoveConsoleClose, false);
    case CHIME_MOVE_REMOTE_OPEN:   return PETDOOR_TUNE(kMoveRemoteOpen, false);
    case CHIME_MOVE_REMOTE_CLOSE:  return PETDOOR_TUNE(kMoveRemoteClose, false);
    case CHIME_DONE:               return PETDOOR_TUNE(kDone, false);
    case CHIME_REFUSED:            return PETDOOR_TUNE(kRefused, false);
    case CHIME_REFUSED_SCHEDULE:   return PETDOOR_TUNE(kRefusedSchedule, false);
    case CHIME_NO_MOVE:            return PETDOOR_TUNE(kNoMove, false);
    case CHIME_STALLED:            return PETDOOR_TUNE(kStalled, false);
    case CHIME_GAVE_UP:            return PETDOOR_TUNE(kGaveUp, false);
    case CHIME_UNCOMMANDED:        return PETDOOR_TUNE(kUncommanded, false);
    case CHIME_SENSOR_FAULT:       return PETDOOR_TUNE(kSensorFault, false);
    case CHIME_TEST:               return PETDOOR_TUNE(kTest, false);
    case CHIME_ACK_LOCK:           return PETDOOR_TUNE(kAckLock, false);
    case CHIME_ACK_UNLOCK:         return PETDOOR_TUNE(kAckUnlock, false);
    case CHIME_ACK_SET:            return PETDOOR_TUNE(kAckSet, false);
    default:                       return {nullptr, 0, false};
  }
#undef PETDOOR_TUNE
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
  // The switches and the vibration sensor are configured at runtime too, so
  // the buzzer has to ask them rather than assume its pin is free.
  if (Position::fittedAt(DOOR_OPEN) && pin == Position::openPin()) {
    return "that is the OPEN limit switch";
  }
  if (Position::fittedAt(DOOR_CLOSED) && pin == Position::closedPin()) {
    return "that is the CLOSED limit switch";
  }
  if (Vibration::enabled() && pin == Vibration::pin()) {
    return "that is the vibration sensor";
  }

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
    case CHIME_WORKING:            return "working";
    case CHIME_MOVE_BEACON_OPEN:   return "beacon-open";
    case CHIME_MOVE_BEACON_CLOSE:  return "beacon-close";
    case CHIME_MOVE_CONSOLE_OPEN:  return "console-open";
    case CHIME_MOVE_CONSOLE_CLOSE: return "console-close";
    case CHIME_MOVE_REMOTE_OPEN:   return "remote-open";
    case CHIME_MOVE_REMOTE_CLOSE:  return "remote-close";
    case CHIME_DONE:               return "arrived";
    case CHIME_REFUSED:            return "refused";
    case CHIME_REFUSED_SCHEDULE:   return "refused-schedule";
    case CHIME_NO_MOVE:            return "never-moved";
    case CHIME_STALLED:            return "stalled";
    case CHIME_GAVE_UP:            return "gave-up";
    case CHIME_UNCOMMANDED:        return "uncommanded";
    case CHIME_SENSOR_FAULT:       return "sensor-fault";
    case CHIME_TEST:               return "test";
    case CHIME_ACK_LOCK:           return "ack-lock";
    case CHIME_ACK_UNLOCK:         return "ack-unlock";
    case CHIME_ACK_SET:            return "ack-set";
    default:                       return "silent";
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
