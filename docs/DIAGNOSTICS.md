# Diagnostics

The serial console is the only interface this firmware has. There is no WiFi,
no app, no cloud — so everything you need to know, you get here.

Every example on this page is **real output captured from a running board**,
not an illustration.

---

## Connecting

**115200 baud.** Pick whichever tool you already have.

```bash
# arduino-cli (bundled inside Arduino IDE.app on macOS)
arduino-cli monitor -p /dev/cu.usbserial-0001 -c baudrate=115200

# screen — exit with Ctrl-A then K, then y
screen /dev/cu.usbserial-0001 115200

# picocom — exit with Ctrl-A then Ctrl-X
picocom -b 115200 /dev/cu.usbserial-0001
```

Or the Arduino IDE's Serial Monitor, with the baud dropdown set to 115200.

### Getting back out

| Tool | Exit |
|---|---|
| `arduino-cli monitor` | `Ctrl-C` |
| `screen` | `Ctrl-A` then `K`, then `y` to confirm |
| `picocom` | `Ctrl-A` then `Ctrl-X` |
| Arduino IDE | Close the Serial Monitor pane |

**`screen` needs care — it is the one that strands the serial port.**

| Keys | What it does | Port afterwards |
|---|---|---|
| `Ctrl-A` `K`, then `y` | **Kill the session.** What you normally want. | **Released** |
| `Ctrl-A` `\` , then `y` | Quit screen entirely | **Released** |
| `Ctrl-A` `d` | *Detach* — session keeps running in the background | **STILL HELD** |
| Closing the terminal window | Same as detaching | **STILL HELD** |

Those last two are the trap. The window disappears, so it looks like you
quit — but the session is alive and still owns the device, and your next flash
fails with:

```
A fatal error occurred: Failed to connect to ESP32: No serial data received.
```

`Ctrl-A` is screen's own escape key, so it never reaches the ESP32. To send a
literal `Ctrl-A` to the board, press `Ctrl-A` twice.

### Output stair-steps down the screen

```
---- status ----
                 target       : MAC ac:23:3f:11:22:33
                                     uptime       : 89 s
