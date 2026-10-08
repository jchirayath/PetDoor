# Parking lot

Work that is **deliberately not being done**, and what would unblock it. Distinct
from two neighbouring files, and the distinction is the point:

- [STANDARD_EXCEPTIONS.md](STANDARD_EXCEPTIONS.md) — rules this repo does not meet,
  each with an expiry. Those are commitments.
- [ROADMAP.md](ROADMAP.md) — features someone intends to build.
- **This file** — things that are understood, usually already half-built, and
  waiting on something outside the code. Nothing here needs a decision; each one
  needs an event.

A parked item is not a TODO. If it can be worked on, it does not belong here.

Reviewed: **2026-10-06**.

---

## Beacon low battery — waiting on the beacon

**State: firmware complete and unexercised.**

`BEACON_LOW_BATTERY_MV`, the latch, the `BEACON_LOW` event, the dashboard
rendering and the email all exist and are tested. What does not exist is a single
reading, because the reference Minew beacon is in Eddystone mode and **never emits
TLM (`0x20`) frames** — and TLM is where the battery voltage lives.

`s` reports the telemetry age when a frame has been seen; on this beacon it never
has.

**Unblocked by:** enabling the TLM slot in the beacon's configuration app. That is
a beacon setting, not a code change. Once it reports, the path should light up with
no firmware work at all — which is the thing to be suspicious of, so check the
millivolts against a meter before trusting the first alert.

Worth knowing why it matters rather than being a nice-to-have: a flat beacon
battery does not shut the door, it stops the door working. The firmware refuses to
actuate until it has heard the collar once since boot (invariant 5), so a dead
beacon leaves the door inert with the animal outside, quietly.

---

## The recurring panic — unexplained; every hypothesis so far eliminated

**State: recurring, instrumented, not mitigated.**

On 6 Oct the door panicked (`ESP_RST_PANIC`) about eight seconds after collecting
an `ota` command, 3.5 hours into a boot. It recovered on the next boot, and it has
since repeated — see the recurrence section below.

**Every hypothesis below has now been eliminated or weakened to the point of
uselessness. The decision tree this entry used to carry has run out.** The
measurements that did it, all from boot **#1044** which ran clean for 16.4 hours
and counting:

| hypothesis | verdict | evidence |
|---|---|---|
| uploader stack exhaustion | **disproven** | `ustack` 4,084–4,268 free of 7,168, against the 2,220 that prompted it |
| plain heap leak | **disproven** | free heap moved -360 bytes across 12 h |
| ~3.2 h uptime accumulation | **disproven** | 16.4 h clean, nearly 5× the supposed interval |
| heap fragmentation | **weak** | `maxalloc` 77,812 after 16.4 h — the FLOOR of the range once sampled, not below it |

**On fragmentation specifically**, because it was the last one standing. It was
once recorded as ruled out on forty minutes of flat `maxalloc` (77,812–86,004),
and that window was fairly criticised as too short to exclude a slow
accumulation. The objection no longer applies: a 16.4-hour sample reads
`heap=117688 maxalloc=77812 heaplow=59720`, so the largest contiguous block is
still 77 KB after most of a day. Nothing this firmware allocates comes within an
order of magnitude of that. A heap can be fragmented and healthy; this one is
not failing allocations.

**Get `maxalloc` from the log server, not the console.** It rides in the uploaded
status line only, which is why it went unexamined for so long. One command:
`docker exec -i podcast-petdoor-1 python3 petdoor-logserver.py --doors` prints the
whole line per door. The panic alert emails carry it too.

**The stack hypothesis, and why its fix is worth keeping anyway.**
`beginOtaWindow()` only raises a flag; `radioUp()` and `ArduinoOTA.begin()` both
run on the uploader task, whose margin was 2,220 bytes — against a `config.h`
comment that justified its 5,120-byte size from a measured 2,650-byte peak that
had since been exceeded. Both stacks were raised to 7,168 and the margins have
measured 3,724–4,272 ever since. That did not fix the panic, and the table above
retires it as a cause, but a task running 2.2 KB from the edge was a real defect
found on the way; do not undo it.

**The recurrences.** The event ring shows `BOOT reset=4` at boot **#1037**
(~2026-10-07 00:53Z) and **#1042** (~2026-10-07 18:54Z), roughly eighteen hours
apart. The #1042 one came about 46 minutes after a maintenance window closed, with
the door idle and nothing logged in between — so, unlike the original, it was
**not** within seconds of an `ota` command. Whatever the cause is, it is not
OTA-triggered.

The decision tree this entry used to end on — *wide `heap`-to-`maxalloc` gap means
the heap, small `ustack` means the stack, neither means start again* — has been
walked to its third branch. **Neither. Start again.** That is not a dead end so
much as the point of having instrumented it: three candidates are gone and the
numbers that killed them are cheap to re-take.

