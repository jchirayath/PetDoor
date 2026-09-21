# Troubleshooting

Start with `s` in the serial monitor. It answers most of these questions in one
screen.

> New to the serial console? [DIAGNOSTICS.md](DIAGNOSTICS.md) covers how to
> connect, every command, and how to read each field.

```
---- status ----
  target       : MAC ac:23:3f:11:22:33
  uptime       : 3721 s
  presence     : PRESENT
  door         : OPEN
  rssi         : -58 dBm filtered (raw -61), ~1.8 m
  last seen    : 112 ms ago, 34821 samples
  closing in   : 11200 ms
  radio        : 291043 adverts, last 44 ms ago, 0 samples dropped
  free heap    : 189204 bytes
```

| Field | What it tells you |
|---|---|
| `target` | Which matchers are active. `<none configured>` means the door will never move. |
| `presence` | The state machine's output. `(no fix)` means no recent samples. |
| `door` | What was last **commanded**, not where the door is. `UNKNOWN` means nothing has been commanded since boot. |
| `samples` | Should climb ~10/s. **Stuck at 1 is the duplicate-filter bug.** |
| `adverts` | Total advertisements from *all* devices. Should climb constantly. |
| `samples dropped` | Control task falling behind. Small numbers are harmless. |
| `opening in` / `closing in` | Only shown while a transition is pending. |

---

## Nothing on the serial monitor

- Baud rate must be **115200**.
- Wrong port. `arduino-cli board list` shows what is connected.
- Some dev boards need the **USB CDC on boot** option enabled (S3/C3), or need
  a different USB port on the board (native USB vs UART bridge).
- A USB cable that is charge-only. Very common. Try another cable.
- Press the board's reset button after opening the monitor — you may simply have
  missed the banner.

## It says "NO BEACON CONFIGURED"

Working as intended. The firmware refuses to touch the door until you tell it
what to look for. Follow [BEACON-SETUP.md](BEACON-SETUP.md).

If you *did* set `BEACON_MAC` and still see this:

- `secrets.h` must be in **`petdoor/`**, next to `config.h`.
- Check for a typo in the macro name — `BEACON_MAC`, not `BEACON_MAC_ADDRESS`.
- An all-zero MAC (`"00:00:00:00:00:00"`) is deliberately treated as "not
  configured", so the placeholder does not count.
- Make sure the `#define` is not still commented out. `secrets.example.h` ships
  with every line commented.
- Confirm the banner. It prints exactly what was parsed:
  `target beacon : MAC ac:23:3f:11:22:33`.

---

## The beacon never appears in the discovery table

- Is the beacon actually advertising? Check with any phone BLE scanner app
  (nRF Connect, LightBlue). If a phone cannot see it either, it is the beacon.
- Pull tab still in, or flat battery.
- The table holds only `DEVICE_TABLE_SIZE` (40) devices with least-recently-seen
  eviction. In a dense RF environment yours can be evicted. Move the beacon
  right next to the ESP32.
- Is discovery mode even on? It defaults on only when no beacon is configured.
  Toggle with `d`.
- If **nothing at all** appears and `adverts` is not climbing, the radio is the
  problem — see below.

## `adverts` is not climbing / the LED is fast-blinking

Fast blink means the scan watchdog considers the radio unhealthy: not one
advertisement from any device for `SCAN_WATCHDOG_MS`. There is essentially
always some BLE traffic around, so this means the stack is wedged, not that the
neighbourhood is quiet.

The watchdog restarts the scan automatically and logs it:

```
[ble] no advertisements for 15012ms, restarting scan
```

If it recovers, fine. If it happens repeatedly:

- **Brownout.** The BLE radio has substantial current peaks. An underpowered
  supply or a thin USB cable causes resets and stack failures. Try a better
  supply — this is the most common cause by far.
- Board without a Bluetooth radio. **ESP32-S2 has none.**
- Wrong board selected at compile time.
- Relay coil transients coupling back into the supply. Power the relay module
  from a separate 5 V feed with a common ground.

## `[fatal] BLE scanner failed to start; rebooting in 5 s`

The BLE stack would not initialise at all. Almost always:

- Insufficient memory — check `free heap` if you can get a status print in.
- Wrong board/partition selection.
- A board with no BLE radio.

---

## `samples` is stuck at 1 — the classic bug

**This is the bug this project exists to fix.** If you are seeing it in a
modified copy of this firmware, you have almost certainly changed one line.

In `ble_scanner.cpp`:

```cpp
g_scan->setAdvertisedDeviceCallbacks(&g_callbacks, true);
                                                   ^^^^
```

That second argument is `wantDuplicates` and it **must stay `true`**. Left at
the library default of `false`, an infinite scan calls back exactly once per
device, forever — one RSSI sample, then silence. Proximity cannot work.

