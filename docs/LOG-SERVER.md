# Collecting logs on a server

**Entirely optional.** With no endpoint configured the door logs to its own
memory and nothing leaves it — that is the default, and for most builds it is
enough. This page is for when you want the history somewhere you can read it
without plugging in a cable.

There are three levels, and you can stop at any of them:

| | What you get | What it costs |
|---|---|---|
| **Local only** *(default)* | Last 128 events, read with `l` over serial | nothing |
| **+ log server** | Full history, a web page, CSV export | a machine that is always on |
| **+ portal** | Charts, trip analysis, beacon health | nothing more |

---

## Level 1: local only (the default)

Do nothing. The door keeps its last `EVENT_LOG_CAPACITY` (128) events in NVS,
oldest rolling out as new ones arrive, so it **cannot fill up**. Read them with
`l` in the serial console, or `L` for CSV.

This survives power cuts and needs no network, no server and no WiFi. If you
only want to answer "what happened last night", stop here.

---

## Level 2: run a log server

A door only remembers 128 events. A server remembers everything, and you can
read it from a browser instead of a serial cable.

### Start it

The server is one Python file with **no dependencies** — standard library only,
Python 3.8+. It runs on anything that is always on: a Pi, a NAS, an old laptop.

```bash
cd tools/logserver
python3 petdoor-logserver.py --init
```

```
database : /home/you/petdoor.sqlite3
key      : 4f8a1c9e2b7d3056a1e8f4c2d9b06537

Put this in petdoor/secrets.h:
  #define LOG_SHARED_KEY "4f8a1c9e2b7d3056a1e8f4c2d9b06537"
```

Then run it:

```bash
python3 petdoor-logserver.py --port 8080
```

| URL | What it is |
|---|---|
| `POST /ingest` | Where the door uploads |
| `GET /` | Review page — recent events, counts, brownouts |
| `GET /export.csv` | Everything, in the same CSV the door emits |
| `GET /health` | For a process supervisor |

### Point the door at it

In `petdoor/secrets.h` (git-ignored):

```c
#define WIFI_SSID        "your-network"
#define WIFI_PASSWORD    "your-password"
#define LOG_ENDPOINT_URL "http://192.168.1.50:8080/ingest"
#define LOG_SHARED_KEY   "4f8a1c9e2b7d3056a1e8f4c2d9b06537"
#define LOG_DEVICE_ID    "back-door"        // optional; lets one server collect several
```

Reflash. Check it with `u` in the console to force an upload rather than
waiting for an idle window:

```
[wifi] radio up — BLE sampling is degraded until this finishes
[wifi] uploaded 14 events, clock synced
[wifi] radio down
```

### Keep it running

```ini
# /etc/systemd/system/petdoor-log.service
[Unit]
Description=PetDoor log server
After=network.target

[Service]
ExecStart=/usr/bin/python3 /opt/petdoor/petdoor-logserver.py --port 8080
Environment=PETDOOR_DB=/var/lib/petdoor/petdoor.sqlite3
Restart=always
User=petdoor

[Install]
WantedBy=multi-user.target
```

---

## Why HTTP and not HTTPS

Deliberate, and worth understanding before you change it.

The door **signs** every upload with HMAC-SHA256 over your shared key. The key
itself is never transmitted — only a signature over the timestamp and body — so
an eavesdropper cannot forge events, replay an old batch, or learn the key.

TLS would additionally hide the contents, at a price the ESP32 pays badly:

| | HTTP + HMAC | HTTPS |
|---|---|---|
| Extra radio time per upload | ~0 | **1–3 s TLS handshake** |
| Extra heap | ~0 | ~40 KB |
| Key sent over the wire | never | never |
| Events forgeable by an attacker | **no** | no |
| Events readable by a LAN sniffer | yes | no |

**Radio time is the resource this project cannot spare.** The ESP32 shares one
antenna between WiFi and Bluetooth, and every second the radio spends on WiFi is
a second it is not hearing your pet's beacon. A TLS handshake roughly doubles
the length of each upload window.

The trade that makes sense: door events are **low-secrecy but high-integrity**.
It matters a great deal that nobody can forge "door opened"; it matters much
less that someone already inside your network can see that the door opened. HMAC
gives you the first for free.

**If the endpoint is across the public internet**, do not turn on TLS in the
firmware — put the server behind a VPN (Tailscale, WireGuard) or a
TLS-terminating reverse proxy. The ESP32 then still speaks plain HTTP to
something local, and the encryption happens on hardware that can afford it.

> Without `LOG_SHARED_KEY`, uploads are accepted unsigned. That is fine on a
> network where you trust everything, and a bad idea otherwise — anyone who can
> reach the port could post fabricated events.

### Changing the key

```bash
python3 petdoor-logserver.py --show-key     # what the door should be using
python3 petdoor-logserver.py --rotate-key   # issue a new one
python3 petdoor-logserver.py --set-key abc123
python3 petdoor-logserver.py --no-key       # accept unsigned uploads
```

**Rotating takes effect immediately**, so the door's next upload is rejected
until you reflash it with the new key. That is deliberate — a key you can
rotate without touching the door would not be much of a key. The door reports it
clearly rather than failing silently:

```
[wifi] upload REJECTED (HTTP 401) — the server did not accept our signature.
       Check LOG_SHARED_KEY matches.
[wifi] events are kept locally and will be retried
```

Nothing is lost while the key is wrong: events stay in the door's ring and go up
once it matches again, provided you fix it before 128 new events push the old
ones out.

---

## Level 3: charts

Two ways, and they show the same analysis:

- **Self-hosted** — the log server already serves a dashboard at `/`. Deploy it
  with one command and open it in a browser: see
  [WEB-DASHBOARD.md](WEB-DASHBOARD.md).
- **Hosted** — download `GET /export.csv` and drop it into the
  [Door Log portal](PORTAL.md). Nothing to run, but you import by hand.

Self-hosting is the better fit once a door is uploading on its own: the page
refreshes itself and there is no file to move around.

---

## When uploads fail

The door is built to degrade quietly. Events are always written to NVS first, so
a failed upload never loses anything; the ring simply holds them until the next
attempt.

| Symptom | Meaning |
|---|---|
| `no LOG_ENDPOINT_URL set` | Local-only mode. Working as configured |
| `upload REJECTED (HTTP 401)` | Key mismatch, or the door's clock is far out |
| `upload failed, HTTP 404` | Wrong path — it should end in `/ingest` |
| `endpoint unreachable` | Server down, wrong IP, or WiFi did not associate |
| `could not associate` | WiFi credentials or signal. The radio is handed back to BLE |

`s` shows the running state:

```
  log endpoint : http://192.168.1.50:8080/ingest  (signed)
  last response: HTTP 200
  wifi         : idle (radio off), 12 uploads, 0 failures, clock synced
```

Because the door re-sends its whole ring each time and the server de-duplicates
on `(device, boot, uptime, event)`, a retry after a failure is harmless — it
adds only what the server had not already seen.
