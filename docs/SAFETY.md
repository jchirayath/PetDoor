# Safety

**Read this before you connect PetDoor to anything that moves.**

This firmware pulses two relays. Those relays drive a motor. That motor moves a
door that a living animal walks through. Everything below follows from that.

---

## The one-paragraph version

PetDoor has **no obstruction detection and no current sensing**. With limit
switches fitted it can tell whether the door arrived; it still cannot tell what
is underneath it. Every protection against crushing an animal has to come from
your door hardware, not from this code. **If your door mechanism cannot stop
safely on its own when something is in the way, do not automate it.**

Fit the limit switches anyway. They do not make the door safe, but they are
what lets it notice a travel that failed — and a door that notices can fail
*open*, which is the one useful thing software can do here.

---

## With and without limit switches

Both are supported, both ship, and the switches are off by default because most
builds have none. The difference is worth being blunt about, because it changes
what the words in the log mean.

| | No switches | Switches fitted |
|---|---|---|
| "the door is open" | it was **commanded** open | a switch **says** it is open |
| a travel that jammed | reported as success | `STALLED` — logged, sounded, and a close is **reversed** |
| a press the controller swallowed | reported as success | `NO_MOVE` — and the door does not change what it believes |
| the door moved by itself | invisible | `UNCOMMANDED` — logged and sounded, from a limit switch or from how long the movement lasted |
| a limit switch stops making | the door keeps deciding on evidence that stopped arriving | `SENSOR_FAULT` 6 after `REED_LOST_MS` at rest — sounded, logged and emailed |
| the "arrived" chime | a stopwatch expiring | an arrival |
| travel time | something you measure with a stopwatch | measured per travel, and `calibrate` adopts it |

Two switches, about $2, two wires. See
[WIRING.md](WIRING.md) and [TUNING.md](TUNING.md).

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
| **A stalled close is reversed, not retried** | `TRAVEL_GRACE_MS` | Driving a door repeatedly onto whatever stopped it. Needs a switch at the closed end |
| **Retries after a failed close are minutes apart and attempt-limited** | `CLOSE_RETRY_DELAY_MS`, `CLOSE_RETRY_LIMIT` | A door grinding away at an obstruction all evening |
| **After the limit the door stays open and says so** | `CLOSE_RETRY_LIMIT` | A door that silently gave up, which is worse than one that is visibly open |
| **A repeat press is never sent while a travel is in flight** | — | A second press mid-travel, which real controllers read as STOP and which parks the door halfway |
| **A failed travel does not change what the door believes** | — | "Already closed" refusing the next close, after a close that never happened |

## What it does not protect against

**None of these have a software fix in this project.** They are your wiring and
your mechanism.

- **An animal in the doorway when the door closes.** Nothing in this firmware
  can detect that. The 15-second dwell and the boot grace window reduce the
  odds; they do not eliminate them.
- **Power loss part-way through travel.** The door stops wherever it is. On
  restart the firmware reports `DOOR_UNKNOWN` and will not close for 30 s, but
  it cannot recover the door's real position.
- **An animal under a door that is already closing.** The reversal above
  triggers on a *stall* — the door failing to arrive. A door that closes
  successfully onto something soft arrives, and reports success. Only the
  mechanism can prevent that.
- **A jammed or iced door, with no switches fitted.** The firmware pulses the
  relay and assumes success; it has no way to notice the motor stalled. With
  switches it notices, and for a close it reverses — but what it has detected
  is a door that did not *arrive*, which is not the same as a door that hit
  something.
- **A broken switch or a lost magnet.** This reads as "not at that end", which
  is the safe direction — the door declines to believe it got somewhere it has
  not — but with a broken closed-end switch *every* close reports a stall, and
  the door ends up parked open after `CLOSE_RETRY_LIMIT` attempts. That is the
  intended failure, and it is loud: three blips on the LED, an entry in the
  log, and `gaveup=1` in the uploaded status. It is still a door that stopped
  closing because a magnet fell off. Check the magnets; see
  [WIRING.md](WIRING.md).
