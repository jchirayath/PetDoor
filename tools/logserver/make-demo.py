#!/usr/bin/env python3
"""Build the public sample dashboard — demo.html, demo.css, demo.js — from the
real dashboard's three files.

The sample must show what the real dashboard shows, so it is GENERATED from the
real one rather than maintained beside it. Run it after any change to
dashboard.html, dashboard.css or dashboard.js.

The data is synthetic and generated here. It is never a copy of a real log:
this page is public, and the reason the live analytics sit behind a login is
that a door log says when a house is empty.

    ./make-demo.py            # writes demo.{html,css,js} next to this script

THREE FILES, because of the Content-Security-Policy. This script used to patch
one self-contained dashboard.html, and it broke the day the <style> and
<script> blocks were lifted out into dashboard.css/js so the CSP could ENFORCE
rather than merely report: every anchor it replaced lived in the extracted
code, so it exited with "dashboard.html has changed shape" and refused to run.
Nobody noticed, because the only thing that calls it is a person.

That is not a cosmetic failure — IT IS THE DRIFT THE SCRIPT EXISTS TO PREVENT,
and it ran for months. By the time this was rewritten the hand-maintained demo
was missing eighteen functions the dashboard had grown, had no tab bar at all,
and still called its heading "Door Log" — a name the dashboard had dropped.
So the rule is worth restating: NEVER hand-edit demo.html, demo.css or
demo.js. Edit the dashboard and re-run this.

WHY THE SAMPLE SHOWS NO CONTROL PANEL. The synthetic payload has no "control"
key, so the page sees web control as off — exactly as a real server does
without --allow-web-control. The Settings tab hides itself, and the Controls
tab keeps only its log, which is the half that reads correctly without a door
behind it. That is deliberate: a public page offering Open, Close and Lock
would be a page that looks like it can drive somebody else's door. For the
same reason the payload carries no "notify" block, so the sample shows nothing
about email alerts or quiet periods — those controls live inside the panel that
is off. If the demo should ever show them, it needs a read-only variant of the
panel, not a `"control": True` in the sample.
"""
import datetime as dt
import json
import os
import random
import re
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
    devices = [{"device": "petdoor-sample", "version": "1.1.0",
                "build": "Sep 20 2026 17:30:00", "boots": 1,
                "last_seen": newest, "last_ip": "",
                # What the door reports about itself on every upload.
                "status": ("rssi=-61 raw=-63 dist=1.2 present=0 door=CLOSED "
                           "gap=1840 samples=48210 adv=2104883 weak=61 "
                           "heap=141208 up=186420"),
                "door_ip": "192.168.1.57",
                "remote": 1}]
    # A few commands, so the logs show what they are for rather than an empty
    # state. Same shape the server returns: delivered, with the door's own
    # verdict — including one it refused, because that is the interesting case
    # and a demo that only shows successes teaches the wrong lesson.
    #
    # BOTH KINDS, deliberately. renderChanges() splits them by verb: control
    # verbs into the Controls log, everything else into the Settings log. With
    # only settings here the Controls tab — the one tab besides Activity that
    # the sample shows at all — rendered "Nothing has been asked of the door
    # yet", which is a poor advertisement for the half of the page that exists
    # to answer "who moved my door".
    day = 86400
    commands = [
        {"device": "petdoor-sample", "command": "door open",
         "queued": newest - 5 * day, "delivered": newest - 5 * day + 95,
         "ack": "1 applied"},
        {"device": "petdoor-sample", "command": "lock",
         "queued": newest - 3 * day, "delivered": newest - 3 * day + 260,
         "ack": "1 applied"},
        {"device": "petdoor-sample", "command": "unlock",
         "queued": newest - 3 * day + 7200, "delivered": newest - 3 * day + 7310,
         "ack": "1 applied"},
        {"device": "petdoor-sample", "command": "thresholds -58 -68",
         "queued": newest - 6 * day, "delivered": newest - 6 * day + 240,
         "ack": "1 applied"},
        {"device": "petdoor-sample", "command": "pulse 500",
         "queued": newest - 4 * day, "delivered": newest - 4 * day + 180,
         "ack": "1 applied"},
        {"device": "petdoor-sample", "command": "openfilter 11 0.2",
         "queued": newest - 2 * day, "delivered": newest - 2 * day + 300,
         "ack": "0 applied, 1 refused: open filter rejected: must not be "
                "slower than the close filter"},
        {"device": "petdoor-sample", "command": "dwell 1500 12000 5000",
         "queued": newest - day, "delivered": newest - day + 210,
         "ack": "1 applied"},
    ]
    return {"events": rows, "devices": devices, "commands": commands}


