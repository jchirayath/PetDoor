# Tuning

Choosing thresholds that work in *your* coop.

The defaults are a starting point, not a recommendation. RSSI depends on your
beacon's transmit power, your coop's walls, where the ESP32's antenna sits, and
what else is radiating nearby. Two identical builds ten metres apart will want
different numbers.

Budget twenty minutes and a helper, or a long extension lead.

---

## What you are actually choosing

Four numbers, and only the first two need much thought:

| | What it means |
|---|---|
| `RSSI_ENTER_DBM` | Signal strength at the point where you want the door to open |
| `RSSI_EXIT_DBM` | Signal strength below which the beacon is convincingly gone |
| `ENTER_CONFIRM_MS` | How long "near" must hold first |
| `EXIT_CONFIRM_MS` | How long "far" must hold first |

RSSI is negative and measured in dBm. **Less negative is stronger and closer.**
−50 is a strong signal; −90 is barely there. This trips everyone up at least
once: `-65` is *greater* than `-75`.

---

## Before you start

- Mount the ESP32 **where it will actually live**, with its real enclosure and
  its real power supply. Moving it 30 cm or putting it in a metal box changes
  every number you are about to measure.
- Put the beacon **where it will actually be** — on the collar, in the pocket,
  wherever. A beacon in your hand at chest height reads very differently from
  one at ground level on a bird.
- Have the door mechanism **disconnected** for the walking-around part — or
  better, use **maintenance mode** (below), which stops the door acting without
  anybody unplugging anything.

---

## Calibrating a door that is already mounted

The two bullets above pull in opposite directions. The numbers are only valid
for the door **where it lives** — the ESP32's antenna is a trace on the PCB and
it is directional, so the same beacon at the same distance can read 7 dB apart
depending on which way the board faces and what metal is beside it. A threshold
calibrated on a bench describes the bench.

But calibrating in place means standing at the door holding the collar, which is
exactly the condition that makes the door actuate. Measuring it changes it, and
the relay cycles the whole time you are trying to read a number.

**Maintenance mode is the way out.** For a bounded window the door keeps
listening to the beacon and stops acting on it:

```
M                      on the console — a 60-minute window
maint 30               from the server or the Controls tab — 30 minutes
maint off              end it early
```

While a window is open:

- **the beacon cannot open or close the door.** Note this blocks *both*
  directions, unlike `lock`, which still lets a locked door close when the
  collar leaves. You are standing at the door holding the collar; a door that
  shuts on you is no use for measuring and no fun to stand in.
- **`o` and `x` still work.** The window stops the *beacon* commanding the
  door. It does not take the door away from you.
- **the console is reachable over WiFi**, which is the point — see below.
- **the door accumulates the RSSI distribution** at whatever position the
  collar is in. `s` shows it; `r` clears it when you move.

### It ends by itself, and that is deliberate

Every window has a deadline. It is capped by `MAINT_MAX_MS` (4 hours), it is
never written to flash, and it does not survive a reboot. A door left inert by a
forgotten flag, a dropped network, or a brownout is a door that cannot let an
animal in that night — so "I will turn it off later" is not something this mode
lets you rely on. The dashboard shows the remaining minutes for the same reason.

### The console, without a cable

Set `CONSOLE_PASSWORD` in `secrets.h`, then during a window:

```
nc 192.168.1.42 23           # the door's address; telnet works too
password: ...
```

The door's address is on the **Controls tab** while a window is open, and the
serial log prints it when the window opens. Do not count on `petdoor.local` —
mDNS is only registered while an OTA window is running, not for this.

You get the **whole** console — `c`, `t`, `f`, `s`, `l`, everything this page
describes. That is the difference between calibrating a mounted door and taking
it off the wall to do it.

Two things worth being clear about. This is the same console the cable offers,
so **it can open your door**; that is why it only listens during a window, why
the window expires, and why an unset `CONSOLE_PASSWORD` disables it entirely.
And it is **plaintext on your LAN** — an acceptable trade for a bounded window
on a home network, and not something to forward through your router.

### The measurement itself

Percentiles, not averages. The average is what misleads: a collar sitting a hair
off the close threshold has a perfectly reasonable-looking mean and still never
produces an unbroken stretch long enough to act on. What sets a usable threshold
is the **tails** — the strongest reading from away, and the weakest from at the
door — because those are what the door has to tell apart.

```
calibration  : n=482  min -104  p5 -74  median -63  p95 -47  max -42
```

Walk the collar to a position, press **`r`**, wait a minute, press **`s`**.
Repeat for each position that matters. Then:

- the **open** threshold must sit above the *strongest* reading you saw from
  away (its `max`, or `p95` if you are willing to discard outliers);
