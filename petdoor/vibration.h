// vibration.h — an optional sensor that answers "did the door actually START
// moving?", which is a different and much faster question than "did it arrive".
//
// The door is open loop: it pulses a relay and assumes. A press the controller
// swallowed looks exactly like one it obeyed, which is why RELAY_PULSE_COUNT is
// a BLIND retry that can just as easily stop a door that was already moving.
//
// A limit switch cannot help with that — it has nothing to say until the full
// travel time has elapsed. A vibration sensor reports within about a second,
// and the two compose: vibration says "started", the reed switch says
// "finished", and a jam is started-but-never-arrived.
//
// WHY THIS COUNTS EDGES IN AN INTERRUPT RATHER THAN POLLING
//
// The control task ticks every 100 ms. These modules (SW-420, 801S and
// relatives) emit short pulses as the spring makes and breaks — frequently
// shorter than a tick. Polling would sample between them and conclude the door
// never moved, which is the exact false negative this is meant to eliminate. So
// an ISR counts edges and the tick reads the count.
//
// WHERE TO MOUNT IT: ON THE DOOR, NOT ON THE CONTROLLER BOARD. A sensor bolted
// beside the relay hears the relay, on every actuation, whether or not the door
// moved — which is precisely the signal it exists to distinguish from movement.
// There is a blanking window after each pulse for the vibration that reaches it
// through the structure, but blanking cannot rescue a sensor sitting on top of
// the thing making the noise.

#pragma once

#include <Arduino.h>

#include "config.h"

namespace Vibration {

// pin < 0 disables it and every other call becomes a no-op. That is the
// default: this firmware must not attach an interrupt to an arbitrary GPIO on
// the assumption something harmless is wired to it.
void begin(int pin, bool activeLow);

// Change the wiring at runtime, like the buzzer and the limit switches. The
// old interrupt is detached first — leaving one attached to a pin that is
// about to be repurposed is how a relay pulse starts counting as movement.
bool configure(int pin, bool activeLow);

const char *pinProblem(int pin);
bool enabled();
int pin();
bool activeLow();

// Total edges seen since boot. Diagnostics only — useful for aiming the
// module's sensitivity screw with `s` and a hand on the door.
uint32_t pulses();

// Pulses seen since the current travel's blanking window closed.
uint32_t pulsesThisTravel();

// The door has just been commanded to move. Starts the blanking window and
// resets the count. Called from the control task.
void travelStarted(uint32_t nowMs);

// Advances the blanking window. Cheap; call every tick.
void tick(uint32_t nowMs);

// Did the door demonstrably move during the travel just ended? False when no
// sensor is fitted, so callers must check enabled() before treating a false as
// evidence of anything.
bool movedThisTravel();

}  // namespace Vibration
