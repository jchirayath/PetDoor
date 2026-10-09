// chime.h — the door's audible annunciator.
//
// The door takes ~15 seconds to travel and gives no sign that it heard you.
// The relay clicks, and then nothing happens for a quarter of a minute, which
// is indistinguishable from a relay that clicked into a controller that
// swallowed the press. This plays a pattern while the door is believed to be
// moving and a different one when that time is up, so "it is working, wait" and
// "it should be there now" are two sounds rather than a guess.
//
// Two honest limits, stated here because they decide what the patterns can be:
//
//   * THIS IS NOT SPEECH. An ESP32 can play recorded words, but not through a
//     buzzer — that needs a DAC, an amplifier and a speaker. What a buzzer can
//     do is patterns, so "wait" is a slow repeating tick and "OK" is a rising
//     two-tone. They are distinguishable across a yard, which is the actual
//     requirement.
//
//   * THE DOOR STILL HAS NO POSITION FEEDBACK. The "done" chime means the
//     travel time you configured has elapsed, not that the door arrived. It is
//     a timer, and it will chime cheerfully at a door stuck halfway. Only a
//     limit switch can tell you where the door really is; see docs/SAFETY.md.
//
// Non-blocking by construction: play() starts a pattern and returns, tick()
// advances it. The control task is the only caller, so a chime can never delay
// the door — and the door's relay pulse can and does delay the chime, which is
// the right way round.

#pragma once

#include <Arduino.h>

#include "config.h"

enum ChimeTune : uint8_t {
  CHIME_NONE = 0,
  CHIME_WORKING,  // repeats until something else plays: the door is moving

  // ---- who asked for this movement ---------------------------------------
  //
  // Three sources, three sounds, and the distinction is carried by HOW MANY
  // beeps rather than by pitch — because an active buzzer has one pitch it
  // chose at the factory, and a set of tunes separated only by pitch collapses
  // into one sound on half the hardware this supports.
  //
  // The mnemonic is distance: the beep count is how far away the thing that
  // asked for it was.
  //
  //   2 beeps   the collar, which was standing at the door
  //   3 beeps   a hand on the console, a cable away
  //   4 beeps   the network, which could have been anywhere
  //
  // Within each, the LONG beep says which way the door is going: last for
  // opening (rising), first for closing (falling). That is the shape every
  // appliance anyone owns already uses, and it survives on one pitch.
  //
  //   beacon open      .-        console open      ..-       remote open   ...-
  //   beacon close     -.        console close     -..       remote close  -...
  //
  // Worth being able to tell apart from the coop without a screen: a door that
  // opened because the collar arrived is the system working, and a door that
  // opened because something on the network asked is a thing to go and read
  // about.
  CHIME_MOVE_BEACON_OPEN,
  CHIME_MOVE_BEACON_CLOSE,
  CHIME_MOVE_CONSOLE_OPEN,
  CHIME_MOVE_CONSOLE_CLOSE,
  CHIME_MOVE_REMOTE_OPEN,
  CHIME_MOVE_REMOTE_CLOSE,

  // ---- how it turned out --------------------------------------------------
  //
  // These are the point of having a buzzer at all. The door takes twelve
  // seconds and gives no sign of itself, so the useful thing a sound can do is
  // report the END of a travel — and there are four quite different endings,
  // which used to share one "refused" tone between them.
  CHIME_DONE,     // arrived: verified by a limit switch, or the timer expired
  CHIME_REFUSED,  // the door declined to move at all
  // Refused because a SCHEDULED WINDOW is in force, as opposed to any other
  // refusal. Worth its own sound because the remedy is different: a manual
  // lock needs unlocking, and a window needs either waiting or editing.
  //
  // Somebody standing at the door at midnight with the collar in their hand
  // cannot tell "I locked this" from "a window I set weeks ago is refusing
  // it" — and that is exactly the moment they start taking the door apart.
  //
  // Two long low beeps with a WIDE gap: "not... now". The gap is the
  // distinction, not the pitch, so it survives on an active buzzer where
  // every beep is the same note. Nothing else in the set is two long beeps
  // separated like this.
  CHIME_REFUSED_SCHEDULE,
  // The relay fired and the vibration sensor felt nothing for the whole travel:
  // the controller swallowed the press, or the door is jammed solid. Nothing
  // moved, so nothing is trapped — said twice, flatly, because it means "try
  // again" rather than "go and look".
  CHIME_NO_MOVE,
  // It started and never arrived. Five fast beeps, the most urgent thing in
  // the set, because this is the one that means something may be under the
  // door. A stalled close is also reversed; see actuator.h.
  CHIME_STALLED,
  // Close attempts exhausted: the door is staying open, deliberately, until
  // somebody deals with whatever is in the way. Long-short-short, which
  // nothing else in the set resembles.
  CHIME_GAVE_UP,
  // A limit switch reported the door at an end that nothing commanded it to go
  // to. Two isolated blips — the sound of the door asking a question.
  CHIME_UNCOMMANDED,
  // A sensor has been caught lying, and this is the only tune that REPEATS —
  // sparsely, every SENSOR_FAULT_BEEP_MS. Three short low blips, well spaced:
  // the door clearing its throat, not an alarm. See SENSOR_FAULT_BEEP_MS in
  // config.h for why this one earns an exception to the play-once rule.
  CHIME_SENSOR_FAULT,

