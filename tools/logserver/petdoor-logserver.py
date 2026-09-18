#!/usr/bin/env python3
"""PetDoor log server — receives event batches from one or more doors.

Deliberately dependency-free: Python 3.8+ standard library only, one file, one
SQLite database. It is meant to run on whatever you already have — a Pi, a NAS,
an old laptop — without a package manager, a container, or a reverse proxy.

    python3 petdoor-logserver.py --init          # create the db, print a key
    python3 petdoor-logserver.py                 # run on :8080

    python3 petdoor-logserver.py --show-key      # what the door should be using
    python3 petdoor-logserver.py --rotate-key    # issue a new one

Why plain HTTP:

The door signs each upload with HMAC-SHA256 over the shared key, so the key
never crosses the wire and nobody can forge events. TLS would add a 1-3 second
handshake to every upload, and on an ESP32 that is radio time stolen from
Bluetooth scanning — the one resource the door cannot spare. If this needs to
be reachable across the internet, put it behind a VPN or a TLS-terminating
reverse proxy rather than asking the ESP32 to do TLS.
"""

import argparse
import hashlib
import hmac
import html
import http.server
import json
import os
import secrets
import socketserver
import sqlite3
import sys
import time
from datetime import datetime, timezone

DB_PATH = os.environ.get("PETDOOR_DB", "petdoor.sqlite3")
# Reject uploads whose timestamp is further than this from ours, so a captured
# batch cannot be replayed indefinitely.
CLOCK_SKEW_S = 900

EVENT_LABEL = {
    "OPEN": "came in", "CLOSE": "went out", "BOOT": "restarted",
    "REFUSED": "refused", "FIX_GOT": "beacon found", "FIX_LOST": "beacon lost",
}
RESET_REASON = {1: "power-on", 3: "software", 4: "panic", 5: "interrupt watchdog",
                6: "task watchdog", 7: "watchdog", 9: "BROWNOUT"}


# --------------------------------------------------------------------------- db
def db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    with db() as conn:
        conn.executescript("""
        CREATE TABLE IF NOT EXISTS settings (k TEXT PRIMARY KEY, v TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS events (
            device   TEXT NOT NULL,
            epoch    INTEGER NOT NULL,
            uptime   INTEGER NOT NULL,
            boot     INTEGER NOT NULL,
            type     TEXT NOT NULL,
            detail   INTEGER NOT NULL,
            rssi     INTEGER NOT NULL,
            received INTEGER NOT NULL,
            -- The door has no unique event id, so identity is the tuple that
            -- cannot repeat for one device: which boot, how far into it, what
            -- happened. Re-uploading the same ring is therefore idempotent.
            PRIMARY KEY (device, boot, uptime, type)
        );
        CREATE INDEX IF NOT EXISTS events_epoch ON events(epoch);
        """)


def get_key():
    with db() as conn:
        row = conn.execute("SELECT v FROM settings WHERE k='shared_key'").fetchone()
        return row["v"] if row else ""


def set_key(key):
    with db() as conn:
        conn.execute("INSERT INTO settings(k,v) VALUES('shared_key',?) "
                     "ON CONFLICT(k) DO UPDATE SET v=excluded.v", (key,))


def store(device, rows):
    added = 0
    now = int(time.time())
    with db() as conn:
        for r in rows:
            cur = conn.execute(
                "INSERT OR IGNORE INTO events(device,epoch,uptime,boot,type,detail,rssi,received)"
                " VALUES(?,?,?,?,?,?,?,?)",
                (device, r["epoch"], r["uptime"], r["boot"], r["type"],
                 r["detail"], r["rssi"], now))
            added += cur.rowcount
    return added


def parse_csv(text):
    rows = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("epoch,"):
            continue
        p = line.split(",")
        if len(p) < 6:
            continue
        try:
            rows.append({"epoch": int(p[0]), "uptime": int(p[1]), "boot": int(p[2]),
                         "type": p[3].strip()[:16], "detail": int(p[4]), "rssi": int(p[5])})
        except ValueError:
            continue
    return rows


