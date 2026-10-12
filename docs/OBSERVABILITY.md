# Observability — what the door says, and where

Four sinks, and an event reaches some of them and not others. **That asymmetry is
where the bugs live.** Three were found in a single evening, each the same shape —
the door knew something and no sink a person reads carried it:

- `droppedSamples()` was counted and printed on the console. Never uploaded. So on
  a mounted door with no cable, "is this door missing the collar because the
  firmware is too busy" was unanswerable.
- A sensor fault *clearing* was reflected in the status line and nowhere else. The
  event log showed four faults arriving and none leaving, so a fault the door had
  shrugged off looked identical to one still live.
- A **panic** was entirely silent. The door crashed, recovered, and nothing said
  so; it was found an hour later by someone asking an unrelated question.

This table exists so the next one is visible before it costs an hour.

Reviewed: **2026-10-06**.

---

## The four sinks

| Sink | Reaches you | Survives a reboot | Needs |
|---|---|---|---|
| **Console** (`Con`) | only while something is attached | no | a USB cable, or a network console inside a maintenance window |
| **Buzzer** | immediately, at the door | no | a passive buzzer fitted, and not muted — see below |
| **Event log** | on the next upload | **yes** — NVS ring | nothing |
| **Status line** | on the next upload, ~5 min | no — current state only | WiFi configured |
| **Email** | minutes | n/a | the log server configured with `PETDOOR_NOTIFY_TO` |

The **buzzer is the only sink that works with no cable, no WiFi and no server** —
which is why it carries distinct patterns rather than one beep. The **event log is
the only one that survives a power cut**, which is why a crash is reconstructable
at all.

---

### The buzzer can be muted, and that changes this table

`mute` silences the **routine** sounds: the tick while moving, the arrival
chime, the refusal note and the command acknowledgements. It is stored, so it
survives a reboot — somebody who silenced a door at bedtime should not have it
start ticking again because the power blinked.

**Faults still sound through a mute**, deliberately: a stall, a door that never
moved, exhausted close retries, an uncommanded travel and a sensor fault. The
buzzer is the only sink that reaches somebody standing at the door, and a mute
that silenced the alarms as well would quietly remove a whole column from this
table rather than trim it.

`beep` also still sounds, because that is how an undocumented buzzer pin gets
found, and a mute that silenced the test would make the buzzer look broken.

Distinct from `buzzer off`, which sets the pin to `-1` and forgets the wiring.
`mute` is a flag; `buzzer off` is a configuration change.

**Reachable from the console as well as the dashboard** — `w` then `mute` or
`unmute`, beside `buzzer` and `beep`. It was dashboard-only to begin with, which
got the priority exactly backwards: a sensor fault repeats every
`SENSOR_FAULT_BEEP_MS`, and the person most likely to want it quiet is the one
standing at the door with a cable and a screwdriver, who had no way to do it.

It sits in the timing menu rather than on a top-level key because it is a saved
setting like everything else there, and because every new single-character key
is another letter that does something when a word is mistyped at the top level.

**The only thing now reachable remotely and not from the console is `reboot`**,
and that is deliberate: `!` reboots into FLASH mode, which is a different act,
and a plain restart from a cable is unplugging it. A test in
`tests/server/run.sh` asserts that list stays exactly one entry long, so a new
remote verb cannot quietly become dashboard-only.

## Producer × sink

`✓` carried, `—` not carried, `!` deliberately not.

| Event | Console | Buzzer | Event log | Status | Email |
|---|---|---|---|---|---|
| `BOOT` | ✓ | — | ✓ | boots | **only if abnormal** — panic, watchdog, brownout. A power-on or a software restart is ordinary, and an OTA push *is* a software restart. An abnormal reset also **flushes immediately** rather than waiting for the cadence: a door that keeps falling over never lives long enough to report it |
| `OPEN` / `CLOSE` | ✓ | ✓ (who asked, by beep count) | ✓ | `door` | ! routine |
| `REFUSED` | ✓ | ✓ (lock and schedule sound different) | ✓ | — | ! routine |
| `WAKE` | ✓ | — | ✓ | — | ! expected on this hardware every time |
| `RETRY` | ✓ | — | ✓ | `attempt` | ! the `GAVE_UP` that may follow says more |
| `NO_MOVE` | ✓ | ✓ | ✓ | — | ! nothing moved, so nothing is trapped |
| `STALLED` | ✓ | ✓ (5 fast — most urgent) | ✓ | — | ✓ rate-limited |
| `GAVE_UP` | ✓ | ✓ | ✓ | `gaveup` | ✓ **never** rate-limited |
| `UNCOMMANDED` | ✓ | ✓ | ✓ | — | ✓ rate-limited — *"This door is not where it was left"*. Not left to the `MANUAL` mail: `enterOverride()` is guarded by `if (!g_override)`, so a hand-move on an already-manual door writes no `MANUAL` event at all |
| `SENSOR_FAULT` | ✓ | ✓ **repeats** every 15 min — and `SF_REED_MISSED` has its OWN tune (one long, then three blips), because it is the only one whose remedy is a screwdriver at the door rather than a look at the console | ✓ | `sfault` | ✓ |
| `SENSOR_OK` | ✓ | — | ✓ | `sfault=0` | ✓ **only if the fault emailed** |
| `BEACON_LOW` | ✓ | — | ✓ | `battlow` | ✓ on the falling edge |
| `MAINT` | ✓ | — | ✓ | `maint` | ✓ |
| `MANUAL` | ✓ (and in the boot banner) | — | ✓ **with HOW in the spare column** | `ovr` | ✓ **on arrival, no cooldown, and repeated for as long as it lasts** — titled *"This door was moved by hand"* when the source is a hand |
| `CONSOLE` | ✓ | — | ✓ | — | ✓ — a network console can open the door |
| `FIX_GOT` / `FIX_LOST` | ✓ | — | ✓ | `present` | ! several a day |
| *door goes silent* | n/a | n/a | n/a | n/a | ✓ server-side watchdog, 2 h, plus one recovery mail |

