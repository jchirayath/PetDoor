# Beacon setup

Finding your beacon's address and, if you want to, configuring it.

The reference build uses a [Minew](https://www.minew.com/) beacon, but nothing
here is Minew-specific except the vendor app. Any beacon with a fixed address
works — see [HARDWARE.md](HARDWARE.md#the-beacon).

---

## You do not need the vendor app

The firmware finds the beacon for you. **Discovery mode is the ground truth** —
it shows you exactly what the ESP32 can hear, from where the ESP32 actually sits,
which is more useful than what a phone app claims from across the room.

For most builds the whole beacon setup is: take the battery tab out, read the
MAC out of the discovery table, put it in `secrets.h`.

---

## Step 1 — activate the beacon

Most beacons ship with a plastic pull-tab isolating the coin cell. Pull it out.
Some need a button press to wake.

A beacon that is advertising will show up in any BLE scanner app within seconds.
If it does not, the battery is dead or the tab is still in.

## Step 2 — flash the firmware with nothing configured

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 petdoor
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/cu.usbmodemXXXX petdoor
```

With no beacon configured the firmware boots into **discovery mode** and
explicitly will not touch the door:

```
  !! NO BEACON CONFIGURED — the door will not be operated. !!
  Discovery mode is on. Find your beacon below, then set
  BEACON_MAC in secrets.h (copy secrets.example.h) and reflash.
  A Minew beacon usually shows up as class Minew or iBeacon.
```

## Step 3 — read the discovery table

Serial monitor at **115200 baud**. Every two seconds:

```
---- BLE devices in range ----
  MAC                RSSI  Age     Class      Name / manufacturer data
   ac:23:3f:11:22:33  -52    118ms Minew      MST01 mfg=4c000215...
   65:00:5e:00:00:01  -78    402ms AirTag     mfg=000000000000...
   c8:00:5e:00:00:08  -85   1204ms Generic     svc=0000fe9f-...
  3 device(s), 14206 advertisements total
```

Columns:

- **MAC** — the address you will configure. A `->` in the left margin marks a
  device matching your configured target (nothing will be marked yet).
- **RSSI** — signal strength. Less negative is closer.
- **Age** — how long since this device was last heard. A beacon advertising
  properly stays under a second.
- **Class** — best-effort labelling to help you spot yours: `Minew`, `iBeacon`,
  `Eddystone`, `AirTag`, `Chipolo`, `Apple`, `Fitness`, `Generic`, `Unknown`.
  It is a display hint only and plays no part in matching.
- **Name / manufacturer data** — the advertised name, if the beacon sends one,
  plus the raw manufacturer bytes.

### Identifying which one is yours

Do not guess from the class label. **Confirm by moving it:**

1. Put the beacon right next to the ESP32. Note which MAC has the strongest
   RSSI — typically −40 to −55.
2. Walk it twenty metres away. That MAC's RSSI should drop sharply, to −80 or
   weaker.
3. Bring it back. It should recover.

The one that tracks your movements is your beacon. Write the MAC down.

If two candidates both track you, cover one with your hand or put it in a metal
tin — the other is the one that keeps reading strongly.

> **The table only holds `DEVICE_TABLE_SIZE` (40) devices**, with least-recently
> seen eviction. In a dense apartment block yours may be pushed out. Move closer
> to the ESP32 and further from everything else, or raise `DEVICE_TABLE_SIZE`
> temporarily.

## Step 4 — configure it

```bash
cp petdoor/secrets.example.h petdoor/secrets.h
```

```c
#define BEACON_MAC "ac:23:3f:11:22:33"
```

Case does not matter; the firmware lower-cases both sides before comparing.
`secrets.h` is git-ignored, so your beacon's address never reaches the public
repo and `git pull` will not clobber it.

Reflash.

## Step 5 — confirm the match

The banner should now name your target:

```
  target beacon : MAC ac:23:3f:11:22:33
```

Type `s`:

```
  presence     : PRESENT
  rssi         : -52 dBm filtered (raw -55), ~1.2 m
  last seen    : 94 ms ago, 1204 samples
```

**`samples` climbing steadily is the thing to check.** It should rise by roughly
10 per second with a beacon advertising at 100 ms. If it is stuck at 1, you are
looking at the duplicate-filter bug — see
[ARCHITECTURE.md](ARCHITECTURE.md#the-duplicate-filter-trap) and
[TROUBLESHOOTING.md](TROUBLESHOOTING.md).

Discovery mode does not start when a beacon is configured, because the table
costs heap and CPU in the BLE callback. Turn it on any time with `d`.

Now go to [TUNING.md](TUNING.md).

---

## Matching on iBeacon identity instead

Matching by MAC ties you to one physical beacon. If it is lost or its battery
housing cracks, you must reflash the ESP32 with the replacement's MAC.

Matching by **iBeacon UUID/major/minor** avoids that: program a spare beacon
with the same identity and swap it in with no reflash.

```c
#define BEACON_UUID  "E2C56DB5-DFFB-48D2-B060-D0F5A71096E0"
#define BEACON_MAJOR 1      // -1 for "any"
#define BEACON_MINOR 1      // -1 for "any"
```

Dashes optional; the parser accepts them or bare hex, and requires exactly 32
hex digits. If it is not a valid UUID the firmware says so at boot and ignores
it:

```
[ble] BEACON_UUID is not a valid 128-bit UUID; ignoring it.
```

**Matchers are ANDed.** Setting both `BEACON_MAC` and `BEACON_UUID` means the
beacon must satisfy both — the strictest option, and the one that defeats the
point of a swappable spare. For a swappable spare, set the UUID and leave
`BEACON_MAC` empty.

### Reading the iBeacon identity from the table

An iBeacon's manufacturer data decodes as:

```
4c 00 | 02 15 | <16-byte UUID> | <major> | <minor> | <power>
└ Apple │ iBeacon
```

So in `mfg=00000000000000000000000000000000000000000000000`**`1c5`**:

| Bytes | Meaning |
|---|---|
| `4c00` | Apple company ID |
| `0215` | iBeacon type and length |
| `e2c56db5dffb48d2b060d0f5a71096e0` | UUID |
| `0001` | major = 1 |
| `0001` | minor = 1 |
| `c5` | measured power, −59 dBm at 1 m |

The table truncates manufacturer data to 40 hex characters, which is enough to
read the UUID and the start of the major.

---

## Configuring a Minew beacon

Only needed if you want to change the advertising interval, transmit power, or
iBeacon identity. Out of the box, with the tab pulled, a Minew beacon advertises
happily and MAC matching works with no configuration at all.

Minew's configuration app is **BeaconSET+** (iOS and Android; Minew also ships a
desktop tool). The general flow for these tags:

1. Install the app and open it near the beacon.
2. Connect to the beacon from the list. The default connection password for
   Minew tags is commonly `minew` — check the card in the box, since this varies
   by model and firmware.
3. Change what you need:
   - **Advertising interval** → 100–300 ms for a good sample stream.
   - **Transmit power** → mid-range to start.
   - **UUID / major / minor** → only if you are matching on iBeacon identity.
4. Save, disconnect, and **confirm in the discovery table** that the beacon is
   still being heard and still has the MAC you expect.

That last step matters. Vendor apps vary between models and firmware revisions,
and a setting that looks applied in the app may not be. The discovery table is
what the ESP32 actually hears, and it is the only confirmation that counts.

> Changing the transmit power **invalidates your tuning**. Re-run
> [TUNING.md](TUNING.md) afterwards.

---

## Battery life

A coin-cell beacon at a 100 ms advertising interval typically runs for several
months; at 1000 ms, a year or more. The exact figures depend on the model,
transmit power and temperature, and vendor claims are optimistic.

The firmware **fails safe** when the battery dies: the target has still been
heard since boot, so the signal goes stale, stale counts as far, and the door
closes after `EXIT_CONFIRM_MS`. It stays closed until the beacon returns.

A beacon that is already dead when the ESP32 boots is handled separately — the
door is never operated at all until the target has been heard once since boot,
so a flat battery cannot be mistaken for "the animal has gone away".

Signs the battery is going:

- Range dropping — the door opens only when you are much closer than before.
- `samples dropped` staying at zero while the total `samples` count climbs more
  slowly than it used to.
- The `Age` column in the discovery table creeping up.

Keep a spare cell, and check it when you do the seasonal coop clean.

---

## Changing the battery

Do this with the animal clear of the doorway. The moment the beacon stops
advertising the signal goes stale, stale counts as far, and the door closes
after `EXIT_CONFIRM_MS`.

**Getting the case open.** Look for screws before you pry anything. The tag in
the reference build has them, and on these tags they are usually hidden — under
the label, or under the rubber feet — so peel the label back at one corner and
check. Forcing a screwed case open splits the shell.

Other tags of this shape are a two-part snap-fit shell instead, with a seam
running round the edge and a notch or flattened spot on that seam as the
intended pry point. Work a plastic spudger, guitar pick or thumbnail in there
and walk it round the perimeter; the clips let go one at a time. Use plastic,
not a screwdriver: a gouged seam may not close weather-tight again, and this tag
lives in a coop.

If the tag has a rubber O-ring in the seam, make sure it seats back in its
channel on reassembly. That gasket is the only thing keeping condensation off
the board.

**The cell.** Note which way up it sits before you lift it out — positive face
toward the back cover on every one of these tags we have seen, but check rather
than assume. Lift it with a fingernail or a plastic tool; sliding metal across
the contacts shorts the cell. Match what you pull: these tags take a CR2477 or a
CR2032 depending on the model, and the two are not interchangeable.

**Afterwards.** The MAC does not change across a battery swap, so nothing needs
reconfiguring — `TARGET_MAC` still matches and your tuning still holds. Confirm
it anyway, in discovery mode:

- the beacon is back in the table, with the MAC you expect;
- its `Age` is small and its interval is what you set.

The discovery table is what the ESP32 actually hears. A tag that looks alive
because its LED blinked is not the same as a tag the door can see.
