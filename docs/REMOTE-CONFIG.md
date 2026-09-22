# Managing an installed door remotely

Once the door is screwed to a wall, the serial console is gone — and with it, in
earlier builds, every way of changing anything. This page is about getting it
back.

> **Requires the log server.** A door with no `LOG_ENDPOINT_URL` has no channel
> to be managed over, and nothing here applies. See
> [LOG-SERVER.md](LOG-SERVER.md).
>
> **And requires one flash to get here.** A door running firmware from before
> this existed cannot be told about it remotely — that is the loop this closes.
> See [FLASHING.md](FLASHING.md).

---

## The problem this solves

Over-the-air updates were added so the buttons would no longer be needed. But
the OTA window could only ever be opened by pressing **`p` on the serial
console** — so the feature meant to remove physical access could not itself be
reached without it. The first time you genuinely need OTA, on a door already
mounted, it is not there.

The fix is not another inbound service. The door is behind NAT, its WiFi radio
is down almost all the time, and opening a port to a microcontroller that moves
a door is a poor trade. Instead it uses the channel that already exists:

```
   door  ──── POST /ingest, signed ────►  your server
         ◄─── reply: queued commands ────
              (signed with the same key)
```

The door calls you. Anything queued rides back in the reply to an upload it was
going to make anyway. No port, no NAT traversal, no polling that was not already
happening.

**Turnaround is one upload interval** — about five minutes on a typical door,
since uploads happen whenever the beacon has been away and settled for
`WIFI_MIN_UPLOAD_INTERVAL_MS`.

---

## Queueing a command

```bash
python3 petdoor-logserver.py --queue pulse 500
python3 petdoor-logserver.py --commands        # what is queued, sent, acknowledged
```

`--commands` shows the whole lifecycle, which is the point:

```
  #3    2026-09-20 16:32  back-door   ota                    acked: 3 applied
  #2    2026-09-20 16:32  back-door   thresholds -58 -68     acked: 3 applied
  #1    2026-09-20 16:32  back-door   pulse 500              acked: 3 applied
  #4    2026-09-20 16:33  back-door   pulse 300              QUEUED — waiting for the door to call in
```

Three states, and they mean different things:

| State | Meaning |
|---|---|
| `QUEUED` | The door has not called in yet, or is running firmware without `REMOTE_CONFIG` |
| `delivered` | Sent, but the door has not reported back yet — one more upload |
| `acked: …` | The door applied it, or says why it refused |

A command is **never silently consumed**. A door on older firmware does not send
`X-PetDoor-Remote`, so the server holds everything rather than marking it
delivered to something that will never read it.

`--clear-commands` drops anything not yet delivered.

---

## What you can change

Everything the serial console can set, and nothing else.

