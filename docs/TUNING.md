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
- Have the door mechanism **disconnected** for the walking-around part. You are
  measuring signal, not cycling a motor.

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
- **filtered** — after the median and the EWMA. **This is the number you tune
  against.** Everything the door decides uses this value.
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
value lags by a second or two — that is the EWMA doing its job.

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

| Symptom | Change | Cost |
|---|---|---|
| Filtered value still jumps around a lot | `RSSI_MEDIAN_WINDOW` 7 → 11 | Slower to react |
| Filtered value is stable but sluggish | `RSSI_EWMA_ALPHA` 0.35 → 0.5 | More jitter |
| Occasional single wild readings get through | `RSSI_MEDIAN_WINDOW` 7 → 9 or 11 | Slower to react |
| Very slow to notice you have arrived | `RSSI_EWMA_ALPHA` 0.35 → 0.5, or lower `ENTER_CONFIRM_MS` | More jitter |

`RSSI_MEDIAN_WINDOW` must be **odd and between 3 and 15**. A bigger window
rejects more outliers but adds lag: at ~10 samples per second, a window of 11 is
roughly a second of extra delay.

`RSSI_EWMA_ALPHA` is 0.0–1.0. Lower is smoother and slower; 1.0 disables the
smoothing entirely and leaves you with just the median.

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
