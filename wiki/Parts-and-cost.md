# Parts and cost

About **$80 all in**, and the door is most of it.

| | Cost | Notes |
|---|---|---|
| **Automatic pet or coop door** | $50–60 | The big one. Any door with a motor and buttons. |
| **ESP32 board with relays** | $15–20 | Integrated board — ESP32 and two relays on one PCB. |
| **BLE beacon** | $10–15 | For the collar. See [[The beacon]]. |
| **USB-to-TTL adapter** | $8–10 | One-off, for programming. Reusable forever. |

**Already own a motorised door?** Then it is the board and the beacon — around
**$30** to make a door you already have open for one specific animal.

### Two optional pounds well spent

Neither is needed to make the door work, and the firmware ships with both
switched off. Add them whenever you like — they are turned on with one command,
not a reflash.

| | Cost | What it buys |
|---|---|---|
| **Piezo buzzer** | ~$1 | The door stops being silent. A tick while it moves, a chime when it should have arrived, a low buzz when a locked door refuses the collar, and a distinct beep per command so you know one landed |
| **Two reed switches + magnets** | ~$2 | The door stops *guessing*. Its reported position becomes measured, the arrival chime becomes an actual arrival, and a button press the controller swallowed becomes visible instead of silent |

The reed switches are the better value of the two. Everything this project
cannot currently promise — that the door arrived, that it is where it says it
is, that the press took — traces back to not having them.

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