**Attribute each panic to the RIGHT boot.** `esp_reset_reason()` at boot *N*
reports why boot *N-1* ended, so the `reset=4` rows above record panics that
terminated boots **#1036** and **#1041** — not #1037 and #1042. This matters
because #1041 is the boot in which five `calibrate` passes, a maintenance window,
the network console and ~90 WiFi uploads all ran. The 46 minutes immediately
before it died were idle, but the boot as a whole was heavily loaded.

**A pattern that looked strong and did not hold: both well-characterised panics
landed ~3.2-3.5 hours into a boot.** Kept here because the figures are real and
the reasoning is instructive, not because it is still believed.

| panic | boot duration before it |
|---|---|
| original, 6 Oct | **~3.5 h** |
| ended boot #1041 | **~3.2 h** (started ~15:43Z, died ~18:54Z) |
| ended boot #1036 | between **1.6 h and 4.0 h** — last event at 5,724 s, and #1039 fixes the far bound; not pinned |

n=2 pinned, so this was suggestive rather than established — **and it was
disconfirmed within a day.** Boot **#1044** ran **16.4 hours** (58,962 s) with no
panic, nearly five times the supposed interval. One hour of that was a deliberate
stress of 20 open/close cycles; the rest was idle, then idle with the beacon back
in range. Uptime alone does not cause it, and the fault is **not** reproducible by
leaving the door up.

**Two theories died here in two days, both built on n=2** — this one, and (in
CLAUDE.md) a close-travel difference blamed on temperature that turned out to be
orientation. Treat any pattern from two data points on this door as a prompt for
a third measurement, not as a handle.

**What is still untested is the expensive BLE path.** For most of boot #1044 the
target beacon was never heard at all — 3.4 million advertisements processed, 0
samples dropped. Invariant 18 is explicit that the advertisement handler allocates
NOTHING for a device that is not the target, so those clean hours exercised the
cheap path exhaustively and the costly one barely. With the collar in range the
handler builds `String`s per advertisement, samples cross a queue, and the filters
and discovery table run — the path whose heap exhaustion that invariant says "used
to panic doors". Boot #1041, which did panic, had the beacon at ~0.4 m producing
hundreds of samples a minute; by 16.4 h boot #1044 had it at ~4 m with
`samples=17671` and was still healthy.

So the remaining experiment is **sustained uptime with the collar close**, not
merely in range. It is the one condition the panicking boots shared and the clean
one did not. State it as the weak hypothesis it is: there is no positive evidence
for it, only an absence of coverage.

**Beware of resetting the clock you are trying to measure.** On 7 Oct an OTA push
and a USB flash each restarted the door, and each one postponed the very failure
being hunted. Once a watch is running, leave the door alone.

**Still blocked on:** a backtrace. It exists only on the serial console, so it
needs a cable attached at the moment it happens. The cable was deliberately kept
on rather than mounting the door, precisely to catch the next one. There is no
bound on that wait — the uptime theory that appeared to give one is dead — so the
practical move is to make the fault more likely rather than to sit and watch: run
the door for a long stretch with the collar **close**, which is the one condition
the clean 16.4-hour boot never reproduced.

---

## Hardware-dependent tests (TEST-0, TEST-7)

**State: permanently partial, by the nature of the thing.**

Listed in [STANDARD_EXCEPTIONS.md](STANDARD_EXCEPTIONS.md) without an expiry, and
parked here because "write the missing tests" is not actionable: you cannot
unit-test *"the relay pulse was long enough for this particular vendor controller
to notice"*. That took a stopwatch, a real motor, and the discovery that the first
press is swallowed every time.

What is actionable has been done — `beacon.*`, `proximity.*`,
`sensor_verdict.h` and the server's alerting are all pure logic and all tested,
180 host checks and 53 server checks. The rule of thumb for anyone extending this:
**if logic can be lifted out of the hardware path, lift it and test it** —
`sensor_verdict.h` exists for precisely that reason and nothing else.

**Unblocked by:** nothing. This is the shape of the project.

---

## Bluedroid

**State: supported, compiled in CI, not used.**

NimBLE is the default and the reference door runs it. Bluedroid sits at 94% of
`min_spiffs` with ~114 KB spare and has been creeping — it was ~123 KB two features
ago — so it is the configuration that will break first.

The maintainer has said it will not be used. It is **not** being removed, because
both stacks compiling is a stated invariant and CI covers all four board targets,
so keeping it costs nothing but a job — and nothing in wall-clock, since the four
compile jobs run in parallel and this is not the slowest.

**Its CI job is deliberately not a required status check on `main`.** The other
three compile jobs are. Required-and-doomed is the worst of the available options:
the first change that tips it past 100% would block every merge for a
configuration nobody runs. It still runs and a red mark is still the signal.

**Unblocked by:** a decision to drop it, which would delete the stack-adapter
block at the top of `ble_scanner.cpp` and simplify that file considerably — or by
it failing to fit, which forces the same decision.

---

## Adding to this file

An item needs three things: what state it is actually in, what would unblock it,
and why it matters if it is not obvious. If you cannot write the second one, it is
not parked — it is either abandoned (delete it) or on the roadmap (move it).