  CHIME_TEST,     // once: prove the wiring, from the console or the server

  // Acknowledgements for commands that merely change a SETTING, played the
  // moment one is applied. Valuable precisely because the remote channel is
  // slow: a command sits for up to five minutes, so the beep is how you learn
  // it landed without walking to a screen.
  //
  // Deliberately only three of them. The distinction that matters out there is
  // "the door is about to move" versus "the door took a note", so everything
  // that takes a note shares one short blip, and only the lock — which
  // changes what the door will DO — gets its own.
  //
  //   lock    two short, low     ..
  //   unlock  one medium, high   -
  //   set     one short          .
  CHIME_ACK_LOCK,
  CHIME_ACK_UNLOCK,
  CHIME_ACK_SET,
};

namespace Chime {

// A buzzer is one of two quite different devices sold under the same name, and
// which one you have decides how it is driven:
//
//   ACTIVE  — contains its own oscillator. Apply DC and it sounds, at one fixed
//             pitch it chose at the factory. Driven here with digitalWrite.
//   PASSIVE — a bare transducer. Apply DC and it ticks once and goes silent; it
//             needs a square wave, and the pitch is whatever you feed it.
//             Driven here with the LEDC peripheral.
//
// If you do not know which you have, `beep` on the console will tell you: an
// active buzzer sounds the same for every tune, a passive one plays the rising
// two-tone properly. Guessing wrong is harmless — it just sounds wrong.
//
// pin < 0 disables the annunciator entirely and every other call becomes a
// no-op. That is the default: this firmware ships for boards that have no
// buzzer at all, and must not drive an arbitrary GPIO on the assumption there
// is something harmless attached to it.
void begin(int pin, bool passive, bool activeLow);

// Change the wiring at runtime. Same validation as begin(), and it stops
// whatever is playing before it moves to the new pin — leaving a tone running
// on a pin that is about to be detached is how a buzzer gets stuck on.
//
// Runtime-settable for the same reason the relay pulse is: which pin your board
// wired its buzzer to is a property of YOUR board, is frequently undocumented,
// and finding it by reflashing one guess at a time is miserable. `buzzer 27`
// then `beep` costs seconds.
bool configure(int pin, bool passive, bool activeLow);

// Why a pin was refused, for the message the user actually reads. Returns
// nullptr if the pin is usable; a non-null string is either a refusal (when
// configure() also returns false) or, for the strapping pins, a warning about a
// pin that will work but may affect booting.
const char *pinProblem(int pin);
bool pinUsable(int pin);

int pin();
bool passive();
bool activeLow();
bool enabled();

// Silence the routine sounds without forgetting the buzzer pin.
//
// Distinct from `buzzer off`, which sets the pin to -1 and makes you re-enter
// the wiring to get sound back. This is a flag you can toggle from the portal
// and it survives a reboot.
//
// FAULTS STILL SOUND. A stall, a door that never moved, exhausted close
// retries, an uncommanded travel and a sensor fault all ring through a mute,
// because the buzzer is the only sink that reaches somebody standing at the
// door and docs/OBSERVABILITY.md counts it as one. Muting the ticking is a
// comfort; muting the alarm is a different decision and is not this one.
void setMuted(bool muted);
bool muted();
// True for the tunes a mute silences — the routine ones. Pure, so the split
// can be read in one place rather than inferred from call sites.
bool isRoutine(ChimeTune tune);

void play(ChimeTune tune);
void stop();

// Advances the pattern. Safe to call at any rate; nothing happens until the
// current step's time is up. The control task's 100 ms tick is the floor on
// how precise a pattern can be, which is why no step in chime.cpp is shorter
// than 120 ms — below that the timing blurs and short-vs-long stops reading.
void tick(uint32_t nowMs);

ChimeTune playing();
const char *tuneName(ChimeTune t);

}  // namespace Chime
