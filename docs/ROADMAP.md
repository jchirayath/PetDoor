# Roadmap

What is built, what is next, and — more usefully — **why the next things are
next**. Each item says what it buys and what it costs, so it can be argued with
rather than just agreed to.

Nothing here is a commitment to a date.

---

## The one thing that shapes everything else

**The door is open loop.** It knows what it *commanded*, not what happened.
Almost every caveat in this project traces back to that single fact:

- a door that jammed halfway still reports as open;
- the "arrived" chime is a timer expiring, not an arrival;
- a button press the controller swallowed looks exactly like one it obeyed,
  which is why `RELAY_PULSE_COUNT` is a **blind** retry that can stop a moving
  door;
- `DOOR_TRAVEL_MS` is a number you measure with a stopwatch, because nothing can
  work it out.

Closing that loop is therefore the highest-value work available, and the next
two items both do it from different directions.

---

## Next

### 1. Fit the reed switches — *firmware done, hardware pending*

Two normally-open switches, one at each end of travel. **The firmware already
supports them**; both pins default to `-1` and it is switched on with one
command once they are wired — `sensors 32 33`, from the console or the browser,
no reflash.

| Buys | Costs |
|---|---|
| Reported state becomes *measured* state | ~$2 and two wires |
| The arrival chime becomes an actual arrival | Mounting a magnet on a moving door |
| A travel that does not complete is logged as `STALLED` and sounded | |
| A stale belief self-corrects, so the next request actuates | |

They deliberately **cannot** drive the motor — see
[ARCHITECTURE.md](ARCHITECTURE.md#position-sensors-and-why-they-do-not-close-the-loop).

### 2. Vibration sensing — *not started*

A vibration switch (SW-420, ~$1) answers a **different and faster** question
than a limit switch: not "did it arrive" but "did it *start*".

That matters because it arrives in about a second, where a limit switch cannot
report until the full travel time has elapsed. For the swallowed-press problem
it is the better signal, and it makes retry *safe*: today's `presses 2` is blind
and can stop a moving door, whereas a vibration-gated retry only fires when the
door demonstrably did not move.

The two compose — vibration says "started", the reed switch says "finished", and
between them a jam is detectable as started-but-never-arrived.

**Open questions before building it:** where it must be mounted to hear the
motor (the ESP32 is on the controller housing, which may be coupled enough), and
how much false triggering comes from wind and from the relay's own click. Both
want testing on the bench before the logic is written.

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
| **More than 3 blind presses** | Already pushing it at 2. The fix is a sensor, not more guessing. |

---

## Done, for context

Roughly in order, because the sequence explains the shape of the thing.

| | |
|---|---|
| Dual-rate filtering | Open on the fast filter, close on the slow one — so a departure is never noticed before an arrival |
| NimBLE build | 89% of flash down to 65%, and free heap 66 KB up to 138 KB |
| Event log + signed upload | The door keeps its own history; a server is optional |
| Remote configuration | Every tunable, over the network, on a door you cannot reach |
| Lock / unlock | Stop the collar opening it, without taking the collar off |
| Web control + settings form | Drive and tune it from a browser, behind your own authentication |
| Email notifications | When somebody opens, locks, unlocks or reboots it |
| OTA firmware | Proven in the field: 1.35 MB over WiFi, self-confirming, rolls itself back |
| Annunciator | A buzzer that says moving / arrived / refused, and a distinct beep per command |
| Position sensors | Supported, inert until fitted |
| Firmware history | Every build a door has run, with the commit and how it arrived |