Full explanation in
[ARCHITECTURE.md](ARCHITECTURE.md#the-duplicate-filter-trap).

## The MAC keeps changing

You are tracking a phone, an AirTag, a Tile or a smartwatch. These rotate their
Bluetooth address every few minutes specifically to prevent this. You cannot
turn it off and there is no workaround.

You need a real beacon that advertises a fixed address — see
[HARDWARE.md](HARDWARE.md#the-beacon).

The symptom is distinctive: it works for a few minutes after each reflash, then
silently stops.

---

## The door never opens

Work down this list:

1. **`target` shows `<none configured>`?** See above.
2. **Beacon never heard since boot?** The status line says
   `rssi : beacon has never been heard`. The firmware will not operate the door
   at all until the target has been heard once — a deliberate rule so a flat
   beacon battery cannot read as "absent". Fix the beacon, or check that the MAC
   matches exactly.
3. **`presence : ABSENT` with a good RSSI?** Your `RSSI_ENTER_DBM` is stronger
   than the signal you actually get. Type `c`, read the *filtered* value where
   you stand, and set the threshold a few dB below it. See
   [TUNING.md](TUNING.md).
4. **`presence : PRESENT` but `door : UNKNOWN`?** The open command is being
   refused. Watch for a `[door]` line; if none appears, see the lockout section.
5. **`door : OPEN` but the door did not move?** The firmware pulsed the relay
   and believes it succeeded — it is open-loop and cannot tell. The problem is
   downstream: wiring, relay polarity, motor, or mechanism. Go to
   [WIRING.md](WIRING.md#bench-test-procedure).

## The door never closes

1. **`presence` stays `PRESENT`?** Your `RSSI_EXIT_DBM` is weaker than the
   signal you get from your "gone" position. Take a filtered reading from there
   and set the exit threshold above it. Narrowing the hysteresis gap toward 8 dB
   is the usual fix.
2. **Within 30 s of boot?** `BOOT_GRACE_MS` deliberately blocks closing. Wait.
3. **Beacon left inside the coop?** The door correctly stays open. This is the
   safe failure mode, but it is still a failure — check where the beacon is.
4. **`closing in` counting down but never reaching zero?** Something keeps
   pushing the signal back above `RSSI_EXIT_DBM`, cancelling the pending close.
   Widen the gap between the thresholds.

## The door opens and closes repeatedly

- **Hysteresis band too narrow.** Widen the gap between `RSSI_ENTER_DBM` and
  `RSSI_EXIT_DBM` to 10–12 dB.
- **The beacon rests right on a threshold.** Move the threshold so its resting
  position is not on the boundary.
- **Filter too twitchy.** Raise `RSSI_MEDIAN_WINDOW` to 11, lower
  `RSSI_EWMA_ALPHA` to 0.2. This affects the close decision only and costs no
  open latency, so it is a cheap thing to try.
- **`ENTER_CONFIRM_MS` too short** for someone who merely walks past. Raise it
  to 3000–5000.

Note that `MIN_ACTUATION_INTERVAL_MS` puts a 5-second floor on how fast this can
possibly happen, so true rapid chatter is not possible — but a cycle every ten
seconds certainly is.

## The door runs backwards, or runs constantly

**Stop and disconnect the motor.**

Almost always `RELAY_ACTIVE_LOW`. Most cheap blue relay boards are active low
and the firmware defaults to active high:

```c
#define RELAY_ACTIVE_LOW 1
```

The tell: with the relay module powered and connected but idle, a relay is
clicked in and staying energised.

If the polarity is right but the directions are swapped, exchange the two
signal wires, or swap `PIN_RELAY_OPEN` and `PIN_RELAY_CLOSE`.

Confirm with the bench test in
[WIRING.md](WIRING.md#bench-test-procedure) before reconnecting the motor.

## The relay clicks but the door does not move

The firmware is doing its job — you can hear the coil — and nothing happens
downstream. Work through this in order; each step rules something out.

**1. Does a manual short move the door?** With the relay module powered, bridge
`COM` and `NO` on that channel with a wire or a screwdriver blade. If the door
moves, the wiring from the relay to your controller is good and the controller
is fine. If it does *not* move, the fault is downstream — check the crimps and
that you are on the controller's button terminals.

Note what this does **not** prove. A hand-made short lasts a second or more; the
firmware's pulse is 200 ms. So a successful manual bridge is equally consistent
with "the contacts never close" and "the pulse is too short". The next two steps
separate them.

**1a. If the hand-short works but the relay never does — suspect the contacts.**
This is the single most likely cause, and it is a property of the relay module
rather than anything in this firmware.

Your door controller's button input is a **dry circuit**: a few milliamps at low
voltage, sometimes microamps. The common blue relay boards use silver-alloy
contacts rated 10 A at 250 V. Those contacts grow a thin oxide film in normal
use. At mains current the film is punched through instantly and you never know
it is there. At button-input current there is not enough energy to break
through, so the contact reads open — or flickers — while the relay is
mechanically closed and clicking healthily.

A finger works because it is a different kind of contact: far larger area, much
higher pressure, and it wipes across the metal as it lands.

The symptoms are distinctive:

- A clean single *click* from the relay, no buzzing (so the coil is fine)
- Continuity across `COM`/`NO` that **flickers** during a long pulse
- The door moving on some actuations and not others, with nothing changed
- Getting *worse* over a session, as switching cycles build up more film
- Pulse length appearing to matter, then not — each test is really a coin flip

Confirm it by metering `COM`–`NO` during a `pulse 3000`. Mechanically the relay
is closed for three seconds; if the meter disagrees, the contacts are the fault.

**The fix is a different switching part, not a firmware setting:**

1. **An optocoupler** (PC817 or similar) across the button terminals. No
   contacts at all, so there is nothing to oxidise. This is the correct part for
   switching a logic-level input and costs well under a dollar.
2. **A signal relay with gold-plated or bifurcated contacts**, explicitly rated
   for "dry circuit" or "low level" switching. The 10 A power relays sold on the
   usual modules are the wrong tool for this job.
3. **A small MOSFET**, if the button circuit is DC and you can establish its
   polarity.

As a free stopgap, move the load to the module's **other channel** — fewer
operations means less film, and if that channel works reliably it confirms the
diagnosis outright.

**1b. Does the door's OWN button sometimes need pressing twice?**

If pressing the physical button by hand occasionally does nothing, and a second
press works, then the controller is swallowing presses and the relay is not at
fault at all. Try:

```
w
presses 2 1000
```

Two presses a second apart, per actuation. Saved on the device, and settable
remotely with `--queue presses 2 1000`.

**This is a blind retry, and it can make things worse.** The door has no
position feedback, so it cannot tell whether the first press worked. If it did,
the second arrives while the door is moving — and many controllers read a press
mid-travel as *stop*. You would be trading "sometimes does not move" for
"sometimes stops halfway", which is worse: a door parked halfway is neither open
nor shut, and the firmware believes it is whichever it last commanded.

So watch a dozen cycles before trusting it, and be specific about which failure
you are trading for which. A position sensor is the real fix — it is the only
thing that lets the door know whether it needs to try again.

> This also explains a misleading symptom. A hand-short across COM/NO **bounces**
> — finger contact is never clean — so it delivers several presses where the
> relay delivers exactly one. "The hand-short works every time and the relay
> does not" therefore does *not* prove the contacts are bad. It is equally
> consistent with a controller that needs more than one press.

**2. Lengthen the pulse.** Many door controllers debounce their button input and
will ignore a tap shorter than 300–500 ms as electrical noise. This costs
nothing to try and needs no reflash:

```
w
pulse 1000
```

Then `o` and `x`. If the door now moves, that was it — walk the value back down
until it stops working and leave yourself margin. The setting is saved on the
device.

**3. If a long pulse still does nothing, meter the contacts.** Set `pulse 3000`
so the closure is easy to catch, put a meter on continuity across `COM` and
`NO`, and type `o`. You should see it close for three seconds.

- **No continuity at any point** — the coil is clicking but the contacts are not
  making. Either the module is faulty, or the coil is not fully pulling in
  because the 5 V rail sags when it energises. Try a separate supply for the
  relay module; a coil draws 60–90 mA and a shared supply that also feeds the
  ESP32's radio peaks can brown out.
- **Continuity is present but the door still does not move** — the contact is
  good and the controller is not responding to it. Check you are across the
  button and not, say, across a limit switch or an interlock input.

**4. Check you are on `NO`, not `NC`.** Meter `COM`–`NO` with the relay idle: it
must read **open**. If it reads closed you are on `NC`, the controller has seen
its button held down since power-on, and the pulse is inverted. See
[WIRING.md](WIRING.md#which-output-terminals-to-use).

> If the fix was a longer pulse and your motor has no limit switches, read
> [the held-contact warning](WIRING.md#momentary-pulse-vs-held-contact) — the
> control task is blocked for the whole pulse, and a direct-drive motor keeps
> pulling whether or not the door reached its stop.

## Both relays click when only one should

Should be impossible — `pulse()` always releases the opposite relay and waits
`DIRECTION_CHANGE_GAP_MS` before asserting. If you see it:

- The two relay inputs are wired to the same GPIO.
- The relay module is active low and floating inputs are being read as asserted.
  Check the common ground.
- Supply sag: one coil energising is browning out the rail enough to upset the
  other input. Power the relays separately.

---

## Nothing happens when I type a command

- Some serial monitors require you to press Enter. Any trailing newline is
  ignored by the firmware, so that is fine.
- Check line ending settings; "No line ending" works.
- `o` and `x` are compiled out when `ALLOW_MANUAL_SERIAL_CONTROL` is `0`. Type
  `h` — if they are not listed, that is why.

## Compile errors

**`getManufacturerData()` returns `std::string`** — you are on ESP32 Arduino
core **2.x**. This project needs **3.x**. Update the board core in Arduino IDE's
board manager, or for PlatformIO use the `pioarduino` platform fork already
configured in `platformio.ini` (the registry's `espressif32` is pinned to 2.x).

**`Sketch too big`** — the build uses about 83% of the default partition scheme.
If you have added code, select a partition scheme with a larger app partition
(Tools → Partition Scheme in the Arduino IDE).

**`static_assert` failures** — these are guardrails, and each says what is
wrong:

| Assertion | Fix |
|---|---|
| `RSSI_ENTER_DBM must be greater...` | Enter must be *less negative* than exit. `-65` > `-75`. |
| `MIN_ACTUATION_INTERVAL_MS must be shorter...` | Lower the lockout or raise `EXIT_CONFIRM_MS`. |
| `RSSI_MEDIAN_WINDOW must be an odd number between 3 and 15` | Use 3, 5, 7, 9, 11, 13 or 15. |
| `RSSI_FAST_WINDOW must be an odd number between 1 and 15` | Use 1, 3, 5 … 15. Unlike the close window, 1 is allowed. |
| `RSSI_FAST_ALPHA must be in (0, 1]` | Greater than 0, at most 1.0. |
| `The fast filter must not be slower than the slow one` | `RSSI_FAST_WINDOW` ≤ `RSSI_MEDIAN_WINDOW` **and** `RSSI_FAST_ALPHA` ≥ `RSSI_EWMA_ALPHA`. |
| `SCAN_WINDOW_MS must be <= SCAN_INTERVAL_MS` | Window cannot exceed interval. |
| `PIN_RELAY_OPEN and PIN_RELAY_CLOSE must be different pins` | Self-explanatory. |
| `NimBLEDevice.h: No such file or directory` | You set `PETDOOR_USE_NIMBLE=1` without the library. Library Manager → **NimBLE-Arduino** (2.5.1+). |
| `Sketch too big` | You are on the default partition scheme. Switch to **Minimal SPIFFS** — see [ESP32-PRIMER.md](ESP32-PRIMER.md#what-you-need-to-program-it). |

**Sketch folder not recognised by the Arduino IDE** — the folder must be named
`petdoor` and contain `petdoor.ino`. Keep those in sync.

**Not sure which BLE stack is running** — the boot banner and the `s` command
both say. If you flipped `PETDOOR_USE_NIMBLE` and the banner still says
`Bluedroid`, the define did not reach the compiler: put it in `secrets.h` rather
than passing it on a command line, which is the reliable route in the Arduino
IDE.

**Detection got worse after switching to NimBLE** — the two stacks drive the
same controller, so the radio behaviour should be identical, but check `s`
first: `worst gap` and `samples` are the numbers that matter. If the gap grew,
the duplicate filter is the thing to suspect; `applyScanSettings()` passes
`true` for `wantDuplicates` on both stacks and that argument is what makes
repeat advertisements arrive at all. See
[ARCHITECTURE.md](ARCHITECTURE.md#the-duplicate-filter-trap).

## Upload fails

- Hold BOOT while upload starts, release when it begins writing. Some boards
  need this every time.
- Close the serial monitor first — it holds the port. A `screen` session you
  merely *detached* (`Ctrl-A d`, or by closing the window) is still holding it:
  `screen -ls` then `screen -X -S <id> quit`. See
  [DIAGNOSTICS.md](DIAGNOSTICS.md#getting-back-out).
- Wrong port, or a charge-only USB cable.
- On Linux, add yourself to the `dialout` group.

---

## The ESP32 reboots on its own

- **Brownout** is the usual cause. `Brownout detector was triggered` in the log
  confirms it. Better supply, better cable, separate feed for the relay coils.
- Relay coil back-EMF. A decent relay module has flyback diodes; a bare relay
  needs one.
- Check `free heap` in the status output. It should be stable at a few hundred
  KB. If it falls steadily over hours, that is a leak worth reporting as an
  issue.

Note that a reboot is handled safely — `BOOT_GRACE_MS` blocks closing for 30 s,
and the door is not operated until the beacon has been heard again. But
repeated reboots mean the door state is repeatedly unknown, which is worth
fixing.

---

## Still stuck

Open an issue with:

- The **full serial output from boot**, including the banner.
- The output of `s`.
- Your `secrets.h` **with the MAC redacted**.
- Your board, beacon model, and relay module.
- What the door did versus what you expected.

Reports from real coops are the most useful thing this project receives.
