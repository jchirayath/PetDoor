// position.h — optional limit switches telling the door where it actually is.
//
// Without these the firmware is open-loop: it knows what it COMMANDED, not what
// happened. A door that jammed halfway still reads as open, the "arrived" chime
// is a stopwatch expiring rather than a door arriving, and a controller that
// swallowed a button press is indistinguishable from one that obeyed. Every
// caveat in docs/SAFETY.md about not knowing the door's position traces back to
// their absence.
//
// Two normally-open switches — a reed switch and a magnet is the right choice
// for a coop, being sealed glass with nothing exposed to peck at or corrode.
// Each closes only when the door reaches that end of its travel:
//
//     GPIO <openPin>   ──[ reed switch ]── GND      door fully OPEN
//     GPIO <closedPin> ──[ reed switch ]── GND      door fully CLOSED
//
// Configured as INPUT_PULLUP, so a switch reads LOW when it is at that end and
// the pin idles HIGH otherwise. That polarity is deliberate: a broken wire or a
// pulled-off magnet reads as "not at that end", never as a false arrival. The
// door then declines to believe it has got somewhere it has not.
//
// WHAT THIS DOES NOT DO: it never commands the motor. The proximity logic
// remains the only thing that decides to open or close, and the interlock and
// lockout in door.cpp remain the only things that gate a pulse. These switches
// only correct what the firmware BELIEVES, and say so loudly when belief and
// reality disagree. Letting a sensor drive the motor is a much larger change
// with a much worse failure mode — a stuck switch that commands a door — and it
// is deliberately not attempted here.

#pragma once

#include <Arduino.h>

#include "config.h"
#include "door.h"

namespace Position {

// Either pin may be -1, which disables that end. With both -1 the whole module
// is inert and the door behaves exactly as it did before sensors existed —
// which is the default, because most builds have none fitted.
void begin(int openPin, int closedPin, bool activeLow);

// Runtime-settable, like the buzzer and for the same reason: which pins you
// used is a property of your wiring, and finding out you guessed wrong should
// not cost a reflash. Rejects a pin that cannot be an input, and refuses to
// share a pin with the relays, the status LED, the buzzer, or the other sensor.
bool configure(int openPin, int closedPin, bool activeLow);

// nullptr when the pin is usable; otherwise why not. A "warning:" prefix means
// usable but worth knowing about.
const char *pinProblem(int pin);

// Samples both pins and runs the debounce. Call from the control task.
//
// Debounced because a reed switch chatters as the magnet passes, and a door
// settling can bounce it several times. An undebounced sensor would announce
// three arrivals for one.
void tick(uint32_t nowMs);

// Where the door actually is: DOOR_OPEN, DOOR_CLOSED, or DOOR_UNKNOWN when
// neither switch is made — which is the normal reading MID-TRAVEL, and also
// what you get with no sensors fitted.
DoorState state();

// Both switches closed at once. Physically impossible on a working door, so it
// means a shorted wire, a stuck switch, or two magnets. Reported rather than
// guessed at: state() returns DOOR_UNKNOWN while this holds.
bool fault();

bool enabled();
int openPin();
int closedPin();
bool activeLow();

// Raw debounced level per switch, for the console and the status line.
bool openMade();
bool closedMade();

}  // namespace Position