# The sample page's own chrome: a chip beside the heading and a bar above the
# page saying what this is. Split in two now that the page's styling lives in a
# stylesheet — the CSS is appended to demo.css, the markup goes into demo.html.
SAMPLE_CSS = """
/* --- sample-page chrome: added by make-demo.py, not by hand --- */
.sample-chip{font-size:12px;font-weight:600;vertical-align:middle;margin-left:10px;
  padding:3px 9px;border-radius:999px;background:var(--out-soft);color:var(--out);
  border:1px solid var(--out)}
.sample-bar{background:var(--out-soft);border-bottom:1px solid var(--out);
  padding:11px 18px;font-size:14px;color:var(--ink);text-align:center}
.sample-bar a{color:var(--ink);font-weight:600}
"""

SAMPLE_BAR = """<div class="sample-bar">
  <b>This is a demonstration.</b> Every figure below comes from synthetic data &mdash;
  no real household appears here. The dashboard is an <b>optional separate module</b>
  you run yourself; the door works without it.
  <a href="/dashboard">Sign in to the live door &rarr;</a> &middot;
  <a href="/">About PetDoor</a>
</div>"""

# The generated files say so, in case somebody opens one before this script.
STAMP = ("/* GENERATED by make-demo.py from %s. Do not edit: your changes are\n"
         "   lost the next time anyone regenerates the sample page. Edit %s. */\n")


def patcher(src, name):
    """A replace that REFUSES to half-work.

    Every edit here is anchored on text in a file this script does not own, so
    an anchor that stops matching must stop the build rather than silently
    produce a sample page missing a piece. That is the failure this script had:
    it raised its voice only when run, and nothing runs it but a person.
    """
    def rep(old, new, why):
        nonlocal src
        if src.count(old) != 1:
            sys.exit(f"make-demo: {why}: expected 1 match in {name}, "
                     f"found {src.count(old)}.\n"
                     f"{name} has changed shape — update this script.")
        src = src.replace(old, new)

    def get():
        return src

    return rep, get


