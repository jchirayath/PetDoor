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
comes from an unauthenticated advertisement — so it is fuzzed under ASan/UBSan
as well as unit tested. Most of the firmware still has no tests; adding them to
`proximity.*` is the obvious next step, since it is pure logic too.

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

Flat, controller and motor connected, both reeds + vibration + buzzer fitted.
Pins: relays 16/17 **active HIGH**, LED 23, reeds **32**/**25**, buzzer **27**
(passive), vibration **33**. None of the sensor pins are compiled-in defaults —
they are set at runtime and saved on the device.

| | |
|---|---|
| travel, reed to reed | open **10,203 ms**, close **11,229 ms** |
| repeatability | within ~220 ms across separate travels |
| travel, closed BY HAND | **12,500 ms** — measured as vibration, not reeds |
| relay pulse | **1,500 ms** stored on the device (500 ms is swallowed) |
| vibration while moving | ~2,900 edges/s; **0** at rest |

Reed-to-reed is shorter than the stopwatch figures in `docs/REQUIREMENTS.md`
§1, and correctly so: a reed makes before the door reaches its physical stop.
It is also the number the arrival deadline wants.

A hand is slower than the motor, which is why `VIBRATION_TRAVEL_MAX_PCT` is the
loose end of the band: 12.5 s against a motorised 11.2 s is 111%, comfortably
inside, and the inference in `concludeVibrationRun()` was verified on this door
with the closed reed unplugged. Do not tighten that bound to flatter the
motorised figure — the travels this code exists to notice are the hand-driven
ones.

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

Git-ignored, and denied in `.claude/settings.json`, because this is a public
repo. It typically carries `BEACON_MAC`, `RSSI_ENTER_DBM`, `RSSI_EXIT_DBM`,
`RELAY_ACTIVE_LOW`, `OTA_PASSWORD`, `CONSOLE_PASSWORD` and the log-server
credentials.

**The deny is a guardrail, not a sandbox — do not treat it as a guarantee.**
`Read(./petdoor/secrets.h)` genuinely stops the Read tool, and a dozen
`Bash(<tool>:*secrets.h*)` rules stop the obvious shell equivalents. But those
match the COMMAND TEXT, not the file, so anything that reaches the file without
spelling its name walks straight through — `grep -r PASSWORD petdoor/` is
allowed by `Bash(grep:*)` and prints the line. That one cannot be closed by a
substring rule without banning recursive grep altogether.

So the rule that actually protects these values is a behavioural one:

**Do not read `secrets.h`, and do not route around the deny to do it.** Ask the
user to read a value out, or to extract it into a file you can use without
seeing. One session needed `OTA_PASSWORD`, wrote the `sed` one-liner to pull it
out, and then handed the command to the user rather than running it — that is
the expected behaviour, and it came from judgement rather than from the config.

**Ask rather than theorise.** `RELAY_ACTIVE_LOW` was a leading hypothesis for
over an hour of one session for want of a question that would have taken one
exchange.


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
    door. Do not start it at boot.
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
