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
| `PETDOOR_VERSION` | `"1.0.0"` | Firmware version, shown in the banner and in `s`, and sent with every upload so a server collecting from several doors can tell which build produced an event. Bump it when you change behaviour someone might need to correlate against. A companion `PETDOOR_BUILD` is set by the compiler from `__DATE__`/`__TIME__` and is not a setting — it tells two builds of the same version apart. |
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
| `RSSI_MEDIAN_WINDOW` | `7` | Median filter width for the **close** decision. Must be **odd, 3–15**. Bigger = steadier but slower. |
| `RSSI_EWMA_ALPHA` | `0.35f` | Smoothing after the median for the **close** decision, 0.0–1.0. Lower = smoother and slower. |
| `RSSI_FAST_WINDOW` | `1` | Median width for the **open** decision. Odd, **1–15** — 1 is allowed here, because the open path is confirmed by `ENTER_CONFIRM_MS` rather than by smoothing. |
| `RSSI_FAST_ALPHA` | `0.9f` | Smoothing for the **open** decision. Higher = faster. |
| `PATH_LOSS_EXPONENT` | `2.5f` | For the displayed distance estimate only: ~2.0 open air, 2.5–3.0 through a coop wall, 3.0+ cluttered. **Never affects the open/close decision**, which uses RSSI directly. |
| `BEACON_LOW_BATTERY_MV` | `2400` | Beacon battery level (from Eddystone-TLM) at or below which the status LED flashes at 2 Hz. `0` disables the warning. |
| `BEACON_MEASURED_POWER_DBM` | `-59` | Fallback calibrated RSSI at 1 m, used for the distance display when the beacon does not advertise one. iBeacon frames carry this; **Eddystone and sensor tags do not** and show `?` without it. Set to `0` to go back to `?`. Display only. |

### BLE host stack

| Setting | Default | Meaning |
|---|---|---|
| `PETDOOR_USE_NIMBLE` | `1` | Leave it alone. `1` = NimBLE, the stack this firmware uses, via the **NimBLE-Arduino** library. `0` selects the core's old Bluedroid stack, kept only as a fallback — see below for why we stopped using it. |
| `NIMBLE_SCAN_RSP_TIMEOUT_MS` | `100` | NimBLE only. How long to wait for a scan response before reporting the advertisement anyway. |

We ran Bluedroid, the stack bundled with the Arduino core, for most of this
project's life. A door in service then panicked on three consecutive boots,
and the cause was heap: its low-water mark was under 7 KB, where NimBLE leaves
80 KB. We moved and did not look back.

Both drive the same radio through the same controller; only the host changes,
and Bluedroid is over half the firmware image. Measured on `min_spiffs`
(1.875 MB app partition):

| `PETDOOR_USE_NIMBLE` | `PETDOOR_ENABLE_WIFI` | flash | | RAM |
|---|---|---|---|---|
| **`1`** *(default)* | `1` *(default)* | **1,358,607** | **69%** | 69,184 |
| `0` | `1` | 1,815,239 | 92% | 74,400 |
| **`1`** | `0` | **694,099** | **35%** | 40,880 |
| `0` | `0` | 1,152,923 | 58% | 46,204 |

NimBLE alone saves **454 KB of flash**. The RAM column above is only *static*
allocation, and it badly understates the benefit — most of what Bluedroid costs
is heap it takes at runtime. Measured on the reference build (ESP32 WROOM-32,
Minew beacon, WiFi on, same NVS settings, same beacon, back-to-back):

| | Bluedroid | NimBLE |
|---|---|---|
| free heap | 62,824 | **134,144** |
| heap low-water | **6,968** | 80,152 |

**+72 KB of free heap, and eleven times the headroom at the low-water mark.**
That second number is the one that matters: under Bluedroid this firmware ran
within 7 KB of exhaustion, which is why `LOG_ALLOW_TLS` had to be made opt-in —
the TLS handshake could not get a buffer. With NimBLE there is room.

**This is why NimBLE became the default.** A door in service panicked on three
consecutive boots, reporting reset reason 4. The cause was that low-water mark:
an allocation failing at 7 KB takes the chip down. Bluedroid is still supported
and still builds, but it has no headroom left once WiFi, the uploader, OTA and
the network console are all resident, so it is no longer what a new build
should get by default.