# ------------------------------------------------------------------------ auth
def verify(body, timestamp, signature, key):
    """Returns (ok, reason). Mirrors what the firmware signs."""
    if not key:
        return True, "unsigned (no key configured on the server)"
    if not signature:
        return False, "server requires a signature but none was sent"
    try:
        ts = int(timestamp)
    except (TypeError, ValueError):
        return False, "missing or malformed timestamp"
    # A door that has not synced NTP sends 0; allow it through so a clockless
    # door can still deliver, but say so.
    if ts != 0 and abs(time.time() - ts) > CLOCK_SKEW_S:
        return False, f"timestamp is {int(abs(time.time() - ts))}s out — replay or bad clock"
    mac = hmac.new(key.encode(), f"{timestamp}\n".encode() + body, hashlib.sha256)
    if not hmac.compare_digest(mac.hexdigest(), signature.strip().lower()):
        return False, "signature does not match — the door's key differs from ours"
    return True, "signed" + (" (door has no clock yet)" if ts == 0 else "")


# ------------------------------------------------------------------------- web
PAGE = """<!doctype html><meta charset="utf-8"><title>PetDoor log</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{{color-scheme:light dark;--ink:#17212B;--dim:#6E7F8E;--line:#DFE5EB;
--bg:#FAFBFC;--panel:#fff;--in:#2A9D8F;--out:#E9A23B;--bad:#C4453B}}
@media(prefers-color-scheme:dark){{:root{{--ink:#E6EDF3;--dim:#8B9AA8;--line:#26313C;
--bg:#0F151B;--panel:#161E26;--in:#26937F;--out:#BE8129;--bad:#E06C60}}}}
*{{box-sizing:border-box}}
body{{margin:0;background:var(--bg);color:var(--ink);font:15px/1.5 ui-sans-serif,
-apple-system,Segoe UI,Roboto,Arial,sans-serif;padding-block:28px;padding-left:18px;
padding-right:18px}}
.wrap{{max-width:940px;margin:0 auto}}
h1{{font-size:22px;margin:0 0 2px;letter-spacing:-.02em}}
p.sub{{color:var(--dim);margin:0 0 22px;font-size:13.5px}}
.cards{{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:1px;
background:var(--line);border:1px solid var(--line);border-radius:9px;overflow:hidden;
margin-bottom:22px}}
.card{{background:var(--panel);padding:13px 15px}}
.card .k{{font-size:10.5px;text-transform:uppercase;letter-spacing:.09em;color:var(--dim);
font-weight:600}}
.card .v{{font-size:24px;font-weight:600;margin-top:2px;font-variant-numeric:tabular-nums}}
table{{width:100%;border-collapse:collapse;background:var(--panel);border:1px solid var(--line);
border-radius:9px;overflow:hidden;font-size:13.5px}}
th{{text-align:left;font-size:10.5px;text-transform:uppercase;letter-spacing:.08em;
color:var(--dim);padding:10px 13px;border-bottom:1px solid var(--line)}}
td{{padding:8px 13px;border-bottom:1px solid var(--line)}}
tr:last-child td{{border-bottom:0}}
.mono{{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-variant-numeric:tabular-nums}}
.pill{{display:inline-block;font-size:11.5px;font-weight:600;padding:2px 9px;border-radius:99px}}
.i{{background:color-mix(in srgb,var(--in) 18%,transparent);color:var(--in)}}
.o{{background:color-mix(in srgb,var(--out) 22%,transparent);color:var(--out)}}
.b{{background:color-mix(in srgb,var(--bad) 18%,transparent);color:var(--bad)}}
.s{{background:color-mix(in srgb,var(--dim) 18%,transparent);color:var(--dim)}}
a{{color:var(--in)}}
.bar{{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:22px;font-size:13px}}
</style>
<div class="wrap">
<h1>PetDoor log</h1>
<p class="sub">{sub}</p>
<div class="cards">{cards}</div>
<div class="bar"><a href="/export.csv">Download all as CSV</a>
<span style="color:var(--dim)">&mdash; drop it into the Door Log portal for charts</span></div>
<table><thead><tr><th>When</th><th>Door</th><th>Event</th><th>Detail</th><th>Signal</th></tr></thead>
<tbody>{rows}</tbody></table>
</div>"""


