# Configuration

Every setting, what it does, and when you would change it.

---

## Three places a setting can come from

Later wins:

```
  1. config.h          compiled-in default
  2. secrets.h / -D    your build-time override
  3. the device (NVS)  set from the serial console — survives reflashing
```

**Most of what you will actually tune is now settable from the console and
stored on the device**, so you do not reflash to change it:

| Console | Sets | Persists |
|---|---|---|
| `m` | Beacon MAC list | yes |
| `t` | Open/close thresholds | yes |
| `w` | Dwell times, lockout, interlock gap | yes |
| `f` | Filter window and smoothing | yes |

Each menu shows whether the value in force is `compiled in` or `saved on
device`, and each has a `clear` option that forgets the stored value and falls
back to the compiled-in default. The boot banner shows the same.

This matters most on a board with no auto-reset wiring, where every flash means
a manual button sequence — see
[DIAGNOSTICS.md](DIAGNOSTICS.md#flashing-a-board-with-no-auto-reset).

## How build-time overriding works

All defaults live in [`petdoor/config.h`](../petdoor/config.h), and every one is
wrapped in `#ifndef`:

```c
#ifndef RSSI_ENTER_DBM
#define RSSI_ENTER_DBM -65
#endif
```

`config.h` includes `secrets.h` first, if it exists. So anything you define in
`secrets.h` wins, and you never have to edit `config.h` at all.

**Preferred — `secrets.h`:**

```bash
cp petdoor/secrets.example.h petdoor/secrets.h
```

```c
#define BEACON_MAC     "ac:23:3f:11:22:33"
#define RSSI_ENTER_DBM -60
#define RELAY_ACTIVE_LOW 1
```

`secrets.h` is git-ignored, so your beacon's address stays out of the public
repo and `git pull` never clobbers your tuning.

**Also works — build flags.** Useful in CI or for a quick experiment:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 \
  --build-property "compiler.cpp.extra_flags=-DRSSI_ENTER_DBM=-60" petdoor
```

In `platformio.ini`:

```ini
build_flags =
    -DBEACON_MAC='"aa:bb:cc:dd:ee:ff"'
    -DRSSI_ENTER_DBM=-60
```

Note the quoting for string values — the shell, the build system and the
preprocessor all want their share.

> Do not hard-code values at the point of use. Anything tunable belongs in
> `config.h` wrapped in `#ifndef`, so users can override it without forking.

---

## 1. Beacon identity

Which BLE device is "the key". Matchers are **ANDed**: every matcher you enable
must match. Leave one at its placeholder to disable it.

If **no** matcher is configured, the firmware boots into discovery mode, scans,
prints everything it hears, and **never touches the door**.

| Setting | Default | Description |
|---|---|---|
| `BEACON_MAC` | `""` | Beacon MAC address, upper or lower case. `""` disables MAC matching. Accepts a **comma-separated list** — any listed address matches. The simplest and most reliable matcher. |
| `BEACON_MAX_MACS` | `4` | How many addresses `BEACON_MAC` may list. Each costs 18 bytes of RAM and is walked in the BLE callback. |
| `BEACON_UUID` | `""` | iBeacon 128-bit UUID, with or without dashes. `""` disables iBeacon matching. |
| `BEACON_MAJOR` | `-1` | iBeacon major. `-1` means "any". Only consulted when `BEACON_UUID` is set. |
| `BEACON_MINOR` | `-1` | iBeacon minor. `-1` means "any". |

**Several beacons.** `BEACON_MAC` takes a comma-separated list, and **any**
listed address opens the door — one beacon per animal, or a spare you can swap
in without reflashing:

```c
#define BEACON_MAC "ac:23:3f:11:22:33, ac:23:3f:11:22:44"
```

Separators may be commas, semicolons or spaces. Case does not matter. Raise
`BEACON_MAX_MACS` past 4 if you need more; anything beyond the limit is ignored
with a warning at boot.

The list is ORed internally but still **ANDed with `BEACON_UUID`** — it means
"any of these addresses", not "any of these, or the UUID".

**Which should you use?**

- **MAC only** is right for most builds. One beacon, fixed address, done.
- **UUID/major/minor** lets you swap in a replacement beacon without reflashing
  the ESP32 — program the new beacon with the same identity and it just works.
  Useful if the beacon lives on a collar and might get lost.
- **Both** is the strictest: the beacon must have that MAC *and* advertise that
  iBeacon identity.

A MAC that is all zeros, dashes, colons or spaces is treated as "not
configured", so the placeholder in `secrets.example.h` does not accidentally
count as a real target.

> A phone or an AirTag cannot be used. They rotate their Bluetooth address every
> few minutes specifically to prevent this kind of tracking. You need a beacon
> that advertises a fixed address — see [HARDWARE.md](HARDWARE.md).

---

## 2. Proximity

How near is "near". See [TUNING.md](TUNING.md) for the procedure.

| Setting | Default | Description |
|---|---|---|
| `RSSI_ENTER_DBM` | `-65` | Filtered signal at or above this counts as near. |
| `RSSI_EXIT_DBM` | `-75` | Filtered signal at or below this counts as far. |
| `ENTER_CONFIRM_MS` | `1500` | How long "near" must hold before the door opens. |
| `EXIT_CONFIRM_MS` | `15000` | How long "far" must hold before the door closes. |
| `SAMPLE_MAX_AGE_MS` | `3000` | An advertisement older than this stops counting as a live reading. |
| `MIN_SAMPLES_FOR_FIX` | `3` | Ignore the beacon until this many samples have arrived, so one stray packet can never move the door. |
| `RSSI_MEDIAN_WINDOW` | `7` | Median filter width. Must be **odd, 3–15**. Bigger = steadier but slower. |
| `RSSI_EWMA_ALPHA` | `0.35f` | Smoothing applied after the median, 0.0–1.0. Lower = smoother and slower. |
| `PATH_LOSS_EXPONENT` | `2.5f` | For the displayed distance estimate only: ~2.0 open air, 2.5–3.0 through a coop wall, 3.0+ cluttered. **Never affects the open/close decision**, which uses RSSI directly. |
| `BEACON_LOW_BATTERY_MV` | `2400` | Beacon battery level (from Eddystone-TLM) at or below which the status LED flashes at 2 Hz. `0` disables the warning. |
| `BEACON_MEASURED_POWER_DBM` | `-59` | Fallback calibrated RSSI at 1 m, used for the distance display when the beacon does not advertise one. iBeacon frames carry this; **Eddystone and sensor tags do not** and show `?` without it. Set to `0` to go back to `?`. Display only. |

**`RSSI_ENTER_DBM` must be greater (less negative) than `RSSI_EXIT_DBM`.** The
gap between them is the hysteresis band that stops the door flapping. A
`static_assert` in `proximity.cpp` catches it if you get this backwards.

The dwell times are deliberately asymmetric. Opening is fast because arriving
should feel responsive; closing is slow because a false close is far worse than
a late close.

---

## 3. Door and relay

**Read [SAFETY.md](SAFETY.md) and [WIRING.md](WIRING.md) before changing
anything here.**

| Setting | Default | Description |
|---|---|---|
| `PIN_RELAY_OPEN` | `16` | GPIO pulsed to open. Must differ from `PIN_RELAY_CLOSE`. |
| `PIN_RELAY_CLOSE` | `17` | GPIO pulsed to close. |
| `PIN_STATUS_LED` | `23` | Status LED, active high. |
| `RELAY_ACTIVE_LOW` | `0` | `1` for the common blue relay boards, whose coil energises when the input is pulled to GND. `0` for active-high boards and MOSFET drivers. **Getting this wrong means the door runs backwards or runs constantly.** |
| `RELAY_PULSE_MS` | `200` | Momentary pulse length. Raise only if your motor needs the contact held for the whole travel — see [WIRING.md](WIRING.md#momentary-pulse-vs-held-contact). |
| `MIN_ACTUATION_INTERVAL_MS` | `5000` | Minimum gap before the door may **close** again. Protects the motor from thrash. **Opening is never rate-limited** — delaying an open is the one direction that can strand an animal outside a door it just watched close. |
| `DIRECTION_CHANGE_GAP_MS` | `250` | Dead time before asserting a relay, with the opposite one released. Both relays energised at once is a short across the motor's direction contacts. |
| `BOOT_GRACE_MS` | `30000` | The door is never driven closed for this long after boot. Prevents a power blip from slamming the door on an animal standing in it. |

**`MIN_ACTUATION_INTERVAL_MS` must be shorter than `EXIT_CONFIRM_MS`**, or the
lockout delays closing. Also `static_assert`ed.

---

## 4. BLE scanning

| Setting | Default | Description |
|---|---|---|
| `SCAN_INTERVAL_MS` | `100` | How often a scan window starts. |
| `SCAN_WINDOW_MS` | `99` | How long the radio listens within each interval. Must be **≤ `SCAN_INTERVAL_MS`**. |
| `SCAN_ACTIVE` | `1` | Active scanning also requests scan-response data, which is what carries the device name on most beacons. `0` for passive: slightly lower power, no names. |
| `SCAN_WATCHDOG_MS` | `15000` | If not one advertisement arrives from **any** device for this long, the stack is wedged — tear the scan down and restart it. |

Window ≈ interval gives a near-100% duty cycle. Combined with duplicate
reporting (see [ARCHITECTURE.md](ARCHITECTURE.md#the-duplicate-filter-trap)),
that is what produces a steady sample stream rather than a single reading.

Lowering the duty cycle saves a little power at the cost of a slower, noisier
sample stream. It is rarely worth it on a mains-powered coop controller.

---

## 5. Diagnostics

| Setting | Default | Description |
|---|---|---|
| `SERIAL_BAUD` | `115200` | Serial console speed. |
| `CONTROL_TICK_MS` | `100` | Control loop **idle** period. The loop normally wakes the moment a BLE sample arrives; this only caps how long it waits when the beacon is silent. It also sets how often the status LED is refreshed, so it bounds the shortest LED pattern that can be rendered — much above 250 ms and the blip and fault flutter visibly break. |
| `DEVICE_TABLE_SIZE` | `40` | Max distinct devices held in the discovery table. Costs RAM and BLE-callback time. |
| `TABLE_MIN_UPDATE_MS` | `500` | Per-device throttle on discovery-table writes, so a busy RF environment cannot flood the heap from the BLE callback. |
| `EVENT_LOG_CAPACITY` | `128` | Events kept in the persistent NVS log. Each entry is 16 bytes and the whole ring is rewritten on every event, so keep it modest — 128 is 2 KB and covers weeks of a door cycling a few times a day. |
| `DISCOVER_DUMP_INTERVAL_MS` | `2000` | How often discovery mode prints the table. |
| `CALIBRATE_INTERVAL_MS` | `500` | How often calibration mode prints a reading. |
| `ALLOW_MANUAL_SERIAL_CONTROL` | `1` | Allow `o` / `x` to drive the relays directly. Invaluable while wiring. **Set to `0` for an unattended deployment** — these commands bypass the proximity logic, the lockout *and* the boot grace window. |

---

## Worked examples

### A small coop, beacon on a collar

Short range, and you want the door open only when the bird is genuinely at the
door:

```c
#define BEACON_MAC       "ac:23:3f:11:22:33"
#define RSSI_ENTER_DBM   -58
#define RSSI_EXIT_DBM    -70
#define ENTER_CONFIRM_MS 1000
```

### A big run, generous range

The door should be open whenever the flock is anywhere in the run:

```c
#define BEACON_MAC     "ac:23:3f:11:22:33"
#define RSSI_ENTER_DBM -75
#define RSSI_EXIT_DBM  -88
#define EXIT_CONFIRM_MS 30000
```

### Noisy RF environment

Neighbours, wifi, a dozen Bluetooth devices. Trade responsiveness for stability:

```c
#define RSSI_MEDIAN_WINDOW 11
#define RSSI_EWMA_ALPHA    0.2f
#define ENTER_CONFIRM_MS   3000
```

### Two birds, two beacons

```c
#define BEACON_MAC "ac:23:3f:11:22:33, ac:23:3f:11:22:44"
```

The door opens when **either** is near, and only closes once **both** have been
gone for `EXIT_CONFIRM_MS`.

### A beacon that shows "?" instead of a distance

Eddystone beacons and sensor tags carry no calibrated power. Measure it — hold
the beacon 1 m away, run `c`, read the *filtered* value — then:

```c
#define BEACON_MEASURED_POWER_DBM -59
```

### Commissioned and unattended

```c
#define ALLOW_MANUAL_SERIAL_CONTROL 0
```

### An active-low relay board

The most common single fix:

```c
#define RELAY_ACTIVE_LOW 1
```
