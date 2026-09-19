# Wiring

Read [SAFETY.md](SAFETY.md) first. This page assumes you have, and that you
have chosen a door mechanism that fails safe.

---

## Default pinout

Set in [`petdoor/config.h`](../petdoor/config.h). Override in `secrets.h`
rather than editing `config.h`, so `git pull` does not clobber your build.

| Signal | GPIO | Config | Notes |
|---|---|---|---|
| OPEN relay | 16 | `PIN_RELAY_OPEN` | Momentary pulse, `RELAY_PULSE_MS` |
| CLOSE relay | 17 | `PIN_RELAY_CLOSE` | Momentary pulse |
| Status LED | 23 | `PIN_STATUS_LED` | Active high, series resistor required |

### Choosing different pins

If 16/17 are taken on your board, pick replacements that are plain GPIO with no
boot-time role. On a classic ESP32 (WROOM), safe choices include **GPIO 4, 5,
13, 14, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33**.

Avoid:

- **GPIO 0, 2, 12, 15** — strapping pins. A relay module's pull-up or pull-down
  on one of these can stop the board booting.
- **GPIO 6–11** — wired to the onboard SPI flash. Using them bricks the boot.
- **GPIO 34–39** — input only. They cannot drive a relay.
- **GPIO 1 and 3** — the serial console you need for setup and tuning.

`PIN_RELAY_OPEN` and `PIN_RELAY_CLOSE` must differ; a `static_assert` in
`door.cpp` catches it if they do not.

---

## The active-high vs active-low trap

**This is the mistake that costs people a door.**

Most cheap blue relay boards — the ones with the songle relays and the
three-pin `VCC / GND / IN` header — are **active low**. The coil energises when
the input pin is pulled to **GND**, and releases when it is high.

The firmware defaults to `RELAY_ACTIVE_LOW 0`, which is correct for:

- Active-high relay boards
- Logic-level MOSFET drivers
- Opto-isolated boards with an explicit active-high jumper

Set it to `1` in `secrets.h` for the common active-low boards:

```c
#define RELAY_ACTIVE_LOW 1
```

### How to tell which you have, before connecting a motor

With the relay module powered and its input connected to the ESP32, but
**nothing connected to the relay's output terminals**:

1. Boot the ESP32 and let it finish. Do not send any commands.
2. Listen. At idle, both relays should be **released** — silent, with their
   indicator LEDs off.
3. If a relay clicks in at boot and *stays* energised, `RELAY_ACTIVE_LOW` is
   set wrong. Change it, reflash, and check again.

Getting this backwards means the door runs continuously, or runs the moment you
apply power, or both — which is exactly the failure that hurts an animal.

The firmware writes the idle level to the pin latch *before* `pinMode()` makes
it an output, so the pin never presents the asserted level during boot. That
protects you from a glitch. It cannot protect you from an inverted
`RELAY_ACTIVE_LOW`.

---

## Wiring diagram

```
                  ESP32 dev board
                 ┌───────────────┐
    5 V supply ──┤ VIN       GND ├── common ground ──┐
                 │               │                   │
                 │       GPIO 16 ├──────────┐        │
                 │       GPIO 17 ├───────┐  │        │
                 │       GPIO 23 ├──[R]──┼──┼─ LED ──┤
                 │               │       │  │        │
                 └───────────────┘       │  │        │
                                         │  │        │
                   2-ch relay module     │  │        │
                 ┌───────────────┐       │  │        │
    5 V supply ──┤ VCC           │       │  │        │
                 │           IN1 ├───────┼──┘  (OPEN)
                 │           IN2 ├───────┘     (CLOSE)
                 │           GND ├───────────────────┘
                 │               │
                 │  COM1 NO1 NC1 ├── COM1 + NO1 → door controller OPEN input
                 │  COM2 NO2 NC2 ├── COM2 + NO2 → door controller CLOSE input
                 │               │   NC1 and NC2 stay empty
                 └───────────────┘
```

Key points:

- **Common ground is mandatory.** The ESP32 ground and the relay module ground
  must be tied together, or the relay inputs float and switch at random.
- The status LED needs a **series resistor** (220–1 kΩ). ESP32 GPIO pins are
  3.3 V and have no current limiting. Many dev boards already have an onboard
  LED you can point `PIN_STATUS_LED` at instead.
- Power the relay coils from the **5 V rail, not from a GPIO pin**. A relay coil
  draws 60–90 mA; a GPIO pin can source about 12 mA.
- Size the 5 V supply for the ESP32's Wi-Fi/BLE current peaks *plus* both relay
  coils. 1 A is a comfortable minimum.

---

## Which output terminals to use

Each relay channel brings out three screw terminals. **Use `COM` and `NO`.
Leave `NC` empty.**

| Terminal | | Use it? |
|---|---|---|
| `COM` | common | **yes** |
| `NO` | normally open — open at rest, closed while the relay is energised | **yes** |
| `NC` | normally closed — closed at rest, open while energised | **no** |

The two wires go across your door controller's button, in parallel with it (or
in place of it). The existing button keeps working if you leave it fitted.

### Why `NO`, and why `NC` is the dangerous choice