- **The relay wired to the wrong contacts.** Landing on `COM`/`NC` instead of
  `COM`/`NO` inverts every state: the door controller sees its button held down
  permanently, and the firmware's 200 ms "press" becomes a 200 ms *release*.
  That is a motor that runs continuously, or starts the moment power is applied.
  (The "press" is 1000 ms by default, not 200 — see
  [below](#a-press-too-short-to-register).)
  It is downstream of the ESP32, so no amount of firmware can detect or prevent
  it. Use `COM` and `NO`; leave `NC` empty. See
  [WIRING.md](WIRING.md#which-output-terminals-to-use).
- **The beacon being left inside the coop.** The door will simply stay open.
  This is the safe failure, but it is a failure.
- **A door you locked remotely.** `lock` stops the beacon opening the door, by
  design — so an animal outside cannot let itself back in. The firmware cannot
  tell which side of the door it is on, and will not guess. `door open` still
  works while locked and is the way to let it in; turnaround is one upload
  interval, typically minutes. Do not use the lock for something you may need
  to undo in seconds.
- **A beacon carried by a predator-sized animal.** Anything holding the beacon
  opens the door. Proximity is the whole authentication model.
- **Anything broadcasting your beacon's address.** BLE advertisements are
  unauthenticated, so the firmware cannot distinguish your beacon from a device
  imitating it. Reading the address takes a free phone app; rebroadcasting it
  takes a $10 board. There is no fix within BLE advertising — treat the door as
  a convenience, not a security barrier.

---

## A scheduled lockout will shut an animal out

If you set a lockout window, understand what it does: **while the window is in
force the door will not open for the collar, including for an animal that is
still outside.** That is not a flaw, it is the entire purpose — a door that let
anything wearing the collar in at 3 a.m. would not be a lockout. But it means a
straggler stays out until the window ends.

Before you rely on one:

- **Count your animals in** before the window starts, the same way you would if
  you were bolting the door by hand. The door cannot count.
- **Leave slack at both ends.** There is no daylight-saving handling: the offset
  is a fixed number of minutes. A window that starts half an hour after dusk and
  ends half an hour before you get up costs nothing and forgives a clock that is
  an hour out twice a year.
- **The window is inert until the door has a clock.** With no network, or before
  NTP answers, there is no lockout at all — the door reports this in `s` and in
  the schedule editor. Do not assume a door locked itself; check.
- **It never prevents closing**, and nothing in a schedule can hold a door shut
  against an animal standing in the doorway. Closing is governed by the same
  proximity logic as always.

The door sounds the refusal chime and logs a `REFUSED` entry each time a window
turns the collar away, so "it would not let her in last night" is answerable
from the log rather than from memory.

**You can hear a window refusing.** When the collar arrives at a door a window
is holding shut, the buzzer plays two long low beeps with a wide gap between
them — "not… now" — once per arrival, and distinct from the single long beep a
manual lock gives. The console says `[sched] refused: window [n] is in force`
and the event log records it.

That matters at 3 a.m. with an animal outside: it is the difference between
"the door is broken" and "the door is doing what I told it to", and it is the
only way to tell without a screen. It does not make the lockout any less of a
decision to think through — see above — it just stops you dismantling a door
that is working correctly.

## A press too short to register

`RELAY_PULSE_MS` defaults to **1000 ms**. It used to be 200.

This is not a margin. On the reference controller a **500 ms press is
swallowed** and a 1000 ms press works, every time. A relay that clicks into a
door that does not move is the most-reported symptom of this project, and for
a full day of bench work this number was the cause.

It matters for safety in a roundabout way: a door that "sometimes does not
respond" gets a second press added by hand, and a second press mid-travel reads
as STOP on real controllers — so the fix for an unreliable press used to be a
door parked halfway. The wake press below replaced that guesswork.

## The controller may not be awake

The ESP32 does not drive your motor. It closes a relay across a **button** on a
commercial controller, and on the reference hardware that controller **sleeps**:
the first press after a long idle is always swallowed, and the second works.
Confirmed 4 out of 4 after 180 s idle, while six presses seconds apart all
worked.

A coop door is idle for hours between uses, so **nearly every real actuation is
a cold one.** The firmware therefore sends a **wake press** when the controller
has been idle for `WAKE_IDLE_MS`, waits, and only then presses to actuate.

The dangerous case is the one it is designed around. If the controller turns out
to have been awake, the wake press *moves the door* — and a second press
mid-travel would stop it halfway. So after a wake press the firmware watches
for `WAKE_PROBE_MS` and **does not press again if the door started moving**.
With a vibration sensor or a switch at the starting end, that check is a
measurement. Without either, it is a guess bounded by the idle timer, which is
why `WAKE_IDLE_MS` defaults to a conservative 30 s.

Set `WAKE_IDLE_MS` to 0 if your controller does not sleep.

> **Leave `RELAY_PULSE_COUNT` at 1.** It is the old blind double-press and it
> *stacks* with the wake press: at 2, a cold actuation sends four presses, and
> on this hardware a press mid-travel is a STOP. It is kept only for a
> controller the wake logic cannot handle.

## The door may move on its own

The vendor controller has modes of its own. Before they were disabled it drove
the reference door with nothing commanding it — most visibly **leaving the open
limit about fifteen seconds after arriving, twice.**

This firmware assumes it is not the only thing that moves the door. A door that
reaches an end with no travel in flight is logged as `UNCOMMANDED`, sounded, and
uploaded immediately. With limit switches fitted that is measured. Without them,
it is inferred from how long the vibration sensor felt the door moving: a run
lasting about as long as a full travel, on a door that was sitting at a limit,
can only have gone one way.

An inferred **close** is acted on — the firmware records the door as closed.
An inferred **open** is logged but not believed, and the position is recorded as
`UNKNOWN` instead. The asymmetry is on purpose: wrongly believing the door is
open is what refuses the next open request and shuts an animal out, and no
inference is allowed to cause that. Only a switch commits an open.

**If you see that event repeatedly, your controller still has an automatic mode
enabled.** Find it and turn it off before trusting any measurement — including
travel time, which is why `calibrate` begins with a quiet period and aborts if
the door moves during it. See
[COOP-CONVERSION.md](COOP-CONVERSION.md).

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

   While you are here, put a meter across `COM` and `NO` on each channel. At
   idle it must read **open**, and closed only for the ~200 ms of the pulse. If
   it reads closed at idle you are on `NC`; move the wire before a motor is
   anywhere near this. This is the last point at which that mistake is free.
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

## Manual control does not expire, and a door left CLOSED shuts an animal out

Move this door and you take control of it. That is true whether you press Open
or Close on the portal, type `o` or `x` at the console, **or move the flap by
hand** — all three set the same state, and while it is set:

- nothing automatic moves the door, **in either direction**: not the collar,
  not the close dwell, not the schedule;
- your own commands are never refused because of it;
- it **survives a reboot and a power cut**;
- only `door auto` ends it.

**It records that a person acted, not where the door should be.** An earlier
version stored a position to hold, and that was worse than useless: a door
moved by hand or restarted came back asserting one thing while sitting at
another, and the assertion won. Where the door is now comes from its limit
switches, so the two cannot disagree.

**The dangerous case is leaving it CLOSED.**

> **An animal outside cannot get in, and nothing will ever correct that.**

Not a stall, which fails open. Not a schedule, which gives up at the boundary.
A decision that stays made, and from outside the door looks exactly like one
that is simply shut. So before you use it, know how you are reminded:

| | |
|---|---|
| the dashboard | a pill naming the position and saying automation is off, with **no countdown**, because there is none |
| email | on arrival, no cooldown, and **repeated** for as long as it lasts |
| the LED | a distinct pattern in both positions |
| the boot banner | stated on every restart |
| the log | a `MANUAL` event, so the moment it began is timestamped |

**A hand on the flap counts as taking control, and that rule is specific to
this door.** It is a motorised vertical panel: wind cannot blow it shut and an
animal cannot push it, so an uncommanded travel is a person. On a door where
that is not true — a hinged flap, anything a gust can move — the same rule
would let weather silently stop the door working, which is the opposite of
what it is for. Check your mechanism before relying on it.

**If you want something that cannot be forgotten, use a maintenance window
instead.** `maint 240` makes the door inert for up to four hours and then
releases itself. For cleaning out, moving a bird, or working on the mechanism,
that is the better tool.

**Calibration is refused while manual control is held**, because `calibrate`
drives the door twice and would move it out from under you. Send `door auto`
first.

## Reporting a safety problem

If you find a way this firmware can injure an animal that is not listed above,
please open an issue and label it `safety`. That is more valuable than a feature
request.