With WiFi compiled out as well the image is a third of its default size.

**Detection is not faster, and was never going to be.** Measured over matched
9-minute windows, the target beacon yielded 1.93 samples/sec on Bluedroid and
1.89 on NimBLE — the same number within noise. Sample rate is set by how often
the beacon advertises, not by the host stack. What NimBLE buys is space and
heap, not speed.

Total advertisement callbacks are lower on NimBLE (50.2/sec versus 60.5), which
is expected rather than a regression: Bluedroid raises a callback for the
advertisement *and* another for the scan response, where NimBLE raises one per
pair. The target sample rate — the number the door actually uses — is unchanged.

WiFi, NTP sync, HMAC-signed upload and ArduinoOTA were all verified working on
the NimBLE build. Whether NimBLE is more or less prone to losing the beacon is
**not yet established**: the two windows above saw different amounts of beacon
movement, and WiFi upload bursts degrade BLE sampling on either stack, so a
short comparison cannot separate the causes.

Turning it on is one line in `secrets.h`:

```c
#define PETDOOR_USE_NIMBLE 1
```

…plus installing **NimBLE-Arduino** (2.5.1 or later) from Library Manager. The
boot banner and `s` both report which stack is running, so there is never any
doubt.

**Why it is opt-in rather than automatic.** Switching stacks changes the code
driving the radio on a device that moves a door. That should be a decision you
made, not a side effect of which libraries happen to be sitting in your
`libraries` folder — and the default build has to keep working for someone who
has installed nothing.

**`NIMBLE_SCAN_RSP_TIMEOUT_MS` is not a knob you should need**, but it is not
safe to remove. NimBLE's own default is 10240 ms, chosen for scans that finish;
this firmware scans endlessly. A beacon advertising as scannable
(`ADV_IND`/`ADV_SCAN_IND`) that does not answer scan requests would be withheld
for 10.24 s at a time — past `SAMPLE_MAX_AGE_MS` (3000), so the door would read
"no fix" permanently. Broadcast-only beacons are reported immediately and never
touch this path.

---

### Why there are two filters

The four `RSSI_*` filter settings above are two pairs, not four knobs. The same
stream of advertisements is filtered twice — slowly for the close decision,
quickly for the open decision.

That split exists because the two directions want opposite things. Opening late
can shut an animal out of the coop; opening early only lets in a draught.
Closing early can shut a door on an animal. One filter has to be tuned somewhere
between "reacts now" and "ignores dropouts", and whichever way you tune it, one
of those two directions pays.

The practical difference, measured over a 1 Hz beacon:

| | one filter, tuned fast (3 / 0.70) | one filter, shipped (7 / 0.35) | two filters (shipped) |
|---|---|---|---|
| Door opens after the animal arrives | 3500 ms | 7500 ms | **1500 ms** |
| Survives a 4-second signal fade | **no — closes** | yes | yes |
| Cancels a pending close on return | 1000 ms | 4000 ms | **0 ms** |

1500 ms is `ENTER_CONFIRM_MS`. The open path now adds no lag of its own at all;
the dwell timer is the only thing left between a strong signal and an open door.

The knock-on effect is that `EXIT_CONFIRM_MS` can be much shorter safely. With a
single filter the exit dwell has to be at least as long as the worst fade you
want to ride out. With two, the slow median absorbs the fade first:

| Fade to survive | Minimum `EXIT_CONFIRM_MS`, one filter (3 / 0.70) | …with two filters |
|---|---|---|
| 2 s | 2000 ms | 500 ms |
| 4 s | 4000 ms | 500 ms |
| 8 s | 8000 ms | 3000 ms |

**The open pair must never be slower than the close pair** — `RSSI_FAST_WINDOW ≤
RSSI_MEDIAN_WINDOW` and `RSSI_FAST_ALPHA ≥ RSSI_EWMA_ALPHA`. Reversing them
would make the door notice an animal leaving before it noticed one arriving. A
`static_assert` catches it at compile time and the `f` console menu refuses it at
runtime.

