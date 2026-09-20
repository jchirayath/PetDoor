# How it works

Four stages, each fixing a different failure mode. The detail lives in
[ARCHITECTURE.md](https://github.com/jchirayath/PetDoor/blob/main/docs/ARCHITECTURE.md);
this is the shape of it.

```
  beacon  ──►  listen  ──►  filter  ──►  decide  ──►  act  ──►  door controller
                                                                 (owns the motor)
```

## 1. Listen

The ESP32 scans continuously and reports **every** advertisement, not just the
first one from each device. That sounds obvious; it is the bug this project
exists to fix.

The predecessor sketch scanned forever but got exactly **one reading per device,
ever**, because the BLE library's duplicate filter is on by default. Proximity
never updated. The symptom was not a crash — just a door that stopped
responding to where the beacon was.

With duplicates enabled, a typical beacon yields several readings a second.

## 2. Filter

Raw Bluetooth signal strength is noisy — it swings several dB while nothing
moves, because radio reflects off walls, water and animals.

- **Median filter** — kills isolated spikes and deep multipath fades, which are
  the dominant noise. A mean would let one bad reading drag the average; a
  median discards it entirely.
- **Exponential smoothing** — removes what is left.

This runs **twice, at two speeds**. Opening late can shut an animal out; closing
early can shut a door on one. A single filter has to compromise between "reacts
now" and "ignores dropouts". Two filters do not:

| | feeds | tuned for |
|---|---|---|
| fast | the open decision | reacting immediately |
| slow | the close decision | ignoring dropouts |

## 3. Decide

**Two thresholds, not one.** A single threshold makes the door flap whenever the
signal sits near it. Two thresholds with a dead band between them mean the door
opens at one distance and closes at a further one, and does nothing in between.

Then **dwell timers** in both directions: the signal has to stay strong for a
moment before opening, and stay weak for longer before closing. The asymmetry is
deliberate — opening is the safe direction, so it is quick; closing is the
dangerous one, so it is patient.

## 4. Act

A **momentary relay pulse** — the same as someone pressing the door's button.
The door's own controller then runs the motor to its own limit switches and
stops.

That division is the core safety idea: **the ESP32 decides *when*, the door
hardware decides *how far*.** Nothing in this firmware can drive a door past its
stops, because it never drives the motor at all.

---

## What it deliberately does not do

- **It never closes on a stale signal it has never heard.** The door is not
  operated at all until the beacon has been heard once since boot, so a flat
  battery can never be mistaken for "the animal has gone away".
- **It never closes during the boot grace window.** A power blip must not slam
  the door on an animal standing in it.
- **It never rate-limits opening.** The actuation lockout applies to closing
  only, because delaying an open is the one direction that can strand an animal
  outside a door it just watched close.
- **It never energises both relays at once.** They are interlocked, with a
  settling gap, because both at once is a short across the motor's direction
  contacts.

Full list, and the reasoning:
[ARCHITECTURE.md](https://github.com/jchirayath/PetDoor/blob/main/docs/ARCHITECTURE.md#invariants).
