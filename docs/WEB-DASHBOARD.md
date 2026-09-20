# Running the dashboard on your own server

**Entirely optional.** Your door works, and keeps its own log, whether or not
any of this exists. This page is for when you want a web page you can open from
the sofa instead of plugging in a serial cable.

Everything here runs equally well on a Raspberry Pi on your desk or a VM on the
internet. Nothing is tied to a hosting provider, and there is no cloud account.

---

## How the pieces fit

```mermaid
flowchart LR
    D["🚪 Door<br/><i>ESP32</i>"]
    S["Log server<br/><i>one Python file</i>"]
    B["🌐 Dashboard<br/><i>your browser</i>"]

    D -- "signed HTTP POST<br/>when the pet is away" --> S
    S -- "stores in SQLite" --> S
    S -- "serves charts" --> B

    style D fill:#E9A23B,stroke:#C8862A,color:#3b2a10
    style S fill:#2A9D8F,stroke:#21867A,color:#ffffff
    style B fill:#f3f4f6,stroke:#9AA5B1,color:#1f2937
```

Three parts, and you can stop after any of them:

| Part | What it is | Where it runs |
|---|---|---|
| **The door** | Records events to its own memory | Always. No network needed |
| **The log server** | Receives, stores, and serves the dashboard | A machine that stays on |
| **The dashboard** | Charts in a browser | Served by the log server |

---

## Choose where it runs

```mermaid
flowchart TD
    Q{"Do you want to see it<br/>from outside the house?"}
    Q -- no --> LAN["<b>Run it on your LAN</b><br/>Pi, NAS, spare laptop<br/><i>simplest and most private</i>"]
    Q -- yes --> PUB["<b>Run it on a VM</b><br/>with a domain name"]
    PUB --> TLS["Add TLS for the dashboard<br/><i>certbot, one command</i>"]

    style LAN fill:#2A9D8F,stroke:#21867A,color:#ffffff
    style PUB fill:#E9A23B,stroke:#C8862A,color:#3b2a10
    style TLS fill:#f3f4f6,stroke:#9AA5B1,color:#1f2937
```

**On your LAN** is the better default. The data never leaves your house, there
is nothing exposed to the internet, and there is no certificate to renew.
Anything always-on will do — a Pi Zero is more than enough.

**On a public VM** if you want it from anywhere. You then need a domain name
pointing at the machine, and TLS for the dashboard.

---

## 1. Deploy it

One command, and it is the same whichever you chose:

```bash
cd tools/logserver

./deploy.sh pi@raspberrypi.local              # LAN, port 8080
./deploy.sh you@vm.example.com petdoor.example.com   # public, with nginx
```

It checks it can log in, copies two files, writes a `systemd` unit, starts the
service, and prints the shared key your door needs. **Re-running it upgrades in
place** and never touches the database or rotates the key.

What it needs on the far end: SSH key access, `python3`, `systemd`, and `sudo`.
Nothing else — no pip, no Docker, no package installs.

<details>
<summary>What if I would rather not run a script against my server?</summary>

Copy two files and run one command:

```bash
scp tools/logserver/petdoor-logserver.py tools/logserver/dashboard.html you@host:~/
ssh you@host
python3 petdoor-logserver.py --init      # prints the shared key
python3 petdoor-logserver.py --port 8080
```

That is the whole thing. `deploy.sh` only adds the `systemd` unit so it comes
back after a reboot, and the nginx vhost if you gave it a domain.
</details>

### If you gave it a domain

The DNS record has to exist **before** you deploy, or nginx has nothing to
answer for and certbot cannot verify you own it:

```
petdoor.example.com.   A   198.51.100.10
```

Then, optionally, TLS for the dashboard:

```bash
ssh you@host sudo certbot --nginx -d petdoor.example.com
```

---

## 2. Point the door at it

The deploy script prints these. Put them in `petdoor/secrets.h`, which is
git-ignored:

```c
#define WIFI_SSID        "your-network"
#define WIFI_PASSWORD    "your-password"
#define LOG_ENDPOINT_URL "http://petdoor.example.com/ingest"
#define LOG_SHARED_KEY   "4f8a1c9e2b7d3056a1e8f4c2d9b06537"
#define LOG_DEVICE_ID    "back-door"          // optional, if you have several
```

Reflash, then press `u` in the serial console to force an upload rather than
waiting for the door to find an idle moment:

```
[wifi] radio up — BLE sampling is degraded until this finishes
[wifi] uploaded 14 events, clock synced
[wifi] radio down
```

Open the dashboard and the events are there.

### Why the door uses plain HTTP even when you have TLS

