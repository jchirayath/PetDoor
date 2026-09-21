# Flashing the door from a laptop

A field procedure: you are at the door with a laptop and a USB-to-TTL adapter,
and you want to get new firmware onto it.

It is written for the awkward case — a door already installed, reached with a
laptop balanced on something, possibly outdoors, possibly in the dark. So the
order matters: **everything that can be done at a desk is done at the desk.**
Installing a toolchain while crouched in a doorway is how flashes go wrong.

> **Do you actually need to go?** If the door is already running firmware with
> `REMOTE_CONFIG` and can reach your log server, you can change any setting and
> push new firmware without touching it — see
> [REMOTE-CONFIG.md](REMOTE-CONFIG.md). This page is for the first flash, or a
> door that has stopped calling in.

---

## What to bring

| | |
|---|---|
| **Laptop** | with the build already done — see below |
| **USB-to-TTL adapter** | CP2102 or similar, with its USB cable |
| **Jumper leads** | three: GND, TXD, RXD. Already fitted on most builds |
| **Something to stand the laptop on** | the flash takes about 15 seconds; the fiddling takes longer |
| **A torch** | `IO0` and `EN` are small and unlabelled on some boards |

You do **not** need the relay module, the beacon, or the door powered down. The
door keeps running its old firmware right up to the moment the new one lands.

---

## At the desk, before you go

### 1. Install the toolchain

```bash
# macOS
brew install arduino-cli
# or: https://arduino.github.io/arduino-cli/latest/installation/

arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

This project is developed against **arduino-cli 1.3.1** and **esp32 core
3.3.5**. Core 2.x will not compile it.

If you are using the NimBLE build (recommended — 89% of flash drops to 65%):

```bash
arduino-cli lib install NimBLE-Arduino@2.5.1
```

### 2. Create `petdoor/secrets.h`

Git-ignored, and the only place your credentials live. Without it the door
builds but has no network and no beacon:

```c
#pragma once

#define BEACON_MAC        "ac:23:3f:11:22:33"   // your beacon, lower case
#define RSSI_ENTER_DBM    -58                   // from docs/TUNING.md
#define RSSI_EXIT_DBM     -68
#define RELAY_ACTIVE_LOW  1                     // most blue relay boards

// Optional — omit all of these for a door with no network at all.
#define WIFI_SSID         "your-network"
#define WIFI_PASSWORD     "..."
#define LOG_ENDPOINT_URL  "http://logs.example.com/ingest"
#define LOG_SHARED_KEY    "..."                 // petdoor-logserver.py --show-key
#define LOG_DEVICE_ID     "back-door"
#define OTA_PASSWORD      "..."                 // guards over-the-air pushes
```

> **`OTA_PASSWORD` is worth setting even if nothing else is.** Without it,
> anyone on your network who finds the door during an OTA window can flash it.

### 3. Build it, and confirm it fits

```bash
cd /path/to/PetDoor

arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  --build-property "compiler.cpp.extra_flags=-DPETDOOR_USE_NIMBLE=1" \
  --build-property "compiler.c.extra_flags=-DPETDOOR_USE_NIMBLE=1" \
  --build-path /tmp/petdoor-build \
  petdoor
