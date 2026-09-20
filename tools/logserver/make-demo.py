#!/usr/bin/env python3
"""Build demo.html — the public sample dashboard — from dashboard.html.

The sample page must show what the real dashboard shows, so it is generated
FROM the real one rather than maintained beside it. Run this after any change
to dashboard.html or the two will drift.

The data is synthetic and generated here. It is never a copy of a real log:
this page is public, and the reason the live analytics sit behind a login is
that a door log says when a house is empty.

    ./make-demo.py            # writes demo.html next to this script
"""
import datetime as dt
import json
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SEED = 11                      # fixed, so the sample is reproducible


def synthesise(days=21):
    """A plausible dog: out five or six times a day, mostly brief."""
    random.seed(SEED)
    now = dt.datetime.now()
    start = (now - dt.timedelta(days=days)).replace(hour=0, minute=0, second=0, microsecond=0)
    ev = [(start + dt.timedelta(hours=5), "BOOT", 0)]

    for d in range(days + 1):
        day = start + dt.timedelta(days=d)
        if day.date() > now.date():
            break
        weekend = day.weekday() >= 5
        slots = [8, 11, 15, 19, 22] if weekend else [6.5, 10, 14.5, 18.5, 21.5]
        if random.random() < 0.5:
            slots.append(random.choice([12.5, 16.5]))
        cursor = None
        for h in sorted(x + random.gauss(0, 0.4) for x in slots):
            if random.random() < 0.08:
                continue
            out = day + dt.timedelta(hours=max(0.2, min(23.0, h)))
            # Outings must not overlap, or they interleave once sorted and the
            # close-parity that defines a trip slips.
            if cursor and out < cursor:
                out = cursor + dt.timedelta(minutes=random.randint(10, 30))
            mins = (random.choice([3, 4, 5, 6, 7, 8, 9, 12, 15, 18])
                    if random.random() < 0.82 else random.choice([28, 35, 44, 52]))
            back = out + dt.timedelta(minutes=mins, seconds=random.randint(0, 59))
            if back > now - dt.timedelta(minutes=10) or back.date() != day.date():
                continue
            ev += [(out, "OPEN", random.randint(-58, -42)),
                   (out + dt.timedelta(seconds=random.randint(25, 70)), "CLOSE", random.randint(-80, -69)),
                   (back - dt.timedelta(seconds=random.randint(5, 20)), "OPEN", random.randint(-56, -44)),
                   (back + dt.timedelta(seconds=random.randint(25, 70)), "CLOSE", random.randint(-82, -70))]
            cursor = back + dt.timedelta(minutes=2)
        for _ in range(random.randint(0, 2)):       # brief fix losses, as real doors show
            t = day + dt.timedelta(hours=random.uniform(1, 23))
            if t > now - dt.timedelta(minutes=10):
                continue
            ev += [(t, "FIX_LOST", random.randint(-95, -78)),
                   (t + dt.timedelta(seconds=random.randint(1, 4)), "FIX_GOT", random.randint(-88, -70))]

    ev.sort(key=lambda e: e[0])
    rows, uptime = [], 0
    for when, typ, rssi in ev:
        uptime += random.randint(20, 900)
        rows.append({"device": "petdoor-sample", "epoch": int(when.timestamp()),
                     "uptime": uptime, "boot": 1, "type": typ,
                     "detail": 1 if typ == "BOOT" else 0, "rssi": rssi})
    newest = max(r["epoch"] for r in rows)
    devices = [{"device": "petdoor-sample", "version": "1.0.0",
                "build": "Sep 20 2026 00:26:25", "boots": 1,
                "last_seen": newest, "last_ip": ""}]
    return {"events": rows, "devices": devices}


BANNER = """
/* --- sample-page chrome --- */
.sample-chip{font-size:12px;font-weight:600;vertical-align:middle;margin-left:10px;
  padding:3px 9px;border-radius:999px;background:var(--out-soft);color:var(--out);
  border:1px solid var(--out)}
.sample-bar{background:var(--out-soft);border-bottom:1px solid var(--out);
  padding:11px 18px;font-size:14px;color:var(--ink);text-align:center}
.sample-bar a{color:var(--ink);font-weight:600}
</style>
<div class="sample-bar">
  <b>This is a demonstration.</b> Every figure below comes from synthetic data &mdash;
  no real household appears here.
  <a href="/dashboard">Sign in to the live door &rarr;</a> &middot;
  <a href="/">About PetDoor</a>
</div>"""


def main():
    src_path = os.path.join(HERE, "dashboard.html")
    with open(src_path) as fh:
        src = fh.read()

    def rep(old, new, why):
        nonlocal src
        if src.count(old) != 1:
            sys.exit(f"make-demo: {why}: expected 1 match, found {src.count(old)}.\n"
                     f"dashboard.html has changed shape — update this script.")
        src = src.replace(old, new)

    sample = json.dumps(synthesise(), separators=(",", ":"))

    rep('    const r=await fetch("/api/events",{cache:"no-store"});\n'
        '    if(!r.ok) throw new Error("HTTP "+r.status);\n'
        '    const j=await r.json();',
        '    /* SAMPLE PAGE: synthetic data, baked in. This page never calls\n'
        '       /api/events, which is the private route, so it can be public. */\n'
        '    const j=window.__PETDOOR_SAMPLE__;', "data loader")
    rep('/* ---- data comes from the log server ---- */',
        f'window.__PETDOOR_SAMPLE__={sample};\n'
        '/* ---- data comes from the log server ---- */', "data injection point")
    rep("<title>PetDoor Dashboard</title>",
        "<title>PetDoor Dashboard &mdash; sample</title>", "title")
    # dashboard.html is a fragment with no <head>/<body>, so hang the extra
    # styling and the banner off the end of its stylesheet.
    rep("</style>", BANNER, "stylesheet end")
    rep("<h1>Door Log</h1>",
        '<h1>Door Log <span class="sample-chip">sample</span></h1>', "heading")

    out = os.path.join(HERE, "demo.html")
    with open(out, "w") as fh:
        fh.write(src)
    print(f"wrote {out} ({len(src)/1024:.0f} KB, {len(sample)/1024:.0f} KB of sample data)")


if __name__ == "__main__":
    main()