### Numbers that only ride in the status line

No event, no buzzer — they are continuous, so they are state rather than news:

| Field | Why it is there |
|---|---|
| `rssi` `raw` `dist` `present` | the proximity decision's inputs |
| `adv` `samples` `gap` `weak` | radio health. `gap` is the worst interval between samples |
| `drop` | samples the BLE task could not hand over. **Should read 0 forever** — the queue is 32 deep against a ~2 Hz beacon, about 16 s of slack |
| `heap` `maxalloc` `heaplow` | **`maxalloc` is the one that matters.** An allocation needs a *contiguous* block, so a wide `heap`-to-`maxalloc` gap is fragmentation — the failure that takes a door out while every other number looks healthy |
| `cstack` `ustack` | minimum-ever-free stack. `ustack` is the one to watch when an OTA window will not open: `beginOtaWindow()` only raises a flag, and the radio work runs on the uploader task |
| `mopen` `mclose` `cal` | measured travel times, which is how travel time is tuned from a browser |
| `act` `attempt` `retry` | what the actuation path is doing, so a door staying open on purpose is distinguishable from one that has died |

**`heap` is sampled with the radio up.** `publishStatusLines()` runs inside the
upload path, so the uploaded figure is ~50 KB below idle. It is not a leak and two
samples cannot show one — this door read 89 KB mid-upload and 124 KB idle half an
hour later, same boot.

---

## Adding an event

Ask the five questions in order. Most of the asymmetries above are deliberate and
the reasoning is in the right-hand column; a new row needs the same.

1. **Console?** Almost always yes. Use `Con`, not `Serial`, in `petdoor.ino` —
   `Serial` cannot be read by anyone without a cable.
2. **Buzzer?** Only if somebody standing at the door can act on it. Every added
   tune makes the others harder to tell apart, and `chime.h` argues each one.
3. **Event log?** Yes if you would want it a week later. **Including recoveries** —
   a log of failures with no resolutions makes a fault that cleared look live.
4. **Status line?** Yes if it is *state*; no if it is *news*. If diagnosing it
   without a cable would be impossible, it belongs here — that is the `drop=`
   lesson.
5. **Email?** Only if somebody should act today. Each unnecessary one makes the
   next real message likelier to be filtered, and a filtered alert is worse than
   no alert because it feels like coverage.

If the answer to 4 is "no" and the answer to 1 is "yes", stop and check you are not
about to repeat the `drop=` mistake: console-only means invisible on a mounted door.

## A worked example of getting this wrong

On 2026-10-10 the reference door was moved by hand. A limit switch measured it.
What the owner received was an email headed **"A door sensor has stopped telling
the truth"**, advising them to check a connector and a magnet-to-reed gap that
were both fine.

Two separate failures produced it, and they are worth keeping as a pair because
neither alone would have:

1. **A verdict that could fire mid-travel.** `judgeIdleNoise()` stood down for a
   commanded travel (`Actuator::busy()` invalidates the window before it opens)
   and for a *concluded* run, but not for one still in flight. Mid-move the door
   has left one end and not reached the other, so `endChanged` was false and
   `runs` was 0 while the sensor correctly reported 12,642 real edges. The window
   expired one second before the switch confirmed the door had moved.
2. **The right answer reached no sink a person reads.** The door *did* know: it
   logged `UNCOMMANDED` and took manual control. But `enterOverride()`'s reason
   string — "the door was moved by hand" — went only to the console, and the
   door was on a bench with nobody attached to it. `LOG_OVERRIDE` carried no
   source, so the email could not tell a hand from a dashboard press.

The second is this file's own failure mode, and the fourth instance of it: the
door knew something and the only sink carrying it was one nobody was reading.
Ask the five questions above for the *explanation* of an event, not just for the
event — "which sink says WHY" is the one that was missed here.

## The watchdog's own health

Everything in the email column above that is *not* triggered by an arriving
event — a door gone quiet, a door still held — depends on one background thread
in the log server. For a long time nothing reported whether that thread was
alive.

That cost a real diagnosis. On 2026-10-11 the reference door was gone for
**9.6 hours** against a two-hour threshold, and afterwards it was impossible to
establish whether the watchdog had fired and the mail was lost, or the watchdog
had never run. A dead thread and a healthy quiet one produced identical
evidence: nothing.

So each sweep now stamps `settings.watchdog_last_sweep`, and
`--doors` prints it first, before any door:

```
  watchdog   : swept 42s ago (every 5 min, stale after 2h)
```

Three rules it is built on, each of which was a choice:

* **Stamped AFTER the work, not before.** A thread that wakes and throws every
  time is not a working watchdog; a stamp at the top of the loop would claim it
  was. A failing sweep leaves the old timestamp and writes
  `watchdog_last_error` instead.
* **In the database, not in memory.** The useful question after a crash is
  "when did this last work", and an in-memory marker is erased by the very
  restart being investigated.
* **Unknown reads as NOT healthy.** A watchdog that has never swept reports
  stalled, because the alternative is a reassuring blank beside something dead.
  Stale is three intervals, not one — crying wolf about the thing whose job is
  crying wolf gets both ignored.

**The general rule this is an instance of:** a sink that reports on other
things has to report on itself, or its silence is ambiguous. Ask the five
questions above of the *monitor*, not just of the event.