```

You want to see something like:

```
Sketch uses 1310119 bytes (66%) of program storage space.
```

Three things about that command:

- **`PartitionScheme=min_spiffs` is required, not an optimisation.** With WiFi
  enabled the firmware does not fit the default layout at all (136%).
- **`compiler.cpp.extra_flags`, not `build.extra_flags`.** The latter carries
  `-DCORE_DEBUG_LEVEL`, `-DESP32` and the loop/event core settings; overriding
  it drops them and inflates the image by ~33 KB.
- **`--build-path`** keeps the output somewhere you can point the upload at,
  rather than a temporary directory that may be cleaned up between commands.

Drop both `-DPETDOOR_USE_NIMBLE=1` flags for the stock Bluedroid build (90% of
flash, no extra library).

**If it does not compile at the desk, it will not compile at the door.** Do not
leave until this step is clean.

---

## At the door

### 4. Connect the adapter

Three wires. **TX and RX cross over** — the single most common mistake, and it
produces a port that opens cleanly and never says anything:

| Adapter | ESP32 | Usual colour |
|---|---|---|
| `GND` | `GND` | black |
| `TXD` | `RX0` / `GPIO3` | green |
| `RXD` | `TX0` / `GPIO1` | white |
| `VCC` | **leave disconnected** | red |

Leave `VCC` off if the board has its own supply. Two supplies fighting is the
mild failure; 5 V onto a `3V3` pin is the one that ends the board.

Colours vary between manufacturers — **trust the silkscreen on the adapter**.
Full detail and a loopback test in
[WIRING.md](WIRING.md#the-usb-to-ttl-link).

### 5. Check you can see it

```bash
arduino-cli board list
```

You want a port — `/dev/cu.usbserial-0001` on macOS, `/dev/ttyUSB0` on Linux,
`COM3` on Windows. Then confirm the door is actually talking:

```bash
screen /dev/cu.usbserial-0001 115200
```

Press `s`. A configured door prints its status; it does **not** chatter on its
own, so silence before you press anything is normal. Quit with `Ctrl-A` then
`K`, then `y`.

> **A live port proves nothing about the ESP32.** The adapter's USB chip
> enumerates whether or not the board on the other end has power. If `s` gets
> no reply, check the board's power LED before suspecting anything else.

**Close the serial monitor before uploading.** It holds the port, and a
detached `screen` session counts.

### 6. Put the chip into download mode

Most dev boards do this automatically. Boards without `DTR→IO0` and `RTS→EN`
wiring — including the reference build — need it by hand:

1. **Press and hold `IO0`** (sometimes `BOOT` or `FLASH`)
2. **Tap `EN`** (sometimes `RST`) while still holding `IO0`
3. **Keep holding `IO0`** for another second
4. **Release `IO0`**

The order matters and step 3 is the one people miss: the chip samples `IO0` as
it comes out of reset, so `IO0` must still be down *after* `EN` is released.

You will know it worked because the board goes quiet — no boot banner, and `s`
gets no reply. It stays in download mode until the next reset, so there is no
rush.

> The `!` console command reboots into download mode with no buttons at all —
> but **only on the ESP32-S3/C3/C6**. The original ESP32 has no such flag, and
> on that chip `!` says so rather than pretending.

### 7. Upload

```bash
arduino-cli upload -p /dev/cu.usbserial-0001 \
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  --input-dir /tmp/petdoor-build \
  petdoor
```

About 15 seconds. You want:

```
Writing at 0x0014ca40 [==============================] 100.0%
Wrote 1310119 bytes ... in 12.0 seconds
Hash of data verified.
Hard resetting via RTS pin...
```

> **"Hard resetting via RTS pin" is usually a lie.** If `RTS` is not wired —
> and on the reference build it is not — the chip is still sitting in download
> mode. **Tap `EN`** to actually boot the new firmware.

### 8. Confirm it came up

```bash
screen /dev/cu.usbserial-0001 115200
```

Press `s` and check four things:

```
  firmware     : v1.0.0 (built Sep 20 2026 18:30:00)   <- your build, not the old one
  ble stack    : NimBLE                                 <- if you built with it
  boot         : #31, last reset: power-on
  free heap    : 138420 bytes
```

Then, if the door has a network, wait for the first upload — a minute or two —
and look for:

```
[wifi] uploaded 128 events, clock synced
[ota] image confirmed good (upload succeeded); rollback cancelled
```

**That second line matters.** Until it appears the image is provisional, and a
reboot will roll the door back to the previous firmware. That is the safety net
working; see [REMOTE-CONFIG.md](REMOTE-CONFIG.md#a-bad-push-rolls-itself-back).

Your saved settings — beacon list, thresholds, dwell times, relay pulse, lock
state — live in NVS and **survive the flash**. You should not have to re-enter
anything.

---

## When it goes wrong

| Symptom | Cause |
|---|---|
| No port appears at all | Charge-only USB cable. Try another before anything else. |
| Port opens, total silence, `s` gets nothing | TX/RX swapped, a lead unseated, no common ground, or the board has no power. Check the power LED first. |
| `Failed to connect to ESP32: No serial data received` | Not in download mode. Redo step 6, keeping `IO0` held after `EN` is released. |
| Upload succeeds, door does not restart | `RTS` is not wired. Tap `EN`. |
| `Sketch too big` | You omitted `PartitionScheme=min_spiffs`. |
| Compile error about `getManufacturerData()` | esp32 core 2.x. Install 3.x. |
| `NimBLEDevice.h: No such file` | `arduino-cli lib install NimBLE-Arduino@2.5.1` |
| It flashed, but now it will not join WiFi | Leave it alone. Do not reflash in a panic — reboot it, and the bootloader restores the previous image by itself. |

The last row is worth reading twice. Since `OTA_REQUIRE_CONFIRM`, a bad image
that boots but cannot reach the server is **self-correcting**: reboot and the
old one comes back.

---

## After the first flash, you should not need this page

A door running this firmware with a working log endpoint can be reconfigured
and updated entirely over the network — thresholds, dwell times, the beacon
list, lock state, and new firmware:

```bash
petdoor-logserver.py --queue ota      # opens a window, tells you where to push
petdoor-logserver.py --doors          # what it sees, and its address
```

That is the whole point of the remote channel, and
[REMOTE-CONFIG.md](REMOTE-CONFIG.md) covers it. This page exists for the first
flash, and for the day something goes wrong enough that the door stops
answering.

Keep the adapter and the three leads somewhere you will find them again.
