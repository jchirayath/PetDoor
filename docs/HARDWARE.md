# Hardware

What to buy, and why.

---

## Bill of materials

| Part | Notes |
|---|---|
| ESP32 dev board | Classic ESP32 (WROOM-32). Newer S3/C3/C6 also build. |
| BLE beacon | Must advertise a **fixed** address. See below. |
| 2-channel relay module | Or two logic-level MOSFET drivers. Rated for your door motor. |
| Door motor + controller | Whatever your coop door already uses. |
| 5 V supply | Sized for the ESP32 *and* both relay coils. 1 A minimum. |
| LED + 220 Ω–1 kΩ resistor | Optional; many dev boards have one you can reuse. |
| Weatherproof enclosure | Not optional in a coop. |

Total, excluding the door mechanism, is usually £20–35 / $25–45.

---

## The ESP32 board

**Recommended: a classic ESP32 WROOM-32 dev board.** They are cheap,
universally available, extremely well documented, and the reference build uses
one.

Any board with the ESP32 Arduino core 3.x support will work. Note:

- **Classic ESP32** uses the **Bluedroid** BLE stack.
- **ESP32-S3 / C3 / C6** use **NimBLE**.

The firmware builds on both, and deliberately only uses BLE APIs common to the
two stacks. If you contribute code here, keep it that way.

**ESP32-S2 will not work** — it has no Bluetooth radio at all. This catches
people out because the board looks identical to an S3.

### Antenna

The PCB trace antenna on a standard WROOM module is fine for this. What matters
much more is placement:

- Keep it **away from metal**. A metal enclosure is a Faraday cage; a metal roof
  30 cm away will reflect and null in ways that make your tuning meaningless.
- Keep the antenna end of the module **clear of the enclosure wall** and of the
  power supply.
- Mount it in its final position **before** tuning. See
  [TUNING.md](TUNING.md).

An external-antenna variant (U.FL connector) buys you range and a stable mount
point if the electronics have to live somewhere awkward. It is a nice upgrade,
not a requirement.

### Flash and partitions

The firmware uses about **83% of the default partition scheme** (roughly 1.09 MB
of 1.31 MB). That is comfortable but not roomy. A 4 MB board is the norm and is
plenty; if you add substantial features, switch to a larger app partition rather
than trimming existing ones.

---

## The beacon

**This is the part people get wrong, so it is worth being precise.**

You need a BLE device that advertises a **fixed, non-rotating Bluetooth
address**, continuously, forever, on battery.

### What will not work

- **A phone.** iOS and Android both randomise their BLE address every 15 minutes
  or so. This is a deliberate anti-tracking measure and you cannot switch it off.
- **An Apple AirTag.** Same reason, more aggressively. AirTags are *designed* to
  be untrackable by third parties. The firmware will happily show you one in the
  discovery table, labelled `AirTag`, and its MAC will be different next time you
  look.
- **A Tile, a Chipolo, most smartwatches, most fitness bands.** Same story.
- **Anything that sleeps.** A device that advertises only when you press a
  button gives you no sample stream.

If your "beacon" changes MAC, proximity will appear to work for a few minutes
after each reflash and then silently stop. That symptom is almost always this.

### What will work