To go back to the old single-filter behaviour, set the open pair equal to the
close pair (`open same` in the `f` menu).

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
| `PIN_BUZZER` | `-1` | Optional annunciator. `-1` disables it. Ticks while the door is travelling, chimes when `DOOR_TRAVEL_MS` is up, buzzes once if a locked door refuses the collar. Runtime-settable and saved on the device, so an undocumented board's buzzer pin can be found by trying it. See [WIRING.md](WIRING.md#the-annunciator). |
| `BUZZER_PASSIVE` | `0` | `0` = active buzzer (own oscillator, sounds on DC). `1` = passive transducer (needs a square wave). Guessing wrong is harmless; it just sounds wrong. |
| `BUZZER_ACTIVE_LOW` | `0` | Set to `1` for a board that sounds its buzzer when the pin is pulled to GND. Symptom of getting it wrong: continuous screaming from boot that goes *quiet* during a chime. Active buzzers only. |
| `PIN_SENSOR_OPEN` | `-1` | Limit switch closed when the door is fully OPEN. `-1` = not fitted, which is the default and makes the whole feature inert. Runtime-settable with `sensors <open> <closed>` and saved on the device, so switches can be added without a reflash. See [WIRING.md](WIRING.md#position-sensors). |
| `PIN_SENSOR_CLOSED` | `-1` | Limit switch closed when the door is fully CLOSED. |
| `SENSOR_ACTIVE_LOW` | `1` | `1` = switch shorts the pin to GND, pin idles high on an internal pull-up. Almost always what you want: a broken wire then reads as "not at that end" rather than a false arrival. `0` needs your own pull-down. |
| `SENSOR_DEBOUNCE_MS` | `50` | A reed switch chatters as the magnet passes and a door settling bounces it. Without this the door announces three arrivals for one. |
| `RELAY_ACTIVE_LOW` | `0` | `1` for the common blue relay boards, whose coil energises when the input is pulled to GND. `0` for active-high boards and MOSFET drivers. **Getting this wrong means the door runs backwards or runs constantly.** |
| `RELAY_PULSE_MS` | `1000` | Momentary pulse length — how long the relay holds the controller's button down. **Not a margin:** on the reference controller a 500 ms press is *swallowed* and 1000 ms works, every time. This was 200 ms before anyone measured it. **Adjustable at runtime** with `w` → `pulse <ms>` (50–10000), saved on the device. See [SAFETY.md](SAFETY.md#a-press-too-short-to-register). |
| `RELAY_PULSE_COUNT` | `1` | Presses inside a single press, 1–3. **Leave it at 1.** It is the old blind double-press and it *stacks* with the wake press below, so at 2 a cold actuation sends four presses — and a press mid-travel reads as STOP. Kept only for a controller the wake logic cannot handle. |
| `RELAY_PULSE_GAP_MS` | `1000` | Gap between repeated presses, 200–5000 ms. Only used when `RELAY_PULSE_COUNT` > 1. |
| `MIN_ACTUATION_INTERVAL_MS` | `5000` | Minimum gap before the door may **close** again. Protects the motor from thrash. **Opening is never rate-limited** — delaying an open is the one direction that can strand an animal outside a door it just watched close. |
| `DIRECTION_CHANGE_GAP_MS` | `250` | Dead time before asserting a relay, with the opposite one released. Both relays energised at once is a short across the motor's direction contacts. |
| `BOOT_GRACE_MS` | `30000` | The door is never driven closed for this long after boot. Prevents a power blip from slamming the door on an animal standing in it. Cannot be overridden by hand — not by `o`/`x`, not from the dashboard. |

**`MIN_ACTUATION_INTERVAL_MS` must be shorter than `EXIT_CONFIRM_MS`**, or the
lockout delays closing. Also `static_assert`ed.

### 3b. The actuation attempt

These govern what happens *after* the relay fires: whether the controller was
awake, whether the door moved, whether it arrived, and what to do when it did
not. The reasoning behind each is in
[ARCHITECTURE.md](ARCHITECTURE.md#the-actuation-path) and
[SAFETY.md](SAFETY.md#the-controller-may-not-be-awake).

| Setting | Default | Description |
|---|---|---|
| `DOOR_TRAVEL_MS` | `0` | How long the door takes to travel, ms. `0` = "not measured", which disables both announcement and verification. Deliberately not a guess: a travel time set too short reads every good travel as a stall, and for a close that means reversing a door that was closing perfectly well. |
| `DOOR_TRAVEL_OPEN_MS` | `DOOR_TRAVEL_MS` | Per-direction override. On a **mounted** door these are not equal — gravity assists the close and opposes the open. Measured flat, the reference door took 12,180 ms to open and 12,704 ms to close; upright they diverge further. |
| `DOOR_TRAVEL_CLOSE_MS` | `DOOR_TRAVEL_MS` | As above. Set both with `w` → `travel 12200 12700`, or let `calibrate` measure them. |
| `TRAVEL_GRACE_MS` | `3000` | How much longer than the travel time to wait before calling it a stall. A door is slower in January and slower as it wears; keeping the margin separate keeps the travel time an honest measurement. |
| `ARRIVAL_WAIT_MAX_MS` | `60000` | The arrival deadline when switches are fitted but no travel time is known. With a switch at the destination the door does not need a travel time to know it arrived — this only bounds the wait, and the duration becomes the measurement. |
| `WAKE_IDLE_MS` | `30000` | Idle time after which the controller is assumed asleep and a **wake press** is sent first. `0` disables it. Below this, one press. Deriving the press count from idle time is what satisfies both "the first press is swallowed" and "a repeat press mid-travel is a STOP" — a fixed "always press twice" satisfies neither. |
| `WAKE_PROBE_MS` | `1800` | How long to watch after a wake press before deciding it did nothing. Must be comfortably past motion onset (~1300 ms measured) or a press that *did* take gets followed by one that stops the door. It is also dead time on every cold actuation. |
| `MOTION_ONSET_MS` | `2500` | How long to wait for the door to start moving after the actuating press. Only meaningful with a vibration sensor or a switch at the starting end; with neither, nothing can observe a start and this is unused. |
| `SWALLOW_RETRY_LIMIT` | `2` | How many times a press that demonstrably moved nothing may be repeated **immediately**. Safe to retry at once: nothing moved, so nothing is trapped, and the failure is distinguishable from a stall. |
| `FAILED_ATTEMPT_COOLDOWN_MS` | `30000` | How long to leave a direction alone after an attempt in it achieved nothing. Without this the door loops: a failed attempt commits nothing, so whatever asked for it asks again on the next tick — ten presses a second. **Per direction**, so a failed close never delays the open after it. |
| `CLOSE_RETRY_DELAY_MS` | `300000` | After a **stalled close**: how long before trying again. Minutes, not seconds — whatever stopped the door needs time to move or be noticed. |
| `CLOSE_RETRY_LIMIT` | `3` | How many close attempts may stall before the door stays **open** and says so. An open door is an inconvenience; a door grinding onto an obstruction is not. |
| `CAL_QUIET_MS` | `90000` | How long `calibrate` requires the door to sit still, with nothing commanded and no switch changing, before it trusts a measurement. 90 s because that is the quiet period that recorded zero movement once the vendor's own modes were disabled. |
| `VIBRATION_MOVING_PULSES` | `50` | The edge count that means the door is genuinely **in motion**, as opposed to `VIBRATION_MIN_PULSES`, which only means something happened. Used for the wake probe, where a false positive suppresses the actuating press entirely. Real travel emits ~2,000 edges/s, so this is reached in ~25 ms of movement. |

All of `travel`, `wake` and `retry` are settable at runtime — from the console
under `w`, or over the network — and saved on the device, because every one of
them is a property of *your* controller rather than of this firmware.

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

## 4b. WiFi log upload (optional, off by default)

**Leave `WIFI_SSID` empty and the radio is never brought up** — no WiFi, no
cloud, exactly as before. Put credentials in `secrets.h`, never in `config.h`.

| Setting | Default | Description |
|---|---|---|
| `PETDOOR_ENABLE_WIFI` | `1` | **Compile-time.** `0` removes the uploader, OTA and the whole network stack from the binary — measured at **649 KB of flash and 28 KB of RAM** (69% → 35% of the partition). Leaving it at `1` does not bring the radio up; that is what `WIFI_SSID` controls. |
| `WIFI_SSID` | `""` | Network name. Empty disables everything below. |
| `WIFI_PASSWORD` | `""` | Network password. |
| `LOG_ENDPOINT_URL` | `""` | Where the CSV batch is POSTed. **Empty means local-only**: events are still recorded and still roll oldest-out, but nothing is sent and the radio is never brought up. |
| `LOG_SHARED_KEY` | `""` | Optional. When set, each upload is signed with HMAC-SHA256. **The key is never transmitted** — only a signature over the timestamp and body — so plain HTTP is still safe from forgery. See [LOG-SERVER.md](LOG-SERVER.md#why-http-and-not-https). |
| `LOG_ALLOW_TLS` | `0` | Compile in TLS for uploads. **Off by default because it does not fit on a classic ESP32** — measured, a handshake drove free heap from 80 KB to 18 KB and failed to connect, and it costs ~170 KB of flash. Uploads are signed with HMAC instead, which gives integrity without it. |
| `LOG_DEVICE_ID` | `"petdoor"` | Identifies this door to the server, so one endpoint can collect from several. |
| `WIFI_IDLE_SETTLE_MS` | `60000` | Everything must have been quiet this long before an upload is allowed. Runtime-settable together with the two below via `upload <settle> <interval> <heartbeat>`. |
| `WIFI_MIN_UPLOAD_INTERVAL_MS` | `300000` | Never upload more often than this, however many events arrive. **This is the one that protects the BLE scan** — WiFi and BLE share an antenna, so a short interval keeps the radio up and starves the listening the door exists to do. Floor of 60 s when set remotely. |
| `WIFI_CONNECT_TIMEOUT_MS` | `15000` | Give up associating after this long, so a missing access point cannot hold the radio. |
| `OTA_PASSWORD` | `""` | Password for over-the-air firmware updates. **Empty disables OTA entirely** — without one, anyone on the network could reflash the door. |
| `OTA_WINDOW_MS` | `300000` | How long an update window stays open before the radio is handed back to BLE. |
| `REMOTE_CONFIG` | `1` | Accept configuration and commands from the log server, carried back in its reply to an upload. Every reply must be signed with `LOG_SHARED_KEY`. Cannot change WiFi, the endpoint, the key or the OTA password — see [REMOTE-CONFIG.md](REMOTE-CONFIG.md). |
| `REMOTE_CMD_QUEUE_DEPTH` | `12` | Most commands accepted from one reply. |
| `REMOTE_CMD_MAX_LEN` | `128` | Longest single command line. A MAC list is the long one. |
| `REMOTE_RESTART_DELAY_MS` | `20000` | Delay before a restart a command asked for, so the acknowledgement is uploaded first. |
| `OTA_REQUIRE_CONFIRM` | `1` | Hold a freshly flashed image unconfirmed until it completes an upload; the bootloader restores the previous image if it never does. The Arduino core otherwise confirms every image at boot, making rollback unreachable. |
| `WIFI_HEARTBEAT_MS` | `1800000` | Call in at least this often even while the animal is home. `0` disables it, and the door then only speaks when the beacon is away. Runtime-settable; `0` disables it, at the cost of a door whose animal stays indoors going silent and collecting no commands. |
| `NTP_SERVER` | `"pool.ntp.org"` | Used to set the clock, so log entries carry real timestamps. |

### Why uploads are deferred rather than immediate

**The ESP32 has one 2.4 GHz radio, shared between WiFi and BLE.** Associating
with an access point takes 2–6 seconds of radio-intensive work — the POST itself
is trivial — and during that window BLE sampling is starved.

Uploading on each event would be the worst possible schedule, because events
happen when the animal is **at the door**. It would blind the radio exactly when
detection matters. Worse, `FIX_LOST` is itself an event, so a burst that starves
BLE can trigger another burst.

So events are written to NVS the instant they happen (no radio involved), and
the upload waits until the beacon is absent, the door is closed, and both have
been settled for `WIFI_IDLE_SETTLE_MS`. In practice the log reaches your
endpoint a minute or two after your pet leaves.

The upload runs in its own task, so a slow association cannot stall door
decisions, and it **aborts if the animal returns mid-flush**.

> Do not change this to upload on every event. The deferral is the entire
> reason WiFi can coexist with the beacon at all.

### Signing, and why not TLS

With `LOG_SHARED_KEY` set, the door signs each upload with HMAC-SHA256 over the
timestamp and body. The key never crosses the wire, so an eavesdropper can read
the events but cannot forge or replay them.

TLS would additionally hide the contents, and costs a **1–3 second handshake
plus ~40 KB of heap on every upload**. Radio time is the one resource this
project cannot spare — it is the same antenna the beacon needs. Door events are
low-secrecy but high-integrity, so HMAC buys the part that matters for free.
Signing added **zero flash**, because mbedTLS is already linked for WPA2.

For an endpoint across the internet, put it behind a VPN or a TLS-terminating
proxy rather than asking the ESP32 to do TLS.

### Scheduled lockout

| Setting | Default | Meaning |
|---|---|---|
| `SCHEDULE_MAX_WINDOWS` | `4` | How many windows can be stored. Each costs 6 bytes of NVS. |
| `SCHEDULE_UTC_OFFSET_MIN` | `0` | Minutes east of UTC. `-480` is US Pacific standard time, `-420` daylight. |

Windows themselves are set on the door, not compiled in — `n` on the console,
or `schedule add 22:00-06:00 Mon-Fri` from the browser. They survive reboots.

Three behaviours worth knowing before you use it:

- **No clock, no lockout.** Every window is inert until NTP has answered. A door
  that guesses the time can lock an animal out at noon believing it is midnight,
  so it refuses to guess. `s` says so plainly.
- **It gates opening only.** A window can never stop the door closing, and it
  cannot let an animal back in — see
  [SAFETY.md](SAFETY.md#a-scheduled-lockout-will-shut-an-animal-out).
- **There is no daylight-saving handling.** The offset is a fixed number of
  minutes, deliberately: a zoneinfo database on a microcontroller is a lot of
  machinery whose failure mode is a door locking an hour early twice a year.
  Change the offset yourself, or leave slack at both ends of your windows.

Refusals caused by a window are logged as `REFUSED` and sounded on the buzzer,
so a door that turned the collar away overnight can be asked about afterwards.

### Vibration sensor

| Setting | Default | Meaning |
|---|---|---|
| `PIN_VIBRATION` | `-1` | GPIO for an SW-420/801S digital output. `-1` disables it. Runtime-settable with `vibration <pin>` **and saved on the device**, like the buzzer and the switches — it is wiring, and wiring should survive a power cut. It did not, originally: the pin was runtime-only, so a brownout silently took the sensor away and with it the ability to tell a swallowed press from a successful one. Found by reflashing a door that had been configured by hand. |
| `VIBRATION_ACTIVE_LOW` | `1` | Only decides whether the internal pull-up is on — the sensor is read as *edges*, so either polarity works. |
| `VIBRATION_BLANK_MS` | `400` | How long after a relay pulse to ignore the sensor, so the relay's own click is not mistaken for the door. |
| `VIBRATION_MIN_PULSES` | `3` | Edges needed before a travel counts as movement, so one spurious reading is not enough. |

It answers **"did the door start moving?"** — within about a second, where a
limit switch cannot answer "did it arrive?" until the whole travel has elapsed.
A travel with no vibration at all is logged as `NO_MOVE`, which is distinct from
`STALLED`: one means the door never started, the other that it started and did
not finish. Different causes, different fixes.

Use the **digital** output, not the analog one: the free pins on a typical relay
board are on ADC2, which cannot be read while WiFi is active. Power the module
from **3.3 V**, not 5 V — its output swings to its supply, and 5 V exceeds what
an ESP32 pin will tolerate.

**Mount it on the door, not on the controller board.** A sensor sitting beside
the relay hears the relay on every actuation, whether or not the door moved,
which is exactly the signal it exists to distinguish from movement. Blanking
helps with vibration arriving through the structure; it cannot rescue a sensor
sitting on top of the thing making the noise.

### Two switches, and they do different jobs

| | Controls | When |
|---|---|---|
| `PETDOOR_ENABLE_WIFI` | whether the network stack is **in the binary** | compile time |
| `WIFI_SSID` | whether the radio ever **comes up** | runtime |

`PETDOOR_ENABLE_WIFI=1` with no `WIFI_SSID` — the default — links the stack but
never transmits. The flag is about size; the SSID is about behaviour.

Measured on a classic ESP32:

```
with WiFi     1,358,607 flash (69% of min_spiffs)   69,184 static RAM
without         694,099 flash (35%)                 40,880 static RAM
               -664,508                              -28,304
```

Setting it to `0` costs you log upload, NTP timestamps and **over-the-air
updates** — which means the IO0/EN buttons come back. The commands remain and
say so rather than failing silently:

```
u  ->  [wifi] not compiled in (PETDOOR_ENABLE_WIFI is 0)
p  ->  [ota] not compiled in — use the IO0/EN buttons
```

The event log is unaffected: it lives in NVS and never needed a network. That is why the project builds with the
`no_ota` partition scheme — see [ESP32-PRIMER.md](ESP32-PRIMER.md).

## 5. Diagnostics

| Setting | Default | Description |
|---|---|---|
| `SERIAL_BAUD` | `115200` | Serial console speed. |
| `CONTROL_TICK_MS` | `100` | Control loop **idle** period. The loop normally wakes the moment a BLE sample arrives; this only caps how long it waits when the beacon is silent. It also sets how often the status LED is refreshed, so it bounds the shortest LED pattern that can be rendered — much above 250 ms and the blip and fault flutter visibly break. |
| `CONTROL_TASK_STACK` | `5120` | Control-task stack in bytes. Sized from measurement — check the `task stacks` line in the `s` output before changing it. |
| `WIFI_TASK_STACK` | `5120` | Uploader-task stack. Raise it if you enable `LOG_ALLOW_TLS`; a TLS handshake needs several KB more stack than a plain POST. |
| `DEVICE_TABLE_SIZE` | `40` | Max distinct devices held in the discovery table. Costs RAM and BLE-callback time. |
| `TABLE_MIN_UPDATE_MS` | `500` | Per-device throttle on discovery-table writes, so a busy RF environment cannot flood the heap from the BLE callback. |
| `EVENT_LOG_CAPACITY` | `128` | Events kept in the persistent NVS log. Each entry is 16 bytes and the whole ring is rewritten on every event, so keep it modest — 128 is 2 KB and covers weeks of a door cycling a few times a day. |
| `DISCOVER_DUMP_INTERVAL_MS` | `2000` | How often discovery mode prints the table. |
| `CALIBRATE_INTERVAL_MS` | `500` | How often calibration mode prints a reading. |
| `ALLOW_MANUAL_SERIAL_CONTROL` | `1` | Allow `o` / `x` to drive the relays directly. Invaluable while wiring. **Set to `0` for an unattended deployment** — these commands bypass the proximity logic, the lockout *and* the boot grace window. |
| `MANUAL_HOLD_MS` | `300000` | How long a manual `o` keeps the door open before automatic control resumes. Suppresses automatic **closing** only — the beacon can always still open it. `0` restores the old single-pulse behaviour. Cleared by `x` or `O`. Never persisted: a reboot always returns to automatic. |
| `DOOR_TRAVEL_MS` | `0` | How long your door takes to travel, measured with a stopwatch. `0` = not measured, and nothing is announced. The firmware never *waits* for it. It uses it for two things: checking that `MIN_ACTUATION_INTERVAL_MS` is not shorter than the travel (which would let a reversing command land mid-travel and stop the door partway), and announcing travel — the status LED goes near-solid and the buzzer ticks for this long after each actuation, then chimes. A **stopwatch, not a sensor**: it chimes at a door stuck halfway just as happily. Runtime-settable with `travel <ms>`. See [WIRING.md](WIRING.md#measure-your-doors-travel-time). |

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
#define RSSI_MEDIAN_WINDOW 11     // close path: ride out the interference
#define RSSI_EWMA_ALPHA    0.2f
#define ENTER_CONFIRM_MS   3000
```

Leave the open pair alone while doing this. Widening the close window costs
nothing in open latency any more, which is the point of the split — reach for
`ENTER_CONFIRM_MS` if the door still opens too eagerly in a noisy spot.

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
