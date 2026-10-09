# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

ESP32 firmware that opens and closes a chicken coop / pet door by pulsing two
relays, based on the proximity of a BLE beacon (a Minew beacon in the reference
build). Public repo, MIT licensed, aimed at hobbyists who will copy it onto
their own hardware.

## Build and verify

Verification is **host tests plus "it compiles for the target"**, and both are
required before claiming a change works.

```bash
tests/host/run.sh      # unit tests + fuzz for beacon.*, and the BLE-adapter guard
```

Run it for any change to `beacon.*` or to the stack adapter in
`ble_scanner.cpp`. It needs no hardware and no Arduino toolchain, and it exists
because a bug that compiled perfectly ran on a door for weeks: the NimBLE
adapter converted binary payloads through `c_str()`, which stops at the first
NUL, and both frames this firmware parses carry a NUL in byte 2. Battery
telemetry never decoded and iBeacon UUID matching could never match. See
`tests/host/README.md`.

`beacon.*` is the only attacker-controlled input the firmware has — every byte
comes from an unauthenticated advertisement — so it is fuzzed under ASan/UBSan as
well as unit tested. **180 host checks** now cover it plus `proximity.*` (the
filters, the hysteresis state machine and both `millis()`-wrap cases) and
`sensor_verdict.h` (both sensor-health decision tables). `tests/server/run.sh`
adds 53 on the log server's alerting.

What is left untested needs a chip, a radio or a relay, and that is the honest
boundary rather than a backlog — see `docs/PARKING_LOT.md`. The rule for extending
it: **if logic can be lifted out of the hardware path, lift it and test it.**
`sensor_verdict.h` exists for that reason and nothing else.

Then compile — for both stacks:

```bash
ARDUINO_CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
"$ARDUINO_CLI" compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs petdoor
```

The bundled CLI above ships inside Arduino IDE.app; a standalone `arduino-cli`
on PATH works identically. Target is **ESP32 Arduino core 3.x** (3.3.5 is what
this was developed against).

**Build with `PartitionScheme=min_spiffs`.** It is required, not an
optimisation: with WiFi enabled the image does not fit the default scheme at all
(136%). `min_spiffs` keeps a second app slot, which is what `wifi_logger.cpp`'s
ArduinoOTA support needs — the firmware *does* flash over the air. In the
Arduino IDE it is **Tools → Partition Scheme → Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)**;
PlatformIO picks it up from `board_build.partitions` in `platformio.ini`.

Two compile-time switches move the size a lot. Measured on min_spiffs:

| | flash | of 1.875 MB |
|---|---|---|
| **default (NimBLE + WiFi)** | **1,394,135** | **70%** |
| `-DPETDOOR_USE_NIMBLE=0` (Bluedroid) | 1,851,251 | 94% |
| `-DPETDOOR_ENABLE_WIFI=0` | 718,039 | 36% |

Bluedroid has ~114 KB of flash left, and that number only goes one way: it was
~123 KB two features ago. Still supported, but it is the
configuration that will break first — check it before and after any sizeable
change, not just at the end.

**NimBLE is the default.** It was opt-in until a door in service panicked on
three consecutive boots (`reset=4`) and the cause turned out to be heap
exhaustion:

| | Bluedroid | NimBLE |
|---|---|---|
| flash | 92% | 69% |
| free heap | 63 KB | 134 KB |
| **heap low-water** | **6.9 KB** | **80 KB** |

6.9 KB is the least free RAM that door ever had. An allocation failing down
there panics the chip. Bluedroid still builds and still works, but it has no
headroom left once WiFi, the uploader, OTA and the network console are all
resident — so it is a supported option, not a sensible default.

Measure with `compiler.cpp.extra_flags`, **not** `build.extra_flags` — the
latter carries `-DCORE_DEBUG_LEVEL`, `-DESP32` and the loop/event core settings,
and overriding it drops them and inflates the image by ~33 KB.

The default now **requires the NimBLE-Arduino library**. That reverses a
long-standing rule that a clean checkout must compile with no extra libraries,
and it was not given up lightly — but a default that panics is worse than a
default that needs one Library Manager entry. PlatformIO installs it from
`platformio.ini`; Arduino IDE users add it by hand, and `ble_scanner.cpp` emits
an `#error` naming the library and the opt-out flag rather than letting them
hit a bare "NimBLEDevice.h: No such file".

Do not quote the static-RAM delta as NimBLE's benefit — it is heap, and it is
an order of magnitude larger. **Both stacks must keep compiling** — check both
before claiming a BLE change works:

