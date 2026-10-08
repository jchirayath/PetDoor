# Privacy

Short version: **nothing is sent to the authors of this project. There is no
telemetry, no analytics and no phone-home.** A door with no `LOG_ENDPOINT_URL`
sends nothing anywhere at all.

If you *do* run the log server, you are running it. The data goes to your
machine, and you are the one holding it — which also means you are the one
responsible for it, particularly for the parts that are about other people.

This page describes what the system handles, so you can decide what you are
comfortable keeping. It is a description of the software's behaviour, not a legal
privacy policy, and it is not legal advice.

---

## Two deployments, and they are very different

**Without a log server.** Leave `LOG_ENDPOINT_URL` unset and the door never
brings up WiFi for uploads. Everything stays on the chip: configuration in NVS,
and a ring of the **last 128 events**, which wraps. Nothing crosses the network.
This is the private configuration, and it is the default.

**With a log server.** The door calls *out* to the endpoint you configure,
signed with your key, and nothing ever connects *in* to the door. The server is
yours — the reference one runs on a host the owner controls. See
[LOG-SERVER.md](LOG-SERVER.md).

## What the door itself holds

| | |
|---|---|
| Configuration in NVS | pins, thresholds, travel times, the beacon address, the schedule |
| Event ring | the last **128** events: door movements, fixes gained and lost, faults, boots |
| Nothing else | no history beyond the ring, no audio, no images, no network capture |

## What the server stores

Per the schema in `tools/logserver/petdoor-logserver.py`:

| table | holds |
|---|---|
| `events` | one row per event: door name, wall-clock time, uptime, boot number, event type, a detail integer, **RSSI**, and one spare integer whose meaning depends on the type |
| `devices` | current firmware, build, boot count, last seen, and **`last_ip`** |
| `firmware` | every distinct firmware a door has run, and when |
| `commands` | every command queued, when it was delivered, and what the door answered |
| `scans` | **discovery dumps — the addresses of nearby Bluetooth devices.** See below |

`last_ip` is the door's **own LAN address** when it sends the `X-PetDoor-IP`
header, and otherwise the address the upload appeared to come from — which,
behind NAT, is your household's **public IP**. The server also writes operator
IP addresses to its own log when a dashboard control action is accepted or
rejected.

## The two parts that are genuinely sensitive

### A door log is an occupancy log

This is the one people do not think about. Every `OPEN`, `CLOSE`, `FIX_GOT` and
`FIX_LOST` row is a timestamped record of **when the thing carrying the beacon
arrived and left**. Over weeks that is a movement pattern for a household — the
sample dashboard draws exactly that, as a daily-rhythm strip, because it is
useful.

It is useful and it is sensitive, and the same chart that tells you your cat is
off its routine tells anyone else when the house is empty. Treat the database and
the dashboard as you would a door-entry log, because that is what they are. Do
not publish screenshots of a real door's activity without thinking about what the
rhythm reveals.

### Discovery scans capture other people's devices

`scan` dumps the discovery table — **every Bluetooth device the door has heard,
not just your beacon.** Those addresses belong to neighbours, visitors and
passers-by: phones, watches, earbuds, cars, and medical devices.

This is not hypothetical. **Before this repository was first published, its docs
had to be scrubbed of captured scan output that contained neighbours' device
addresses and a household medical device identifier.** Git history is public and
permanent, so a paste like that cannot be taken back by a later commit.

So:

- **Never paste raw scan output** into an issue, a pull request, a commit, a
  doc or a screenshot. Redact all but your own beacon.
- Run `scan` when you need to find a beacon address, and delete the rows
  afterwards if you do not need them.
- Modern phones randomise their Bluetooth addresses, which limits the damage.
  Beacons, tags and many wearables **do not**, and are trackable by address.

## Who can see it

- **The public page** shows the project, not any door. No per-door data.
- **The dashboard** shows everything, including the ability to move the door, so
  it must sit behind authentication — put an authenticating proxy in front of it
  and do not expose it directly. See
  [WEB-DASHBOARD.md](WEB-DASHBOARD.md) and [PORTAL.md](PORTAL.md).
- **Email.** Consequential commands and abnormal resets notify the address you
  configure. Those messages contain the door name and what happened.
- **The authors.** Nothing. There is no channel by which this project's authors
  receive your data.

## Retention, and collecting less

Rows are kept **indefinitely** until you delete them — there is no automatic
expiry. The door's own ring is bounded at 128 events and overwrites itself.

To hold less:

| | |
|---|---|
| Hold nothing centrally | leave `LOG_ENDPOINT_URL` unset |
| Stop periodic check-ins | set the heartbeat to `0` (`upload <settle> <interval> 0`) |
| Drop third-party addresses | `DELETE FROM scans;` and avoid running `scan` |
| Trim history | delete old `events` rows; nothing depends on them |
| Keep it off the internet | the door calls out, so the server does not need to be publicly reachable to work |

## If you run a server for someone else's door

You are then holding occupancy data about another household, and scan data about
their neighbours. Tell them what you keep, give them a way to have it deleted,
and do not keep it longer than it is useful to them. If you are in a jurisdiction
with data-protection law, that data is very likely in scope, and this page is not
a substitute for advice on it.

---

See also: [DISCLAIMER.md](DISCLAIMER.md) for warranty and support,
[SECURITY.md](../SECURITY.md) for vulnerability reporting, and
[SAFETY.md](SAFETY.md) for the safety analysis.