A purpose-built BLE beacon. The reference build uses a
**[Minew](https://www.minew.com/)** beacon — the MST01 is a common, cheap,
coin-cell-powered tag with a fixed public address.

Other good options:

- Any iBeacon-format tag from a beacon vendor (Estimote, Kontakt.io, Blue Charm,
  generic AliExpress "iBeacon" tags).
- An Eddystone beacon — the firmware will see and classify it; match on MAC.
- **A second ESP32** running an iBeacon advertiser sketch. Cheap, totally under
  your control, and easy to give a known fixed address — but it needs power, so
  this suits a fixed installation rather than something on a collar.
- An nRF51/nRF52 dongle flashed with beacon firmware.

The firmware does not care about the beacon's format when matching by MAC. It
only parses iBeacon frames to extract the **calibrated transmit power** (for the
displayed distance estimate) and the **UUID/major/minor** (if you match on
those).

### Beacon settings that matter

| Setting | Recommendation |
|---|---|
| **Advertising interval** | 100–300 ms. Faster gives a better sample stream and a more responsive door; slower saves battery. |
| **Transmit power** | Start at the middle of the range. Higher = longer range but a flatter RSSI curve near the door, which makes tuning *harder*. |
| **Format** | iBeacon, if you want the distance estimate to display. Otherwise irrelevant. |

There is a real trade-off between advertising interval and battery life: a
coin-cell beacon at 100 ms might last months, where at 1000 ms it lasts a year
or more. Given the median filter wants a steady stream, err toward the faster
end and plan on changing batteries.

### Mounting the beacon

**On a collar is the normal case**, and what this project is tuned for. A small
BLE tag on a dog or cat collar sits at roughly the right height, moves with the
animal, and is easy to replace. Most beacon tags have a lanyard hole; a cable
tie or a small keyring loop through the collar is enough.

Two things to get right:

- **Height matters more than you expect.** A beacon at ankle height on a small
  dog reads several dB weaker than the same beacon held at chest height, and
  the animal's body between beacon and ESP32 costs several more. Tune with the
  beacon **on the animal**, not in your hand — see [TUNING.md](TUNING.md).
- **Orientation changes constantly.** That is normal and is exactly what the
  median filter and dwell timers absorb.

For a chicken, a leg band or a small pouch on a saddle works, but it is fiddly
and birds are good at removing things. Consider whether one beacon on the bird
that is reliably last in is enough.

### General mounting notes

- On a collar or leg band, the beacon's orientation changes constantly, and a
  body between the beacon and the ESP32 costs several dB. This is normal and is
  what the filter and the dwell timers absorb.
- **Waterproof it.** Coops are wet. Most beacon tags are splash-resistant at
  best.
- Have a spare. If you match on iBeacon UUID rather than MAC, you can program
  the spare with the same identity and swap it in without reflashing the ESP32.

See [BEACON-SETUP.md](BEACON-SETUP.md) for configuring one and finding its
address.

---

## The relay module

A standard **2-channel 5 V relay module** is what most builds use.

Things to check before buying:

- **Contact rating** must exceed your motor's stall current, not its running
  current. A motor draws several times its running current at startup and when
  stalled.
- **Opto-isolation** on the input is worth having, especially with a mains load.
- **Coil voltage** must match your supply — a 5 V module on a 3.3 V rail will
  click unreliably or not at all.
- Most cheap blue relay boards are **active low**. The firmware defaults to
  active high. See [WIRING.md](WIRING.md#the-active-high-vs-active-low-trap) —
  this is the single most common wiring mistake in this project.

### Alternatives to relays

- **Logic-level MOSFET modules** — silent, faster, no contacts to weld, but
  low-side switching only and DC only. Excellent for a 12 V linear actuator.
- **Solid-state relays (SSRs)** — silent and long-lived, but leak a little
  current when off, which some motor controllers dislike.

Both are active-high, so leave `RELAY_ACTIVE_LOW` at `0`.

---

## The door mechanism

**This is the safety-critical choice, and it is covered in
[SAFETY.md](SAFETY.md). Read that before buying anything.** The short version:

- **Best:** a commercial coop-door kit with its own limit switches and current
  limit, exposing momentary OPEN and CLOSE inputs. PetDoor just presses the
  buttons. This is the design the firmware assumes.
- **Good:** a 12 V linear actuator with built-in end stops.
- **Avoid:** a heavy guillotine door on a direct drive with no obstruction
  detection.

---

## Power

- A 5 V 1 A USB phone charger into the dev board's USB port is the usual
  arrangement.
- Feeding 5 V into `VIN` instead is more robust against a marginal USB
  connector.
- **Never** feed 5 V into the `3V3` pin — that bypasses the regulator and
  destroys the board.
- If your coop's power is unreliable, a small UPS or a battery-backed supply
  avoids exercising the boot-grace path every evening. The firmware handles
  power loss safely, but a door that reboots at dusk is still an annoyance.

---

## Environment

A coop is one of the harsher places you can put electronics: damp, dusty,
ammonia-laden, and occupied by animals that peck at things.

- Sealed enclosure, cable entry **pointing down**.
- Add a desiccant pack, and consider conformal coating in a wet climate.
- Mount it where a bird cannot reach it and a rodent cannot chew the cable.
- Check the operating temperature range if you are somewhere that freezes.
  The ESP32 itself is rated to −40 °C; batteries and LCDs are not, and coin
  cells lose a lot of capacity in the cold — which shows up as a beacon that
  stops being heard on winter nights.
