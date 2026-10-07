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

**Two things, and the first needs no hardware:**

```bash
tests/host/run.sh      # unit tests + fuzz for the payload parsers

ARDUINO_CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
"$ARDUINO_CLI" compile --fqbn esp32:esp32:esp32 petdoor
```

`tests/host/run.sh` needs no hardware and no Arduino toolchain. It runs **180
checks** and a fuzz pass under ASan/UBSan:

| | |
|---|---|
| `beacon.*` | the advertisement parsers — the only attacker-controlled input the firmware has, so also fuzzed |
| `proximity.*` | the filters and the presence state machine, including both `millis()`-wrap cases |
| `sensor_verdict.h` | the two sensor-health decisions, whole decision tables |
| a lint rule | fails the build if `String(x.c_str())` returns to the BLE adapter |

It exists because a bug that compiled cleanly ran on a real door for weeks — see
[tests/host/README.md](tests/host/README.md). `tests/server/run.sh` adds 53 checks
on the log server's alerting.

**The rest of the firmware is verified on hardware**, because it needs a chip, a
radio and a relay: you cannot unit-test "the relay pulse was long enough for this
vendor controller to notice". So if you are changing something that is pure logic,
**lift it out and test it** — `petdoor/sensor_verdict.h` exists for exactly that
reason. If you are changing something that is not, say in the PR what you ran on
what hardware.

A standalone `arduino-cli` on `PATH` works identically. Target is **ESP32
Arduino core 3.x** (3.3.5 is what this was developed against).

Or with PlatformIO:

```bash
pio run
```

Say in the PR whether you tested on hardware, and on which board. "Compiles,
untested on hardware" is a perfectly acceptable and useful thing to write — it
just tells the reviewer what to check.

Flash usage sits at **70%** of `min_spiffs`, which is the scheme this firmware
requires — the default does not fit at all. Bluedroid
(`-DPETDOOR_USE_NIMBLE=0`) is at **94%** with ~114 KB spare and is the
configuration that will break first, so check it before and after any sizeable
change, not just at the end. If your change pushes either much higher, say so.

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

**The full list lives in [CLAUDE.md](CLAUDE.md#invariants--do-not-regress-these)
— 23 of them, each with the incident behind it.** This file used to carry its own
copy, which drifted to eleven and still named a function that had been deleted; one
list is the fix, not two tidier ones.

They are not incidental. Each protects an animal or a motor, and most were written
after something went wrong on a real door. The shape of them:

- **Thresholds and timings have relationships**, and `static_assert` enforces the
  ones that could misbehave silently — the hysteresis band, the lockout against the
  exit dwell, the fast filter never being slower than the slow one.
- **A stale or absent signal can close the door but never open it**, and nothing
  actuates before the beacon has been heard once since boot. A flat beacon battery
  must not read as "the animal has gone".
- **One task owns the door.** `controlTask` is the only caller of `Actuator`, and
  `Actuator` is the only caller of `DoorController::press()`/`commit()`. Never
  actuate from `loop()` or from a callback.
- **Opening is never rate-limited.** The actuation lockout applies to closing only;
  `check()` returns early for `DOOR_OPEN` before it ever consults `lockedOut()`.
  Delaying an open is the one direction that can trap an animal.
- **A stalled close fails open**, reversed rather than retried, and says so
  everywhere when it gives up. A door that silently stopped trying is worse than
  one that is visibly open.
- **The BLE callback stays cheap, never blocks, and allocates nothing for a device
  that is not the target.** Running out of heap in that task is what used to panic
  doors.
- **Timestamps are never subtracted raw**, because one task stamps them and another
  reads them. Use `sampleAgeMs()` / `advAgeMs()`.

If you have a good reason to change one, that is a discussion to have in an issue
first, not a surprise in a diff.

### Breaking the other BLE stack

Both **NimBLE** (the default, every chip) and **Bluedroid**
(`PETDOOR_USE_NIMBLE 0`, classic ESP32 only) must keep building. Use only BLE
APIs common to both, or guard with `#if defined(CONFIG_NIMBLE_ENABLED)`.
Check both before claiming a BLE change works — the default no longer exercises
the Bluedroid path, so it is the one that will rot unnoticed.

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
