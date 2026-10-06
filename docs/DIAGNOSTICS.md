# Diagnostics

The serial console is the door's own interface, and the only one guaranteed to
be there: WiFi, the log server and remote management are all optional and all
off by default. On a door with none of them, everything you need to know, you
get here. On a door that has them, this is still where you go when the network
is the thing that is broken.

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
screen  3897 you       5u   CHR    9,7   989 /dev/cu.usbserial-0001
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
| `o` | **Open the door now** — bypasses the proximity logic, the lockout and a gave-up state. Still interlocked, still wake-pressed, still verified |
| `x` | **Close the door now** — same, and the only thing it cannot override is the boot grace |
| `l` | Show the persistent event log — what the door actually did |
| `L` | Same log as CSV, for capture and analysis |
| `u` | Upload the event log over WiFi now, if configured |
| `p` | Open a firmware update window — flash over WiFi, no buttons |
| `P` | Close the update window early |
| `m` | Edit the beacon MAC list, saved on the device |
| `t` | Edit the open/close thresholds, saved on the device |
| `w` | Edit dwell times and the actuation lockout, saved on the device |
| `O` | Clear a manual hold, handing the door back to the beacon |
| `k` | **Lock** — the beacon may no longer open the door. Survives reboot. |
| `K` | Unlock |
| `f` | Edit both filter shapes — the slow close filter and the fast open filter — saved on the device |
| `!` | Reboot into flash mode — no IO0/EN buttons needed |
| `C` | **Calibrate the travel time**, both directions. Needs both limit switches and an open maintenance window (`M` first); begins with a 90-second quiet period and aborts if the door moves during it. See [TUNING.md](TUNING.md#measuring-the-travel-time) |

### `o` holds the door open

`o` pulses the OPEN relay **and** suspends automatic closing for
`MANUAL_HOLD_MS` (5 minutes by default). Without that hold, `o` is a single
pulse: the control task runs milliseconds later, sees an open door with no
beacon in range, and closes it again — so the door visibly starts moving and
then stops. That is not a relay fault, and it made bench testing extremely
confusing before the hold existed.

```
[cmd] forcing OPEN
[cmd] holding open for 300 s — automatic closing is paused.
[cmd] 'x' closes now, 'O' hands control back immediately.
```

`s` shows the remaining time while a hold is active. `x` closes and clears it;
`O` clears it without moving the door.

The hold suppresses automatic **closing only** — a beacon arriving during a hold
still finds an open door, and `x` can never keep the door shut against an animal
walking up to it. It is not saved to NVS, so a reboot always returns to
automatic control.

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
| `OPEN` / `CLOSE` | A travel **resolved**. `detail` is who asked — `0` collar, `1` console, `2` network, `3` a safety reversal — and the spare field carries flags: verified by a switch, preceded by a wake press, needed a repeat press |
| `REFUSED` | An actuation was refused. `reason=2` lockout, `reason=3` boot grace, `reason=4` already travelling, `reason=5` waiting to retry a failed close, `reason=6` gave up closing, `reason=100` a schedule window |
| `STALLED` | It started and never arrived. A stalled **close** is reversed |
| `NO_MOVE` | Every press was swallowed, or the door is jammed solid. Nothing moved, so nothing is trapped |
| `GAVE_UP` | Close attempts exhausted. `detail` is how many. **The door is staying open until a person deals with it** |
| `UNCOMMANDED` | The door reached an end that nothing commanded it to — **a hand, the wind, or the door's own controller**. `detail` is which end: `1`/`2` when a limit switch measured it, `11`/`12` when it was inferred from the duration of the movement. Logged, sounded and uploaded at once; not emailed, because most of them are you |
| `RETRY` | A press was repeated because the previous one moved nothing. `detail` is which attempt |
| `SENSOR_FAULT` | A sensor disagreed with the other one badly enough to be called broken. `detail` names which (see below); the spare field carries the evidence. **Emailed.** Distinct from `STALLED`/`NO_MOVE`, which say the *door* misbehaved — this says the thing *watching* the door is lying |
| `BEACON_LOW` | The beacon's **own battery** crossed the threshold. `detail` 1 = went low, 0 = recovered; the spare field carries the millivolts. Latched with a recovery margin, so one crossing is one entry. **This is the event that gives you days of notice** — see below |
| `WAKE` | Recorded **only** when a wake press turned out to move the door by itself — the near-miss worth counting. The ordinary case is carried as a flag on the actuation instead, so the ring is not filled with it |
| `MAINT` | A maintenance window started, ended, or expired |
| `CONSOLE` | The network console: attached, refused, or a wrong password |
| `FIX_GOT` / `FIX_LOST` | Beacon acquired or went stale — what precedes a close |

`REFUSED` is the one worth knowing about: it is what explains a door that did
not move when you expected it to. Repeats are collapsed, so a boot-grace window
logs once rather than sixteen times.

### Detecting a door that moved on its own

A manual close — or the vendor controller acting on a mode of its own — is
`UNCOMMANDED`. There are two ways the door notices, and it prefers the first:
a **limit switch** that saw the door arrive, or, when no switch can see it, the
**duration** of what the vibration sensor felt.

What it compares is **the last END the door was seen at**, not the last reading.
That distinction is the whole feature. A door being pushed shut reads `OPEN`,
then `UNKNOWN` for the ten seconds it is in transit, then `CLOSED` — so
comparing against the previous *reading* always compares against `UNKNOWN`, and
"did it change ends?" is always answered no. Written that way the event could
never fire for any door that physically travels, which is all of them.

Three things must hold before it is called uncommanded, and each excludes a real
false positive:

| | Excludes |
|---|---|
| it was at a **known** end before | the first reading after boot — the door was not moving, we simply had not looked |
| it is at a **different** end now | a reed chattering as the door settles on its stop |
| no travel resolved in the last `UNCOMMANDED_SETTLE_MS` | a travel given up on as `ASSUMED` or `STALLED` that was still finishing, and whose reed made a second later |

A hand-pushed door produces thousands of edges with no travel in flight, which
is also the signature of a sensor firing at rest. So the noise check stands down
whenever the door actually changed ends, or whenever the movement came in
discrete runs, rather than blaming the one part that reported the truth. A
"run" that goes on past `VIBRATION_RUN_MAX_MS` is the exception — nothing on a
door moves for forty-five seconds, so that one is counted as the chatter it is.

#### Without limit switches: inferring the travel from its duration

The switches are optional and most builds will not have them, so on those doors
everything above is dead and the believed state silently goes stale. A stale
**open** is the expensive one: the next open request is refused as "already
there" and the animal stands at a shut door.

The vibration sensor cannot say which way the door went. It does not need to —
**a door sitting at a limit can only travel one way**, so the direction follows
from where it was. What vibration has to establish is that the movement was a
*full travel* rather than a shove, and duration answers that, because the travel
time is already measured per direction.

A run of movement is accepted as a travel when it lasts between
`VIBRATION_TRAVEL_MIN_PCT` and `VIBRATION_TRAVEL_MAX_PCT` of that direction's
measured travel time. On the reference door, whose close takes 11.2 s, that is a
band of roughly **7.8 s to 17.9 s** — comfortably clear of someone leaning on
the panel for a second or two. Runs survive a quiet patch of
`VIBRATION_RUN_GAP_MS`, because these modules are a spring in a tube and go
briefly silent mid-travel; without that, one travel would be chopped into
several that each match nothing.

**The two directions are not treated alike, and that is deliberate.** This is an
inference, and it will sometimes be wrong. What it costs when it is wrong is not
symmetric:

| Inference | If wrong | Cost |
|---|---|---|
| it **closed** | the door is really open | the next close is refused, the door stays open. Visible, and fail-open |
| it **opened** | the door is really closed | the next **open** is refused and the animal is shut out |

The second is the one request that must never be refused. So an inferred close
commits the believed state, and an inferred open only logs it — dropping the
belief to `UNKNOWN`, which refuses nothing in either direction. **Only a limit
switch commits an open.**

The log says which happened. `detail` is the end the door reached — `1` open, `2`
closed — when a switch measured it, and the same value plus 10 (`11`, `12`) when
it was inferred, with the duration in the spare column. The dashboard renders the
two differently on purpose: an inference and a measurement are not the same
claim, and showing them identically is how one quietly becomes the other.

`s` reports the band, so you can see at a glance whether a hand-closed door would
be noticed on this door:

```
  vibration    : GPIO 33, 15932 edges since boot (pull-up on)
                 uncommanded travel inferred from 7.8-17.9 s of movement
```

If it says `no close travel time` instead, press `c` to calibrate — nothing can
be inferred without a yardstick.

### How a broken sensor is told from a broken door

Both sensors can fail quietly, and a quiet sensor is worse than none: the door
keeps deciding, just on evidence that is no longer arriving. A reed whose magnet
has come off turns every close into a `STALLED`, and the fail-open rule then
parks the door open night after night with nothing looking wrong.

What makes this detectable without false alarms is that **the two sensors check
each other** — each diagnosis uses the other as ground truth, so a single
failing part is identifiable rather than merely suspected:

| `detail` | What it saw | What is broken |
|---|---|---|
| `1` | both limit switches made at once | a shorted wire, a stuck switch, or a stray magnet |
| `2` | a reed-**verified** travel produced no vibration | **the vibration sensor.** A switch confirmed arrival, so the door definitely moved; there is no reading where the door is at fault |
| `3` | vibration accumulating with the door standing still | **the vibration sensor**, too sensitive or mounted on the frame rather than the door |
| `4` | a full travel's worth of vibration, and it never arrived | **the limit switch at that end.** The door ran, so this is not an obstruction — an obstruction stops the vibration too |

Faults 2 and 4 need `SENSOR_FAULT_STRIKES` consecutive disagreements, so one
short travel or one glancing magnet is not enough. All four are **latched**: one
failure is one console message, one log entry and one email, not one per travel.

The pair `2`/`4` is the useful part. Both begin as "a travel did not go as
expected", and vibration is what splits them — plenty of movement means the door
ran and the switch missed it; little movement means the door stopped, which is
an obstruction and is already reported as `STALLED`.

**Why `BEACON_LOW` matters more than it looks.** A flat beacon does **not** shut
the door on anything: the firmware refuses to act until it has heard the collar
once since boot (invariant 5), so a cell that dies overnight leaves the door
wherever it was, with the animal on the wrong side and nothing obviously broken.
It stops the door *working* rather than making it dangerous — which is exactly
the kind of failure nobody notices until it matters. The log server also emails
on this one.

It only works if your beacon sends **Eddystone-TLM** frames. Many beacons never
do, and an iBeacon-only mode never does, so "no reading" is normal rather than a
fault — `s` says `not reported` and the uploaded status carries `batt=-1`. And
note it reported nothing at all on any firmware before the NUL fix: TLM service
data begins `20 00`, which the BLE adapter truncated at the NUL, so the warning
was inert for the whole life of the feature.

**An `OPEN` without the verified flag is an intention, not a record.** With no
limit switches fitted, every actuation reads that way — which is the honest
answer, and the reason the flag exists.

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
  relay pulse  :   200 ms held closed (the "button press")

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
| `pulse 200` | how long the relay stays closed, 50–10000 ms |
| `presses 2 1000` | press n times per actuation, ms apart (n 1–3) |
| `clear` | back to compiled-in defaults |

`pulse` is the one to reach for when **the relay clicks but the door does not
move**: many controllers debounce their button and ignore a tap shorter than
300–500 ms. Try `pulse 1000`, test with `o` and `x`, then walk it back down.
Full procedure in
[TROUBLESHOOTING.md](TROUBLESHOOTING.md#the-relay-clicks-but-the-door-does-not-move).

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
  two filters run over the same samples:
  CLOSE window  : 7 samples (max 15)
  CLOSE alpha   : 0.35  (higher = faster, noisier)
  OPEN  window  : 1 samples
  OPEN  alpha   : 0.90
```

The same advertisements are filtered twice — slowly for the close decision,
quickly for the open decision. Opening late can shut an animal out; closing
early can shut a door on one, and the two want opposite amounts of smoothing.

The presets act on the **close** pair:

| Type | Effect |
|---|---|
| `fast` | window 3, alpha 0.7 — twitchy close; rarely needed now |
| `default` | window 7, alpha 0.35 — the shipped shape |
| `smooth` | window 11, alpha 0.2 — noisy RF |
| `3,0.7` | window,alpha directly (window odd, 1–15) |
| `open 1,0.9` | set the **open** pair directly |
| `open same` | make the open pair match the close pair (old single-filter behaviour) |
| `clear` | forget both saved pairs |

Lag is roughly `(window/2 + 1/alpha)` × the sample interval, and now applies to
**closing** only. Opening is limited by `ENTER_CONFIRM_MS` alone — at the
shipped open pair the filter adds no lag of its own.

The open pair may not be slower than the close pair; the menu refuses it.

Window 1 with alpha 1.0 disables filtering entirely and switches the door on
raw RSSI — the exact failure this project exists to fix. The firmware warns if
you go there.

## Status LED

One LED, in priority order — most urgent wins.

| Pattern | Meaning |
|---|---|
| **Solid** | Door **OPEN** |
| **Near-solid**, brief gap every 0.6 s | Door is **moving** |
| **Brief blip** every 2 s | Door **CLOSED** |
| **Double blip** every 2 s | Door is closed **and locked** — the collar may not open it |
| **Two quick blips** every 2 s | **A sensor has failed.** The door is still deciding, but on evidence it can no longer trust. One fewer blip than "gave up" below, because from a distance they would otherwise look identical — and they mean opposite things: gave-up is a door that *knows* it has a problem, this is a door that has lost the ability to tell |
| **Three quick blips** every 2 s | **Gave up closing.** Close attempts are exhausted and the door is staying open until somebody clears the way |
| 1 Hz blink | No beacon configured, or never heard since boot |
| **2 Hz flash** | **Beacon battery low** (see `BEACON_LOW_BATTERY_MV`) |
| 5 Hz flutter | Radio unhealthy — no advertisements from anything |
| Off | Position **unknown** |

The three-blip pattern sits **above** the battery flash on purpose. A flat
beacon battery is a job for the weekend; a door that has stopped closing is
tonight's.

**What the top three patterns mean depends on whether you fitted the limit
switches**, and this is the clearest place the difference shows up:

| | No switches | Switches fitted |
|---|---|---|
| solid / blip | what was last **commanded** | where the door **is** |
| off | nothing commanded since boot | genuinely **between** the two ends |
| near-solid | a stopwatch running for `DOOR_TRAVEL_MS` | a travel actually in flight, ending when the switch says so |

So without switches, a jammed motor still shows solid. With them, a door
stopped partway shows **off** — neither open nor closed, which is the truth.

With `DOOR_TRAVEL_MS` at its `0` default and no switches, "moving" never
appears at all: nothing has told the firmware how long to show it for.

> Door state is shown here rather than by flashing a relay: a relay's indicator
> is driven by its coil, so "flashing" one would energise the motor and
> physically move the door.

## Buzzer

Optional, off by default, and the only diagnostic you can read from across the
yard without looking at anything. Set it up with `w` → `buzzer <pin>`; full
detail in [WIRING.md](WIRING.md#the-annunciator).

The set answers two questions: **who asked for this movement**, at the moment
it is commanded, and **how it turned out**, twelve seconds later.

#### Who asked — counted, not pitched

The number of beeps is how far away the thing that asked for it was. The long
beep is **last to open** and **first to close**, so you also hear which way the
door is going.

| Sound | Who |
|---|---|
| `. -`  — short, long | **the collar** arrived (`- .` = it left) |
| `. . -` — short, short, long | **the console**: somebody typed `o` (`- . .` = `x`) |
| `. . . -` | **the network**: `door open` from the dashboard (`- . . .` = `door close`) |

#### How it turned out

| Sound | Meaning |
|---|---|
| tick … tick … tick | Still **moving** — one tick a second while the travel is in flight |
| rising pair, two equal beeps | **Arrived.** Verified by a limit switch, or, with none fitted, the travel timer expiring |
| **two long** low beeps | It **never moved**. The press was swallowed, or the door is jammed solid. Nothing is trapped — the door is still where it was |
| **five fast** beeps | It **STALLED**: started and never arrived. A stalled *close* is reversed |
| **long, then two short** | **Gave up** closing. The door is staying open until somebody clears the way |
| **three short low blips**, repeating every 15 min | **A sensor has failed** — the only tune that repeats. See below |
| **blip … pause … blip** | The door moved and **nothing commanded it**. A hand, the wind, or a mode on the door's own controller |
| one long low buzz | **Refused**: the door is locked by hand, or refused for some other reason. Once per arrival, not repeatedly |
| **two long** low beeps, well separated | **Refused by a SCHEDULED WINDOW.** "Not… now." The wide gap is what tells it from the two-long "never moved" above, which stutters |
| three even beeps | Answering your `beep` — proves the buzzer, says nothing about the door |
| **two short**, low | Acknowledging `lock` |
| **one medium**, high | Acknowledging `unlock` |
| one short blip | Acknowledging a setting change |

With no limit switches fitted, **the rising pair is a timer expiring, not an
arrival** — a door jammed halfway gets the same chime, which is most of the
argument for fitting them. With switches, it is an arrival, and the three
failure sounds above become reachable.

### Why they sound the way they do

A manual lock takes precedence in the announcement as well as in the logic: if
you locked the door by hand you hear the single beep, even if a window happens
to be in force too. The two-beep version means *only* a window is refusing —
which matters because the remedies differ. A manual lock needs `unlock`; a
window needs waiting, or editing with `n`.

Everything here is told apart by **rhythm first, pitch second**. An active
buzzer has one pitch it chose at the factory, so a set of tunes separated only
by pitch would collapse into a single sound on half the hardware this supports.
Pitch then makes them nicer on a passive one.

That is why "who asked" is a **count**. Three sources need three sounds, and
counting beeps is the only distinction that survives a one-pitch buzzer intact.
The direction rides on *where* the long beep sits, which is the shape every
appliance anyone owns already uses: rising means finished, falling means
stopping.

The endings are then kept clear of that scheme and of each other. No movement
tune has two equal beeps, which is what keeps "arrived" from being heard as a
movement; the stall is a five-beep clatter at a 70 ms gap, well under every
other tune's, so it reads as urgency rather than as a count; and nothing else
in the set opens with an 800 ms beep, which is what makes "gave up" findable.

**The sensor-fault reminder is the one exception to playing once**, and it is a
considered one. A door that gave up is standing open where you can see it; a
reed that stopped making looks like a perfectly ordinary door right up until the
night it matters. A fault with no outward sign is the case where silence is the
wrong default.

It is kept sparse for exactly the reason the rule exists: three short low blips
a quarter of an hour apart is a reminder, not an alarm, and it never becomes the
thing somebody disconnects. It also never interrupts — if anything else is
playing, or a travel is in flight, it waits for the next interval. Set
`SENSOR_FAULT_BEEP_MS` to 0 to silence it; raise it if the door is near a
bedroom window. There is deliberately no quiet-hours logic: a fault that stays
silent until morning is a fault you learn about in the morning.

"Gave up" plays **once**, not on a loop. The door reports that state
continuously on its LED, in the log and in the uploaded status line — and a
buzzer repeating it all night would be an alarm, and an alarm nobody can stand
gets unplugged, which loses the message entirely.

One sound per batch of remote commands, not one per command — several usually
arrive together. A refusal always wins over a confirmation: "something did not
take" is the part you need to hear.

Silence where you expected a sound is nearly always the pin or the buzzer type,
not the door. `beep` separates the two in one keystroke: if `beep` is silent the
buzzer is wrong, and if `beep` works but the door never ticks, `travel` is `0`.

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
[door] opening — the collar is here (rssi -35 dBm, ~1.2 m)
[door] wake press sent (the controller had been idle)
[door] OPEN — ARRIVED, verified by the limit switch in 12184 ms
[fix] lost  (last heard 3240 ms ago) — counts as FAR
[presence] ABSENT  (rssi -76 dBm, ~8.4 m, 90 samples)
[door] closing — the collar is gone (rssi -76 dBm, ~8.4 m)
[door] CLOSED — ARRIVED, verified by the limit switch in 12702 ms
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
| `[door] opening` / `closing` | A travel has just been **commanded**, and by whom |
| `[door] OPEN` / `CLOSED` | A travel **resolved**, and how |
| `[pos]` | A limit switch said something about where the door is |
| `[batt]` | The beacon's own battery crossed the low threshold, or came back above it |
| `[sensor]` | A sensor has been judged broken by cross-checking it against the other |
| `[cal]` | Travel-time calibration |
| `[radio]` | The scan watchdog's view of radio health changed |

`[fix] lost` followed by `[presence] ABSENT` about `EXIT_CONFIRM_MS` later is
the normal, healthy close sequence. `[fix] lost` appearing while the beacon is
sitting right next to the ESP32 means its advertising interval is too slow —
see the rate recipe below.

### A commanded travel and a resolved travel are two different lines

This is the single biggest change in how the console reads. A movement is
announced when it is **asked for**, and again when it is **over** — because on
this hardware those are twelve seconds and several possible outcomes apart.

```
[door] closing — the collar is gone (rssi -81 dBm, ~11.0 m)
[door] wake press sent (the controller had been idle)
[door] nothing moved; pressing again (retry 1)
[door] !! travelling CLOSED STALLED — it started and never arrived.
[door] !! An obstruction, a jam, or a controller that stopped partway.
[door] !! The door's position is now UNKNOWN.
[door] !! It was CLOSING, so it is being reopened. A door
[door] !! stopped partway shut is exactly when something
[door] !! may be under it.
[door] !! next attempt in 5 min (attempt 2 of 3).
```

The endings, and what each one is telling you to do:

| Ending | Means | Do |
|---|---|---|
| `ARRIVED, verified … in N ms` | A switch at the destination confirmed it | nothing. Compare N against `travel` occasionally |
| `travel time elapsed (assumed; no limit switch at that end)` | The stopwatch ran out. Nothing measured this | fit the switches, if you want this to mean anything |
| `!! N presses and the door NEVER MOVED` | Every press was swallowed, or it is jammed solid. Nothing is trapped | raise `pulse` — 500 ms is swallowed by some controllers and 1000 ms is not |
| `!! … STALLED` | It started and never arrived | go and look. Something is in the way, or `travel` is set too short |
| `!! N close attempts have now stalled. GIVING UP` | The door is staying **open** | clear the way, then `x` |
| `[pos] !! the door moved to X and NOTHING commanded it` | A hand, the wind, or a mode on the door's own controller | if it repeats, find the vendor mode and turn it off |

A `[door] opening`/`closing` line that is **not** followed by a resolution
means the travel is still in flight; `s` says which phase it is in and how long
it has before it counts as a stall.

If presence changed but no `[door]` line followed at all, the actuation was
refused — lockout, boot grace, a schedule window, the retry delay after a
failed close, or the door was already in that state. `s` says which.

```
[cmd] forcing OPEN
[cmd] discovery ON
[cmd] proximity filter reset
[ble] no advertisements for 15012ms, restarting scan
[fatal] BLE scanner failed to start; rebooting in 5 s
```

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

### Bench-testing the buzzer, LED and sensors

All four peripherals are **runtime-configurable**, so none of this needs a
reflash — which is the point. Finding out you guessed a pin wrong should cost
you a console session, not a trip back to the laptop with the board in pieces.

**Start with `M`.** Maintenance mode blocks the door in *both* directions for a
bounded window, unlike the lock, which only stops the beacon opening it. That is
the envelope you want while your hands are near the doorway. It expires on its
own, so a forgotten window cannot leave the door inert.

**1 — Buzzer.** `w`, then:

```
buzzer 27            the GPIO it is on
beep                 three beeps
```

Silence means the wrong pin, or the wrong kind. A bare transducer needs a tone
rather than DC: `buzzer 27 passive`, then `beep` again. One that sounds when
pulled to ground wants `buzzer 27 active low`.

**2 — Limit switches.** `w`, then `sensors 32 25` — open pin first, closed pin
second. Use `-1` for an end with no switch fitted. Then `s`:

```
  position     : open GPIO 32, closed GPIO 25, active LOW — reads OPEN MADE
```

With the motor still inhibited, walk the door by hand to each end of travel (or
just pass the magnet across each reed) and read `s` again. Expect **exactly one
end MADE at a time**. Both MADE at once is physically impossible and means a
shorted pair or a pin collision — the firmware will take the pins but it cannot
detect that fault for you.

The switches are wired **normally open to GND**, read with `INPUT_PULLUP`, so a
pin idles HIGH and reads LOW only at that end. That polarity is deliberate: a
broken wire or a magnet that has fallen off reads as "not at that end", never as
a false arrival.

**3 — Vibration sensor.** `w`, then `vibration 33`. Now `s`, tap the sensor, and
`s` again:

```
  vibration    : GPIO 33, 412 edges since boot (pull-up on)
```

The count must climb. It counts edges in an ISR rather than polling because
these modules emit pulses shorter than the 100 ms control tick — polling samples
between them and concludes the door never moved.

Mount it **on the door, not on the controller board**. A sensor bolted beside
the relay hears the relay on every actuation whether or not the door moved,
which is exactly the signal it exists to tell apart from movement.

**4 — Status LED.** `w`, then `led` — three flashes, the same trick as `beep`.

```
led                  three flashes on the status LED
```

Nothing at all means the wrong pin. Lit solid and then going dark means it is
wired backwards. The test overrides every other pattern while it runs, faults
included, and expires by itself after about 2.4 seconds — a status light you
could leave stuck in test mode would be a status light that lies.

To check the LED is telling the truth about the *door* rather than just that the
pin works, disconnect the motor, leave maintenance mode with `M`, and use the
relay recipe above: `o`, then `x`. Solid after `o`, brief blip every 2 s after
`x`. The full pattern table is under [Status LED](#status-led).

| Step | Command | Working looks like |
|---|---|---|
| Buzzer | `w` → `beep` | three beeps |
| Limit switches | `w` → `sensors <o> <c>`, then `s` | exactly one end `MADE` |
| Vibration | `w` → `vibration <pin>`, then `s` | edge count climbing |
| Status LED | `w` → `led` | three flashes |

---

## Flashing a board with no auto-reset

> Doing this at an installed door? [FLASHING.md](FLASHING.md) is the field
> runbook — what to bring, what to build first, and the order to do it in.

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

> **Add `--protocol network` if the upload fails.** `arduino-cli` finds network
> boards over mDNS, and when discovery has not completed it treats the address
> as a serial port and tries to talk esptool at it — which fails with a bare
> `exit status 2`. The flag says explicitly that this is a network upload:
>
> ```bash
> arduino-cli upload --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
>   -p 192.168.1.66 --protocol network --upload-field password=... petdoor
> ```

> **The push must come from the same build you are looking at.** Use
> `compile --upload`, or push the `.bin` from a compile you just ran — a bare
> `arduino-cli upload` ships whatever happens to be sitting in the build
> directory, which has written a Bluedroid image to a NimBLE door before now.
> The hint the firmware prints is a bare `upload`; treat it as the address
> rather than the command.

Three things it deliberately does:

- **Refuses while the beacon is present.** An update reboots the door and shares
  the radio; neither should happen with your pet at the doorway.
- **Times out** after `OTA_WINDOW_MS`, so a forgotten window hands the antenna
  back rather than starving BLE indefinitely. `P` closes it early.
- **Requires a password.** Without `OTA_PASSWORD` set, OTA is off — otherwise
  anyone on your network could reflash the door.

> The radio is up for the whole window, so BLE sampling is degraded throughout.
> That is why it is a window you open rather than a service that listens.

#### Confirm the image before you reboot the door

A freshly pushed image boots **on probation**. The bootloader holds it in
`PENDING_VERIFY` and rolls it back to the previous slot on the next reset
unless the firmware marks it good — and it only does that once **an upload to
your log server has succeeded**, because an image that boots but cannot be
managed is exactly the one not to keep.

So after a push:

```
u                     force an upload now
[ota] image confirmed good (upload succeeded); rollback cancelled
```

Until you see that line, **any reset loses the update** and you are back on the
old build with no warning beyond the version in the banner. `s` reports the
state, and a door left alone will get there on its own at the next heartbeat —
but that can be up to `WIFI_HEARTBEAT_MS` (30 minutes by default) away, because
opportunistic uploads wait for the door to be idle and *a door sitting open is
not idle*.

This is easy to trip over: push, reset to "make sure it took", and the reset is
what un-takes it. It was tripped over on real hardware — the banner read two
builds old and nothing else said why.

#### If `p` says "could not associate"

Open a **maintenance window** first (`M`), then `p`.

Associating is abandoned early for an *opportunistic* radio request so a
missing access point cannot hold BLE hostage. An OTA request now counts as
deliberate and waits out `WIFI_CONNECT_TIMEOUT_MS` — but on firmware built
before that fix, `p` could only associate when the door was already idle, and
"idle" requires the door to be **CLOSED**.

That made the door you most need to update the one that refused: a door sitting
open with the beacon away is where a stall, an exhausted close retry, or a flat
beacon battery leaves it. A maintenance window forces the idle flag true, which
is why it is the workaround — and why OTA appeared to work whenever one
happened to be open.

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