def build():
    """The three sample files, as text. Writes nothing."""
    def read(name):
        with open(os.path.join(HERE, name)) as fh:
            return fh.read()

    html, css, js = read("dashboard.html"), read("dashboard.css"), read("dashboard.js")
    sample = json.dumps(synthesise(), separators=(",", ":"))

    # ---- the page ---------------------------------------------------------
    rep, get_html = patcher(html, "dashboard.html")
    rep("<title>PetDoor Dashboard</title>",
        "<title>PetDoor Dashboard &mdash; sample</title>", "title")
    rep('<link rel="stylesheet" href="/dashboard.css">',
        '<link rel="stylesheet" href="/demo.css">', "stylesheet link")
    rep('<script src="/dashboard.js"></script>',
        '<script src="/demo.js"></script>', "script link")
    # Which page the site nav calls the current one moves from the private
    # dashboard to this public sample, and the dashboard link becomes an
    # invitation to sign in rather than a claim that you already are.
    rep('<a href="/demo"><span class="wide-only">Sample dashboard</span>',
        '<a href="/demo" aria-current="page"><span class="wide-only">Sample dashboard</span>',
        "nav: the sample is the current page")
    rep('<a aria-current="page" href="/dashboard">Your door</a>',
        '<a class="go" href="/dashboard">Sign in</a>', "nav: sign-in link")
    rep("<h1>PetDoor</h1>",
        '<h1>PetDoor <span class="sample-chip">sample</span></h1>', "heading")
    # Between the site nav and the page itself: the first thing read, which is
    # where "none of this is a real household" belongs.
    rep('</nav>\n\n<div class="wrap">', "</nav>\n\n" + SAMPLE_BAR + '\n\n<div class="wrap">',
        "the sample bar, above the page")

    # ---- the script -------------------------------------------------------
    rep, get_js = patcher(js, "dashboard.js")
    rep('    const r=await fetch("/api/events",{cache:"no-store"});\n'
        '    if(!r.ok) throw new Error("HTTP "+r.status);\n'
        '    const j=await r.json();',
        '    /* SAMPLE PAGE: synthetic data, baked in. This page never calls\n'
        '       /api/events, which is the private route, so it can be public. */\n'
        '    const j=window.__PETDOOR_SAMPLE__;', "data loader")
    rep('/* ---- data comes from the log server ---- */',
        f'window.__PETDOOR_SAMPLE__={sample};\n'
        '/* ---- data comes from the log server ---- */', "data injection point")

    html_stamp = ("<!-- GENERATED by make-demo.py from dashboard.html. Do not edit:\n"
                  "     your changes are lost the next time anyone regenerates the\n"
                  "     sample page. Edit dashboard.html. -->\n")

    return {"demo.html": html_stamp + get_html(),
            "demo.css": STAMP % ("dashboard.css", "dashboard.css") + css + SAMPLE_CSS,
            "demo.js": STAMP % ("dashboard.js", "dashboard.js") + get_js()}, len(sample)


# The synthetic log is dated from "now" and reshuffled on every run, so two
# builds never produce byte-identical files. Everything OUTSIDE that one line
# is deterministic, and that is the part the drift check compares.
SAMPLE_LINE = re.compile(r"^window\.__PETDOOR_SAMPLE__=.*$", re.M)


def _comparable(text):
    return SAMPLE_LINE.sub("window.__PETDOOR_SAMPLE__=<synthetic data>;", text)


def check():
    """Are the committed sample files what this script would produce?

    Run from tests/server/run.sh, because the drift this catches is invisible
    in every other way. The generator broke when the page's CSS and JS were
    extracted for the Content-Security-Policy; nothing calls it but a person,
    so for months it exited with "dashboard.html has changed shape" and the
    sample page quietly aged — eighteen functions behind the dashboard, no tab
    bar, and a heading naming a page that had been renamed. A check that runs
    in CI is the difference between fixed and fixed-for-good.
    """
    built, _ = build()
    stale = []
    for name, body in built.items():
        path = os.path.join(HERE, name)
        try:
            with open(path) as fh:
                have = fh.read()
        except OSError as exc:
            stale.append(f"{name}: cannot be read ({exc})")
            continue
        if _comparable(have) != _comparable(body):
            stale.append(name)
    if stale:
        print("  FAIL  the sample page is out of step with the dashboard: "
              + ", ".join(stale))
        print("        run tools/logserver/make-demo.py and commit the result.")
        return 1
    print("  ok    demo.{html,css,js} match what make-demo.py builds "
          "from dashboard.{html,css,js}")
    return 0


def main():
    if "--check" in sys.argv[1:]:
        return check()
    built, sample_bytes = build()
    for name, body in built.items():
        with open(os.path.join(HERE, name), "w") as fh:
            fh.write(body)
        print(f"wrote {name} ({len(body)/1024:.0f} KB)")
    print(f"      including {sample_bytes/1024:.0f} KB of synthetic data")
    return 0


if __name__ == "__main__":
    sys.exit(main())