The firmware is imitating a finger on a button:

```cpp
digitalWrite(pin, RELAY_ASSERT);
delay(RELAY_PULSE_MS);            // 200 ms
digitalWrite(pin, RELAY_RELEASE);
```

A pushbutton is open at rest and closed while pressed. `COM`↔`NO` is exactly
that: open at idle, closed for 200 ms, open again.

Wire `COM`↔`NC` instead and every one of those states inverts. The door
controller sees its button **held down permanently**, with a 200 ms *release*
each time the firmware tries to act. Depending on the controller that is a motor
that runs continuously, or one that starts the instant you apply power, or both
— the failure [SAFETY.md](SAFETY.md) exists to prevent. Nothing in the firmware
can detect this or protect you from it; it is downstream of the ESP32 entirely.

### These are dry contacts

The relay contacts are an isolated switch. They do **not** supply power — they
only close your door controller's own button circuit. Do not run the module's
5 V rail into `COM`; feeding voltage into a controller input that expects a
simple contact closure can destroy it.

That isolation is the whole point of the relay: it is what lets a 3.3 V logic
pin control a mains-adjacent motor circuit without the two ever meeting.

### If the relay clicks at boot, this is not the problem

A relay that energises at power-up and stays energised is `RELAY_ACTIVE_LOW` set
wrong, not a terminal mistake. Diagnose it with **nothing connected to the
output terminals** — see
[the active-high vs active-low trap](#the-active-high-vs-active-low-trap) above.

---

## Momentary pulse vs held contact

The firmware pulses each relay for `RELAY_PULSE_MS` (200 ms default) and then
releases it. This matches a door controller that has **separate OPEN and CLOSE
momentary inputs** — press to start, the controller runs the motor to its own
limit switches and stops. This is the arrangement PetDoor is designed for, and
the one that puts the stopping logic in hardware where it belongs.

**If your motor instead needs the relay held closed for the whole travel**, you
have two options:

1. **Preferred:** put a proper motor controller between PetDoor and the motor —
   one with limit switches and a current limit. PetDoor then only ever sends the
   momentary "go" signal, and the controller decides when to stop.
2. **If you must drive the motor directly:** raise `RELAY_PULSE_MS` to slightly
   longer than the door's full travel time.

   ```c
   #define RELAY_PULSE_MS 4000   // door takes ~3.5 s to travel
   ```

   Be aware of what this costs you. `DoorController::pulse()` is **blocking** —
   it holds the control task inside `delay()` for the whole pulse. During those
   seconds no BLE samples are drained, presence is not re-evaluated, and the
   scan watchdog does not run. Samples keep queueing (the BLE callback runs in
   its own task) and are processed on the next tick, so nothing is lost beyond
   queue depth, but the door cannot be commanded to reverse mid-travel. With a
   direct-drive motor and no limit switches, the motor also keeps pulling for
   the full `RELAY_PULSE_MS` whether or not the door reached its stop. Read
   [SAFETY.md](SAFETY.md) again before choosing this.

---

## Bench test procedure

Do this with **no motor connected**. You are testing the relays only.

1. Flash the firmware and open the serial monitor at **115200 baud**.

2. Confirm the banner shows the pins and polarity you expect:

   ```
     open / close  : GPIO 16 / GPIO 17 (active HIGH)
     status LED    : GPIO 23
   ```

3. Confirm both relays are **released at idle**. See the polarity section
   above if not.

4. Type `o`. You should see:

   ```
   [cmd] forcing OPEN
   ```

   and hear exactly **one** click from the OPEN relay, lasting about 200 ms
   before it releases.

5. Type `x`. One click from the CLOSE relay.

6. Type `o` then `x` in quick succession. Note the short pause — about a
   quarter of a second — before the second relay fires. That is
   `DIRECTION_CHANGE_GAP_MS`, the interlock ensuring the two relays are never
   energised close enough together to fight each other across the motor's
   direction contacts.

   If your relays are audibly slow, or you can hear one chattering as the other
   fires, raise it: `w` then `gap 500` in the console, or set
   `DIRECTION_CHANGE_GAP_MS` in `secrets.h`.

7. Confirm with a meter that **COM–NO continuity appears on only one relay at a
   time**, never both.

If all seven pass, connect the motor and repeat steps 4–6 with the door
disengaged, then with the door engaged and the coop empty.

> `o` and `x` bypass the proximity logic, the actuation lockout and the boot
> grace window. Set `ALLOW_MANUAL_SERIAL_CONTROL` to `0` once the build is
> commissioned.

---

## Powering it in the coop

- A USB phone charger and a short USB cable into the dev board's USB port is
  fine and is what most builds use.
- Feeding 5 V into `VIN` also works and survives a marginal USB connector
  better.
- **Do not** feed 5 V into the `3V3` pin. That bypasses the regulator and
  destroys the board.
- Coops are damp. Put the electronics in a sealed enclosure with the cable entry
  pointing down, and keep it away from the dust a flock generates.

Next: [TUNING.md](TUNING.md) to choose your thresholds, or
[TROUBLESHOOTING.md](TROUBLESHOOTING.md) if something above did not behave.
