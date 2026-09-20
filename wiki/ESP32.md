# The ESP32

You do not need to know anything about microcontrollers to build this.

An ESP32 is a **$5–10 computer the size of a stick of gum**:

| | |
|---|---|
| CPU | Dual-core 240 MHz |
| RAM | 520 KB |
| Flash | 4 MB typical |
| **Bluetooth LE** | the part this needs |
| WiFi | present, optional here |
| GPIO | ~25 usable pins, 3.3 V |
| Power | 5 V via USB, ~80–150 mA |

It runs **one program, forever**, starting the instant it gets power. No
operating system, no boot menu, nothing to log into. Power it and it runs; cut
power and it stops; restore power and it starts again from the beginning. That
property is what makes it suitable for a door.

## Which one to buy

A **classic ESP32 (WROOM-32)**. Cheapest, most common, and the reference build.

- **ESP32-S3 / C3 / C6** also work.
- **ESP32-S2 will not** — no Bluetooth radio at all, despite looking identical
  to an S3. This catches people out constantly.
- **ESP8266 will not** — WiFi but no Bluetooth.

For this project, an **integrated ESP32 + 2-relay board** is the convenient
form: one board, one supply, screw terminals.

## What it cannot do

- **Switch mains directly.** A GPIO pin gives 3.3 V at about 12 mA — enough for
  an LED, nowhere near a motor. That is what the relays are for.
- **Survive being wired wrong.** 5 V into the `3V3` pin destroys it.

## Programming it

Arduino IDE, arduino-cli or PlatformIO — all three work, and the project builds
with each. You need the board, a **USB data cable** (many cheap cables are
charge-only, which is the most common first-time failure), and nothing else.

Boards without a USB socket need a **USB-to-TTL adapter**, wired GND→GND and
TX↔RX **crossed**. TX-to-TX gives a silent port and no error message.

## Two things worth knowing up front

**Partition scheme.** Choose **Minimal SPIFFS**. With WiFi enabled the firmware
does not fit the default layout at all — this is required, not an optimisation.

**BLE stack.** The default build uses Bluedroid, which ships with the Arduino
core. An optional one-library switch to **NimBLE** takes flash usage from 89% to
65% and frees about 72 KB of heap, with no change in detection performance.

Full primer:
**[ESP32-PRIMER.md](https://github.com/jchirayath/PetDoor/blob/main/docs/ESP32-PRIMER.md)**