| Command | Does |
|---|---|
| `thresholds <enter> <exit>` | RSSI thresholds in dBm — enter must be above exit |
| `dwell <open> <close> <lockout>` | Dwell times and the actuation lockout, ms |
| `gap <ms>` | Relay interlock dead time |
| `pulse <ms>` | How long the relay is held closed — the "button press" |
| `presses <n> [gap]` | Press n times per actuation. For a controller that swallows presses; see the caveat in [TROUBLESHOOTING.md](TROUBLESHOOTING.md#the-relay-clicks-but-the-door-does-not-move) |
| `travel <ms>` | How long your door takes to move. Drives the "moving" LED and buzzer; 0 silences them |
| `buzzer <pin>\|off` | Which GPIO the buzzer is on. Add `passive` for a bare transducer, `low` if it sounds when pulled to GND |
| `beep` | Sound the buzzer now — how you find an undocumented board's buzzer pin from indoors |
| `sensors <open> <closed>` | Limit switch pins, or `off`. `-1` for an end with no switch. This is how sensors get switched on after they are wired, without a flash |
| `upload <settle> <interval> <heartbeat>` | How often the door calls in, ms. Lower the interval for faster commands, at the cost of radio time the beacon scan would have had. Heartbeat `0` disables it |
| `filter <window> <alpha>` | The close-path filter |
| `openfilter <window> <alpha>` | The open-path filter |
| `macs <csv>` | Beacon list. **Restarts the door** — see below |
| `door open\|close\|auto` | Actuate now, or clear a manual hold |
| `lock` / `unlock` | Stop the beacon opening the door — see below |
| `resetstats` | Zero the proximity statistics |
| `ota` | Open an OTA window so you can push new firmware |
| `defaults` | Revert every stored setting to the compiled-in values |
| `scan` | Upload the discovery table — how you learn a new beacon's address |
| `reboot` | Restart, after the acknowledgement has been sent |

Each one calls **the same setter the serial console calls**, so the validation
that protects a person at the keyboard protects the network path identically.

### From a browser instead

**Everything in that table is also on the private dashboard**, if the server was
started with `--allow-web-control` — the door actions as buttons, and every
tunable as a typed field filled in with the door's current value. Same queue,
same validation, same delay. See
[WEB-DASHBOARD.md](WEB-DASHBOARD.md#controlling-the-door-from-the-browser).

The four that lose state or take the door off the air — `macs`, `defaults`,
`reboot`, `ota` — ask for confirmation there, and the server refuses them
without it even if the request is crafted by hand.

The command line remains the only way in when the dashboard is not reachable,
and the only place the credentials can be changed at all.
Thresholds that form no hysteresis band, a lockout longer than the close dwell,
an open filter slower than the close filter — all still refused, and the refusal
comes back in the acknowledgement.

### `macs` restarts the door

The BLE callback reads the beacon list on every advertisement, so swapping it
underneath is a data race. The list is saved and the door restarts to pick it
up.

The restart is **deferred by `REMOTE_RESTART_DELAY_MS`** (20 s) and waits for the
uploader to go quiet, so the acknowledgement reaches you first. The
acknowledgement lives in RAM; rebooting immediately would lose it and you could
not tell "applied" from "never arrived".

---

## What you cannot change, on purpose

**WiFi credentials, the log endpoint, the shared key, and the OTA password.**

Every one of those is part of the channel the command travels over. A remote
change that broke any of them would strand the door permanently — reachable only
with a ladder and a USB cable. The server refuses to queue them:

```
$ python3 petdoor-logserver.py --queue wifi mynetwork
'wifi' is not remotely settable — it is part of the channel this command travels over
```

The door enforces the same rule independently, because anyone can run a server.

**This is also your recovery path.** Since the lifeline can never be broken
remotely, a door that is reachable stays reachable — so a bad setting is always
undoable with `--queue defaults`.

---

## Why the reply is signed

Uploads go over **plain HTTP by design**: a TLS handshake costs this chip more
heap than it has — measured, free heap fell from 80 KB to 18 KB and the
connection failed outright. Signing instead of encrypting is the accepted trade.

That trade is fine for telemetry, where the worst case is someone learning that
a door opened. It is **not** fine for commands. An unsigned reply would let
anyone on the path retune the door, raise its radio whenever they liked, or open
it.

So the reply carries `X-PetDoor-Signature`, an HMAC-SHA256 over its own
timestamp and body under `LOG_SHARED_KEY` — the same scheme, the same key, the
other direction. The door verifies before obeying and ignores the reply
otherwise:

```
[cmd] reply ignored: BAD SIGNATURE — the server's key differs, or
[cmd] something on the path is trying to reconfigure this door
```

**With no key configured, the door refuses all commands** and the server refuses
to send any. Log uploads can survive having no key; taking orders cannot.

### …and bound to the request that asked for it

A signature alone proves the server *once said* these bytes. It does not prove
it said them **now**, to **this** request — and over plain HTTP that gap is an
attack: record a reply carrying `door open`, play it back whenever you like, and
the signature still checks out.

So the door sends a fresh random **nonce** with every upload and folds it into
the signature it expects back:

```
  signed material = timestamp + "\n" + nonce + "\n" + body
```

A recorded reply then verifies against the wrong material and is rejected:

```
[cmd] reply ignored: BAD SIGNATURE — the server's key differs, or
[cmd] something on the path is trying to reconfigure this door
```

The nonce is burned once a reply has been judged, so one upload authorises at
most one reply. A reply arriving with no upload outstanding is refused before
the signature is even checked.

This repairs an asymmetry: the **upload** direction was always replay-protected,
because the server rejects timestamps outside `CLOCK_SKEW_S`. The reply
direction had no equivalent until the nonce.

> **Both halves must be updated together.** A door that sends no nonce is
> running firmware from before this existed, and the server withholds commands
> rather than sending a reply that door would reject anyway — it says so in the
> log.

The OTA password is still what guards an actual firmware push. This guards the
trigger.

---

## Locking the door

```bash
python3 petdoor-logserver.py --queue lock
python3 petdoor-logserver.py --queue unlock
```

**Locked means the beacon can no longer open the door.** Use it to keep an
animal in overnight, before a vet trip, or while the coop is being cleaned —
without taking the collar off or changing any thresholds.

It is deliberately narrow, and each limit is a decision:

**It does not close a door that is already open.** Locking states a rule about
*future* opens; it does not slam a door an animal may be standing in. A locked
door that is open settles shut on its own when the beacon leaves, through the
normal close path with every dwell and interlock intact.

**It does not block closing.** Closing is the safe direction and is never gated
on anything.

**It does not block you.** `door open` still works while locked, from the
console or the server — the lock is about the collar, not the owner.

That last point is the important one:

> ### If the animal is shut outside
>
> A locked door will not let it back in. It is a lock; that is what a lock does.
>
> **`--queue door open` still works while locked**, and applies the usual
> manual hold, so you can let it in without unlocking and losing the state you
> wanted. That is the escape hatch, and it is the reason the lock does not
> block manual actuation.
>
> Turnaround is still one upload interval. A lock is not the right tool for
> something you may need to undo in seconds.

**It survives a reboot.** A lock that a power cut silently clears is not a lock.
The cost is that it also survives a reboot you did not intend, which is why the
boot banner, `s`, the status line and the dashboard all say so — and why the
door's status LED gives a locked-and-shut door a *double* blip, so a glance
tells you whether it is merely closed or closed against the collar.

**`defaults` does not clear it.** That command exists to undo a bad tuning
change; quietly unlocking a door as a side effect would be a surprise in the one
direction that matters.

---

## Seeing what the door sees

You cannot tune a threshold you cannot read, and you cannot point the door at a
new beacon whose address you have no way to discover. Both used to be
console-only.

Every upload now carries a status line, and `--doors` shows it:

```
  back-door
    firmware   : v1.1.0  (Sep 20 2026)
    boots      : #31
    last heard : 0 min ago
    push to    : 192.168.1.57
    commands   : accepted
    sees       : rssi=-63 raw=-63 dist=1.4 present=1 door=OPEN gap=2087
                 samples=1246 adv=62845 weak=9 heap=138420 up=676
```

`push to` is the door's **own** address, which it now reports itself. The server
sees only whatever last hop connected — behind a reverse proxy that is the
proxy, so before this the door could open an OTA window and leave you with
nowhere to aim.

`commands: NOT SUPPORTED` means that door is running firmware from before this
existed, and anything you queue will sit there. The server says so rather than
letting you wonder.

For the beacons themselves:

```bash
python3 petdoor-logserver.py --queue scan
python3 petdoor-logserver.py --scan          # after the next upload
```

```
back-door — discovery table at 2026-09-20 17:29

MAC                 RSSI   Age    Class      Name / data
ac:23:3f:25:17:0a   -52    118ms  Minew      MST01 mfg=4c000215...
d4:8a:39:11:22:33   -81    402ms  phone      (random address)
```

Same rendering the console produces — one implementation, so the remote view
cannot drift from it. That is how you get the address for `macs`.

---

## A bad push rolls itself back

The door holds a freshly flashed image **unconfirmed** until it proves it can
still be reached, and marks it good only after a **successful upload**. Reboot
before that and the bootloader puts the previous image back.

This matters more than it sounds. The Arduino core normally confirms every
image at boot, before any of this firmware runs — so the bootloader's rollback
could never fire, and an image that booted but could not join WiFi was
permanent. On a door you can walk up to, harmless. On one screwed to a wall,
that is the ladder.

```
[ota] image confirmed good (upload succeeded); rollback cancelled
```

The honest cost: if your server is down for a long stretch **and** the door
reboots, it rolls back a perfectly good image. That is the right way round —
re-pushing is easy and a ladder is not. `OTA_REQUIRE_CONFIRM 0` turns it off.

---

## It calls in even when the animal is home

Uploads used to happen only while the beacon was away and the door shut, which
is sensible — the radio is shared, and talking costs BLE sampling. But a pet
that stays in all weekend kept the door silent, and a silent door collects no
commands.

Past `WIFI_HEARTBEAT_MS` (30 minutes) it now calls in regardless. One burst
every half hour is a rounding error against losing the channel for two days.

---

## Pushing firmware

```bash
python3 petdoor-logserver.py --queue ota
```

Within an upload interval the door opens its OTA window for `OTA_WINDOW_MS`
(5 minutes) and prints the command to use. Then:

```bash
arduino-cli upload --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs \
  -p <door-ip> petdoor
```

The window costs BLE sampling while it is open — one radio, shared — so the door
declines to open one while the beacon is present, and closes it again on a
timeout rather than leaving it up.

---

## Turning it off

`REMOTE_CONFIG 0` restores the old behaviour: the door talks and never listens.
The server still queues commands; they simply sit at `QUEUED` forever, which
`--commands` makes obvious.

There is an honest argument for doing this on a door you can still reach
physically. The channel is small, signed and cannot break its own lifeline — but
it is still a way for a machine on your network to change what a door does, and
the safest feature is the one that is not enabled.
