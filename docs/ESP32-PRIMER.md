# The ESP32, for people who have never used one

You do not need to know anything about microcontrollers to build this. This
page covers what the board is, what it can do, what you need to program it, and
the handful of things that trip everyone up the first time.

If you have used an ESP32 before, skip to [WIRING.md](WIRING.md).

---

## What it is

An ESP32 is a **$5–10 computer the size of a stick of gum**. It has:

| | |
|---|---|
| CPU | Dual-core 240 MHz (roughly a 1998 desktop, running one job) |
| RAM | 520 KB |
| Flash | 4 MB typical — program storage |
| **Bluetooth LE** | The part this project needs |
| WiFi | Optional — off by default in the sense that it does nothing until you give it credentials; used only for the optional log upload and over-the-air flashing. Compile it out entirely with `-DPETDOOR_ENABLE_WIFI=0` to save ~640 KB. |
| GPIO pins | ~25 usable digital in/out, 3.3 V logic |
| Power | 5 V in via USB, ~80–150 mA |
| Price | About the same as a sandwich |

It runs **one program, forever**, starting the instant it gets power. No
operating system, no boot menu, no SD card. Power it and it runs; cut power and
it stops; restore power and it starts again from the beginning. That property
is what makes it suitable for a coop door — nothing to crash, nothing to log
into, nothing to update.

## What it can and cannot do

**Can:**

- Listen to Bluetooth advertisements continuously, forever
- Switch things on and off through its GPIO pins (via relays — see below)
- Talk to your computer over USB for setup and diagnostics
- Store settings that survive power loss (this project uses that for your
  beacon and thresholds)
- Run unattended for years

**Cannot:**

- Switch mains voltage directly. A GPIO pin provides **3.3 V at about 12 mA** —
  enough to light an LED, nowhere near enough for a motor. That is what the
  relay module is for: the ESP32 tells the relay to close, and the relay
  switches the real load.
- Survive being wired wrong. 5 V into the `3V3` pin destroys it.
- Do anything on its own without a program flashed onto it.

### Which variant to buy

Get a **classic ESP32 (WROOM-32) dev board**. They are the cheapest, the most
common, and the reference build for this project.

- **ESP32-S3 / C3 / C6** also work. They use a different Bluetooth stack
  internally (NimBLE rather than Bluedroid); the firmware supports both.
- **ESP32-S2 will not work.** It has no Bluetooth radio at all, despite looking
  identical to an S3. This catches people out constantly.
- **ESP8266 will not work.** It has WiFi but no Bluetooth.

A board with a **USB-C or micro-USB socket and two buttons** (marked `EN`/`RST`
and `BOOT`/`IO0`) is what you want. Boards sold as "ESP32 DevKit V1",
"NodeMCU-32S" or "ESP32-WROOM-32 development board" are all fine.

---

## What you need to program it

### Hardware

1. **The ESP32 board**
2. **A USB data cable.** This is the single most common failure: many cheap USB
   cables are *charge-only* and carry no data. If your computer does not see
   the board at all, try a different cable before anything else.
3. That is it. No programmer, no debugger, no extra hardware.

### Software — pick one

**Option A: Arduino IDE** (easiest if you have never done this)

1. Download the Arduino IDE from [arduino.cc](https://www.arduino.cc/en/software)
2. **File → Preferences → Additional Board Manager URLs**, paste:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
3. **Tools → Board → Boards Manager**, search `esp32`, install **version 3.x**
   (this project requires 3.x; 2.x will not compile)
4. **Tools → Board → ESP32 Arduino → ESP32 Dev Module**
5. **Tools → Partition Scheme → Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)** —
   **this step is required, not an optimisation.** With WiFi enabled the build
   does not fit the default partition scheme at all; see the note below
6. **Tools → Port** — pick the one that appears when you plug the board in
7. Open `petdoor/petdoor.ino`, click **Upload**

