# The dashboard

**Entirely optional.** Out of the box the door keeps its last 128 events in its
own memory, rolling the oldest out so it cannot fill up, and nothing ever leaves
it. The door works identically with no network at all.

Give it a network and an endpoint and it will additionally upload that log to a
server **you** run, signed with a shared key.

> 🔎 **[See a live sample](https://petdoor.aspl.net/demo)** — synthetic data, no
> sign-in needed.

---

## What it shows

Six figures across the top — trips a day, typical trip, longest trip, total time
outside, first out, last out — then:

- **Daily rhythm.** One row per day, every trip drawn in place, night shaded.
  The pattern is the point: you can see the routine, and you can see the day it
  changed.
- **How long he stays out.** A duration histogram, with the median called out
  rather than the average, because a couple of long garden sessions drag a mean
  upward and the median is the honest number.
- **Trips per day** and **time of day.**
- **Beacon signal.** Strength at each return. A steady decline is a flat
  battery, weeks before the door starts missing.

Times render in **your** local timezone. The door uploads UTC and the server
stores UTC; only the display is local, because every chart here answers a
question about a human day.

---

## Public page, private analytics

The server splits its routes so a reverse proxy can protect the half that
matters:

| | |
|---|---|
| **Public** | `/` project page · `/demo` sample dashboard · `/health` |
| **Private** | `/dashboard` · `/api/events` · `/table` · `/export.csv` |
| **Signed** | `/ingest` — HMAC, and must stay outside the login |

**Why bother.** A door log is a record of when somebody is home. Trips per day,
first out, last out and the rhythm strip together describe a household's routine
precisely enough to tell a stranger when the house is reliably empty. That is
worth a login; the project page is not.

The server does no authentication itself — it is stdlib Python and has no
business holding credentials. It arranges the paths so your proxy can. Worked
Caddy, nginx and Apache rules are in the docs.

> `/ingest` must stay **outside** the login. The door signs its uploads and
> cannot complete a browser sign-in; behind SSO it gets a redirect and records
> an HTTP error.

---

## Running it

It is one stdlib-Python file and a SQLite database. A Raspberry Pi is plenty.

```bash
python3 petdoor-logserver.py --init      # create the db and a shared key
python3 petdoor-logserver.py --show-key  # the key the door must use
python3 petdoor-logserver.py --port 8080
```

Then point the door at it in `secrets.h` and give it the key.

Full deployment, key rotation and the proxy rules:
**[WEB-DASHBOARD.md](https://github.com/jchirayath/PetDoor/blob/main/docs/WEB-DASHBOARD.md)**
and
**[LOG-SERVER.md](https://github.com/jchirayath/PetDoor/blob/main/docs/LOG-SERVER.md)**

---

## Uploads are deferred on purpose

The ESP32 shares **one antenna** between WiFi and Bluetooth. Bringing the radio
up to upload degrades BLE sampling while it happens — the console says so when
it does.

So the door batches its log and flushes on a schedule (2 am by default, when the
animal is least likely to be at the door) rather than uploading events as they
happen. The radio is down almost all of the time.
