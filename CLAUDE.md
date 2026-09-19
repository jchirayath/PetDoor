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

**Build with `PartitionScheme=min_spiffs`.** This project is flashed over serial and
never uses over-the-air updates, so the default scheme's second 1.3 MB app slot
is dead space. `no_ota` gives a single 2 MB app partition, which takes usage
from ~85% to ~53% and leaves room for features like WiFi. In the Arduino IDE it
is **Tools → Partition Scheme → Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)**; PlatformIO
picks it up from `board_build.partitions` in `platformio.ini`.

If you ever strain that, switch partitions again rather than trimming features
silently.

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

## Conventions

- Config goes in `config.h` wrapped in `#ifndef`, never hard-coded at the use
  site. Users override via `secrets.h` or `-D` flags.
- Comments explain *why*, especially where a value protects an animal or a
  motor. The code is read by people wiring mains-adjacent relays.
- `static_assert` any config relationship that could silently misbehave.
- No dynamic allocation in the BLE hot path beyond what the Arduino `String`
  API forces; the discovery table is a fixed array with LRU eviction.
- Both Bluedroid (classic ESP32) and NimBLE (S3/C3/C6) must keep building —
  only use BLE APIs common to both, or `#if defined(CONFIG_NIMBLE_ENABLED)`.

## Documentation

`docs/` is written for a stranger with a soldering iron, not for us. If you
change wiring, pins, defaults, or safety behaviour, update the matching doc in
the same change — `docs/SAFETY.md` and `docs/WIRING.md` especially.