Deliberate, and the nginx config above keeps `/ingest` on port 80 for exactly
this reason.

The door signs every upload with **HMAC-SHA256** over your shared key. The key
is never transmitted, so nobody can forge events or replay an old batch. What
TLS would add is hiding the contents — at a cost the ESP32 pays badly:

| | HTTP + HMAC | HTTPS |
|---|---|---|
| Extra radio time per upload | ~0 | **1–3 s handshake** |
| Extra heap | ~0 | ~40 KB |
| Forgeable by an attacker | no | no |
| Readable by a network sniffer | yes | no |

**Radio time is the one resource the door cannot spare** — the same antenna
hears the beacon, so every second spent on WiFi is a second not spent detecting
your pet. Put TLS in front of the *dashboard*, where a browser pays for it, and
leave the door speaking HTTP to nginx.

---

## 3. Read it

The server splits its routes into a **public** half and a **private** half, so
a reverse proxy can protect the part that matters without hiding the project
entirely.

```mermaid
flowchart LR
    P["/"] --> PD["Project page<br/><i>public — no data</i>"]
    I["/images/*"] --> ID["Photographs<br/><i>public</i>"]
    H["/health"] --> HD["Uptime check<br/><i>public</i>"]
    R["/dashboard"] --> RD["Analytics<br/><b>private</b>"]
    A["/api/events"] --> AD["JSON log<br/><b>private</b>"]
    C["/export.csv"] --> CD["Whole log as CSV<br/><b>private</b>"]
    T["/table"] --> TD["Plain table<br/><b>private</b>"]

    style PD fill:#f3f4f6,stroke:#9AA5B1,color:#1f2937
    style ID fill:#f3f4f6,stroke:#9AA5B1,color:#1f2937
    style HD fill:#f3f4f6,stroke:#9AA5B1,color:#1f2937
    style RD fill:#2A9D8F,stroke:#21867A,color:#ffffff
    style AD fill:#2A9D8F,stroke:#21867A,color:#ffffff
    style CD fill:#2A9D8F,stroke:#21867A,color:#ffffff
    style TD fill:#2A9D8F,stroke:#21867A,color:#ffffff
```

**Why bother splitting it.** A door log is a record of when somebody is home.
Trips per day, first out, last out and the daily-rhythm strip together describe
a household's routine precisely enough to tell a stranger when the house is
reliably empty. That is worth a login. The project page is not.

