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

---

## 7. Implementation status

Written after building it. Every **MUST** above is implemented; the table says
where, so a reader can check the claim rather than take it.

The one structural change is a new module, `actuator.*`, which owns one
actuation attempt from the request to its outcome. `door.*` was split to suit
it: it still owns the pins, the interlock and the gating, and no longer owns
sequencing. See
[ARCHITECTURE.md](ARCHITECTURE.md#the-actuation-path).

| | Where | Note |
|---|---|---|
| **R1** wake press, derived from idle time | `Actuator::wakeNeeded()`, `WAKE_IDLE_MS` | And it does not press twice blindly: after the wake press it watches for `WAKE_PROBE_MS` and skips the second press if the door started moving |
| **R2** no repeat press mid-travel | `Actuator::request()` returns `ACT_BUSY` | The *opposite* direction still supersedes — a reversal must never be the request that is refused |
| **R3** interlock unchanged | `DoorController::pulse()` | Carried over verbatim |
| **R4** pulse ≥ 1000 ms | `RELAY_PULSE_MS` | Default changed from 200 |
| **R5** actuation verified, non-arrival not reported as success | `OUT_ARRIVED` vs `OUT_ASSUMED` vs `OUT_STALLED` | `LOGF_VERIFIED` on the log entry is the distinction. Without it, an entry records an intention |
| **R6** detect and log uncommanded movement | `updatePosition()`, `LOG_UNCOMMANDED` | Sounded and uploaded immediately, not held to the next heartbeat |
| **R7** reeds correct belief, never command | `observePosition()` | Carried over. The one addition is that reality saying "closed" clears a gave-up state — which relaxes a refusal and still cannot cause a movement |
| **R8** both reeds made = wiring fault | `Position::fault()` | Carried over, and `madeAt()` honours it too, so a shorted wire cannot produce a cheerful arrival |
| **R9** vibration only answers "did it start" | `movedEvidence()` | Confined to one boolean. Two thresholds, because the cost of being wrong points opposite ways in the two places it is asked |
| **R10** a stalled close fails **open** | `resolve(OUT_STALLED)` → forced `SRC_FAILSAFE` open | Armed in `resolve()` and sent by the next tick, so the stall is announced before the reversal |
| **R11** retry delayed by minutes, attempt-limited | `CLOSE_RETRY_DELAY_MS`, `CLOSE_RETRY_LIMIT` | After the limit: `gaveUp()`, its own LED pattern, its own chime, `gaveup=1` in the uploaded status |
| **R12** a swallowed press may retry at once | `SWALLOW_RETRY_LIMIT` | Bounded, and followed by `FAILED_ATTEMPT_COOLDOWN_MS` — see below |
| **R13** existing invariants carry over | `CLAUDE.md`, `static_assert`s | Three added; see ARCHITECTURE.md invariants 14–17 |
| **R14** announce who asked: beacon / console / remote | `Actuator::moveTune()`, `SRC_REMOTE` | Three sources, three tunes, counted beeps |
| **R15** rhythm, not pitch | `chime.cpp` | Which is *why* it is a count: 2 beeps the collar, 3 the console, 4 the network |
| **R16** LED shows measured position | `updateLed()` | With switches fitted, "off" now means genuinely between the two ends |
| **R17** travel time measurable in service, per direction, learned | `measuredMs()`, `calibrate` | Every verified travel is timed and reported in `s`; `calibrate` adopts it |
| **R18** calibration begins with a confirmed quiet period | `CAL_QUIET_MS` | Aborts if a switch changes during it, and says that a vendor mode is still enabled |

### Found while building, not in the requirements above

**A failed attempt commits nothing, so the door loops.** The condition that
asked for the travel is still true, so the control task asks again on its very
next tick — ten presses a second, forever. R12 permits an immediate retry and
says nothing about what happens after the retries are spent.
`FAILED_ATTEMPT_COOLDOWN_MS` is the answer, per direction so that a failed
close never delays the open after it.

**A reversal would have been refused.** A travel in flight has not committed,
so the door still believes it is at the end it set off *from* — and "already
there" would have refused the open that arrives mid-close, which is the one
request that must never be refused. `request()` now drops the belief to
`UNKNOWN` when a travel is superseded.

**`ACT_RETRY_WAIT` collided with the schedule's refusal code.** `LOG_REFUSED`
carried a hardcoded `detail = 5` for a scheduled lockout, and the new
`ActuationResult` values reach 5. A door waiting out a stalled close would have
been reported on the dashboard as refused by a schedule window that does not
exist. The schedule's code is now 100, deliberately outside the enum.

**A travel must be timed from the press, not from when motion was noticed.**
Motion is observed up to `MOTION_ONSET_MS` later, and a deadline measured from
there grants every travel a couple of extra seconds — enough slack to hide a
door that is getting slower, and enough to make the measured figure
incomparable with the stopwatch numbers in §1.

### Still open

- **Travel times have now been measured in service** (see §8), but on a door
  still lying FLAT. §6 stands: re-run `calibrate` once it is mounted upright,
  because gravity assists the close and opposes the open.
- **Whether the vendor controller stops on a timer or on its own limits** is
  still untested, and still matters for the reason §6 gives.
- **Retry on a vibration-confirmed non-start** is implemented
  (`SWALLOW_RETRY_LIMIT`), which §3's R12 allows. The *other* retry the roadmap
  wanted — a second press when the door demonstrably did not move — is the same
  thing, so that item is now done rather than deferred.

---

## 8. Verified on hardware (2026-10-04, same evening)

Everything below was observed on the reference door — flat, controller and
motor connected, both reeds, vibration sensor and buzzer fitted. Pins exactly
as §1: relays 16/17 active HIGH, reeds 32/25, buzzer 27, vibration 33.

### The requirements that were exercised

| | What happened |
|---|---|
| **R1** wake press | `[door] wake press sent (the controller had been idle)` on every actuation after 30 s idle. It was **swallowed every time**, exactly as F1 predicts, and the actuating press followed |
| **R5** verification | `[door] CLOSED — ARRIVED, verified by the limit switch in 11010 ms`. Five travels, all verified |
| **R12** immediate retry | `[door] nothing moved; pressing again (retry 1)` — a close press was swallowed with no wake press due, `AWAIT_START` timed out, the retry recovered it. The exact failure the vibration sensor was added to catch, caught |
| **R14** source attribution | Log row `CLOSE,1,-59,3` — detail 1 = console, flags 3 = verified + woke. A beacon-driven open logged `OPEN,0,-60,3` |
| **R17/R18** calibration | 90 s quiet period **passed** (so F2's vendor mode really is disabled), then `measured open 10203 ms, close 11229 ms — adopted and saved` |
| Invariant 5 | A post-OTA boot with the beacon switched off recorded `0 open, 0 close, 0 in boot grace` — it refuses to act before hearing the collar once, so a flat beacon battery cannot shut the door |

### Measured numbers, and why they differ from §1

| | §1 (stopwatch, limit to limit) | §8 (firmware, reed to reed) |
|---|---|---|
| open | 12,180 / 12,196 ms | **10,203 ms** |
| close | 12,704 ms | **11,229 ms** |

The firmware's figures are **shorter, and correctly so**: a reed makes before
the door reaches its physical stop, so reed-to-reed is less than limit-to-limit.
Reed-to-reed is also the number the arrival deadline wants, since the reed is
what ends the wait.

Repeatability across separate travels was good — a later close measured
11,010 ms against the calibrated 11,229 (219 ms apart), and an open measured
10,425 against 10,203 (222 ms apart). `TRAVEL_GRACE_MS` at 3,000 ms is roughly
13× that spread.

Vibration counted **58,242 edges** across two calibration travels (~2,900/s,
consistent with the ~2,000/s of §1) and **0 at rest** — so
`VIBRATION_MOVING_PULSES` at 50 has an enormous margin.

### Three bugs the hardware found that review did not

1. **The vibration pin did not survive a reboot.** The reeds and buzzer persist
   to NVS; vibration had no store at all, so it reverted to `-1` on every power
   cut and silently took `NO_MOVE` detection with it. Fixed and verified across
   a reboot.
2. **An OTA window could not be opened while the door was open.** `radioUp()`
   abandoned association on its first iteration unless the door was idle, and
   "idle" requires `CLOSED` — so a door sitting open with the beacon away, which
   is where a stall or an exhausted close retry leaves it, refused every update.
   OTA only ever appeared to work because a maintenance window forces the idle
   flag true. Fixed and verified in the failing state.
3. **A pushed image rolls back if the door is reset before it confirms.**
   Working as designed — confirmation requires a successful upload, because an
   image that boots but cannot be managed is the one not to keep — but easy to
   trip over, and it was tripped over: the banner read two builds old with
   nothing else saying why. See
   [DIAGNOSTICS.md](DIAGNOSTICS.md#confirm-the-image-before-you-reboot-the-door).

### OTA, end to end

Pushed four times over WiFi with no cable. The last two self-confirmed with no
intervention, ~61 s after boot, on a closed and therefore idle door:

```
[ota] image confirmed good (upload succeeded); rollback cancelled
```

That is the mounted-door case, and it is the answer to "can this be maintained
once the USB lead is gone".

### Still unverified

- **STALLED, NO_MOVE as an outcome, GAVE_UP and UNCOMMANDED.** All four need a
  physically obstructed or hand-pushed door; only the internal retry fired
  naturally. The fail-open reversal and the attempt limit are therefore
  **untested on hardware**, which for R10 and R11 is the gap that matters most.
- **Remote commands.** Uploads succeed, but every reply came back
  `[cmd] reply ignored: unsigned` — the server is not signing, so no queued
  command would ever apply. Server-side shared key, not firmware.