def render():
    with db() as conn:
        rows = conn.execute("SELECT * FROM events ORDER BY epoch DESC, received DESC "
                            "LIMIT 300").fetchall()
        total = conn.execute("SELECT COUNT(*) c FROM events").fetchone()["c"]
        devices = conn.execute("SELECT COUNT(DISTINCT device) c FROM events").fetchone()["c"]
        trips = conn.execute("SELECT COUNT(*) c FROM events WHERE type='CLOSE'").fetchone()["c"]
        brown = conn.execute("SELECT COUNT(*) c FROM events WHERE type='BOOT' AND detail=9"
                             ).fetchone()["c"]
        last = conn.execute("SELECT MAX(received) m FROM events").fetchone()["m"]

    cards = "".join(
        f'<div class="card"><div class="k">{k}</div><div class="v">{v}</div></div>'
        for k, v in [("Events", f"{total:,}"), ("Trips outside", f"{trips:,}"),
                     ("Doors", devices), ("Brownouts", brown)])

    cls = {"OPEN": "i", "CLOSE": "o", "REFUSED": "b"}
    out = []
    for r in rows:
        when = (datetime.fromtimestamp(r["epoch"], timezone.utc).strftime("%Y-%m-%d %H:%M")
                if r["epoch"] else f'boot {r["boot"]} +{r["uptime"]}s')
        detail = ""
        if r["type"] == "BOOT":
            detail = RESET_REASON.get(r["detail"], f'reset {r["detail"]}')
            if r["detail"] == 9:
                detail = f'<strong style="color:var(--bad)">{detail}</strong>'
        elif r["type"] == "REFUSED":
            detail = {1: "already there", 2: "too soon", 3: "boot grace"}.get(
                r["detail"], f'reason {r["detail"]}')
        out.append(
            f'<tr><td class="mono">{html.escape(when)}</td>'
            f'<td style="color:var(--dim)">{html.escape(r["device"])}</td>'
            f'<td><span class="pill {cls.get(r["type"], "s")}">'
            f'{html.escape(EVENT_LABEL.get(r["type"], r["type"]))}</span></td>'
            f'<td style="color:var(--dim)">{detail}</td>'
            f'<td class="mono" style="color:var(--dim)">'
            f'{str(r["rssi"]) + " dBm" if r["rssi"] else ""}</td></tr>')

    ago = f"last upload {int((time.time() - last) / 60)} min ago" if last else "nothing received yet"
    sub = f"{ago} &middot; showing newest {len(rows)} of {total:,}"
    return PAGE.format(sub=sub, cards=cards,
                       rows="".join(out) or '<tr><td colspan="5" style="padding:30px;'
                                            'text-align:center;color:var(--dim)">'
                                            'No events yet. Point a door at this server.</td></tr>')


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "PetDoorLog/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s  %s\n" % (self.log_date_time_string(), fmt % args))

    def _send(self, code, body, ctype="text/plain; charset=utf-8"):
        body = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            page = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboard.html")
            if os.path.exists(page):
                with open(page, "rb") as fh:
                    return self._send(200, fh.read(), "text/html; charset=utf-8")
            return self._send(200, render(), "text/html; charset=utf-8")
        if self.path.startswith("/api/events"):
            with db() as conn:
                rows = conn.execute("SELECT device,epoch,uptime,boot,type,detail,rssi "
                                    "FROM events ORDER BY epoch, boot, uptime").fetchall()
            return self._send(200, json.dumps({"events": [dict(r) for r in rows]}),
                              "application/json; charset=utf-8")
        if self.path.startswith("/table"):
            return self._send(200, render(), "text/html; charset=utf-8")
        if self.path.startswith("/export.csv"):
            with db() as conn:
                rows = conn.execute("SELECT * FROM events ORDER BY epoch, boot, uptime").fetchall()
            csv = "epoch,uptime_s,boot,event,detail,rssi\n" + "".join(
                f'{r["epoch"]},{r["uptime"]},{r["boot"]},{r["type"]},{r["detail"]},{r["rssi"]}\n'
                for r in rows)
            return self._send(200, csv, "text/csv; charset=utf-8")
        if self.path == "/health":
            return self._send(200, json.dumps({"ok": True}), "application/json")
        self._send(404, "not found")

    def do_POST(self):
        if not self.path.startswith("/ingest"):
            return self._send(404, "not found")
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0 or length > 1_000_000:
            return self._send(400, "empty or oversized body")
        body = self.rfile.read(length)

        ok, reason = verify(body, self.headers.get("X-PetDoor-Timestamp"),
                            self.headers.get("X-PetDoor-Signature"), get_key())
        if not ok:
            self.log_message("REJECTED from %s: %s", self.client_address[0], reason)
            return self._send(401, f"rejected: {reason}\n")

        device = (self.headers.get("X-PetDoor-Id") or "petdoor")[:32]
        rows = parse_csv(body.decode("utf-8", "replace"))
        added = store(device, rows)
        self.log_message("%s: %d events, %d new (%s)", device, len(rows), added, reason)
        return self._send(200, json.dumps({"received": len(rows), "new": added}) + "\n",
                          "application/json")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description="PetDoor log server")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--init", action="store_true", help="create the database and a key")
    ap.add_argument("--show-key", action="store_true", help="print the key the door must use")
    ap.add_argument("--rotate-key", action="store_true", help="issue a new key")
    ap.add_argument("--set-key", metavar="KEY", help="set a specific key")
    ap.add_argument("--no-key", action="store_true", help="accept unsigned uploads")
    args = ap.parse_args()

    init_db()

    if args.init:
        if not get_key():
            set_key(secrets.token_hex(16))
        print(f"database : {os.path.abspath(DB_PATH)}")
        print(f"key      : {get_key()}")
        print("\nPut this in petdoor/secrets.h:")
        print(f'  #define LOG_SHARED_KEY "{get_key()}"')
        return
    if args.show_key:
        k = get_key()
        print(k if k else "(no key set — unsigned uploads are accepted)")
        return
    if args.rotate_key or args.set_key or args.no_key:
        new = "" if args.no_key else (args.set_key or secrets.token_hex(16))
        set_key(new)
        if new:
            print(f"new key: {new}\n\nUpdate petdoor/secrets.h and reflash, or the door's "
                  "uploads will start being rejected with HTTP 401:")
            print(f'  #define LOG_SHARED_KEY "{new}"')
        else:
            print("key cleared — unsigned uploads will now be accepted")
        return

    if not get_key():
        print("WARNING: no shared key set. Anyone who can reach this port can post events.")
        print("         Run with --init to generate one.\n")
    print(f"PetDoor log server on http://{args.host}:{args.port}")
    print(f"  dashboard : http://{args.host}:{args.port}/")
    print(f"  ingest    : POST http://{args.host}:{args.port}/ingest")
    print(f"  api       : http://{args.host}:{args.port}/api/events")
    print(f"  export    : http://{args.host}:{args.port}/export.csv")
    print(f"  plain     : http://{args.host}:{args.port}/table")
    with Server((args.host, args.port), Handler) as srv:
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped")


if __name__ == "__main__":
    main()
