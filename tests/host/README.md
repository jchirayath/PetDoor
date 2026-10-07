# Host tests

Unit tests and a fuzz target for the parts of the firmware that are pure logic,
built and run **on your machine** — no board, no Arduino toolchain, no beacon.

```bash
tests/host/run.sh                  # unit tests, short fuzz, adapter guard
tests/host/run.sh --fuzz 5000000   # longer fuzz budget
```

Three things run, and the third is not a test:

| | What it does |
|---|---|
| **unit** | `petdoor/beacon.cpp` against real frame layouts: valid frames, every truncated length, wrong frame types, signed/negative values, nullptr, round trips |
| **fuzz** | both parsers against arbitrary bytes under ASan/UBSan — coverage-guided where libFuzzer exists, a fixed-seed driver where it does not |
| **guard** | greps `ble_scanner.cpp` for the one conversion that must never come back (below) |

## Why this exists

A bug compiled cleanly and ran on a real door for weeks.

The NimBLE adapter converted binary advertisement payloads with
`String(s.c_str())`. Arduino `String` built that way stops at the first NUL —
and **both** frames this firmware parses carry a NUL in byte 2: Apple
manufacturer data begins `4C 00`, Eddystone TLM service data begins `20 00`. So
every frame arrived as **one byte** and nothing could ever parse it:

- Eddystone battery telemetry never decoded, so the `BEACON_LOW_BATTERY_MV`
  warning was **inert** — the mitigation for "a dead beacon battery must not
  read as absent" could not fire;
- iBeacon `UUID`/`major`/`minor` matching could never match, so a door
  configured that way would never see its beacon and **never actuate**, while
  still reporting itself configured;
- `measuredPower` was never read, so distance always used the fallback constant.

`beacon.h` documented the contract correctly — *"it may contain NULs; Arduino
String carries an explicit length, so that is safe"* — and Bluedroid honours it.
The NimBLE adapter was the single place that broke it. **"It compiles for the
target" cannot catch that. One unit test with a real payload catches it
immediately**, which is the entire argument for this directory.

## Why `beacon.*` specifically

It is the only attacker-controlled input the firmware has. Every byte arrives
from an unauthenticated BLE advertisement, so anyone within radio range chooses
them. The parsers index straight into the payload — `p[24]`, `p[13]` — so a
length check off by one is an out-of-bounds read inside the BLE host task,
which is also the task with the least heap headroom.

It is also, as `beacon.h` says, the file with no BLE stack dependency — pure
functions over bytes. That makes it free to test, so there was never a good
reason not to.

The fuzz target is the point of the sanitisers, not the search: ASan/UBSan turn
an off-by-one into a failed build rather than a field crash. A crash found here
becomes a new case in `test_beacon.cpp`.

## `Arduino.h`

A shim, deliberately only as large as `beacon.cpp` needs. It is not an Arduino
emulator — if a new call appears in `beacon.cpp` the host build fails loudly and
the shim grows by one method, which is the behaviour we want. It reproduces the
one semantic that matters: `String(const char *)` stops at a NUL and
`String(buf, len)` does not.

## What is not covered

Everything that needs the chip or the radio: the actuation state machine
(`actuator.*`), the proximity filters (`proximity.*` — testable in principle,
not yet done), the BLE scanner itself, and anything touching NVS, WiFi or a
relay. Those are still verified by compiling and by running the door. The
firmware's safety invariants are additionally checked by CI, which asserts the
`static_assert`s actually refuse to compile — see `.github/workflows/build.yml`.