- the **close** threshold must sit below the *weakest* reading you saw at the
  door (its `min`, or `p5`).

**If those two overlap, no threshold pair works** and no amount of tuning will
fix it. That is a real result, not a failed measurement: it means the radio
cannot distinguish those two positions from where the board is mounted. The
answers are to move or reorient the board, or to fit the limit switches in
[HARDWARE.md](HARDWARE.md) and stop inferring position from signal strength.

---

## The short way

If you just want the door to open where the beacon is standing right now, put
it there, press **`t`** in the console and type **`here`**. That reads the
current filtered value, sets the open threshold 3 dB below it and the close
threshold 10 dB below that, and saves it on the device — no reflash, no maths,
and no dependence on the distance model.

The rest of this page is the manual method, which is worth doing if you want to
understand or fine-tune what those numbers mean.

## Step 1 — turn on the calibration stream

Open the serial monitor at **115200 baud** and type `c`:

```
[cmd] calibration stream ON
  Walk to where the door should open and note the filtered value.
  Set RSSI_ENTER_DBM a little below it, RSSI_EXIT_DBM 8-12 dBm lower.
[cal] raw  -61  filtered  -58 dBm  ~1.8 m  PRESENT
[cal] raw  -77  filtered  -71 dBm  ~5.2 m  absent
```

Two readings per second. The columns:

- **raw** — the most recent advertisement, unfiltered. Watch how much it jumps
  while standing perfectly still. That jitter is the reason the filter exists.
- **filtered** — after the slow median and EWMA. **This is the number you tune
  against.** It is what the close decision reads. The open decision reads a
  second, faster filter over the same samples (see below); it tracks the same
  signal a second or so earlier, so tuning against `filtered` is still correct.
- **~m** — a rough distance estimate from the path-loss model. Indicative only;
  it plays no part in any decision. Ignore it for tuning.
- **PRESENT / absent** — the current state machine output.

