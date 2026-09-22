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
  CHIME_DONE,     // once, rising: the travel time has elapsed
  CHIME_REFUSED,  // once, low: the door declined to move
  CHIME_TEST,     // once: prove the wiring, from the console or the server
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

void play(ChimeTune tune);
void stop();

// Advances the pattern. Safe to call at any rate; nothing happens until the
// current step's time is up. The control task's 100 ms tick is the floor on
// how precise a pattern can be, which is why no step below is shorter than
// 120 ms.
void tick(uint32_t nowMs);

ChimeTune playing();
const char *tuneName(ChimeTune t);

}  // namespace Chime