```bash
"$ARDUINO_CLI" compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs petdoor
"$ARDUINO_CLI" compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  --build-property "build.extra_flags=-DPETDOOR_USE_NIMBLE=0" petdoor
```

Do not flash hardware unless the user explicitly asks. Uploading drives a real
motor attached to a real door.

## Flashing

**Use `compile --upload`, never a bare `upload`.** `arduino-cli upload` does not
compile — it ships whatever is already in the build directory. During one
session that meant a Bluedroid image, left there by a verification build, was
written to a door whose firmware was NimBLE: the 92%-flash configuration with
6.9 KB of heap headroom that the table above describes as the one that panicked
a door in service. It never ran, and only because the chip happened to stay in
download mode. The binary must come from the same invocation that flashes it.

```bash
"$ARDUINO_CLI" compile --upload -p /dev/cu.usbserial-0001 \
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs petdoor
```

**This board has no auto-reset wiring**, so every flash needs the buttons:

1. Hold **IO0**, tap **EN**, release **IO0** — the chip enters download mode.
2. Upload. Retry in a loop: the user has to be at the board, and the window is
   whenever they get there, not when the command runs.
3. esptool prints "Hard resetting via RTS pin", but RTS is not wired — **tap EN
   again** or nothing boots. A board that answers nothing after a successful
   flash is almost always sitting in the bootloader waiting for this.

Check the real exit status, not a grep of the output. `... | grep -q Wrote` reports
grep's status and will call a failed upload a success.

**The adapter re-enumerates after a flash.** The device node changes, so any
file descriptor held across an upload goes stale and every subsequent write
fails with `device not configured`. Reopen the port after flashing.

### Over the air, which is how a mounted door is updated

**OTA works and is the way to update a mounted door** — proven repeatedly at
~1.39 MB over WiFi, self-confirming ~61 s after boot on an idle door. Two
things about it that cost time to learn:

* **A pushed image boots on probation and rolls back on the next reset** unless
  it confirms itself, and it only confirms after a SUCCESSFUL UPLOAD to the log
  server. Resetting the door to "check it took" is what un-takes it; the only
  symptom is a banner reading two builds old. Press `u` after a push, or leave
  the door idle and watch for `[ota] image confirmed good`.
* **Push the binary from the compile you just ran.** `compile --upload`, or
  `espota.py -f` against a fresh `--output-dir`. The firmware's own hint prints
  a bare `arduino-cli upload`, which is exactly the command that once wrote a
  Bluedroid image to a NimBLE door.
* **ArduinoOTA's port 3232 is UDP, not TCP.** The invitation is a UDP packet;
  only after the door answers does it open a TCP socket, on an ephemeral port.
  So `nc -z <door> 3232` — a TCP scan — can never succeed, whether the window is
  open or shut. A session was spent watching a window that had in fact opened on
  time and acked `applied`. **Use `espota.py` itself as the probe**, in a retry
  loop: its own handshake is the only honest test of whether the window is up.
* **The build timestamp the dashboard shows does NOT tell you which source the
  image was built from.** `PETDOOR_BUILD` is `__DATE__ " " __TIME__`, and the
  copy the server sees is the one baked into `wifi_logger.cpp`, as the
  `X-PetDoor-Build` header. Edit only `petdoor.ino` and arduino-cli reuses the
  cached `wifi_logger.cpp` object — so the banner reports whenever that file was
  last compiled, which can be hours before the image you just pushed. This
  directly weakens the check above: "a banner reading two builds old" is a
  symptom of a rollback, but an UNCHANGED banner is **not** evidence the push
  failed. Verify with something you actually changed — a new status field, a new
  console line — or force the issue with `compile --clean`.

## The serial console

The only interface the firmware has. On macOS this needs more care than it
looks:

```bash
PORT=/dev/cu.usbserial-0001
exec 3<>$PORT                      # hold it open — see below
stty -f $PORT 115200 cs8 -parenb -cstopb -crtscts min 0 time 10
printf 's\r' >&3
for i in $(seq 1 20); do dd bs=8192 count=1 <&3 2>/dev/null; done
exec 3>&-
```

**The `exec 3<>` is load-bearing.** macOS resets termios when the last
descriptor closes, so `stty` followed by a separate `cat` silently reverts to
**9600** and reads garbage or nothing. A session was lost to diagnosing a
"silent board" that was only being listened to at the wrong baud rate.

`min 0 time N` sets a read timeout in tenths of a second, which is what stops
`dd` blocking forever on a quiet port.