**Option B: arduino-cli** (scriptable, what this project's CI uses)

```bash
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32

arduino-cli board list                      # find your port

FQBN=esp32:esp32:esp32:PartitionScheme=min_spiffs
arduino-cli compile --fqbn $FQBN petdoor
arduino-cli upload  --fqbn $FQBN -p /dev/cu.usbserial-0001 petdoor
```

`PartitionScheme=min_spiffs` matters. The default layout splits the flash into
two equal app slots, leaving only 1.25 MB for the program — and the firmware
does not fit in it:

| Partition scheme | App space | With WiFi | Without WiFi |
|---|---|---|---|
| `default` | 1.25 MB | **136% — will not build** | 86% |
| `no_ota` | 2.0 MB | 85% | 53% |
| `min_spiffs` | 1.875 MB | 89% | 57% |

`min_spiffs` is the recommendation because it keeps a second app slot, which is
what makes over-the-air flashing possible — worth having once the board is
screwed to a coop wall. Choose `no_ota` instead if you would rather have the
headroom and are happy to flash over USB forever.

If you build with `-DPETDOOR_ENABLE_WIFI=0`, every scheme fits comfortably,
including the default.

In the Arduino IDE this is
**Tools → Partition Scheme → Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)**;
PlatformIO reads it from `platformio.ini` automatically.

**Option C: PlatformIO**

```bash
pio run --target upload
```

`platformio.ini` is already configured. Note it points at the `pioarduino`
platform fork, because the official `espressif32` platform is pinned to core
2.x which will not build this project.

### Driver, if your computer does not see the board

The USB socket on the board is wired to a **USB-to-serial chip**. Most modern
systems have the driver already; if not, identify the chip (it is printed on
it, next to the USB socket) and install:

| Chip | Driver |
|---|---|
| CP2102 / CP2104 | Silicon Labs CP210x VCP driver |
| CH340 / CH9102 | WCH CH34x driver |
| FTDI FT232 | FTDI VCP driver |

On Linux, no driver is needed but you must be in the `dialout` group:

```bash
sudo usermod -aG dialout $USER     # then log out and back in
```

---

## The five things that trip everyone up

**1. A charge-only USB cable.** No port appears. Try another cable first.

**2. "Failed to connect to ESP32".** The board needs to be in download mode.
Most boards do this automatically. If yours does not (common with bare modules
or hand-wired USB-serial adapters), hold **IO0**, tap **EN**, release **IO0**,
then upload. Full detail in
[DIAGNOSTICS.md](DIAGNOSTICS.md#flashing-a-board-with-no-auto-reset).

**3. The serial monitor holds the port.** Close it before uploading, or the
upload fails. A detached `screen` session counts.

**4. Core 2.x instead of 3.x.** You get a compile error about
`getManufacturerData()` returning `std::string`. Update the board core.

**5. Choosing a pin that cannot be used.** Not all GPIO are equal —
[WIRING.md](WIRING.md#choosing-different-pins) lists which to avoid and why.

---

## How the pins actually switch a door

The chain is:

```
  ESP32 GPIO  ->  relay module input  ->  relay contacts  ->  door controller
    3.3V, 12mA        opto-isolated         rated for          your existing
                                            the real load       door motor
```

The ESP32 never touches the motor. It pulses a **relay**, which is an
electrically-operated switch: a small signal on one side closes a physically
separate, much beefier contact on the other. That isolation is what lets a
3.3 V chip control a mains-voltage or high-current motor safely.

This firmware pulses the relay **momentarily** (200 ms by default), the same as
a person pressing a button. The door controller then runs the motor to its own
limit switches and stops. The ESP32 decides *when*; the door hardware decides
*how far*. Keeping those separate is the core safety idea — see
[SAFETY.md](SAFETY.md).

---

## Once it is programmed

Everything else happens over the **serial console** at 115200 baud — finding
your beacon, setting thresholds, testing relays, reading diagnostics. You never
need to reflash to change settings; they are stored on the device.

Start with [DIAGNOSTICS.md](DIAGNOSTICS.md) for how to connect, then
[BEACON-SETUP.md](BEACON-SETUP.md) to find your beacon.
