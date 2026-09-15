# Contributing

Thanks for looking. This is a small hobbyist project and pull requests are very
welcome — especially reports and fixes from people running it on a real coop.

---

## The most valuable contribution

**A report from a real deployment.** What beacon, what door mechanism, what
thresholds you ended up with, what went wrong, what surprised you. Open an
issue. This project has one reference build; every other build teaches it
something.

Second most valuable: a **safety** report. If you find a way this firmware can
injure an animal that [docs/SAFETY.md](docs/SAFETY.md) does not already list,
open an issue and label it `safety`. That matters more than any feature.

---

## Before you open a pull request

There is no test suite. **Verification means "it compiles for the target".**

```bash
ARDUINO_CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
"$ARDUINO_CLI" compile --fqbn esp32:esp32:esp32 petdoor
```

A standalone `arduino-cli` on `PATH` works identically. Target is **ESP32
Arduino core 3.x** (3.3.5 is what this was developed against).

Or with PlatformIO:

```bash
pio run
```

Say in the PR whether you tested on hardware, and on which board. "Compiles,
untested on hardware" is a perfectly acceptable and useful thing to write — it
just tells the reviewer what to check.

Flash usage sits around **83%** of the default partition scheme. If your change
pushes it much higher, say so, and switch partition schemes rather than quietly
trimming features.

---

## Things that will get a PR sent back

### Removing the `true` in `setAdvertisedDeviceCallbacks`

```cpp
g_scan->setAdvertisedDeviceCallbacks(&g_callbacks, true);
```

It looks redundant. It is not. It is the entire reason this project exists.
Removing it silently restores the bug the firmware was written to fix, with no
crash and no compile error — just a door that stops responding to where the
beacon is. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#the-duplicate-filter-trap).

**Keep the comment with the code.** The comment is what stops the next person
doing it.

### Hard-coding a tunable

Every tunable belongs in `config.h`, wrapped in `#ifndef`:

```c
#ifndef MY_NEW_SETTING
#define MY_NEW_SETTING 42
#endif
```

That is what lets users override it from `secrets.h` or a `-D` flag without
forking. Never hard-code at the point of use.

### Weakening a safety invariant

These are not incidental. Each protects an animal or a motor:

1. `RSSI_ENTER_DBM > RSSI_EXIT_DBM` — the gap is the hysteresis band.
2. `MIN_ACTUATION_INTERVAL_MS < EXIT_CONFIRM_MS` — or the lockout delays
   closing.
3. **A stale signal can close the door but never open it.**
4. **Never actuate before the beacon has been heard once since boot.** A dead
   beacon battery must not read as "absent".
5. **Never close during `BOOT_GRACE_MS`.** A power blip must not slam the door
   on an animal standing in it.
6. **Relays are interlocked.** `pulse()` always releases the opposite relay and
   waits `DIRECTION_CHANGE_GAP_MS` first.
7. **The BLE callback stays cheap and never blocks.** No `Serial` output, no
   blocking waits. Samples go over a queue; the discovery table is taken with a
   zero timeout and skipped if busy.
8. **Door state is owned by one task.** `controlTask` is the only caller of
   `DoorController`. Never actuate from `loop()` or from a callback.
9. **Opening is never rate-limited.** The actuation lockout applies to closing
   only. Reintroducing it on `requestOpen()` would delay reopening a door that
   just closed on an animal.
10. **Never subtract a cross-task timestamp raw.** Use `advAgeMs()` /
   `sampleAgeMs()`. A timestamp written by the BLE task can sit a few ms ahead
   of the control task's `nowMs`, and `now - then` underflows to ~2^32. See
   [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#timestamps-cross-a-task-boundary-never-subtract-them-raw).

If you have a good reason to change one, that is a discussion to have in an
issue first, not a surprise in a diff.

### Breaking the other BLE stack

Both **Bluedroid** (classic ESP32) and **NimBLE** (S3/C3/C6) must keep
building. Use only BLE APIs common to both, or guard with
`#if defined(CONFIG_NIMBLE_ENABLED)`.

### Renaming the sketch folder

`petdoor/` is simultaneously an Arduino sketch folder and the PlatformIO
`src_dir`. One copy of the source, both toolchains. The folder name and
`petdoor.ino` must stay in sync or the Arduino IDE stops recognising it.

---

## Conventions

- **`static_assert` any config relationship that could silently misbehave.** A
  compile error is a much better outcome than a door that behaves oddly once a
  month.
- **Comments explain *why*, not *what*** — especially where a value protects an
  animal or a motor. This code is read by people wiring mains-adjacent relays,
  and the reasoning is the part they cannot reconstruct from the source.
- **End every `printf` line with `\r\n`, not `\n`.** `Serial.println()` emits
  CRLF for you, but `Serial.printf()` does not. On a raw terminal (`screen`,
  `picocom`, `minicom`) a lone line-feed moves down a row without returning the
  carriage, so output staircases off to the right. The Arduino IDE's Serial
  Monitor hides this, which is exactly why it is easy to reintroduce.
- **No dynamic allocation in the BLE hot path** beyond what the Arduino `String`
  API forces. The discovery table is a fixed array with LRU eviction.
- Two-space indent, 100-column soft limit, to match what is there.
- Match the surrounding style rather than introducing a new one.

## Documentation

`docs/` is written for **a stranger with a soldering iron**, not for us.

If you change wiring, pins, defaults, or safety behaviour, **update the matching
doc in the same change** — [docs/SAFETY.md](docs/SAFETY.md) and
[docs/WIRING.md](docs/WIRING.md) especially. A PR that changes a default without
touching the docs will be sent back.

| File | Covers |
|---|---|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the signal becomes a decision |
| [HARDWARE.md](docs/HARDWARE.md) | Parts, boards, beacon selection |
| [WIRING.md](docs/WIRING.md) | Pinout, relay polarity, bench test |
| [BEACON-SETUP.md](docs/BEACON-SETUP.md) | Finding and configuring a beacon |
| [CONFIGURATION.md](docs/CONFIGURATION.md) | Every setting |
| [TUNING.md](docs/TUNING.md) | Choosing thresholds |
| [DIAGNOSTICS.md](docs/DIAGNOSTICS.md) | Serial console: connecting, commands, reading output |
| [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | When it does not work |
| [SAFETY.md](docs/SAFETY.md) | Read before connecting a motor |

---

## Never commit

- **`petdoor/secrets.h`.** It is git-ignored for a reason — it holds your
  beacon's address. Put new *examples* in `secrets.example.h` instead.
- Build output, `.pio/`, `build/`, `.DS_Store`.

If you are adding a new setting, add a commented example of it to
`secrets.example.h` and document it in
[docs/CONFIGURATION.md](docs/CONFIGURATION.md).

---

## Reporting a bug

Include:

- The **full serial output from boot**, including the banner.
- The output of the `s` command.
- Your `secrets.h` **with the MAC redacted**.
- Board, beacon model, relay module.
- What the door did versus what you expected.

## License

Contributions are accepted under the [MIT License](LICENSE), the same as the
rest of the project.