**`min 0 time N` in `stty` is load-bearing in both directions.** `min 0` is
what stops `dd` blocking forever on a quiet port. Raising it to buffer fuller
reads (`min 200`) makes every read block until the port speaks again, which
hangs the capture the moment the output ends. If a long dump arrives truncated,
add iterations — do not raise `min`.

**But iterations are not seconds, so pace a TIMED operation by the clock.** A
`dd` that finds data returns immediately; only a silent read burns its full
`time N`. This console emits `[wifi]`/`[cmd]` lines every few seconds, so a
135-iteration loop elapsed well under 135 s — which desynchronised a run of five
`calibrate` passes, got one `C` refused with `[cal] the door is already
travelling`, and lost one verdict that printed after the descriptor closed. For
anything with a known duration — a travel, a ~115 s calibration pass — use a
deadline inside the one held descriptor:

```bash
END=$(( $(date +%s) + 150 ))
while [ $(date +%s) -lt $END ]; do dd bs=8192 count=1 <&3 2>/dev/null; done
```

A verdict lost to a closed descriptor is still recoverable: `s` reports both the
`configured` and the `last verified` pair, so the device remembers what the
missed `[cal]` line said.

**Send a submenu key with NO carriage return.** `printf 'w\r'` does not open the
timing menu, it opens and immediately exits it — the CR *is* the empty line that
cancels, and the reply is `[mac] cancelled, nothing changed.` Everything typed
next is then interpreted as top-level keystrokes, which is how a `travel ...`
line became `t` plus a thresholds-menu buffer. Send the bare key, grep the reply
for the menu header, and only then send the line.

**Output printed while no descriptor is open is gone** — there is no flow
control. Capture across a whole operation in one connection rather than
reconnecting between steps, or verdicts that print during the gap are lost.

**Top-level console keys are single characters, and `o` and `x` move the door.**
Sending a word at the top level types it as commands: the `o` in `sensors 32 25`
will pulse the OPEN relay. Submenus are line-based, but a submenu command
returns to the top level when it completes, so the next line is interpreted as
keystrokes again. Confirm which level you are at — `s` is harmless and its
output identifies it — before sending any text.

**The console's submenus are a trap, and the documented dance is the only safe
one.** An EMPTY LINE EXITS a submenu. So `w` followed by a carriage return
lands you back at the top level, and the word you send next is typed as single
keystrokes — which is how `vibration 33` became `r` (reset the filter) and `t`
(open the thresholds menu) on a live door. The `o` in it was swallowed by the
thresholds menu's line buffer purely by luck of ordering; one letter earlier
and it would have pulsed the OPEN relay. Send the submenu key ALONE, confirm
its header came back, then send the line, then confirm with `s` before sending
anything else. Do not assume a submenu command returns to the top level — some
do, some do not.

**A travel now resolves asynchronously, and the console says so twice.**
`[door] opening` is the request; `[door] OPEN — ARRIVED` (or `STALLED`, or
`NEVER MOVED`) is the outcome, up to ~15 s later. Capture across the whole
travel when driving the console from a script, or the verdict lands in the gap
between reads. A cold actuation also sends **two** relay pulses about two
seconds apart — that is the wake press, not a fault.

## The reference door, as measured

