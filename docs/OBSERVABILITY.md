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
| **Buzzer** | immediately, at the door | no | a passive buzzer fitted |
| **Event log** | on the next upload | **yes** — NVS ring | nothing |
| **Status line** | on the next upload, ~5 min | no — current state only | WiFi configured |
| **Email** | minutes | n/a | the log server configured with `PETDOOR_NOTIFY_TO` |

The **buzzer is the only sink that works with no cable, no WiFi and no server** —
which is why it carries distinct patterns rather than one beep. The **event log is
the only one that survives a power cut**, which is why a crash is reconstructable
at all.

---

## Producer × sink

`✓` carried, `—` not carried, `!` deliberately not.

| Event | Console | Buzzer | Event log | Status | Email |
|---|---|---|---|---|---|
| `BOOT` | ✓ | — | ✓ | boots | **only if abnormal** — panic, watchdog, brownout. A power-on or a software restart is ordinary, and an OTA push *is* a software restart |
| `OPEN` / `CLOSE` | ✓ | ✓ (who asked, by beep count) | ✓ | `door` | ! routine |
| `REFUSED` | ✓ | ✓ (lock and schedule sound different) | ✓ | — | ! routine |
| `WAKE` | ✓ | — | ✓ | — | ! expected on this hardware every time |
| `RETRY` | ✓ | — | ✓ | `attempt` | ! the `GAVE_UP` that may follow says more |
| `NO_MOVE` | ✓ | ✓ | ✓ | — | ! nothing moved, so nothing is trapped |
| `STALLED` | ✓ | ✓ (5 fast — most urgent) | ✓ | — | ✓ rate-limited |
| `GAVE_UP` | ✓ | ✓ | ✓ | `gaveup` | ✓ **never** rate-limited |
| `UNCOMMANDED` | ✓ | ✓ | ✓ | — | ! most are a hand on the door |
| `SENSOR_FAULT` | ✓ | ✓ **repeats** every 15 min | ✓ | `sfault` | ✓ |
| `SENSOR_OK` | ✓ | — | ✓ | `sfault=0` | ✓ **only if the fault emailed** |
| `BEACON_LOW` | ✓ | — | ✓ | `battlow` | ✓ on the falling edge |
| `MAINT` | ✓ | — | ✓ | `maint` | ✓ |
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
