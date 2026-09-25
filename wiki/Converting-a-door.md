# Converting a door

The recommended approach is to **tap the buttons** your door controller already
has. Two relay contacts wired across the existing UP and DOWN buttons, so
closing a relay looks exactly like someone pressing that button.

```
                    Your door's controller PCB
                   ┌──────────────────────────┐
   ESP32 GPIO16 ───┤ relay 1 ├── across the UP button contacts
   ESP32 GPIO17 ───┤ relay 2 ├── across the DOWN button contacts
                   │   motor, limits, power   │  (all untouched)
                   └──────────────────────────┘
```

## Why this way

- The controller keeps the motor, the travel limits and any current limiting.
  **PetDoor cannot drive the door past its stops**, because it never drives the
  motor.
- The original timer keeps working as a fallback. If the ESP32 loses power, the
  door still runs on its old schedule.
- Nothing high-current passes through your wiring — you are switching a
  logic-level button input.
- **Completely reversible.** Unplug two wires and the door is stock.

## The short version

1. Power everything off, open the controller housing.
2. Find the button contacts — two solder pads each, shorted while pressed.
   Confirm with a multimeter on continuity.
3. Solder two wires to each button's pads, or tap the connector if there is one.
4. Wire each pair to a relay's **COM** and **NO** terminals. Relay closed =
   button pressed.
5. **Bench-test before reassembling**, with `o` and `x` on the serial console.

Use **COM and NO**, never NC. NC inverts everything: the controller sees its
button held down permanently and the pulse becomes a momentary *release*, which
is a motor that runs continuously or starts the instant power is applied.

### Wiring

![ESP32 with two relays wired to the door controller](https://raw.githubusercontent.com/jchirayath/PetDoor/main/docs/assets/wiring-esp32-2relay.svg)

Each relay is wired **in parallel with an existing button**, so the door cannot
tell the difference between the ESP32 and a finger. The two are interlocked in
firmware — energising both at once would short the motor's direction contacts.

![The relays wired to the door's switch](https://raw.githubusercontent.com/jchirayath/PetDoor/main/images/WiringRelaystoSwitch.jpeg)

## Measure your door's travel time

Time a full open and close with a stopwatch. The reference door takes about
**15 seconds** each way.

This matters because the actuation lockout must be **at least** the travel time.
If it is shorter, the firmware can issue a reversing command while the door is
still moving, and most controllers read a second command mid-travel as *stop* —
so the door creeps partway and halts. That symptom looks like a relay fault and
is not one.

## Other patterns

- **Tap a spare remote** — easiest of all if your door came with one. Solder
  across the remote's button and let the door's own receiver do the work.
- **Drive the motor directly** — only if the controller is dead or absent, and
  only with a proper motor driver with limits. You lose the safety the
  controller was providing.

Full step-by-step with photographs of a real conversion:
**[COOP-CONVERSION.md](https://github.com/jchirayath/PetDoor/blob/main/docs/COOP-CONVERSION.md)**

Wiring detail, relay selection and the bench test:
**[WIRING.md](https://github.com/jchirayath/PetDoor/blob/main/docs/WIRING.md)**

> ⚠️ Read
> **[SAFETY.md](https://github.com/jchirayath/PetDoor/blob/main/docs/SAFETY.md)**
> before connecting anything to a real door. The most important decision in the
> build is the door mechanism, not the code.
