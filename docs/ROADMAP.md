# Roadmap

What is built, what is next, and — more usefully — **why the next things are
next**. Each item says what it buys and what it costs, so it can be argued with
rather than just agreed to.

Nothing here is a commitment to a date.

---

## The one thing that shapes everything else

**The door is driven by two controllers, not one.** The ESP32 closes a relay
across a button on a commercial door controller, and that controller owns the
motor, its travel limits, its own automatic modes and a sleep of its own.

That single fact is where almost every caveat in this project now comes from,
and it replaces the older framing on this page — "the door is open loop" —
because the loop is closeable and the second controller is not going anywhere:

- the first press after a long idle is **always swallowed**, so nearly every
  real actuation is a cold one;
- a repeat press mid-travel reads as **STOP**, which is the opposite failure;
- the controller has been watched **moving the door by itself**;
- "the press failed" and "the door was already there" look identical without
  position feedback.

The firmware now handles each of those explicitly — a wake press that checks
whether it worked, verification against the limit switches, a stalled close
that fails open — see
[ARCHITECTURE.md](ARCHITECTURE.md#the-actuation-path) and
[REQUIREMENTS.md](REQUIREMENTS.md).

**What remains open loop is the thing that matters most and is not solvable in
software: nothing can tell what is under the door.** A door that closes onto
something soft arrives, and reports success. The reversal only triggers on a
door that failed to *arrive*. That is why
[SAFETY.md](SAFETY.md) still opens by saying the mechanism has to fail safe on
its own.

---

## Next

> **Items 1 and 2 are now fitted and working**, on the bench door, measured.
> They are kept here rather than moved to *Done* because the thing they are
> waiting on is a season of weather: reed magnets have a narrow capture range
> and one came off during cycling, and nobody yet knows how often wind and
> passing traffic trip a vibration sensor. The firmware's exposure to both is
> bounded — a false "it started" costs a stall report, not a movement — but the
> numbers below are from a door lying flat indoors.

### 1. Fit the reed switches — **done and proven on the bench door**

Two normally-open switches, one at each end of travel. **The firmware already
supports them**; both pins default to `-1` and it is switched on with one
command once they are wired — `sensors 32 25`, from the console or the browser,
no reflash.

Everything built on them is now in place, which makes this the highest-value
$2 left in the project:

| Buys | Costs |
|---|---|
| Reported state becomes *measured* state | ~$2 and two wires |
| The arrival chime becomes an actual arrival | Mounting a magnet on a moving door |
| A travel that does not complete is `STALLED`, logged and sounded | |
| **A stalled close is reversed, not retried** | |
| Movement nothing commanded is `UNCOMMANDED` and logged | |
| Travel time becomes measurable in service — `calibrate` times both directions | |
| The status LED shows where the door is, not what it was told | |

They still deliberately **cannot** drive the motor — see
[ARCHITECTURE.md](ARCHITECTURE.md#position-sensors-and-why-they-do-not-close-the-loop).
The one reversal in the firmware is it undoing a command of its own.

### 2. Vibration sensing — **done and proven on the bench door**

A vibration switch (SW-420, ~$1) answers a **different and faster** question
than a limit switch: not "did it arrive" but "did it *start*". **The firmware
supports it**; the pin defaults to `-1` and it is switched on with one command
once wired — `vibration 33`.

A travel that produces no vibration at all is `NO_MOVE`, logged and sounded,
distinct from `STALLED`, which means the door started and never arrived. The
relay's own click is blanked for `VIBRATION_BLANK_MS` after each pulse.

**Retry is no longer open.** It was the last item here, and the answer turned
out to be two answers:

- *the press was swallowed* — the door demonstrably did not move, so nothing is
  trapped and it is retried **immediately**, bounded by `SWALLOW_RETRY_LIMIT`.
  This is the safe retry the old `presses 2` was trying and failing to be.
- *the door started and stopped* — something may be under it, so for a close it
  is **reversed** rather than retried, and the next attempt is minutes away and
  attempt-limited.

The blind `presses 2` is still there for a controller nothing else can handle,
and the documentation now says to leave it at 1: it *stacks* with the wake
press, so at 2 a cold actuation sends four presses.

The remaining open question is the one a season of watching answers: whether
wind and passing traffic trigger the sensor often enough to matter. The
firmware's exposure to that is bounded — a false "it started" costs a stall
report, not a movement.

### 3. A dedicated sender address — *small, cosmetic*

Notifications currently send from an address shared with other services on the
host. A `petdoor@` address on a domain the relay already authorises would let
the mail be filtered on. Needs nothing clever; it is SPF/DKIM housekeeping.

---

## Wanted, but blocked on something outside the code

### Camera footage

The dashboard already lays out an outside and an inside frame, and every event
already carries a precise timestamp — which is the hard half of the problem.

What is missing is a *source*. Blink and Wyze both keep clips in their own
clouds with no public API, so a page cannot pull footage directly. The honest
path is an RTSP bridge or any NVR that records to files; the door's log then
tells you exactly which second to scrub to.

### The GitHub wiki

Twelve pages are written and committed under `wiki/`. GitHub does not create a
wiki's git repository until a first page is saved **through the web UI**, and
there is no API for it — so `./wiki/publish.sh` cannot run from a cold start.
One click, once, then it is automatic forever.

---

## Deliberately not planned

Each of these has been considered and set aside. They are here so the same
argument does not have to be had twice.

| | Why not |
|---|---|
| **Letting a sensor command the motor** | A stuck contact would drive a door an animal is standing in. Sensors correct belief; the proximity pipeline decides. |
| **Changing credentials remotely** | WiFi, endpoint, key and OTA password. A mistake takes the door off the network permanently, with no way back but a ladder. |
| **MQTT / Home Assistant integration** | Plausible and genuinely wanted by some. Not while it would mean the door *listening*; the outbound-only channel is what makes remote management safe behind NAT. Would be additive, not a replacement. |
| **Treating this as a lock** | BLE advertisements are unauthenticated plaintext. The door opens for anything broadcasting your beacon's address. No amount of firmware fixes that — see [SAFETY.md](SAFETY.md). |
| **More than 3 blind presses** | Already pushing it at 2, and now superseded: the wake press is a press that checks whether it worked, which is what blind presses were approximating. Leave `presses` at 1. |
| **Letting the firmware retry a stalled close** | It reverses instead. A door stopped partway shut is exactly when something may be under it, and pressing again drives onto whatever that is. |

---

## Done, for context

Roughly in order, because the sequence explains the shape of the thing.

| | |
|---|---|
| Dual-rate filtering | Open on the fast filter, close on the slow one — so a departure is never noticed before an arrival |
| NimBLE build | Now the default: 92% of flash down to 69%, and the heap low-water mark from 6.9 KB to 80 KB — Bluedroid had been panicking doors |
| Event log + signed upload | The door keeps its own history; a server is optional |
| Remote configuration | Every tunable, over the network, on a door you cannot reach |
| Lock / unlock | Stop the collar opening it, without taking the collar off |
| Web control + settings form | Drive and tune it from a browser, behind your own authentication |
| Email notifications | When somebody opens, locks, unlocks or reboots it |
| OTA firmware | Proven in the field: 1.35 MB over WiFi, self-confirming, rolls itself back |
| Annunciator | A buzzer that says moving / arrived / refused, and a distinct beep per command |
| Position sensors | Supported, inert until fitted |
| Firmware history | Every build a door has run, with the commit and how it arrived |
| Verified actuation | A travel has five possible endings instead of one "done", and the log says which — including that nothing moved |
| The wake press | A sleeping vendor controller handled without the blind double-press that could stop a moving door |
| Fail open on a stalled close | The firmware reverses a close that did not complete, waits minutes, limits the attempts, then stays open and says so |
| Uncommanded-movement detection | The door notices when something other than this firmware moved it |
| Travel-time calibration | `calibrate` times both directions in service, after a quiet period that catches a vendor mode still being enabled |
| Three annunciator sources | Beacon, console and network each sound different — by beep count, which is the distinction a one-pitch buzzer survives |
