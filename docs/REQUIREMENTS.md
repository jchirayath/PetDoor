# Requirements and architecture for the firmware rebuild

Written after a full day of bench measurement on real hardware (2026-10-04).
It assumes the repository but **not** the session that produced it: everything
needed to rebuild the firmware is stated here.

The existing `petdoor/` firmware works. This is not a rewrite for its own sake
— it is a rebuild around things we did not know when it was written, chiefly
that **the door is driven by two controllers, not one**, and that the sensors
added during that session change what the firmware can know.

---

## 1. The system, as actually built

```
  BLE beacon  ──rssi──▶  ESP32  ──2 relays──▶  vendor door controller  ──▶  motor
                          │                          │
                          │                          └── has its OWN modes,
                          │                              buttons and timers
                          └── reads: 2 reed switches, 1 vibration sensor
                              drives: 1 status LED, 1 buzzer
```

The ESP32 **does not drive the motor**. It closes a relay across a button on a
commercial coop-door controller, which owns the motor, its travel limits and
its own automatic behaviour. Every requirement below follows from that.

### Measured hardware (door lying FLAT — see §6)

| | |
|---|---|
| Board | ESP32 2-relay, relays GPIO **16**/**17**, LED **23**, button **0** |
| Relay polarity | **ACTIVE HIGH** |
| Reed, open end | GPIO **32**, normally-open to GND, `INPUT_PULLUP`, LOW = at end |
| Reed, closed end | GPIO **25**, same |
| Buzzer | GPIO **27**, passive |
| Vibration | GPIO **33**, ~2,000 edges/s while moving, 0 at rest |
| Pulse width | **1000 ms works. 500 ms is swallowed** |
| Travel, open | 12,180 / 12,196 ms (16 ms spread) |
| Travel, close | 12,704 ms |
| Motion onset | ~1,300 ms after the pulse |

---

## 2. Failure modes found on real hardware

Each of these actually happened. They are the reason for the requirements.

**F1 — The vendor controller sleeps.** The first relay pulse after a long idle
is *always* swallowed; the second works. Confirmed 4/4 after 180 s idle, while
six consecutive presses seconds apart all worked. A coop door is idle for hours
between uses, so **nearly every real actuation is a cold one**.

**F2 — The vendor controller moves the door by itself.** It has modes of its
own. Before they were disabled it drove the door with nothing commanding it —
most visibly leaving the open limit ~15 s after arriving, twice. A 90 s quiet
period after disabling them recorded zero movement. The firmware must assume
**it is not the only thing that moves this door**.

**F3 — A repeat press mid-travel reads as STOP.** A second press while the door
is moving parked it halfway. F1 demands repeat presses; F3 limits when.

**F4 — "Command failed" and "already there" are indistinguishable** without
position feedback. Both produce no movement. Hours were lost to this.

**F5 — Vibration cannot tell direction or position.** It answers "did something
move", nothing more. Two confident position reconstructions built on it were
wrong.

**F6 — Reed magnets have a narrow capture range.** One made as the door passed
and released as it settled, reading as a working sensor to anything sampling
once a second. Retention matters as much as alignment — one magnet came off
entirely during cycling.

**F7 — The door's resting position varies slightly** between a powered stop and
a hand-pushed one, which is what exposed F6.

---

## 3. Requirements

Numbered for reference. **MUST** is load-bearing; **SHOULD** is wanted.

### Actuation

- **R1** The firmware MUST send a wake press before a cold actuation, and MUST
  NOT send one when the controller is known to be awake. Deriving the press
  count from time-since-last-actuation satisfies both F1 and F3; a fixed
  "always press twice" does not.
- **R2** A repeat press MUST NOT be sent while a travel is believed in flight
  (F3).
- **R3** Relay pulses MUST remain interlocked: opposite relay released, a
  direction gap observed, never both energised. *(Carried over unchanged.)*
- **R4** Default pulse width MUST be at least 1000 ms (F: 500 ms is swallowed).

### Position and verification

- **R5** With reeds fitted, an actuation MUST be *verified*: arrival at the
  expected end within the travel timeout. Non-arrival MUST be logged
  (`LOG_STALLED` already exists) and MUST NOT be reported as success.
- **R6** The firmware MUST detect and log door movement it did not command
  (F2) — a reed reporting an end while no travel is in flight. This is new.
- **R7** Reed readings MUST correct the firmware's belief about position, and
  MUST NOT ever command the motor. *(Carried over.)*
- **R8** Both reeds made at once is physically impossible and MUST be treated
  as a wiring fault, not a position. *(Carried over.)*
- **R9** Vibration MUST be used only for "did it start moving", never for
  direction or position (F5).

### Safety

- **R10** A door that stops partway MUST fail **open**, not retry closing. The
  one outcome this project exists to prevent is crushing an animal, and a door
  stalled mid-close is exactly when something may be under it.
- **R11** After a failed close, a retry MUST be delayed by minutes, not
  seconds, and MUST be attempt-limited. After the limit, stay open and say so.
- **R12** A swallowed press (nothing moved) MAY be retried immediately — it is
  distinguishable from a stall, and nothing is trapped.
- **R13** Every existing safety invariant in `CLAUDE.md` carries over unchanged.

### Annunciation

- **R14** Door movement MUST be announced audibly, distinguishing **who asked**:
  beacon, console, or remote. Three sources, not the current two.
- **R15** Tunes MUST differ in **rhythm**, not only pitch. An active buzzer has
  one factory pitch, so pitch-only distinctions collapse to one sound on half
  the supported hardware. *(Existing constraint, must be preserved.)*
- **R16** The status LED SHOULD show measured position where available, not
  only commanded state.

### Calibration

- **R17** Travel time MUST be measurable in service, per direction, and SHOULD
  be learned rather than compiled in — a door is slower in January, and
  mounting it upright changes both directions unequally (§6).
- **R18** Any calibration procedure MUST begin with a **quiet period with no
  commands**, confirmed silent, before any measurement is trusted (F2).

---

## 4. Architecture

Module boundaries that held up well and should survive:

| Module | Responsibility | Verdict |
|---|---|---|
| `ble_scanner` | Scan, match, discovery table | Keep |
| `proximity` | Filters, hysteresis | Keep |
| `door` | Relay pulses, interlock, lockout | Keep, extend |
| `position` | Reeds, debounce, belief correction | Keep |
| `vibration` | "Did it start" via ISR edge count | Keep |
| `chime`, `console`, `eventlog`, `schedule`, `maintenance` | | Keep |

**What changes is the actuation path**, which is currently open-loop with
sensors bolted on beside it. It should become a small state machine that owns
one actuation attempt end to end:

```
IDLE ──command──▶ WAKE? ──▶ PULSE ──▶ AWAIT_START ──▶ AWAIT_ARRIVAL ──▶ VERIFIED
                                          │                  │
                                          │(no motion)       │(timeout)
                                          ▼                  ▼
                                       RETRY(R12)         STALLED(R10)
```

This is the piece that makes R1, R2, R5, R10 and R11 expressible at all. Today
they are spread across the control task, the chime logic and the position
module, which is why the door could be commanded mid-travel.

**Ownership rules that must survive:** door state is owned by one task; the BLE
callback stays cheap and never blocks; nothing actuates from `loop()` or from a
callback.

---

## 5. Carry over, do not rewrite

- The **duplicate-filter fix** in `ble_scanner.cpp` and its comment. The whole
  project exists because of it.
- Every `static_assert` on config relationships.
- The chime rhythm-not-pitch design (R15).
- `LOG_STALLED`, `LOG_NO_MOVEMENT` — already correct.
- Boot grace, actuation lockout, fail-open-on-stale-signal.

## 6. Known unknowns

- **All travel times above are with the door FLAT.** Upright, gravity assists
  closing and opposes opening, and the two directions will diverge. Re-measure
  before trusting any threshold.
- **Whether the vendor controller stops on a timer or on its own limits** is
  untested. It matters: a timer-driven controller upright would change how
  *far* the door travels while the *duration* stayed constant — the one case
  where travel-time calibration silently misleads.
- **Which vendor mode was moving the door** (F2) is unidentified. It is
  currently disabled. If it is a light sensor, behaviour will change with the
  seasons.
