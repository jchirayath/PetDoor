# Safety

**Read this before you connect PetDoor to anything that moves.**

This firmware pulses two relays. Those relays drive a motor. That motor moves a
door that a living animal walks through. Everything below follows from that.

---

## The one-paragraph version

PetDoor is **open-loop**. There are no limit switches, no current sensing, no
obstruction detection. The firmware knows what it *commanded*; it has no idea
where the door actually is or what is underneath it. Every protection against
crushing an animal has to come from your door hardware, not from this code. If
your door mechanism cannot stop safely on its own when something is in the way,
do not automate it.

---

## What the firmware does protect against

These are real and they are tested behaviours, not aspirations. Each one exists
because the failure it prevents is plausible in a coop.

| Protection | Setting | What it prevents |
|---|---|---|
| Never closes for the first 30 s after boot | `BOOT_GRACE_MS` | A power blip at dusk slamming the door on a bird standing in it |
| Never actuates until the beacon has been heard once since boot | — | A flat beacon battery reading as "absent" and shutting the door |
| A stale signal can close the door but never open it | `SAMPLE_MAX_AGE_MS` | A ghost reading opening the door to a predator |
| 15 s of confirmed absence before closing | `EXIT_CONFIRM_MS` | A brief signal dropout closing the door while the animal is still there |
| Two relays are never energised at once | `DIRECTION_CHANGE_GAP_MS` | A dead short across the motor's direction contacts |
| 5 s minimum between any two actuations | `MIN_ACTUATION_INTERVAL_MS` | Motor thrash if the signal sits on the threshold |
| Relay pins are driven to their idle level *before* `pinMode()` makes them outputs | — | A microsecond glitch on boot registering as a real movement |

## What it does not protect against

**None of these have a software fix in this project.** They are your wiring and
your mechanism.

- **An animal in the doorway when the door closes.** Nothing in this firmware
  can detect that. The 15-second dwell and the boot grace window reduce the
  odds; they do not eliminate them.
- **Power loss part-way through travel.** The door stops wherever it is. On
  restart the firmware reports `DOOR_UNKNOWN` and will not close for 30 s, but
  it cannot recover the door's real position.
- **A jammed or iced door.** The firmware pulses the relay and assumes success.
  It has no way to notice the motor stalled.
- **The beacon being left inside the coop.** The door will simply stay open.
  This is the safe failure, but it is a failure.
- **A beacon carried by a predator-sized animal.** Anything holding the beacon
  opens the door. Proximity is the whole authentication model.

---

## Choose a door mechanism that fails safe

The single most important decision in this build is not in the code.

**The single feature to look for when buying: anti-pinch.** Also sold as
obstruction detection, anti-crush or rebound. The door senses resistance and
stops or reverses rather than continuing to pull. This firmware is open-loop
and cannot detect an animal in the doorway — so if anything is going to, it has
to be the door. Test it with a rolled towel before trusting it, and check
whether yours reverses or merely beeps.

**Good choices:**

- A door with a **slip clutch or friction drive** that stalls harmlessly against
  an obstruction.
- A **counterweighted** door light enough that it cannot injure a bird.
- A motor controller with its **own current limit and limit switches**, which
  PetDoor merely triggers via momentary inputs. This is the arrangement the
  firmware is designed around.

**Bad choices:**

- A **guillotine door with a heavy weight** and a direct drive. This is the
  classic coop-door design and it is the one that kills chickens.
- Anything where a stalled motor keeps pulling at full torque.
- Anything driven by mains voltage that you are wiring yourself without
  experience.

## Mains voltage

If your door motor runs on mains voltage, the relay module switching it is
mains-adjacent hardware.

- Use a relay module with proper creepage/clearance and an opto-isolated input.
  Cheap blue relay boards are commonly rated 250 VAC 10 A, and commonly built to
  a standard that does not justify that label.
- Keep the mains side physically separated from the ESP32 side. Do not run
  mains through the same connector block as your 5 V.
- Enclose it. A coop is a damp, dusty, animal-occupied environment.
- If you are not confident doing this, use a low-voltage DC motor instead. A
  12 V linear actuator is a perfectly good coop door drive and removes this
  entire category of risk.

---

## Commissioning procedure

Do these in order. Do not skip ahead because the previous step looked fine.

1. **Flash with no motor connected and no relay module connected.** Confirm the
   banner prints and the ESP32 boots. See [WIRING.md](WIRING.md).
2. **Connect the relay module, still with no motor.** Use the `o` and `x`
   serial commands. You should hear exactly one click per command, from the
   relay you expect. If `o` clicks the CLOSE relay, your wiring is swapped. If a
   relay sits energised continuously, `RELAY_ACTIVE_LOW` is wrong — fix it
   before going further.
3. **Connect the motor with the door removed or disengaged**, if your mechanism
   allows it. Confirm direction.
4. **Reconnect the door and test with the coop empty.** Run a full open and
   close cycle with `o` and `x`. Watch for the door over-travelling, binding, or
   the motor continuing to pull after the door has reached its stop.
5. **Test the automatic cycle with the coop empty.** Walk the beacon in and out
   and watch the serial log.
6. **Only then run it with animals present**, and watch it for several cycles
   before leaving it unattended.

## Before leaving it unattended

- Set `ALLOW_MANUAL_SERIAL_CONTROL` to `0`. The `o` and `x` commands bypass the
  proximity logic, the actuation lockout *and* the boot grace window. They are a
  bench tool, not a deployment feature.
- Confirm the status LED behaviour matches what you expect (see the README).
- Have a way to open the door by hand without power.
- Keep a physical latch or a second mechanism for predator security if the door
  is the only thing between your birds and the night. A firmware that fails open
  is the safe outcome for the animals and the unsafe outcome for predators.

## Reporting a safety problem

If you find a way this firmware can injure an animal that is not listed above,
please open an issue and label it `safety`. That is more valuable than a feature
request.