**The server does no authentication itself** — it is 400 lines of stdlib Python
and has no business holding credentials. It only arranges the routes so your
proxy can. See [protecting the private routes](#protecting-the-private-routes).

### The public page

<p align="center">
  <img src="assets/screenshot-public.jpg" alt="The public project page: what PetDoor is, photographs of the build, how it works, and a sign-in link to the analytics" width="760">
</p>

Served at `/`. It explains what the door is, shows the build, and links to the
analytics behind sign-in. No event data of any kind reaches it.

### The analytics

<p align="center">
  <img src="assets/screenshot-analytics.png" alt="The analytics dashboard: trips a day, typical trip, longest trip, time outside, first and last out, and a daily rhythm strip" width="860">
</p>

Six numbers across the top, then the **daily rhythm** — one row per day, every
trip drawn in place, night shaded. The pattern is the point: you can see the
routine, and you can see the day it changed.

<p align="center">
  <img src="assets/screenshot-analytics-charts.png" alt="Trip duration histogram, trips per day, time of day, and the beacon signal trend" width="860">
</p>

Then how long the trips actually are, how many per day, which hours are busy,
and a **beacon-signal trend** — the strength at each return. A steady decline
there is a flat battery, weeks before the door starts missing.

> The screenshots above are generated from synthetic data, not from a real
> household. That is deliberate: they are in a public repository, and the whole
> reason the analytics sit behind a login is that this data describes when a
> house is empty.

**Times are shown in the viewer's local timezone.** The door uploads UTC and the
server stores UTC; only the display is local. Every chart here answers a
question about a human day, and a UTC day is the wrong day for anyone who does
not live on one — at UTC-7 an evening outing at 18:38 would otherwise report as
"01:38" and land on the following row.

The dashboard shows daily rhythm, how long your pet stays out, trips per day,
time of day, and a beacon-signal trend that warns of a flat battery before it
starts missing.

Across the top it also lists each door with **the firmware it is running, how
many times it has rebooted, and when it last called in**. That row is the
health check: a reboot count climbing between uploads is a power problem, and a
door that has not been heard from in hours is off, off the network, or failing
to upload. It says so rather than leaving you to notice. It refreshes itself once a minute, which is plenty — the door
uploads in bursts, not continuously.

---

## Protecting the private routes

The split is only worth anything if something enforces it. The server listens on
localhost; your proxy decides who reaches which path.

Whatever you use, the rule is the same shape:

| Path | Who |
|---|---|
| `/`, `/images/*`, `/health` | anyone |
| `/dashboard`, `/api/*`, `/table`, `/export.csv` | you |
| `/ingest` | the door — POST only, HMAC-signed, **never** behind the login |

**`/ingest` must stay outside the login.** The door signs its uploads with the
shared key and cannot complete a browser sign-in flow. Put it behind SSO and
uploads fail silently with a redirect the firmware records as an HTTP error.

### Caddy

```caddyfile
petdoor.example.com {
    # Private: everything derived from the log
    @private path /dashboard* /api/* /table* /export.csv*
    basic_auth @private {
        you $2a$14$...        # caddy hash-password
    }
    reverse_proxy 127.0.0.1:8080
}
```

With an identity provider, replace `basic_auth` with your `forward_auth` block
against the same `@private` matcher — the matcher is the part that matters.

### nginx

```nginx
server {
    server_name petdoor.example.com;

    location ~ ^/(dashboard|api|table|export\.csv) {
        auth_basic           "PetDoor";
        auth_basic_user_file /etc/nginx/petdoor.htpasswd;
        proxy_pass           http://127.0.0.1:8080;
    }

    location / {                       # public page, images, health, ingest
        proxy_pass http://127.0.0.1:8080;
    }
}
```

`htpasswd -c /etc/nginx/petdoor.htpasswd you` creates the password file.

### Apache

```apache
<Location "/dashboard">
    AuthType Basic
    AuthName "PetDoor"
    AuthUserFile /etc/apache2/petdoor.htpasswd
    Require valid-user
</Location>
# repeat for /api, /table, /export.csv
ProxyPass        / http://127.0.0.1:8080/
ProxyPassReverse / http://127.0.0.1:8080/
```

### Checking it

```bash
curl -o /dev/null -w '%{http_code}\n' https://petdoor.example.com/            # 200
curl -o /dev/null -w '%{http_code}\n' https://petdoor.example.com/health      # 200
curl -o /dev/null -w '%{http_code}\n' https://petdoor.example.com/dashboard   # 401
curl -o /dev/null -w '%{http_code}\n' https://petdoor.example.com/api/events  # 401
```

If `/api/events` returns 200 without a credential, your rule is matching the
page but not the data behind it, and the analytics are protected in appearance
only. That is the check worth running after any proxy change.

### If you would rather not publish anything

Put the whole host behind the login. The public page is a convenience, not a
requirement, and nothing in the door depends on it.

---

## Managing the shared key

```bash
ssh you@host
cd /opt/petdoor
python3 petdoor-logserver.py --show-key      # what the door should be using
python3 petdoor-logserver.py --rotate-key    # issue a new one
python3 petdoor-logserver.py --no-key        # accept unsigned uploads
```

**Rotating takes effect immediately.** The door's next upload is rejected until
you reflash it with the new key — deliberately, since a key you could change
without touching the door would not be worth much. The door says so plainly:

```
[wifi] upload REJECTED (HTTP 401) — the server did not accept our signature.
[wifi] events are kept locally and will be retried
```

Nothing is lost meanwhile. Events stay in the door's ring and go up once the key
matches, as long as you fix it before 128 newer events push them out.

---

## When it does not work

| Symptom | Where to look |
|---|---|
| Dashboard says "could not reach the log server" | `sudo systemctl status petdoor-log` |
| Door logs `HTTP 401` | Key mismatch — compare `--show-key` with `secrets.h` |
| Door logs `HTTP 404` | The URL must end in `/ingest` |
| Door logs `endpoint unreachable` | Firewall, wrong IP, or nginx not proxying |
| Door logs `could not associate` | WiFi credentials or signal, not the server |
| Dashboard is empty but ingest returns 200 | Events arrived with no clock; check the plain `/table` view |

On the server:

```bash
sudo journalctl -u petdoor-log -f      # live, including rejected uploads
```

A rejected upload is logged with the reason, so a key mismatch is visible from
both ends.

---

## Privacy

Door events say when somebody is coming and going from your house. On a LAN that
stays between your devices. If you publish it, put it behind TLS and consider
whether it should be reachable without a login at all — nginx `auth_basic` is
two lines, and the [nginx docs] cover it.

The log server itself has no accounts and no login: it assumes whoever can reach
it is allowed to read it. That is fine on a home network and is the reason the
LAN option is the recommended one.

[nginx docs]: https://docs.nginx.com/nginx/admin-guide/security-controls/configuring-http-basic-authentication/
