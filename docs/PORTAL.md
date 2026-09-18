# The Door Log portal

**Optional, and separate from the door.** The firmware does not know this
exists, never talks to it, and works exactly the same whether you use it or not.
It is a web page that reads the CSV the door produces and draws charts.

👉 **[Open the portal](https://claude.ai/artifact/4nCm5qpPA37pMQSGpmnhzE)**

---

## What it is for

The serial console answers "what happened last night". The portal answers the
questions that need weeks of data:

- How long does he actually stay out, and is that changing?
- What time does he go out, and has his routine shifted?
- Is the beacon battery starting to go?
- Did that brownout happen at the same time as the door misbehaving?

## Getting data into it

Any of these — it takes the same CSV in every case:

| Source | How |
|---|---|
| **Straight from the door** | `L` in the serial console, copy the output, paste it in |
| **From a log server** | Download `GET /export.csv`, drop the file in |
| **A captured session** | `screen -L -Logfile petdoor.csv …`, drop that in |

Imports **merge and de-duplicate**, so you can keep dropping files in as you
collect them. That matters because the door only remembers its last 128 events —
the portal is where history accumulates beyond what the hardware can hold.

## What it shows

**Daily rhythm** — one row per day, midnight to midnight, with night hours
shaded. Amber marks going out, teal coming in, and the band between them is time
spent outside. Brownouts appear as red bars on the same timeline, so a power
problem sits next to the behaviour it disrupted.

**How long he stays out** — trip durations in bands. It reports the **median,
not the average**, and says so: one long outing drags a mean somewhere no real
trip lives.

**Trips per day** and **time of day** — is today unusual, and what does normal
look like.

**Beacon signal** — RSSI at each *return*, so every reading is taken from
roughly the same place and is comparable over weeks. A sustained drop of 6 dBm
or more is an early warning of a flat battery, and the page says so in words
rather than leaving you to read the slope.

That last one is worth knowing about: it works from data you are already
collecting, and unlike the firmware's battery reporting it does not require your
beacon to broadcast Eddystone-TLM telemetry — which many, including the
reference Minew, do not.

## Cameras

The portal has two frames, outside and inside, and clicking **Footage** on any
event pins that event's exact UTC timestamp into both.

**They are not connected to anything yet, deliberately.** Blink and Wyze both
keep clips in their own cloud behind APIs that are unofficial and change without
notice, so a web page cannot reliably pull footage from either. What works
today is the timestamp — enough to scrub to the right moment in whichever app
you already use.

What would work properly is putting the cameras somewhere with an open
interface: an RTSP bridge such as `docker-wyze-bridge`, or any NVR that records
to files, and then pointing those two frames at the resulting clip URLs.

## Privacy

Everything you import is stored in the artifact itself and is not sent anywhere
else. The page makes no outbound requests with your data.

If you would rather nothing left your machine at all, the same CSV opens in a
spreadsheet — the columns are `epoch,uptime_s,boot,event,detail,rssi` and are
documented in [DIAGNOSTICS.md](DIAGNOSTICS.md#l--the-event-log).