Controller and motor connected, both reeds + vibration + buzzer fitted. Pins:
relays 16/17 **active HIGH**, LED 23, reeds **32**/**25**, buzzer **27**
(passive), vibration **33**. None of the sensor pins are compiled-in defaults —
they are set at runtime and saved on the device.

**Mounted UPRIGHT, which is the configuration in service.** Measured
2026-10-07:

| | |
|---|---|
| travel CONFIGURED | open **11,366 ms**, close **11,000 ms** |
| UPRIGHT, `calibrate`, five passes | open **11,366 ms** mean, close **9,105 ms** mean |
| UPRIGHT repeatability | open spread **78 ms**, close spread **175 ms** (n=5) |
| FLAT, 20 cycles over an hour | open **10,222 ms** mean, close **10,934 ms** mean |
| FLAT repeatability | 602 ms range in each direction (n=20) |
| travel, closed BY HAND | **12,500 ms** — measured as vibration, not reeds |
| relay pulse | **1,500 ms** stored on the device (500 ms is swallowed) |
| vibration while moving | ~2,900 edges/s; **0** at rest |

**Upright REVERSES which direction is slower, and that is the whole reason to
re-measure after mounting.** Lying flat the same door read open 10,203 ms /
close 11,229 ms, so closing was the slow leg by ~1.0 s. Upright, *opening* is
the slow leg by ~2.3 s — gravity opposes the lift and assists the drop. A travel
time carried over from a flat bench is therefore wrong in both directions and
wrong in sign: it gives the open leg ~1.2 s less than it needs while handing the
close ~2.1 s of slack it does not. Re-calibrate after any change in mounting
angle, and do not interpolate between the two sets.

**A close of 10,912 ms was once blamed on the door being COLD. That was wrong —
it was ORIENTATION.** The reasoning looked sound at the time: five `calibrate`
passes upright had measured 9,034–9,209 ms, and the next real close took
10,912 ms, 1.8 s outside the whole range, so "calibrate runs the door warm and
under-reports" was adopted and the configured close raised to 11,000 ms.

Then the door was laid flat and cycled twenty times: closes came in at
10,714–11,316 ms, **mean 10,934 ms**. That brackets the supposedly-cold 10,912 ms
exactly, while every upright close was ~1.7 s faster. The door had simply been
laid flat (or was being handled) when that close was logged. It is the same
gravity argument as the crossover above, applied to the right variable —
reached for temperature when orientation was in plain sight.

**The consequence to act on: 11,000 ms is right for FLAT and ~1.9 s too generous
for UPRIGHT.** Per the `DOOR_TRAVEL_MS` comment's own argument that is two
seconds of slack added to every upright stall decision. It is not dangerous — a
stalled close fails open — but it blunts detection. **When this door goes back
upright, re-calibrate and expect close ≈ 9.1–9.5 s.** Note the vibration band
moves with it: at the flat 11,000 ms the hand-close figure below sits at 114%,
and at an upright 9,105 ms it sits at a thinner 137%.

**What DID show up over twenty cycles is a mild upward drift**: closes rose from
a 10,814 ms mean over the first five to 11,095 ms over the last five, +280 ms,
with opens up +180 ms. Plausibly thermal and bounded; the 3,000 ms
`TRAVEL_GRACE_MS` absorbs it with ~2.7 s to spare, which is the grace doing
exactly the job it exists for. Re-measure after the door has cooled before
reading anything more into it.

Within the warm passes the close was **bimodal rather than noisy**: two at
9,209/9,204 ms and three at 9,034/9,034/9,044 ms, each cluster tight to ~10 ms.
The 9,105 ms mean is a value the door never actually produced — do not quote it
as a typical travel, and do not chase the 175 ms as drift.

Reed-to-reed is shorter than the stopwatch figures in `docs/REQUIREMENTS.md`
§1, and correctly so: a reed makes before the door reaches its physical stop.
It is also the number the arrival deadline wants.

A hand is slower than the motor, which is why `VIBRATION_TRAVEL_MAX_PCT` is the
loose end of the band: 12.5 s by hand against the configured **11.0 s** is
**114%**, comfortably inside, and close to the 111% it read flat. The inference
in `concludeVibrationRun()` was verified on this door with the closed reed
unplugged. Do not tighten that bound to flatter the motorised figure — the
travels this code exists to notice are the hand-driven ones. Note that the band
is a percentage of the CLOSE travel, so it moves whenever that does: at the warm
9,105 ms it would have put the same hand close at 137%, within ~17% of falling
outside the band and not being inferred at all.

**Free heap is not monotonic, so two samples cannot show a leak.** The figure in
the uploaded status line is captured with the WiFi and TLS stack resident, and
the same door read 89 KB mid-upload and 124 KB idle half an hour later. A reading
taken ~60 s after boot is higher still, before buffers settle. Comparing a
post-boot sample against a mid-upload one manufactures a slope that is not there;
an hour of samples at the same phase is the only honest comparison.

**The wake press is swallowed every time** on this controller, as F1 predicts —
so a cold actuation legitimately sends two relay pulses about two seconds
apart. That is not a fault.

## secrets.h

Git-ignored, and denied in the local (uncommitted) `.claude/settings.json`, because this is a public
repo. It typically carries `BEACON_MAC`, `RSSI_ENTER_DBM`, `RSSI_EXIT_DBM`,
`RELAY_ACTIVE_LOW`, `OTA_PASSWORD`, `CONSOLE_PASSWORD` and the log-server
credentials.

**The deny is a guardrail, not a sandbox — do not treat it as a guarantee.**
`Read(./petdoor/secrets.h)` genuinely stops the Read tool. The
`Bash(<tool>:*secrets.h*)` rules that used to stop the obvious shell
equivalents were removed on 2026-10-07; what remains in the deny list is the
`Read` rule and the two bare-`upload` rules. Where those substring rules existed
they matched the COMMAND TEXT, not the file, so anything reaching the file
without spelling its name walked straight through — `grep -r PASSWORD petdoor/`
is allowed by `Bash(grep:*)` and prints the line, and that could not be closed
by a substring rule without banning recursive grep altogether.

**`settings.json` is not the only gate, and it is not the binding one.** With
every `*secrets.h*` Bash rule removed, `grep -c <marker> petdoor/secrets.h` was
still refused — the auto-mode classifier judges these independently and
overrides the allow list. So do not reason about what is reachable from the
permission file: it under-states the restriction in one direction and
over-states it in the other. Try, and if it is refused, hand the command over.

So the rule that actually protects these values is a behavioural one:

**Do not read `secrets.h`, and do not route around the deny to do it.** Ask the
user to read a value out, or to extract it into a file you can use without
seeing. One session needed `OTA_PASSWORD`, wrote the `sed` one-liner to pull it
out, and then handed the command to the user rather than running it — that is
the expected behaviour, and it came from judgement rather than from the config.

**Ask rather than theorise.** `RELAY_ACTIVE_LOW` was a leading hypothesis for
over an hour of one session for want of a question that would have taken one
exchange.

**An `#ifndef`-guarded block appended TWICE is a silent trap: the FIRST copy
wins.** Every override here is `#ifndef`-guarded, which is what makes appending
safe — and also what makes a revised value lose to the stale one already above
it. A block was appended, revised, and appended again; fourteen values matched
so nothing complained, and the fifteenth — `DOOR_TRAVEL_CLOSE_MS` — compiled in
the superseded figure. Nothing warns, because `#ifndef` is doing exactly what it
promises. Re-appending is idempotent only when the values have not changed, so
after any revision check for duplicates (`grep -c` the block's marker comment)
rather than appending again.

**Prove a compiled-in default by clearing the stored one.** These values usually
sit in NVS as well, so the console reporting the right number proves nothing
about the fallback. `clear` in the timing menu reverts to the compiled values and
is exactly what a remote `defaults` does — which makes it both the test and a
rehearsal of the recovery. It is how the duplicate above was caught, and it
should be run while a cable is still attached, because it writes those compiled
values straight back into NVS.


## Layout

`petdoor/` is simultaneously an Arduino sketch folder and the PlatformIO
`src_dir`. That is deliberate — one copy of the source, both toolchains. Keep
the folder name and `petdoor.ino` in sync or Arduino IDE stops recognising it.

| File | Responsibility |
|---|---|
| `petdoor.ino` | setup, control task, serial console, door decision |
| `config.h` | every tunable, each wrapped in `#ifndef` |
| `secrets.h` | git-ignored local overrides; may not exist |
| `ble_scanner.*` | BLE stack, scan, target matching, discovery table |
| `proximity.*` | dual-rate median + EWMA filters, hysteresis state machine |
| `door.*` | relay pulses, interlock, lockout, boot grace, believed state |
| `actuator.*` | one actuation attempt end to end: wake press, verification, retry, fail-open, travel calibration |
| `beacon.*` | iBeacon parsing, classification, distance estimate — pure functions |
| `chime.*` | Optional buzzer: non-blocking patterns, pin/type discovery at runtime |
| `position.*` | Optional limit switches: debounce, measured state. Never commands the motor |
| `maintenance.*` | Bounded window in which the beacon cannot move the door; RSSI histogram for calibration |
| `console.*` | The console as a Stream, fanned out to the UART and a window-bounded network client |
| `eventlog.*` | The durable ring in NVS, the event and sensor-fault enums, and the CSV the uploader sends |
| `schedule.*` | Time windows in which the beacon may not open the door. Inert without a clock |
| `vibration.*` | Optional sensor answering "did it START moving", by counting edges in an ISR |
| `sensor_verdict.h` | The sensor-health decisions as pure functions, so they can be host-tested. No Arduino, no config.h, no globals |

## The bug this project exists to fix

The predecessor sketch scanned continuously but only ever got **one** RSSI
sample per device, so proximity detection never worked. Cause:

```cpp
scan->setAdvertisedDeviceCallbacks(new ScanCallbacks());  // wantDuplicates = false
scan->start(0, nullptr, false);                           // scan forever
```

`setAdvertisedDeviceCallbacks(cb, wantDuplicates = false, ...)` both enables the
controller's duplicate filter and makes `BLEScan.cpp` bail out with
*"Ignoring %s, already seen it"* for any address already in its results map.
Across an infinite scan that means exactly one callback per device, ever.

`ble_scanner.cpp` passes `true`. **Do not "clean up" that argument.** The
comment above it explains why; keep the comment with the code.

## Invariants — do not regress these

1. **`RSSI_ENTER_DBM > RSSI_EXIT_DBM`.** The gap is the hysteresis band.
   Enforced by `static_assert` in `proximity.cpp`.
2. **`MIN_ACTUATION_INTERVAL_MS < EXIT_CONFIRM_MS`**, or the lockout delays
   closing. Also `static_assert`ed.
3. **A stale signal can close the door but never open it.** `hasFix()` gates
   `near`; `far` is true whenever there is no fix.
4. **The open path reads the fast filter, the close path the slow one**, and
   the fast pair must never be slower than the slow pair. `near` uses
   `fastRssi_`, `far` uses `filteredRssi_`, and `!near` cancels a pending close
   so a returning animal is noticed on the next advertisement. Reversing the
   speeds would mean noticing a departure before an arrival; `static_assert` and
   `applyFastFilter()` both refuse it.
5. **Never actuate before the beacon has been heard once since boot.** A dead
   beacon battery must not read as "absent" and shut the door.
6. **Never close during `BOOT_GRACE_MS`.** A power blip must not slam the door
   on an animal standing in it.
7. **Relays are interlocked.** `DoorController::pulse()` always releases the
   opposite relay and waits `DIRECTION_CHANGE_GAP_MS` first. Both relays
   energised at once is a short across the motor's direction contacts.
8. **The BLE callback stays cheap and never blocks.** It runs in the BLE host
   task (NimBLE). Samples go over a queue; the discovery table is taken with a zero
   timeout and skipped if busy. Do not add `Serial` output or blocking waits to
   it.
9. **Door state is owned by one task.** `controlTask` is the only caller of
   `Actuator`, and `Actuator` is the only caller of
   `DoorController::press()`/`commit()`. That chain is what makes this invariant
   checkable rather than aspirational — `requestOpen()`, `requestClose()` and
   `forcePulse*()` were removed precisely so there is one path to the motor. Do
   not actuate from `loop()` or from a callback.
10. **A maintenance window expires by itself.** It is stored as a deadline, is
    bounded by `MAINT_MAX_MS`, and is never written to NVS — a door left inert
    by a forgotten flag, a lost network or a brownout is a door that cannot let
    an animal in. It also blocks *both* directions, unlike the lock, because the
    person calibrating is standing at the door holding the collar.
11. **A scheduled lockout is inert without a clock, and gates opening only.**
    `Schedule::lockedNow()` returns false whenever `EventLog::haveEpoch()` is
    false — a door that guesses the time can lock an animal out at noon
    believing it is midnight. A schedule is an additional reason to refuse,
    never a reason to permit: the manual lock stays absolute.
12. **The network console never listens outside a maintenance window**, and
    never without `CONSOLE_PASSWORD`. It is the full console, so it can open the
    door. Do not start it at boot. **It is also bounded SEPARATELY from the
    window, by `CONSOLE_MAX_MS`** — a window may run for 480 hours, and a
    door-opening service has no business listening on the LAN for twenty days
    merely because the door is inert. The console closes; the window carries on.
    `CONSOLE_MAX_MS` is 4 h, which was `MAINT_MAX_MS` before the window was
    lengthened, so the console's maximum exposure did not change when the
    window's did. Keep those two ceilings separate: a safety bound and a
    security bound only looked like one number while they happened to be equal.
13. **A repeat press is never sent while a travel is in flight.** Same
    direction mid-travel reads as STOP on this hardware and parks the door
    halfway. The *opposite* direction always supersedes, and `request()` drops
    the believed state to `UNKNOWN` when it does: a travel in flight has not
    committed, so "already there" would otherwise refuse the open arriving
    mid-close — the one request that must never be refused.
14. **A stalled close fails open.** Reversed, not retried. The next attempt is
    `CLOSE_RETRY_DELAY_MS` away (minutes), attempts are capped by
    `CLOSE_RETRY_LIMIT`, and after the cap the door stays open and says so on
    the console, in the log, on the LED and in the uploaded status line. A door
    that silently stopped trying is worse than one that is visibly open.
15. **A failed attempt does not move the belief.** `OUT_NO_MOVE` leaves it
    alone; `OUT_STALLED` sets it to `UNKNOWN`. Only an arrival commits.
    Recording a close that never happened is what made the *next* close a
    no-op.
16. **Every wait in the actuator is a deadline, not a `delay()`.** The only
    blocking left is the press itself, and deliberately: the relay's release is
    on the far side of a `delay()`, so a control task that wedges elsewhere
    cannot leave a relay energised. Never add a `delay()` spanning a travel —
    the control task is what drains BLE samples, and a returning animal has to
    be noticed *during* a close.
17. **`LOG_REFUSED`'s `detail` is an `ActuationResult`**, except
    `kRefusedBySchedule = 100`, which is outside the enum on purpose. Adding a
    value to `ActuationResult` must not re-label history already in the log
    server's database — that happened once, when `ACT_RETRY_WAIT` landed on the
    schedule's hardcoded 5.
18. **The BLE advertisement handler allocates nothing for a device that is not
    the target.** Six address bytes are compared with `memcmp` against a
    pre-parsed list, and the handler returns before any `String` exists unless
    the device is the beacon or discovery is on. Do not reintroduce
    `getAddress().toString()` on that path: it is two heap allocations per
    advertisement per device in the BLE host task, and running out of heap
    there is what used to panic doors.
19. **An inferred position may commit a close, never an open.** With no limit
    switch to see it, a travel is inferred from how long the vibration sensor
    felt the door moving — direction follows from the end it started at, because
    a door at a limit has only one way to go. Being wrong is not symmetric:
    a wrong "closed" refuses the next *close* and the door stays open, which is
    fail-open, while a wrong "open" refuses the next *open* and shuts an animal
    out. So `concludeVibrationRun()` calls `observePosition(DOOR_CLOSED)` for a
    close and `setBeliefUnknown()` for an open. `UNKNOWN` refuses nothing. Do
    not "finish" that branch by making it symmetrical.
20. **An inference is logged distinguishably from a measurement.**
    `LOG_UNCOMMANDED`'s detail is the `DoorState` when a switch measured it and
    `kUncommandedInferred + state` (11, 12) when it was inferred, with the
    duration in the spare column. Same reason as invariant 17: a dashboard that
    renders the two identically turns one into the other.
21. **The sensor-health decisions stay pure, in `sensor_verdict.h`.**
    `judgeIdleNoise()` and `judgeReedAtRest()` take every threshold as an
    argument and read no global and no clock, which is the only reason
    `tests/host/test_sensor_verdict.cpp` can walk their decision tables at all.
    Both fail silently in both directions: raised in error is a buzzer every
    quarter hour for a working sensor, never raised is a door deciding on
    evidence that stopped arriving. The clear path for `SF_VIBRATION_NOISY`
    could not be demonstrated on hardware without first deliberately latching a
    fault that the same change had made harder to latch — a live door is a bad
    instrument for a decision table. Do not move a threshold back inside, and do
    not re-test a condition the verdict already weighed: two sources of truth
    for one question is how they drift.
22. **Age helpers use a signed delta, never `a > b`.** `sampleAgeMs()` and
    `advAgeMs()` compute `(int32_t)(now - then)` and clamp negatives to zero.
    Writing `(now > then) ? now - then : 0` looks equivalent and is not: at the
    `millis()` wrap it reports a sample from BEFORE the wrap as brand new, which
    would let a long-dead beacon read as present and open the door. Covered by
    `tests/host/test_proximity.cpp`, both directions.
23. **Never subtract a cross-task timestamp raw.** Use `advAgeMs()` /
    `sampleAgeMs()`. A timestamp written by the BLE task can sit a few ms AHEAD
    of the control task's `nowMs`, and `now - then` then underflows to ~2^32, so
    the freshest reading there is reads as 49 days old — which drops the fix and
    cancels a pending open. Clamping to 0 is the whole fix.

24. **Manual control is a FACT ABOUT A PERSON, not a position to assert.**
    `g_override` records that somebody moved this door — at the console, from
    the portal, or by hand (an `UNCOMMANDED` travel). While set, nothing
    automatic moves it in either direction, manual commands are never refused
    because of it, and it survives a reboot. Only `door auto` clears it.

    **It deliberately stores no target.** An earlier version stored the
    position to hold, and that was the bug: a door moved by hand, or rebooted,
    came back with the flag claiming one thing while the door did another — and
    the flag won, so "held open" could mean "shut, and the beacon may not open
    it". Where the door IS comes from its limit switches. The two can no longer
    disagree because only one of them is remembered.

    Treating an `UNCOMMANDED` travel as manual is safe **on this door** and not
    in general: the flap is a motorised vertical panel, so wind cannot blow it
    shut and an animal cannot push it. On a door where that is untrue, a gust
    would silently stop the door working. Say so before copying the rule.

    **It must count as IDLE for the uploader**, exactly as a maintenance window
    does — see the idle test. Omit that and an overridden door calls in only on
    the 30-minute heartbeat, going half-deaf to commands at the moment the only
    way to release it is to send one.

    It is a deliberate exception to invariant 10, recorded in
    `docs/STANDARD_EXCEPTIONS.md` with an owner and an expiry, and paid for
    with noise rather than a timer: `ovr=` in the status line, a `MANUAL`
    event, a distinct LED, the boot banner, and email on arrival plus a
    repeating reminder. **Do not add a countdown**: it has no clock.

## Conventions

- **Console output in `petdoor.ino` goes to `Con`, not `Serial`.** `Con` is a
  `Stream` that writes to the UART and, during a maintenance window, to an
  authenticated network client as well. A bare `Serial.print` still compiles and
  still works — it just cannot be read by anyone without a cable, which defeats
  the point. `wifi_logger.cpp` and `ble_scanner.cpp` deliberately keep using
  `Serial`: they log transport-level events, and routing those through a
  transport-dependent sink invites recursion.

- Config goes in `config.h` wrapped in `#ifndef`, never hard-coded at the use
  site. Users override via `secrets.h` or `-D` flags.
- Comments explain *why*, especially where a value protects an animal or a
  motor. The code is read by people wiring mains-adjacent relays.
- `static_assert` any config relationship that could silently misbehave.
- No dynamic allocation in the BLE hot path beyond what the Arduino `String`
  API forces; the discovery table is a fixed array with LRU eviction.
- Both Bluedroid and NimBLE must keep building. Everything the two stacks
  disagree about — class names, the callback signature, `start()`'s arguments,
  `String` vs `std::string` — lives in the stack-adapter block at the top of
  `ble_scanner.cpp`. The scanner logic below it is written once, as templates on
  the device type, and must stay stack-agnostic. Add new differences to the
  adapter, never `#if` in the middle of the logic.

## Documentation

`docs/` is written for a stranger with a soldering iron, not for us. If you
change wiring, pins, defaults, or safety behaviour, update the matching doc in
the same change — `docs/SAFETY.md` and `docs/WIRING.md` especially.

**Where the non-obvious documents live, and what each is FOR** — they look
overlapping and are not, and putting something in the wrong one is how it stops
being read:

| File | Holds | Does NOT hold |
|---|---|---|
| `docs/OBSERVABILITY.md` | the producer × sink table: which events reach the console, the buzzer, the log, the status line and email | anything about what the events mean |
| `docs/PARKING_LOT.md` | work blocked on an external event, and what would unblock it | anything that can be worked on now |
| `docs/STANDARD_EXCEPTIONS.md` | rules the repo does not meet, each with an owner and an **expiry** | anything blocked (that is the parking lot) |
| `SECURITY.md` | how to report a vulnerability, and the scope — including that a door refusing to open outranks any confidentiality issue | per-chapter evidence |
| `docs/SAFETY.md` | the **engineering** safety analysis: what the firmware protects against, what it does not, commissioning | warranty or support language — that is the disclaimer |
| `docs/DISCLAIMER.md` | who is **responsible**: no warranty, no support, what this is not, what the installer owns | anything about *how* to make it safe — that is SAFETY.md |
| `docs/PRIVACY.md` | what data exists and where: the event ring, the server's tables, and that a door log is an occupancy log | how to secure the dashboard — that is WEB-DASHBOARD.md |
| `mappings/ASVS-5.0.md` | the per-chapter ASVS position with the mechanism named for each | the generated 345-row matrix, which is the standard's own |
| `tools/logserver/caddy/` | the **live** vhost, verbatim, because it used to exist only on one VM | a generic example — that is `docs/WEB-DASHBOARD.md` |

Two rules about these that are easy to get wrong:

**Closing a deviation means DELETING its row**, not marking it done.
`STANDARD_EXCEPTIONS.md` is a list of what is currently untrue, and a row saying
"fixed" makes it longer without making it more informative.

**A new event needs the five questions in `OBSERVABILITY.md`**, in order. Three
bugs in one evening were the same shape — the door knew something and no sink a
person reads carried it. `drop=` was console-only on a door with no cable, a
sensor fault *clearing* was in no history, and a panic was entirely silent.
