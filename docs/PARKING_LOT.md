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

## The recurring panic — unexplained; the uptime theory is dead

**State: recurring, instrumented, not mitigated.**

On 6 Oct the door panicked (`ESP_RST_PANIC`) about eight seconds after collecting
an `ota` command, 3.5 hours into a boot. It recovered on the next boot, and it has
since repeated — see the recurrence section below.

What was thought ruled out: **heap fragmentation.** `maxalloc` was sampled across
more than forty minutes on two builds and oscillated between 77,812 and 86,004
with no downward trend, so the heap appeared not to be degrading.

**That sampling window was too short to rule it out, and this is the correction
that matters.** If the fault needs ~3.2 hours of uptime to develop (as the table
below suggests), forty minutes of flat `maxalloc` says only that the first 20% of
the run looks healthy — which is exactly what a slow accumulation would look
like. Fragmentation is therefore NOT excluded; it is the leading candidate, and
the sample needs to span a whole boot to mean anything.

What remains: **the uploader task's stack.** `beginOtaWindow()` only raises a flag;
`radioUp()` and `ArduinoOTA.begin()` both run on the uploader task, whose margin
was 2,220 bytes — against a `config.h` comment that justified its 5,120-byte size
from a measured 2,650-byte peak that had since been exceeded. Both stacks are now
7,168, and the margins measured 3,820 and 4,272.

**This is not claimed as the fix.** It was never reproduced, and an OTA window
opened cleanly from the console at 27 minutes' uptime on the same build.

**IT HAS RECURRED — twice, and the stack hypothesis does not survive it.** The
event ring shows `BOOT reset=4` at boot **#1037** (~2026-10-07 00:53Z) and again
at **#1042** (~2026-10-07 18:54Z), roughly eighteen hours apart. The #1042 panic
came about 46 minutes after a maintenance window closed, with the door idle and
nothing logged in between — so, unlike the original, it was **not** within
seconds of an `ota` command.

Against the decision tree below: `ustack` read **4,268 free of 7,168** and
`cstack` 3,780, so the uploader stack is not exhausted — that was the leading
hypothesis and it is now unlikely. Free heap was 127,816 with a 69,648 low-water,
which does not look starved either. That leaves **fragmentation**, discriminated
by `maxalloc`, which rides in the uploaded status line rather than the console —
check the two panic emails, which fire without cooldown.

If it recurs *with* a wide `heap`-to-`maxalloc` gap it is the heap after all; if
`ustack` is small it is the stack; if neither, start again.

**Attribute each panic to the RIGHT boot.** `esp_reset_reason()` at boot *N*
reports why boot *N-1* ended, so the `reset=4` rows above record panics that
terminated boots **#1036** and **#1041** — not #1037 and #1042. This matters
because #1041 is the boot in which five `calibrate` passes, a maintenance window,
the network console and ~90 WiFi uploads all ran. The 46 minutes immediately
before it died were idle, but the boot as a whole was heavily loaded.

**The first thing resembling a pattern: both well-characterised panics landed
~3.2-3.5 hours into a boot.**

| panic | boot duration before it |
|---|---|
| original, 6 Oct | **~3.5 h** |
| ended boot #1041 | **~3.2 h** (started ~15:43Z, died ~18:54Z) |
| ended boot #1036 | between **1.6 h and 4.0 h** — last event at 5,724 s, and #1039 fixes the far bound; not pinned |

n=2 pinned, so this was suggestive rather than established — **and it has since
been disconfirmed.** Boot **#1044** ran **12.07 hours** (43,446 s) with no panic:
nearly four times the supposed interval. One hour of that was a deliberate
stress of 20 open/close cycles; the rest was idle. So uptime alone does not
cause it, and the fault is NOT reproducible by simply leaving the door up.

The 12-hour run also kills a plain leak, and weakens accumulation generally.
Across the final ~10.5 hours free heap moved **-360 bytes** (127,136 -> 126,776)
and the low-water **-564** (67,260 -> 66,696); the control and uploader stacks
lost 84 and 48 bytes of their minimum-ever margin. That is noise, not a slope.
`maxalloc` is still absent from the console, so fragmentation is still not
formally excluded — but a fragmenting heap that never once fails an allocation
across 12 hours and 3.4 million advertisements is a thin story.

**What boot #1044 did NOT exercise, and this is now the most promising lead:
the target beacon was never heard.** `rssi : beacon has never been heard`, with
3,446,154 advertisements processed and 0 samples dropped. Invariant 18 is
explicit that the advertisement handler allocates NOTHING for a device that is
not the target — so twelve clean hours tested the cheap path exhaustively and
the expensive one not at all. With the collar in range the handler builds
`String`s per advertisement, samples cross a queue, the filters and the
discovery table run, and that is the path whose heap exhaustion the invariant
says "used to panic doors". Boot #1041, which did panic, had the beacon at
~0.4 m generating hundreds of samples a minute.

**So the next test is uptime WITH the beacon present**, not uptime alone. If it
panics with the collar in range and survives without it, that localises the
fault to the target path and makes it reproducible — which is what the uptime
theory promised and failed to deliver.

**Beware of resetting the clock you are trying to measure.** On 7 Oct an OTA push
and a USB flash each restarted the door, and each one postponed the very failure
being hunted. Once a watch is running, leave the door alone.

**Still blocked on:** a backtrace. It exists only on the serial console, so it
needs a cable attached at the moment it happens. The cable was deliberately kept
on rather than mounting the door, precisely to catch the next one. There is no
bound on that wait — the uptime theory that appeared to give one is dead — so
the practical move is to make the fault more likely rather than to sit and
watch: run the door with the collar IN RANGE, which is the path twelve clean
hours left untested.

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
