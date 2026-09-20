# The Minew beacon

The reference build wears a **Minew** BLE tag on the collar. This page covers
what it is, why this rather than something you already own, how to configure it,
and what to expect from the battery.

Nothing in the firmware is Minew-specific. Any beacon with a **fixed address**
and a **fast advertising interval** works, and the two requirements are the whole
story — see [what else will work](#what-else-will-work). Minew is simply what
the reference door runs, so it is what the measurements below come from.

For the step-by-step of pointing the door at *your* beacon, see
[BEACON-SETUP.md](BEACON-SETUP.md). This page is about the hardware.

---

## Why a purpose-built beacon at all

The obvious idea is to use something already on the market — an AirTag, a Tile,
a phone. All of them fail, and for the same two reasons.

### 1. Privacy features rotate the address

Consumer trackers deliberately change their Bluetooth address so they cannot be
used to follow people. That is a good feature and it makes them useless here:
there is nothing stable to match against. One AirTag watched for **under two
hours produced four distinct addresses**, none overlapping.

A beacon meant for infrastructure does the opposite. It advertises a **fixed
public address**, printed on the device, and keeps it forever.

### 2. They advertise far too slowly

Measured on this hardware, with the AirTag **two feet** from the ESP32 — its
absolute best case — against a Minew sitting further away:

| | shortest gap | typical gap | longest gap |
|---|---|---|---|
| AirTag at 2 ft | 965 ms | **5,140 ms** | 17,865 ms |
| Minew, further away | 72 ms | **349 ms** | 885 ms |

The Minew is roughly **15× faster, from further away**.

This is not a tuning problem. `SAMPLE_MAX_AGE_MS` is 3 seconds — a reading older
than that stops counting as live. The AirTag's *typical* gap already exceeds it,
so it reads as absent most of the time even at point-blank range, and its worst
gap exceeds `EXIT_CONFIRM_MS`, meaning the door would close while the tag sat
beside it.

> The `worst gap` figure in the `s` status output is how you check any candidate
> beacon yourself. If it approaches `SAMPLE_MAX_AGE_MS`, that beacon cannot
> drive this door reliably.

---

## Which one to buy

Look for a **coin-cell iBeacon tag with a fixed MAC**. Minew's small tags —
the **MST01** is the common one — are cheap, widely available, and light enough
to sit on a collar without annoying the animal.

What actually matters when choosing:

| Requirement | Why |
|---|---|
| **Fixed public MAC** | The door matches on it. No rotation, no privacy mode. |
| **Configurable advertising interval** | You want 100–300 ms. Many tags ship at 1000 ms. |
| **Coin cell, user-replaceable** | You will change it. Sealed units are throwaway. |
| **Small and light** | It lives on a collar. |
| **Printed MAC** | Saves you hunting for it in the discovery table. |

Avoid anything sold primarily as a *finder* or *anti-loss* tag — those are the
ones with rotating addresses. The word to look for is **beacon**.

Expect to pay around **$10–15**. This is the cheapest part of the build.

---

## Settings that matter

Out of the box, with the pull-tab removed, a Minew tag advertises immediately
and MAC matching works with **no configuration at all**. You only need the app
to change these:

| Setting | Recommended | Effect |
|---|---|---|
| **Advertising interval** | **100–300 ms** | The single most important setting. This sets how often the door gets a reading, and therefore how quickly it can react. |
| **Transmit power** | **mid-range to start** | Sets how far the beacon carries. Higher = longer range and a flatter RSSI curve near the door, which makes the distance threshold *less* precise. |
| **UUID / major / minor** | leave alone | Only needed if you match on iBeacon identity rather than MAC. |
| **Connectable / password** | leave alone | Only needed to reconfigure it later. |

### On the advertising interval

Faster is better for the door and worse for the battery, and the relationship is
roughly linear — 100 ms costs about ten times what 1000 ms costs.

- **100 ms** — the most responsive. Roughly ten readings a second.
- **200–300 ms** — the sweet spot for a collar. Still several readings a second,
  noticeably kinder to the cell.
- **1000 ms** — works, but the door has only one reading a second to filter, and
  `worst gap` starts creeping toward `SAMPLE_MAX_AGE_MS` whenever the animal's
  body blocks the radio.

The reference door runs a Minew at its stock interval and sees a **349 ms
typical gap**, which is comfortable.

### On transmit power

**Do not raise it to get more range.** It works, but it flattens the signal
curve near the door: if the beacon is loud, the difference between "at the door"
and "three metres away" shrinks, and the threshold you tuned stops separating
them. Lower power gives a *steeper* curve and a sharper open point.

> Changing transmit power **invalidates your tuning**. Re-run
> [TUNING.md](TUNING.md) afterwards — the RSSI numbers you measured no longer
> mean the same distances.

---

## Configuring it

Minew's app is **BeaconSET+** (iOS and Android; there is a desktop tool too).

1. Install it and open it next to the beacon.
2. Connect to the beacon from the list. The default connection password for
   Minew tags is commonly **`minew`** — check the card in the box, since this
   varies by model and firmware revision.
3. Change what you need from the table above.
4. Save, disconnect, and **confirm in the door's discovery table** that the
   beacon is still being heard and still has the MAC you expect.

That last step is not optional. Vendor apps vary between models and firmware
revisions, and a setting that looks applied in the app may not have been
written. Press `d` on the serial console and look:

```
MAC                 RSSI   Age    Class      Name / data
ac:23:3f:11:22:33   -52    118ms  Minew      MST01 mfg=4c000215...
```

The `Age` column is the proof. If you set 100 ms and `Age` still hovers near
1000 ms, the write did not take. **The discovery table is what the ESP32
actually hears, and it is the only confirmation that counts.**

---

## Battery life

A coin cell at 100 ms typically runs **several months**; at 1000 ms, **a year or
more**. Exact figures depend on the model, transmit power and temperature, and
vendor claims are optimistic. Cold weather shortens it noticeably — a cell that
lasted all summer can fade quickly in the first hard frost.

### Watching it decline

The dashboard's **beacon signal** chart plots the strength at each return. A
steady decline there is the early warning, typically weeks before the door
starts misbehaving. On the console, the same signs:

- Range dropping — the door opens only when you are much closer than before.
- Total `samples` climbing more slowly than it used to.
- The `Age` column in the discovery table creeping up.
- `worst gap` growing between checks.

The firmware also flashes the status LED if the beacon reports a low battery
over Eddystone-TLM — see `BEACON_LOW_BATTERY_MV` in
[CONFIGURATION.md](CONFIGURATION.md).

### What happens when it dies

**The door fails safe.** The beacon has been heard since boot, so the signal
simply goes stale; stale counts as far; the door closes after `EXIT_CONFIRM_MS`
and stays closed until the beacon returns.

A beacon that is **already dead when the ESP32 boots** is handled separately and
more carefully: the door is never operated at all until the target has been
heard once since boot. A flat battery can therefore never be mistaken for "the
animal has gone away and the door should close".

### Changing it

Do this with the animal clear of the doorway. The moment the beacon stops
advertising, the signal goes stale and the door closes after `EXIT_CONFIRM_MS`.

Look for screws before prying anything. Full procedure in
[BEACON-SETUP.md](BEACON-SETUP.md#changing-the-battery).

**Keep a spare cell**, and check it when you do the seasonal clean.

---

## Wearing it

The beacon goes on the collar, and where on the collar matters more than people
expect. A body is mostly water, and water absorbs 2.4 GHz.

- **On the top or side of the neck** — best. Line of sight to the door for most
  approaches.
- **Under the chin, hanging** — workable, and what most tag mounts give you.
- **Trapped between the animal and the ground** — worst. A dog that lies down in
  the doorway can put its own body between the beacon and the ESP32, and the
  signal drops sharply.

If `worst gap` is much larger than the advertising interval, body-blocking is
usually why. That is normal and the dwell timers exist to ride it out; it only
matters if it approaches `SAMPLE_MAX_AGE_MS`.

---

## What else will work

The firmware does not care about the beacon's format when matching by MAC. It
parses iBeacon frames only to read the **calibrated transmit power** (for the
displayed distance estimate) and the **UUID/major/minor** (if you match on
those instead).

So any of these are fine:

- **Any iBeacon tag from a beacon vendor** — Estimote, Kontakt.io, Blue Charm,
  or generic "iBeacon" tags.
- **An Eddystone beacon** — the firmware classifies it and can read its
  battery telemetry. Match on MAC.
- **A second ESP32** running an iBeacon advertiser sketch. Cheap, entirely under
  your control, trivially given a fixed address — but it needs power, so it
  suits a fixed installation rather than a collar.
- **An nRF51/nRF52 dongle** flashed with beacon firmware.

What will **not** work: AirTags, Tiles, SmartTags, phones, smartwatches, and
anything else that rotates its address for privacy. See
[the README](../README.md#can-i-just-use-an-airtag) for why in detail.

---

## One honest limitation

The door opens for **anything broadcasting your beacon's address**, not only for
your beacon. BLE advertisements are unauthenticated plaintext: the address is
readable by anyone within radio range using a free phone app, and rebroadcasting
it takes a $10 board.

There is no fix within BLE advertising, because there is no shared secret to
verify against. This is a property of the technology, not of this firmware.

In practice that is fine for keeping out weather, rodents and the neighbour's
cat, which is what the door is for. It is not fine as your only barrier against
a person. See [SAFETY.md](SAFETY.md).