```

That is a **line-feed without a carriage return**. The firmware emits `\r\n`
everywhere, so if you see this you are either running an older build or a
terminal that needs telling. Fix it at the terminal:

```bash
screen -f /dev/cu.usbserial-0001 115200     # flow control off
stty -F /dev/ttyUSB0 onlcr                  # Linux: map LF to CRLF
```

The Arduino IDE Serial Monitor never shows this, because it treats a bare LF as
a full newline — so a build that looks fine there can still stair-step in
`screen`.

### Output scrolling past too fast

Discovery mode reprints the whole table every 2 seconds, which scrolls anything
you are trying to read straight off the screen. Press **`d`** to turn it off,
then `h`, `s` or `m` will stay put. Press `d` again when you want it back.

The firmware pauses discovery and calibration printing automatically while the
`m` MAC editor is open, so the prompt stays readable.

To scroll back through what has already gone past, `screen` has a copy mode:

| Keys | Action |
|---|---|
| `Ctrl-A` then `[` (or `Ctrl-A` `Esc`) | Enter scrollback / copy mode |
| Arrows, `PgUp` / `PgDn` | Move around |
| `q` or `Esc` | Leave scrollback |

By default screen only keeps a few hundred lines. For more, start it with a
bigger buffer:

```bash
screen -h 5000 /dev/cu.usbserial-0001 115200
```

Or log everything to a file and read it in another window:

```bash
screen -L -Logfile petdoor.log /dev/cu.usbserial-0001 115200
```

### Clearing a stranded session

```bash
screen -ls                       # list sessions
```
```
There is a screen on:
        3897.ttys014.Jacobs-Mac-Studio  (Attached)
```
```bash
screen -X -S 3897 quit           # kill it by its id
screen -wipe                     # tidy up any dead entries
```

To see what is holding the port, whatever the cause:

```bash
lsof /dev/cu.usbserial-0001
```
```
COMMAND  PID   USER   FD   TYPE DEVICE  NODE NAME
screen  3897 jacobc    5u   CHR    9,7   989 /dev/cu.usbserial-0001
```

### Only one program at a time

macOS gives the serial device to a single process — `screen` claims it with
`TIOCEXCL`. A second program trying to open it gets:

```
[Errno 16] Resource busy: '/dev/cu.usbserial-0001'
```

`screen -x` multi-attach does **not** change this: it attaches several
terminals to one *screen session*, and screen is still the sole owner of the
device. A different program — `esptool` during a flash — cannot get in at all.
Even without the lock it would not work, since each byte is delivered to
exactly one reader, so the monitor would swallow the bootloader's handshake.

**So: close the monitor before flashing.**

### Finding your port

```bash
arduino-cli board list
```

| Platform | Looks like |
|---|---|
| macOS | `/dev/cu.usbserial-0001`, `/dev/cu.SLAB_USBtoUART`, `/dev/cu.wchusbserial*` |
| Linux | `/dev/ttyUSB0`, `/dev/ttyACM0` |
| Windows | `COM3`, `COM4`, … |

On Linux, add yourself to the `dialout` group or you will get permission
errors.

If two ports appear and you are unsure which is the ESP32, check the USB
descriptor — an ESP32 board's UART bridge is normally a Silicon Labs CP2102,
a WCH CH340, or an FTDI part:

```bash
python3 -c "import serial.tools.list_ports as p; [print(x.device, x.manufacturer, x.description) for x in p.comports()]"
```

```
/dev/cu.usbserial-0001  Silicon Labs  CP2102 USB to UART Bridge Controller
```

> **Close the monitor before flashing.** It holds the port and the upload will
> fail.

### "It connected but nothing happens"

Almost always **expected**, not a hang. Once a beacon is configured and
presence is stable, PetDoor prints **nothing** — it is event-driven, and with
discovery and calibration both off there are no events. Press `s` for a status
block, or `d` / `c` for a continuous stream.

To confirm the console is really attached rather than wedged, tap **EN** on the
board — you should get the boot banner immediately.

If a launch genuinely fails or blocks, something else already owns the port —
most often a `screen` session you closed the window on rather than quitting:

```bash
screen -ls                          # list sessions
screen -X -S <session-id> quit      # kill a stale one
lsof /dev/cu.usbserial-0001         # what is holding the port
```

---

## Commands

Type a single character. No Enter needed; newlines are ignored.

| Key | Action |
|---|---|
| `h` | Help |
| `s` | Status — presence, door, signal, radio health, heap |
| `d` | Toggle discovery mode (list every BLE device in range) |
| `c` | Toggle the calibration stream (live RSSI) |
| `r` | Reset the proximity filter |
| `o` | Pulse the OPEN relay now — **bypasses the proximity logic** |
| `x` | Pulse the CLOSE relay now — **bypasses the proximity logic** |
| `l` | Show the persistent event log — what the door actually did |
| `L` | Same log as CSV, for capture and analysis |
| `u` | Upload the event log over WiFi now, if configured |
| `p` | Open a firmware update window — flash over WiFi, no buttons |
| `P` | Close the update window early |
| `m` | Edit the beacon MAC list, saved on the device |
| `t` | Edit the open/close thresholds, saved on the device |
| `w` | Edit dwell times and the actuation lockout, saved on the device |
| `f` | Edit the filter shape (median window, smoothing), saved on the device |
| `!` | Reboot into flash mode — no IO0/EN buttons needed |

`o` and `x` are compiled out when `ALLOW_MANUAL_SERIAL_CONTROL` is `0`. If `h`
does not list them, that is why. They bypass the proximity logic, the actuation
lockout **and** the boot grace window — bench tools, not deployment features.

---

## The boot banner

Printed once at startup. It tells you exactly what was compiled in, which is
the fastest way to catch a `secrets.h` that did not take effect.

```
==================================================
  PetDoor — BLE proximity door controller
==================================================
  target beacon : MAC ac:23:3f:11:22:33
  open / close  : GPIO 16 / GPIO 17 (active HIGH)
  status LED    : GPIO 23
  thresholds    : open at >= -65 dBm, close at <= -75 dBm
  dwell         : open after 1500 ms near, close after 15000 ms far
  scan          : 99 ms window / 100 ms interval, active, duplicates ON
--------------------------------------------------
  Type 'h' for commands.

[system] running
```

Check these first:

- **`target beacon`** — `<none configured>` means the door will never be
  operated. If you set `BEACON_MAC` and still see this, your `secrets.h` is not
  being picked up.
- **`active HIGH` / `active LOW`** — must match your relay board. See
  [WIRING.md](WIRING.md#the-active-high-vs-active-low-trap).
- **`duplicates ON`** — must always say ON. See
  [ARCHITECTURE.md](ARCHITECTURE.md#the-duplicate-filter-trap).

---

## `s` — status

```
---- status ----
  target       : MAC ac:23:3f:11:22:33
  uptime       : 89 s
  presence     : PRESENT
  door         : OPEN
  rssi         : -73 dBm filtered (raw -76), ~? m
  last seen    : 85 ms ago, 174 samples
  radio        : 5243 adverts, last 12 ms ago, 0 samples dropped
  free heap    : 126960 bytes
```

| Field | Healthy | What it means |
|---|---|---|
| `target` | your MAC | `<none configured>` = door disabled |
| `presence` | `PRESENT` / `ABSENT` | `(no fix)` appended = no recent samples |
| `door` | — | what was last **commanded**, not where the door is |
| `rssi` | filtered ≈ raw | the **filtered** figure drives every decision |
| `last seen` | < 1000 ms | **`samples` must climb ~10/s** |
| `radio` | `last` < 1000 ms | total adverts from *all* devices |
| `samples dropped` | `0` | control task falling behind |
| `free heap` | stable | should not fall steadily over hours |

Two extra lines appear only while a transition is pending — the quickest way to
see the state machine working:

```
  opening in   : 900 ms
  closing in   : 11200 ms
```

> `~? m` instead of a distance is **correct** for any non-iBeacon beacon.
> The estimate needs the calibrated transmit power that only iBeacon frames
> carry. Eddystone and most sensor tags show `?`. No decision uses it.

---

## `l` — the event log

Serial output vanishes the moment nothing is attached, which is most of the
time. The firmware keeps the last `EVENT_LOG_CAPACITY` (128) events in NVS, so
they survive a power cut and answer "what happened overnight".

```
---- event log (14/128) ----
  when                  boot  uptime    event     detail
  2026-09-17 06:42:11Z  #12     3421s  OPEN      rssi=-47
  2026-09-17 06:58:03Z  #12     4373s  CLOSE     rssi=-71
  2026-09-17 07:02:55Z  #13        0s  BOOT      reset=1
  2026-09-17 07:03:25Z  #13       30s  REFUSED   reason=3 rssi=-52
```

What gets logged — deliberately only rare events, so the ring covers weeks:

| Event | Meaning |
|---|---|
| `BOOT` | `detail` is the reset reason. **`reset=9` is a brownout** |
| `OPEN` / `CLOSE` | A relay actually pulsed |
| `REFUSED` | An actuation was refused. `reason=2` lockout, `reason=3` boot grace |
| `FIX_GOT` / `FIX_LOST` | Beacon acquired or went stale — what precedes a close |

`REFUSED` is the one worth knowing about: it is what explains a door that did
not move when you expected it to. Repeats are collapsed, so a boot-grace window
logs once rather than sixteen times.

`L` prints the same data as CSV for capture:

```bash
screen -L -Logfile petdoor.csv /dev/cu.usbserial-0001 115200
```

### Timestamps

Entries carry uptime always, and wall clock only once something has told the
firmware the date. Until then the `when` column reads `(no clock)` and you work
from `boot` plus `uptime`. The boot counter is what makes that usable — a run
of `BOOT` entries with short uptimes between them is a power problem, and the
reset reason says which.

## `u` — upload the log

Only does anything when `WIFI_SSID` is set — see
[CONFIGURATION.md](CONFIGURATION.md#4b-wifi-log-upload-optional-off-by-default).

Normally uploads happen on their own, once the beacon has been absent and the
door closed for a settled period. `u` forces one immediately, which is useful
for checking your endpoint works without waiting.

```
[wifi] flush requested
[wifi] radio up — BLE sampling is degraded until this finishes
[wifi] uploaded 14 events, clock synced
[wifi] radio down
```

`s` shows the state:

```
  wifi         : idle (radio off), 3 uploads, 0 failures, clock synced
```

> **Forcing a flush deliberately starves BLE for a few seconds.** That is fine
> at a keyboard; it is why automatic uploads wait for an idle window instead.

## `w` — dwell and timing

How quickly the door reacts. Opening and closing are tuned independently, and
should be: opening late is an annoyance, closing early is a door shut on an
animal.

```
---- dwell / timing ----
  open after   :   500 ms near
  close after  : 15000 ms far
  min interval :  2000 ms between actuations (CLOSING only)
  interlock gap:   250 ms before any relay fires

  MEASURED worst gap between samples: 931 ms
  A close dwell at or below that WILL close on a routine
  radio dropout, with the beacon still present.
```

| Type | Effect |
|---|---|
| `fast` | 1000 / 3000 / 2000 — responsive |
| `safe` | 1500 / 15000 / 5000 — the shipped default |
| `500,15000,2000` | open ms, close ms, lockout ms |
| `gap 250` | relay interlock dead time (minimum 100 ms) |
| `clear` | back to compiled-in defaults |

**The menu prints your measured worst sample gap right above the input**, which
is the number that matters: a close dwell below it will close the door on a
routine radio dropout. A lockout at or above the close dwell is refused
outright, since it would delay closing anyway.

**Opening is never rate-limited.** The lockout applies only to closing — see
[ARCHITECTURE.md](ARCHITECTURE.md#invariants).

## `f` — filter shape

How fast raw RSSI is tracked. This is usually the dominant lag, ahead of the
dwell timers.

```
---- filter shape (how fast RSSI is tracked) ----
  median window : 3 samples (max 15)
  ewma alpha    : 0.70  (higher = faster, noisier)
```

| Type | Effect |
|---|---|
| `fast` | window 3, alpha 0.7 — responsive |
| `default` | window 7, alpha 0.35 — the shipped shape |
| `smooth` | window 11, alpha 0.2 — noisy RF |
| `3,0.7` | window,alpha directly (window odd, 1–15) |

Lag is roughly `(window/2 + 1/alpha)` × the sample interval. At a 350 ms sample
rate, `default` costs about 2 s and `fast` about 0.7 s.

Window 1 with alpha 1.0 disables filtering entirely and switches the door on
raw RSSI — the exact failure this project exists to fix. The firmware warns if
you go there.

## Status LED

One LED, in priority order — most urgent wins.

| Pattern | Meaning |
|---|---|
| **Solid** | Door was last commanded **OPEN** |
| **Brief blip** every 2 s | Door was last commanded **CLOSED** |
| 1 Hz blink | No beacon configured, or never heard since boot |
| **2 Hz flash** | **Beacon battery low** (see `BEACON_LOW_BATTERY_MV`) |
| 5 Hz flutter | Radio unhealthy — no advertisements from anything |
| Off | Nothing commanded since boot |

Solid-vs-blip shows what was last **commanded**, not where the door physically
is — this firmware is open-loop and has no limit switches. If the motor jammed,
the LED still shows solid.

> Door state is shown here rather than by flashing a relay: a relay's indicator
> is driven by its coil, so "flashing" one would energise the motor and
> physically move the door.

## Beacon battery

If your beacon broadcasts **Eddystone-TLM** telemetry, `s` reports it:

```
beacon batt  : 2890 mV (~86%)
beacon temp  : 21.4 C, up 412033 s, 1204418 adverts sent
```

Below `BEACON_LOW_BATTERY_MV` (2400 mV default) the status LED switches to its
2 Hz flash. `0 mV` means a mains-powered beacon and is not a fault.

**If these lines never appear, your beacon is not sending TLM frames** — many
ship with it off. Enable the TLM / telemetry frame in the vendor app. iBeacon-
only beacons cannot report battery at all.

The `adverts sent` counter is useful beyond battery: compared against the
`adverts` we received, it gives a true packet-loss ratio.

## `d` — discovery table

Every BLE device the radio can hear, reprinted every 2 seconds.

```
---- BLE devices in range ----
  MAC                RSSI  Dist    Age     Class      Name / manufacturer data
-> ac:23:3f:11:22:33  -65   4.0m    307ms Eddystone   svc=0000feaa-0000-1000-8000-00805f9b34fb
   c8:00:5e:00:00:01  -25   0.6m     51ms AirTag      mfg=000000000000000000000000000000
   c8:00:5e:00:00:05  -61   3.0m    975ms iBeacon    TempSensor_0002 mfg=000000000000000000
   c8:00:5e:00:00:06  -96  33.1m     51ms Generic    SomeSensor_0001 svc=0000fd00-0000-1000-8000
  38 device(s), 291043 advertisements total
```

- **`->`** marks a device matching your configured target. If your beacon is
  listed but unmarked, your `BEACON_MAC` does not match — check for typos.
- **Dist** is a rough estimate from the path-loss model, using the device's own
  calibrated power when it advertises one (iBeacon) and
  `BEACON_MEASURED_POWER_DBM` otherwise. **Indicative only** — treat it as a
  way to see a trend as you walk around, not a measurement. Shows `?` only if
  `BEACON_MEASURED_POWER_DBM` is set to 0.
- **Age** is time since that device was last heard. **This is the single most
  useful number on the page** — see the recipes below.
- **Class** is a best-effort label to help you spot your beacon. It plays no
  part in matching.

Discovery costs heap and CPU in the BLE callback, so it starts **on only when
no beacon is configured**. Toggle it with `d` any time.

The table holds `DEVICE_TABLE_SIZE` (40) devices with least-recently-seen
eviction. In a dense RF environment yours can be pushed out — move the beacon
closer, or raise the limit temporarily.

---

## `c` — calibration stream

Two readings a second. This is what you tune against —
see [TUNING.md](TUNING.md).

```
[cal] raw  -77  filtered  -76 dBm  ~? m  PRESENT
[cal] raw  -74  filtered  -76 dBm  ~? m  PRESENT
[cal] raw  -69  filtered  -75 dBm  ~? m  PRESENT
[cal] raw  -67  filtered  -74 dBm  ~? m  PRESENT
[cal] raw  -76  filtered  -75 dBm  ~? m  PRESENT
[cal] raw  -67  filtered  -71 dBm  ~? m  PRESENT
[cal] raw  -67  filtered  -69 dBm  ~? m  PRESENT
```

That capture shows the filter doing its job: **raw** swings over 10 dB
(−77, −74, −69, −67, −76, −67) while **filtered** glides smoothly from −76 to
−69. Note the −76 outlier in the middle barely moves the filtered value — the
median rejecting a spike.

Always tune against the **filtered** column.

When the beacon is not being heard:

```
[cal] no fix (last seen 4120 ms ago)
```

---

## Event lines

These appear on their own as things happen.

```
[fix] acquired  (rssi -35 dBm, ~1.2 m, 5 samples)
[presence] PRESENT  (rssi -35 dBm, ~1.2 m, 5 samples)
[door] OPEN  (rssi -35 dBm, ~1.2 m)
[fix] lost  (last heard 3240 ms ago) — counts as FAR
[presence] ABSENT  (rssi -76 dBm, ~8.4 m, 90 samples)
[door] CLOSED  (rssi -76 dBm, ~8.4 m)
[radio] UNHEALTHY — nothing heard from any device for 15012 ms
[radio] healthy — advertisements resumed
```

Every state change announces itself, so the console tells the story without you
polling with `s`:

| Line | Meaning |
|---|---|
| `[fix] acquired` | Enough recent samples to trust the reading |
| `[fix] lost` | Signal went stale — **counts as FAR**, so this is what precedes an unexpected close |
| `[presence]` | The hysteresis state machine changed its mind |
| `[door]` | A relay actually pulsed — covers manual `o`/`x` as well as automatic |
| `[radio]` | The scan watchdog's view of radio health changed |

`[fix] lost` followed by `[presence] ABSENT` about `EXIT_CONFIRM_MS` later is
the normal, healthy close sequence. `[fix] lost` appearing while the beacon is
sitting right next to the ESP32 means its advertising interval is too slow —
see the rate recipe below.

```
[cmd] forcing OPEN
[cmd] discovery ON
[cmd] proximity filter reset
[ble] no advertisements for 15012ms, restarting scan
[fatal] BLE scanner failed to start; rebooting in 5 s
```

A `[door]` line only prints when a relay actually pulsed. If presence changed
but no `[door]` line followed, the actuation was refused — lockout, boot grace,
or the door was already in that state.

---

## Recipes

### Is the duplicate-filter fix working?

The single most important health check. Type `s` twice, a few seconds apart,
and watch `samples`:

```
  last seen    : 85 ms ago, 174 samples
  last seen    : 91 ms ago, 208 samples      <-- climbing ~10/s. Good.
```

**If `samples` is stuck at 1, you have the duplicate-filter bug.** In the
discovery table the same fault shows as every device's `Age` climbing forever
and never resetting:

```
   65:00:5e:00:00:03  -68  153193ms Apple   mfg=000000000000000000000000
   65:00:5e:00:00:03  -68  155581ms Apple   mfg=000000000000000000000000
   65:00:5e:00:00:03  -68  157969ms Apple   mfg=000000000000000000000000
```

Each dump is 2388 ms later and the age grows by exactly 2388 ms — the age is
tracking wall-clock time, so that device has not been heard once since it first
appeared. RSSI is frozen too. On a real board this was measured at **0 of 40
devices ever re-heard**; after the fix, **24 of 38**.

Healthy output has ages in the tens or low hundreds of milliseconds.
See [ARCHITECTURE.md](ARCHITECTURE.md#the-duplicate-filter-trap).

### Where does distance actually show up?

| Where | Needs a configured beacon? |
|---|---|
| `d` discovery table, `Dist` column | No — shown for every device |
| `s` status, `rssi ... ~2.1 m` | **Yes** |
| `c` calibration stream | **Yes** |
| `[fix]` / `[presence]` / `[door]` lines | **Yes** |

Everything except the discovery table is computed from the *tracked target*, so
with `target : <none configured>` there is nothing to compute and those lines
do not appear at all. Set a beacon with `m` first.

If a distance shows as `?` when a beacon *is* configured, the beacon advertises
no calibrated power (Eddystone and most sensor tags do not) and
`BEACON_MEASURED_POWER_DBM` is set to 0. Give it a value — see
[CONFIGURATION.md](CONFIGURATION.md).

### Can I use this device as a beacon?

Only if its address is **fixed**. Read the top two bits of the first octet:

| First octet | Type | Usable? |
|---|---|---|
| `0x40`–`0x7F` (`01…`) | resolvable private | **No** — rotates |
| `0xC0`–`0xFF` (`11…`) | random static | **No** — rotates |
| `0x00`–`0x3F` (`00…`) | non-resolvable private | **No** |
| `0x80`–`0xBF` (`10…`) | public, real OUI | **Yes** |

```
ac:23:3f:11:22:33   0xAC = 10101100  ->  public, fixed        USABLE
c8:00:5e:00:00:01   0xC8 = 11001000  ->  random static        rotates
65:00:5e:00:00:01   0x65 = 01100101  ->  resolvable private   rotates
```

The practical test is simpler: note the MAC, wait 20 minutes, look again. If it
is gone, it rotates. Phones, AirTags, Tiles and most wearables all rotate — this
is deliberate anti-tracking behaviour and cannot be switched off.

### What is my beacon's advertising rate?

Watch the **Age** column for one device over a minute. The ceiling approximates
the advertising interval. Two real captures:

```
device                  min     median      max
Minew beacon            72ms      349ms    885ms     <-- healthy
AirTag (at 2 feet)     965ms     5140ms  17865ms     <-- unusable
```

The filter needs a steady stream. Compare your worst gap against:

- `SAMPLE_MAX_AGE_MS` (3000) — a longer gap means **no fix**, which counts as
  *far*
- `EXIT_CONFIRM_MS` (15000) — a gap this long lets the **close path complete**

A beacon whose max gap approaches either number will close the door while it is
sitting right next to the ESP32. Raise its advertising rate, or use a different
beacon.

### Is the radio healthy?

```
  radio        : 291043 adverts, last 44 ms ago, 0 samples dropped
```

`adverts` counts advertisements from **every** device, so it should climb
constantly — there is essentially always BLE traffic around. If `last` grows
past `SCAN_WATCHDOG_MS` (15000) the watchdog restarts the scan and says so. A
fast-blinking status LED means the same thing.

Repeated restarts almost always mean **brownout** — an underpowered supply or a
thin USB cable. The BLE radio has substantial current peaks.

### Setting the open distance without doing any maths

Put the beacon exactly where you want the door to start opening, press **`t`**,
and type **`here`**:

```
---- proximity thresholds ----
  open when  >=  -65 dBm  (~2.0 m)
  close when <=  -75 dBm  (~5.0 m)
  source     : compiled in
  beacon now : -47 dBm  (~0.3 m)
> here

[thr] saved: open >= -50 dBm (~0.4 m), close <= -60 dBm (~1.3 m)
[thr] active immediately; no restart needed.
```

This is the most reliable option because it uses **no path-loss model at all** —
it takes the signal actually observed at the spot you care about, subtracts 3 dB
of margin, and puts the close threshold 10 dB below that. Nothing depends on
`BEACON_MEASURED_POWER_DBM` being calibrated.

The other forms:

| Type | Meaning |
|---|---|
| `here` | Use the current reading as the open point |
| `1m` | Open within 1 m, close beyond ~2.5 m |
| `1m,3m` | Open within 1 m, close beyond 3 m |
| `-59,-69` | Set directly in dBm |
| `clear` | Revert to the compiled-in defaults |

The metre forms convert through the path-loss model, so they are **only as
accurate as `BEACON_MEASURED_POWER_DBM`**. If you have not calibrated that,
prefer `here`.

Entering an open threshold that is not greater than the close threshold is
refused — the gap between them is the hysteresis band.

### Is the wiring right?

With **no motor connected**, type `o`, then `x`, then `o` and `x` back to back.
You should hear exactly one click per command, and roughly a one-second pause
before a reversing pulse fires — that is `DIRECTION_CHANGE_GAP_MS`, the
interlock. Measured on real hardware:

```
[cmd] forcing OPEN    at +  50.2 ms
[cmd] forcing CLOSE   at + 528.4 ms      <-- 478ms gap, vs 450ms expected
                                             (DIRECTION_CHANGE_GAP_MS 250
                                              + RELAY_PULSE_MS 200)
```

Both relays must be **released at idle**. If one sits energised, invert
`RELAY_ACTIVE_LOW`. Full procedure in
[WIRING.md](WIRING.md#bench-test-procedure).

---

## Flashing a board with no auto-reset

Most dev boards reset themselves when `arduino-cli` uploads. Some wiring — a
bare module, or a USB-TTL adapter with only GND/TX/RX connected — has no
DTR→IO0 / RTS→EN path, and the upload fails:

```
A fatal error occurred: Failed to connect to ESP32: No serial data received.
```

### Which chips can skip the buttons

| Chip | Software download mode |
|---|---|
| ESP32-S3 / C3 / C6 | **Yes** — press `!` |
| **ESP32 (original WROOM)** | **No** — buttons, or wire DTR/RTS |

The original ESP32 has no RTC "force download boot" flag; it is an S3/C3/C6
feature. The firmware detects this at compile time, so on a classic ESP32 the
`!` command explains the situation rather than pretending to work.

**The permanent fix for a classic ESP32 is two wires**, after which uploads
reset the board by themselves and nothing here is needed again:

```
  USB-TTL DTR  ->  ESP32 IO0
  USB-TTL RTS  ->  ESP32 EN
```

(Dev boards do this through a two-transistor circuit to avoid holding the chip
in reset; direct wiring works with most adapters.)

**On an S3/C3/C6 already running this firmware, you do not need the buttons.**
Press `!` in the console and confirm with `y`:

```
> !
[sys] reboot into flash mode?
[sys] the door will NOT be controlled until you flash.
[sys] press 'y' to confirm, anything else cancels: y
[sys] rebooting into UART download mode.
[sys]   * the door is NOT controlled until you flash
[sys]   * quit your terminal first, then upload
[sys]   * power-cycle the board to cancel
```

It sets the RTC "force download boot" flag and resets, so the ROM comes up in
download mode instead of running the app. Quit your terminal, upload as normal,
and the new firmware boots.

> **While in download mode the firmware is not running, so the door is not
> controlled and the relay pins are not driven.** On an active-low relay board
> an undriven input can float — the same exposure as any flash, but worth
> knowing before you do it with animals relying on the door. A power cycle
> clears the flag and boots the old firmware again.

The flag is detected at compile time, so on a chip without it the command says
so and you fall back to the buttons.

### Over WiFi, with no buttons at all

Once the firmware on the board supports it, you never need the buttons again.
Set an `OTA_PASSWORD` in `secrets.h` alongside your WiFi credentials, then in
the console press **`p`**:

```
[ota] window open for 300 s — the radio is up, so BLE sampling
[ota] is degraded until it closes.
[ota] push with:  arduino-cli upload --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs -p 192.168.1.66 petdoor
```

Run that command and the firmware goes over the air. The board reboots into it
and the window closes on its own.

Three things it deliberately does:

- **Refuses while the beacon is present.** An update reboots the door and shares
  the radio; neither should happen with your pet at the doorway.
- **Times out** after `OTA_WINDOW_MS`, so a forgotten window hands the antenna
  back rather than starving BLE indefinitely. `P` closes it early.
- **Requires a password.** Without `OTA_PASSWORD` set, OTA is off — otherwise
  anyone on your network could reflash the door.

> The radio is up for the whole window, so BLE sampling is degraded throughout.
> That is why it is a window you open rather than a service that listens.

**Chicken and egg:** the board has to already be running OTA-capable firmware.
Getting there takes one last button-flash — and it also changes the partition
table to the dual-slot layout OTA needs. NVS sits at the same offset in both,
so your beacon, thresholds and event log survive.

### By hand, with the buttons

Needed the first time, or if the board is not running working firmware.

Enter download mode by hand:

1. Press and **hold IO0** (sometimes labelled BOOT or FLASH)
2. **Tap EN** (sometimes labelled RST) and release it
3. Keep holding IO0 for another second or two, then release

The chip stays in download mode until the next reset, so you can do this
*before* starting the upload. A reliable tell that it worked: the sketch's
serial output stops.

Then flash as usual. Afterwards esptool prints:

```
Hard resetting via RTS pin...
```

but with no RTS wired that does nothing — **tap EN** to actually boot the new
firmware.

> With no buttons at all, jumper **IO0 to GND**, power-cycle, then remove the
> jumper.

---

## See also

- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) — symptom-first: "my door will not
  open"
- [TUNING.md](TUNING.md) — choosing thresholds from calibration output
- [BEACON-SETUP.md](BEACON-SETUP.md) — finding and configuring a beacon