If you see `[cal] no fix (last seen ...)`, the beacon is not being heard at all
— go to [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## Step 2 — measure the open point

Stand where you want the door to **open**. Hold still for thirty seconds and
watch the *filtered* column settle.

Write down the value it hovers around. Call it `OPEN_HERE`.

Now walk in and out through that point a few times. You will notice the filtered
value lags by a second or two — that is the EWMA doing its job. The door itself
will already have opened by then: the open decision reads a faster filter.

## Step 3 — measure the gone point

Walk to where you consider the beacon **definitely gone** — the far end of the
run, inside the house, wherever "not here" means for you. Wait thirty seconds.

Write that down as `GONE_HERE`.

## Step 4 — pick the numbers

```c
#define RSSI_ENTER_DBM  (OPEN_HERE - 3)    // a few dB of margin below your open point
#define RSSI_EXIT_DBM   (RSSI_ENTER_DBM - 10)
```

So with `OPEN_HERE` at −58:

```c
#define RSSI_ENTER_DBM -61
#define RSSI_EXIT_DBM  -71
```

**The gap between them is the hysteresis band**, and 8–12 dB is the useful
range. Then sanity-check against `GONE_HERE`:

- If `GONE_HERE` is **weaker than** `RSSI_EXIT_DBM` — good, the door will close.
- If `GONE_HERE` is **stronger than** `RSSI_EXIT_DBM`, the door will never
  close. Your "gone" position is still too close, or the gap is too wide.
  Narrow it toward 8 dB, or accept that the door closes only when you go
  further away.

Put both in `secrets.h`, reflash, and re-run the calibration stream to confirm
the `PRESENT` / `absent` flag now flips where you want it.

### Why margin below the open point

`RSSI_ENTER_DBM` a few dB *below* your measured value, not equal to it, because
the measurement was taken standing still in good conditions. Rain, a different
coat, the beacon facing away from the antenna, a bird standing in the way — all
of these cost a few dB. Without margin the door becomes unreliable in exactly
the conditions where you most want it to work.

---

## Step 5 — tune the dwell times

The defaults are good. Change them only for a specific complaint.

| Complaint | Change |
|---|---|
| Door takes too long to open when you arrive | Lower `ENTER_CONFIRM_MS` toward 750 |
| Door opens when you merely walk past | Raise `ENTER_CONFIRM_MS` to 3000–5000 |
| Door closes while the animal is still around | Raise `EXIT_CONFIRM_MS` to 30000+ |
| Door stays open far too long after you leave | Lower `EXIT_CONFIRM_MS` toward 8000 |

Two constraints:

- **Keep `EXIT_CONFIRM_MS` well above `MIN_ACTUATION_INTERVAL_MS`** (5 s default)
  or the actuation lockout delays closing. A `static_assert` enforces this.
- **Do not drop `EXIT_CONFIRM_MS` below about 5 s** regardless. It is what rides
  out signal dropouts, and dropouts of several seconds are completely normal.

Asymmetry is intentional. Opening late is an annoyance; closing early is a
closed door with an animal on the wrong side of it.

---

## Step 6 — tune the filter, if you need to

Only reach for these if the thresholds alone are not getting you a stable
result.

There are two filters, and they are tuned for opposite things. The **close**
pair (`RSSI_MEDIAN_WINDOW` / `RSSI_EWMA_ALPHA`) wants to be slow and unshakeable.
The **open** pair (`RSSI_FAST_WINDOW` / `RSSI_FAST_ALPHA`) wants to be quick.

| Symptom | Change | Cost |
|---|---|---|
| Door closes on a brief signal dropout | `RSSI_MEDIAN_WINDOW` 7 → 11 | Slower to close. **No effect on opening.** |
| Occasional single wild readings get through | `RSSI_MEDIAN_WINDOW` 7 → 9 or 11 | Slower to close only |
| Filtered value is stable but sluggish to fall | `RSSI_EWMA_ALPHA` 0.35 → 0.5 | Closing gets twitchier |
| Very slow to notice you have arrived | lower `ENTER_CONFIRM_MS` | A brief strong reflection can open the door |
| Door still opens sluggishly at `ENTER_CONFIRM_MS` | already at the floor — nothing left to gain here | — |
| Door opens too eagerly | raise `ENTER_CONFIRM_MS` | Slower to open |

The most common mistake before the filters were split was widening the median to
stop false closes and then finding the door had become slow to open. That
trade-off is gone: **the close pair no longer costs any open latency**, so widen
it freely.

`RSSI_MEDIAN_WINDOW` must be **odd and between 3 and 15**. A bigger window
rejects more outliers but adds lag *to closing*: at ~10 samples per second, a
window of 11 is roughly a second of extra delay.

`RSSI_EWMA_ALPHA` is 0.0–1.0. Lower is smoother and slower; 1.0 disables the
smoothing entirely and leaves you with just the median.

### Touching the open pair

Most people never need to. `RSSI_FAST_WINDOW 1` / `RSSI_FAST_ALPHA 0.9` already
puts open latency at `ENTER_CONFIRM_MS` exactly — the filter adds nothing, so
there is no lag left to remove. The dwell timer is what confirms an arrival, so
reach for `ENTER_CONFIRM_MS` rather than these if the door opens too eagerly or
too slowly.

The open pair may never be slower than the close pair
(`RSSI_FAST_WINDOW ≤ RSSI_MEDIAN_WINDOW`, `RSSI_FAST_ALPHA ≥ RSSI_EWMA_ALPHA`).
The firmware refuses the reverse at compile time and at the console, because it
would mean noticing an animal leaving before noticing one arriving.

From the `f` console menu:

```
open 1,0.9    set the open pair directly
open same     make the open pair match the close pair
              (restores the old single-filter behaviour)
```

---

## Step 7 — the real test

Reconnect the door, with the coop **empty**, and live with it for a day.

Type `s` whenever you want to see what it is thinking:

```
---- status ----
  target       : MAC ac:23:3f:11:22:33
  uptime       : 3721 s
  presence     : PRESENT
  door         : OPEN
  rssi         : -58 dBm filtered (raw -61), ~1.8 m
  last seen    : 112 ms ago, 34821 samples
  closing in   : 11200 ms
  radio        : 291043 adverts, last 44 ms ago, 0 samples dropped
  free heap    : 189204 bytes
```

`closing in` and `opening in` appear only while a transition is pending, and are
the fastest way to tell whether the state machine is doing what you expect.

Watch for:

- **Door cycling repeatedly** — your hysteresis band is too narrow, or the
  beacon is sitting right at the enter threshold. Widen the gap, or move the
  threshold so the resting position is not on the boundary.
- **Door never closing** — `GONE_HERE` is stronger than `RSSI_EXIT_DBM`. Recheck
  step 4.
- **Door opening at odd times** — something else matched, or your enter
  threshold is far too weak. Check `target` in the status output.
- **`samples dropped` climbing** — the control task is falling behind. Harmless
  in small numbers; if it grows steadily, something is blocking `controlTask`
  (a very long `RELAY_PULSE_MS` is the usual cause).

---

## A note on what RSSI can and cannot do

RSSI is not a rangefinder. The distance estimate the firmware prints comes from
a log-distance path-loss model with a single fudge factor
(`PATH_LOSS_EXPONENT`), and it is wrong by a factor of two more often than not.
It is shown because it is a more intuitive way to watch a trend than dBm, and
for **nothing else** — no decision in this firmware uses it.

Tune against the filtered dBm value. Trust the `PRESENT` / `absent` flag. Treat
the metres as decoration.
