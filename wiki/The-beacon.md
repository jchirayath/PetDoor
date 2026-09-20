# The beacon

The collar wears a **BLE beacon** — a coin-cell tag about the size of a large
button, roughly **$12**. The reference build uses a
[Minew](https://www.minew.com/) tag.

Not an AirTag. The reason matters, and it is not a preference.

---

## Why consumer trackers fail

Two independent reasons, either of which alone rules them out. Both were
confirmed on this hardware.

### They rotate their address on purpose

AirTags, Tiles and SmartTags deliberately change their Bluetooth address so they
cannot be used to follow people. Good feature; fatal here, because there is
nothing stable to match against.

One AirTag watched for **under two hours produced four distinct addresses**,
none overlapping.

A beacon meant for infrastructure does the opposite: a **fixed public address**,
usually printed on the case, kept forever.

### They advertise far too slowly

Measured with the AirTag **two feet** from the ESP32 — its absolute best case —
against a Minew sitting further away:

| | shortest gap | typical gap | longest gap |
|---|---|---|---|
| AirTag at 2 ft | 965 ms | **5,140 ms** | 17,865 ms |
| Minew, further away | 72 ms | **349 ms** | 885 ms |

Roughly **15× faster, from further away**.

This is not tunable around. A reading older than three seconds stops counting as
live, and the AirTag's *typical* gap already exceeds that — so it reads as
absent most of the time even at point-blank range. Its worst gap is long enough
that the door would close while the tag sat beside it.

> **Check any candidate yourself** with the `worst gap` figure in the `s` status
> output. If it approaches three seconds, that beacon cannot drive this door.

---

## What to buy instead

Look for the word **beacon**, not *finder* or *anti-loss* — the latter are the
ones that rotate addresses.

| Requirement | Why |
|---|---|
| Fixed public MAC | The door matches on it |
| Configurable advertising interval | You want 100–300 ms; many ship at 1000 ms |
| User-replaceable coin cell | You will change it |
| Small and light | It lives on a collar |

Also fine: any iBeacon tag from a beacon vendor, an Eddystone beacon, an
nRF51/nRF52 dongle with beacon firmware, or a second ESP32 running an advertiser
sketch (needs power, so better for fixed installations than collars).

---

## Settings that matter

Out of the box, with the tab pulled, it advertises immediately and MAC matching
works with **no configuration at all**. You only need the vendor app to change:

- **Advertising interval → 100–300 ms.** The most important setting: it decides
  how often the door gets a reading, and so how fast it can react. Faster costs
  battery roughly linearly.
- **Transmit power → leave it mid-range.** Raising it for more range *flattens*
  the signal curve near the door, which makes your tuned threshold worse at
  separating "at the door" from "three metres away". Lower power gives a
  steeper curve and a sharper open point.

> Changing transmit power **invalidates your tuning**. Re-run
> [TUNING.md](https://github.com/jchirayath/PetDoor/blob/main/docs/TUNING.md).

Minew's app is **BeaconSET+**; the default connection password is commonly
`minew` — check the card in the box. Always confirm the change in the door's own
discovery table (`d` on the console) rather than trusting the app: the `Age`
column is what the ESP32 actually hears, and it is the only confirmation that
counts.

---

## Battery

Several months at 100 ms; a year or more at 1000 ms. Cold weather shortens it
noticeably.

**It fails safe.** When the cell dies the signal goes stale, stale counts as
far, and the door closes and stays closed. A beacon that is *already* dead at
boot is handled more carefully still — the door is not operated at all until the
beacon has been heard once, so a flat battery can never be read as "the animal
has gone away".

The dashboard's beacon-signal chart is the early warning: a steady decline shows
up weeks before the door starts misbehaving.

---

## Where to wear it

A body is mostly water, and water absorbs 2.4 GHz.

- **Top or side of the neck** — best.
- **Hanging under the chin** — workable; what most mounts give you.
- **Under a lying animal** — worst. A dog that lies down in the doorway can put
  its own body between the beacon and the ESP32.

Full detail:
[BEACON-MINEW.md](https://github.com/jchirayath/PetDoor/blob/main/docs/BEACON-MINEW.md)
and
[BEACON-SETUP.md](https://github.com/jchirayath/PetDoor/blob/main/docs/BEACON-SETUP.md).
