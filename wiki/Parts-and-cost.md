# Parts and cost

About **$95 all in**, and the door is most of it.

<p>
  <img src="https://raw.githubusercontent.com/jchirayath/PetDoor/main/images/part-esp32-relay.svg" width="240" alt="ESP32 board with two relays">
  <img src="https://raw.githubusercontent.com/jchirayath/PetDoor/main/images/part-beacon.svg" width="240" alt="BLE beacon tag for the collar">
  <img src="https://raw.githubusercontent.com/jchirayath/PetDoor/main/images/part-usb-ttl.svg" width="240" alt="USB-to-serial adapter">
</p>

| | Cost | Notes |
|---|---|---|
| **Automatic pet or coop door** | $50–60 | The big one. Any door with a motor and buttons. |
| **ESP32 board with relays** | $15–20 | Integrated board — ESP32 and two relays on one PCB. |
| **BLE beacon** | $10–15 | For the collar. See [[The beacon]]. |
| **USB-to-TTL adapter** | $8–10 | One-off, for programming. Reusable forever. |
| **[Two reed switches + magnets](https://www.amazon.com/dp/B08K36VLZ2)** | ~$8 / pack | How the door knows it *arrived*. |
| **[SW-420 vibration sensor](https://www.amazon.com/dp/B0FC5PW8CK)** | ~$7 / pack | How the door knows it *started*. |

**Already own a motorised door?** Then it is the board, the sensors and the
beacon — around **$45** to make a door you already have open for one specific
animal.

### The sensors are required

<p>
  <img src="https://raw.githubusercontent.com/jchirayath/PetDoor/main/images/part-reed.svg" width="240" alt="Reed switch and disc magnets">
  <img src="https://raw.githubusercontent.com/jchirayath/PetDoor/main/images/part-sw420.svg" width="240" alt="SW-420 vibration sensor module">
</p>

Without the reed switches and the vibration sensor the door only *assumes* where
it is: it presses a button and believes the door moved. It cannot tell an arrival
from a stall, a press the controller ignored from one it obeyed, or its own travel
from a hand on the flap. The firmware still runs without them — so a missing or
failed sensor does not lock the door — but a build without them is not one this
project can vouch for.

| | What it tells the door |
|---|---|
| **Reed switches** | That it *arrived*. A stalled close is detected and reversed, a stale belief is corrected, and a door moved by hand is noticed |
| **SW-420 vibration sensor** | That it *started*, within about a second, so a press the controller swallowed is caught at once rather than at the end of the travel |

### One optional extra

<p>
  <img src="https://raw.githubusercontent.com/jchirayath/PetDoor/main/images/part-buzzer.svg" width="240" alt="Wired piezo buzzer">
</p>

| | Cost | What it buys |
|---|---|---|
| **[Passive piezo buzzer](https://www.amazon.com/dp/B07KNV8KVJ)** | ~$1 | The door stops being silent. A tick while it moves, a chime when it has arrived, a low buzz when a locked door refuses the collar, and a distinct beep per command so you know one landed. It must be **passive**: an active buzzer plays one note for everything |

---

## The door is the upgrade worth making

Spend more here than anywhere else. The door decides what happens when something
goes wrong, and no amount of firmware compensates for a bad mechanism.

Look for:

- **Anti-pinch or obstruction detection.** The single most valuable feature.
- **Separate OPEN and CLOSE buttons**, which is what this taps.
- **Limit switches**, so the motor stops itself.
- **A slow, weak drive.** Speed is not a virtue in a door an animal walks
  through.

Avoid guillotine-style doors with a heavy panel and no obstruction sensing,
unless you are confident about the close force.

---

## Why an integrated ESP32 + relay board

You can wire a bare ESP32 to a separate relay module, and it works. The
integrated boards are better here because:

- One board, one supply, no jumper wires to vibrate loose in a coop.
- The relay drive circuitry is already correct — no guessing about transistors
  or flyback diodes.
- They are usually screw-terminal, which survives outdoors better than dupont.

Search for "ESP32 2-channel relay board".

### One caveat on the relays

The common boards use **10 A power relay contacts**, and a door controller's
button input is a *dry circuit* — a few milliamps at low voltage. Power contacts
grow an oxide film that mains current punches through and button-level current
cannot, so they can click perfectly while conducting intermittently.

If your door works sometimes and not others, that is the first thing to suspect.
An optocoupler or a gold-contact signal relay is the proper part. See
[TROUBLESHOOTING.md](https://github.com/jchirayath/PetDoor/blob/main/docs/TROUBLESHOOTING.md#the-relay-clicks-but-the-door-does-not-move).

---

## What you also need

- **A 5 V supply** rated for the ESP32's radio peaks *plus* both relay coils.
  1 A is a comfortable minimum.
- **A USB data cable** — many cheap cables are charge-only and carry no data.
  This is the single most common first-time failure.
- **A multimeter**, for the bench test. Any cheap one.

Full list with the reasoning:
[HARDWARE.md](https://github.com/jchirayath/PetDoor/blob/main/docs/HARDWARE.md).
