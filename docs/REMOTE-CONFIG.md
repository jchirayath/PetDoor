# Managing an installed door remotely

Once the door is screwed to a wall, the serial console is gone — and with it, in
earlier builds, every way of changing anything. This page is about getting it
back.

> **Requires the log server.** A door with no `LOG_ENDPOINT_URL` has no channel
> to be managed over, and nothing here applies. See
> [LOG-SERVER.md](LOG-SERVER.md).

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
| `filter <window> <alpha>` | The close-path filter |
| `openfilter <window> <alpha>` | The open-path filter |
| `macs <csv>` | Beacon list. **Restarts the door** — see below |
| `door open\|close\|auto` | Actuate now, or clear a manual hold |
| `resetstats` | Zero the proximity statistics |
| `ota` | Open an OTA window so you can push new firmware |
| `defaults` | Revert every stored setting to the compiled-in values |

Each one calls **the same setter the serial console calls**, so the validation
that protects a person at the keyboard protects the network path identically.
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

The OTA password is still what guards an actual firmware push. This guards the
trigger.

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
