# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

ESP32 firmware that opens and closes a chicken coop / pet door by pulsing two
relays, based on the proximity of a BLE beacon (a Minew beacon in the reference
build). Public repo, MIT licensed, aimed at hobbyists who will copy it onto
their own hardware.

## Build and verify

There is no test suite — verification means "it compiles for the target".
Always compile before claiming a change works:

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
| default (Bluedroid + WiFi) | 1,811,907 | 92% |
| `-DPETDOOR_USE_NIMBLE=1` | 1,355,435 | 68% |
| `-DPETDOOR_ENABLE_WIFI=0` | 1,152,923 | 58% |
| both | 694,099 | 35% |

**The default build is at 92% and that is close enough to matter.** The network
console and maintenance mode cost ~51 KB. There is room for a little more, but
anyone adding a feature to the Bluedroid build should check this number rather
than assume; `PETDOOR_USE_NIMBLE=1` buys back 24 percentage points and is the
recommended way out of a full image.

Measure with `compiler.cpp.extra_flags`, **not** `build.extra_flags` — the
latter carries `-DCORE_DEBUG_LEVEL`, `-DESP32` and the loop/event core settings,
and overriding it drops them and inflates the image by ~33 KB.

`PETDOOR_USE_NIMBLE=1` needs the NimBLE-Arduino library installed and is
opt-in; the default build must keep working with no extra libraries. It has been
validated on the reference hardware: same target sample rate as Bluedroid, and
free heap 66 KB -> 138 KB (low-water 7.9 KB -> 88 KB), with WiFi, NTP, signed
upload and OTA all working. Do not quote the static-RAM delta as the benefit —
it is heap, and it is an order of magnitude larger. **Both
stacks must keep compiling** — check both before claiming a BLE change works:

```bash
"$ARDUINO_CLI" compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs petdoor
"$ARDUINO_CLI" compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  --build-property "build.extra_flags=-DPETDOOR_USE_NIMBLE=1" petdoor
```

Do not flash hardware unless the user explicitly asks. Uploading drives a real
motor attached to a real door.

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
| `door.*` | relay pulses, interlock, lockout, boot grace |
| `beacon.*` | iBeacon parsing, classification, distance estimate — pure functions |
| `chime.*` | Optional buzzer: non-blocking patterns, pin/type discovery at runtime |
| `position.*` | Optional limit switches: debounce, measured state. Never commands the motor |
| `maintenance.*` | Bounded window in which the beacon cannot move the door; RSSI histogram for calibration |
| `console.*` | The console as a Stream, fanned out to the UART and a window-bounded network client |

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
8. **The BLE callback stays cheap and never blocks.** It runs in the Bluedroid
   task. Samples go over a queue; the discovery table is taken with a zero
   timeout and skipped if busy. Do not add `Serial` output or blocking waits to
   it.
9. **Door state is owned by one task.** `controlTask` is the only caller of
   `DoorController`. Do not actuate from `loop()` or from a callback.
10. **A maintenance window expires by itself.** It is stored as a deadline, is
    bounded by `MAINT_MAX_MS`, and is never written to NVS — a door left inert
    by a forgotten flag, a lost network or a brownout is a door that cannot let
    an animal in. It also blocks *both* directions, unlike the lock, because the
    person calibrating is standing at the door holding the collar.
11. **The network console never listens outside a maintenance window**, and
    never without `CONSOLE_PASSWORD`. It is the full console, so it can open the
    door. Do not start it at boot.

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
