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
import datetime as dt
from urllib.parse import urlparse, parse_qs
import hashlib
import hmac
import html
import http.server
import json
import os
import re
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

# Whether the private dashboard may queue commands. OFF by default, and that
# default is deliberate: an existing install whose private routes are protected
# by nothing but an unguessable hostname must not silently acquire a button
# that opens the door. Turning it on is a decision, taken once, by someone who
# has read docs/WEB-DASHBOARD.md and knows what is in front of these routes.
#
# Environment variable as well as a flag, because the reference deployment runs
# this in a container where the command line lives in a compose file.
ALLOW_WEB_CONTROL = os.environ.get("PETDOOR_WEB_CONTROL", "").lower() in ("1", "true", "yes", "on")

# ---------------------------------------------------------------- email
#
# Who to tell when something consequential is asked of the door. Empty
# disables the whole thing, which is the default: a log server that starts
# mailing people because it was upgraded would be a rude surprise.
#
# The SMTP settings deliberately share the names betty already uses for its
# other services, so this reuses one set of credentials rather than adding a
# second copy of the same secret to go stale independently.
NOTIFY_TO = os.environ.get("PETDOOR_NOTIFY_TO", "").strip()
SMTP_HOST = os.environ.get("SMTP_HOST", "").strip()
SMTP_PORT = int(os.environ.get("SMTP_PORT", "587") or 587)
SMTP_USER = os.environ.get("SMTP_USER", "").strip()
SMTP_PASSWORD = os.environ.get("SMTP_PASSWORD", "")
SMTP_FROM = os.environ.get("SMTP_FROM", "").strip()
SMTP_CRYPTO = os.environ.get("SMTP_CRYPTO", "tls").strip().lower()

# Which commands are worth an email.
#
# NOT everything. A tuning session is a dozen commands in a minute, and a
# mailbox that fills with "pulse 500" is one whose PetDoor mail gets filtered
# into a folder nobody opens — at which point the alert that mattered is lost
# with the rest. These are the ones with a consequence you would want to know
# about from somewhere else:
#
#   door open   the door is now open and the weather is coming in
#   unlock      the collar can open it again
#   lock        it cannot, which explains an animal outside
#   defaults    every tuning value you set is gone
#   macs        the beacon list changed and the door is restarting
#   reboot      it went away for a minute
#   ota         a window opened for somebody to push firmware
#
# Settings changes are deliberately absent. They are recorded on the Settings
# tab, they are reversible, and none of them is a thing happening AT the door.
NOTIFY_VERBS = ("door", "lock", "unlock", "defaults", "macs", "reboot", "ota")
NOTIFY_DOOR_ARGS = ("open",)   # `door close`/`auto` are the safe direction

EVENT_LABEL = {
    "OPEN": "came in", "CLOSE": "went out", "BOOT": "restarted",
    "REFUSED": "refused", "FIX_GOT": "beacon found", "FIX_LOST": "beacon lost",
    "STALLED": "did not complete its travel",
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
        -- One row per door: what it is running and when it last called in.
        -- Reboots are the number the door itself counts, so a climbing value
        -- between uploads is a restart loop visible without a serial cable.
        -- Commands queued for a door, delivered in the reply to its next
        -- upload. The door polls us; we never connect to it, which is what
        -- makes this work through NAT and without opening a port.
        CREATE TABLE IF NOT EXISTS commands (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            device    TEXT NOT NULL,
            command   TEXT NOT NULL,
            queued    INTEGER NOT NULL,
            delivered INTEGER,
            ack       TEXT
        );
        CREATE INDEX IF NOT EXISTS commands_pending
            ON commands(device, delivered);
        -- Discovery-table dumps, uploaded on request. This is how you find a
        -- new beacon's address on a door you cannot plug into.
        CREATE TABLE IF NOT EXISTS scans (
            device TEXT NOT NULL,
            epoch  INTEGER NOT NULL,
            text   TEXT NOT NULL
        );
        CREATE INDEX IF NOT EXISTS scans_device ON scans(device, epoch);
        -- Every distinct firmware a door has run, and when it first said so.
        --
        -- devices.version is overwritten on every upload, so without this the
        -- moment a door changed firmware left no trace at all — which is the
        -- one fact you want when behaviour changes on a given afternoon and
        -- you are trying to work out whether you caused it.
        CREATE TABLE IF NOT EXISTS firmware (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            device    TEXT NOT NULL,
            version   TEXT,
            build     TEXT,
            first_seen INTEGER NOT NULL,
            boots     INTEGER,
            -- The commit it was built from, when the build said so. A version
            -- string moves on release days and a build timestamp only means
            -- something on the machine that produced it; this is the only one
            -- anybody else can check out.
            git       TEXT,
            -- How it arrived, as far as we can tell: an OTA window was open
            -- shortly before, or it simply appeared (a cable).
            via       TEXT
        );
        CREATE INDEX IF NOT EXISTS firmware_device ON firmware(device, first_seen);
        CREATE TABLE IF NOT EXISTS devices (
            device   TEXT PRIMARY KEY,
            version  TEXT,
            build    TEXT,
            boots    INTEGER,
            last_seen INTEGER,
            last_ip  TEXT
        );
        """)


# Commands the door refuses to take from us, listed here so the server never
# even offers them. The door enforces this itself — it has to, since anyone
# could run a server — but sending one would be a bug worth catching early.
#
# Each of these is part of the channel this command runs over. Change the WiFi
# credentials, the endpoint, the key or the OTA password remotely and the door
# stops being reachable, permanently, with no way back but physical access.
FORBIDDEN = ("wifi", "ssid", "endpoint", "key", "otapass", "password")

VALID_VERBS = ("ota", "thresholds", "dwell", "gap", "pulse", "filter",
               "openfilter", "macs", "door", "resetstats", "defaults",
               "scan", "reboot", "lock", "unlock", "presses",
               "travel", "buzzer", "beep", "sensors", "upload")

# ---------------------------------------------------------------- web control
#
# What the dashboard may send, and how each argument is checked.
#
# The firmware validates all of this again — every remote command calls the
# same setter the serial console calls, which is the guarantee that matters,
# since anyone could run a server. These checks exist for a different reason:
# a value rejected here is rejected NOW, with a message naming the range, and a
# value rejected by the door is rejected in five minutes' time when it next
# calls in. That difference is the whole experience of tuning a door remotely.
#
# Ranges mirror the constants in petdoor/door.h, proximity.h and config.h. If
# they ever drift, the door is still the authority and simply refuses.


def _whole(lo, hi, unit=""):
    def check(tok):
        try:
            v = int(tok)
        except ValueError:
            return None, f"'{tok}' is not a whole number"
        if not lo <= v <= hi:
            return None, f"{v}{unit} is outside {lo}-{hi}{unit}"
        return v, ""
    return check


def _odd(lo, hi):
    def check(tok):
        v, err = _whole(lo, hi)(tok)
        if err:
            return None, err
        if v % 2 == 0:
            return None, f"{v} must be odd — an even median window has no middle value"
        return v, ""
    return check


def _frac(lo, hi):
    def check(tok):
        try:
            v = float(tok)
        except ValueError:
            return None, f"'{tok}' is not a number"
        if not lo <= v <= hi:
            return None, f"{v} is outside {lo}-{hi}"
        return v, ""
    return check


def _word(*allowed):
    def check(tok):
        if tok.lower() not in allowed:
            return None, f"'{tok}' must be one of: {', '.join(allowed)}"
        return tok.lower(), ""
    return check


_MAC = re.compile(r"^[0-9a-f]{2}(:[0-9a-f]{2}){5}$")


def _mac_list(tok):
    macs = [m.strip() for m in tok.split(",") if m.strip()]
    if not macs:
        return None, "give at least one MAC address"
    if len(macs) > 8:
        return None, f"{len(macs)} addresses is more than the door tracks (8)"
    for m in macs:
        if not _MAC.match(m):
            return None, f"'{m}' is not a MAC address (aa:bb:cc:dd:ee:ff, lower case)"
    return ",".join(macs), ""


def _enter_above_exit(v):
    return "" if v[0] > v[1] else "the open threshold must be ABOVE the close threshold"


def _lockout_below_close(v):
    return "" if v[2] < v[1] else "the minimum interval must be BELOW the close dwell"


# verb -> (validators, minimum argument count, cross-field check, needs confirming)
WEB_COMMANDS = {
    # --- actions -----------------------------------------------------------
    "door":       ([_word("open", "close", "auto")], 1, None, False),
    "lock":       ([], 0, None, False),
    "unlock":     ([], 0, None, False),
    "beep":       ([], 0, None, False),
    "scan":       ([], 0, None, False),
    "resetstats": ([], 0, None, False),

    # --- detection ---------------------------------------------------------
    "thresholds": ([_whole(-120, 0, " dBm"), _whole(-120, 0, " dBm")], 2,
                   _enter_above_exit, False),
    "filter":     ([_odd(1, 15), _frac(0.01, 1.0)], 2, None, False),
    "openfilter": ([_odd(1, 15), _frac(0.01, 1.0)], 2, None, False),

    # --- timing ------------------------------------------------------------
    "dwell":      ([_whole(100, 600000, " ms")] * 3, 3, _lockout_below_close, False),
    "travel":     ([_whole(0, 120000, " ms")], 1, None, False),

    # --- the relay ---------------------------------------------------------
    "pulse":      ([_whole(50, 10000, " ms")], 1, None, False),
    "gap":        ([_whole(100, 5000, " ms")], 1, None, False),
    "presses":    ([_whole(1, 3), _whole(200, 5000, " ms")], 1, None, False),

    # --- the buzzer --------------------------------------------------------
    # Pin first, then any of passive/active/low/high. "off" is handled before
    # the validators run, since it is a word where a number belongs.
    "buzzer":     ([_whole(-1, 48), _word("passive", "active", "low", "high"),
                    _word("passive", "active", "low", "high")], 1, None, False),

    # --- the limit switches ------------------------------------------------
    # Both pins, then an optional polarity. -1 for an end with no switch, so
    # one can be fitted before the other. "sensors off" disables both.
    #
    # Shipping this before the hardware exists is deliberate: the firmware
    # defaults to -1 and behaves exactly as it did without sensors, so the
    # switches can be wired and turned on from here without another flash.
    "sensors":    ([_whole(-1, 48), _whole(-1, 48), _word("low", "high")], 2,
                   None, False),

    # --- how often the door calls in --------------------------------------
    # settle / minimum interval / heartbeat, all ms. The interval floor is the
    # load-bearing one: WiFi and BLE share an antenna, so a short interval
    # keeps the radio up and starves the beacon scan the door exists to do.
    # Heartbeat 0 turns it off, which is allowed but means a door whose animal
    # stays indoors goes silent and collects no commands.
    "upload":     ([_whole(5000, 300000, " ms"), _whole(60000, 3600000, " ms"),
                    _whole(0, 21600000, " ms")], 3,
                   lambda v: "" if v[1] > v[0] else
                             "the interval must exceed the settle time, or the gate never opens",
                   False),

    # --- things that need you to mean it -----------------------------------
    # Not because they are dangerous to the household, but because each one
    # either loses state or takes the door off the air for a minute, and a
    # thumb on a phone is a low bar for that.
    "macs":       ([_mac_list], 1, None, True),
    "defaults":   ([], 0, None, True),
    "reboot":     ([], 0, None, True),
    "ota":        ([], 0, None, True),
}


def web_command_needs_confirm(command):
    parts = command.split()
    spec = WEB_COMMANDS.get(parts[0].lower()) if parts else None
    return bool(spec and spec[3])


def web_command_allowed(command):
    """The web panel's allowlist and argument check. Returns (ok, reason).

    Applied BEFORE queue_command()'s own checks, never instead of them.
    """
    parts = command.split()
    if not parts:
        return False, "empty command"
    verb = parts[0].lower()
    if verb not in WEB_COMMANDS:
        return False, f"'{verb}' is not a setting this panel can change"

    validators, need, cross, _ = WEB_COMMANDS[verb]
    args = parts[1:]

    # "buzzer off" disables it; there is no pin to range-check.
    if verb == "buzzer" and args and args[0].lower() in ("off", "none"):
        return (True, "") if len(args) == 1 else (False, "'buzzer off' takes nothing else")

    # Same for the switches.
    if verb == "sensors" and args and args[0].lower() in ("off", "none"):
        return (True, "") if len(args) == 1 else (False, "'sensors off' takes nothing else")

    # macs takes the rest of the line as one comma-separated value.
    if verb == "macs":
        args = [" ".join(args).replace(" ", "")] if args else []

    if len(args) < need:
        return False, (f"'{verb}' needs {need} value{'' if need == 1 else 's'}, "
                       f"got {len(args)}")
    if len(args) > len(validators):
        return False, f"'{verb}' takes at most {len(validators)} value(s)"

    values = []
    for tok, check in zip(args, validators):
        v, err = check(tok)
        if err:
            return False, f"{verb}: {err}"
        values.append(v)

    if verb == "sensors" and len(values) >= 2 and values[0] >= 0 and values[0] == values[1]:
        return False, "sensors: the two switches cannot share one pin"

    if verb == "upload" and len(values) >= 3 and values[2] != 0 and values[2] < 300000:
        return False, ("upload: heartbeat must be 0 (off) or at least 300000 ms — "
                       "anything shorter is a poll, not a safety net")

    if cross and len(values) >= need:
        err = cross(values)
        if err:
            return False, f"{verb}: {err}"
    return True, ""


def notify_worthy(command):
    """Is this command worth an email? See NOTIFY_VERBS for the reasoning."""
    parts = command.split()
    if not parts:
        return False
    verb = parts[0].lower()
    if verb not in NOTIFY_VERBS:
        return False
    if verb == "door":
        return len(parts) > 1 and parts[1].lower() in NOTIFY_DOOR_ARGS
    return True


def send_notification(subject, body):
    """One plain-text message. Returns (ok, reason).

    Never raises into the caller: a mail relay having a bad afternoon must not
    turn into a failed command or a 500 on the control endpoint. The command
    has already been queued by the time this runs, and the door will collect it
    whether or not anybody gets told.
    """
    if not NOTIFY_TO:
        return False, "no PETDOOR_NOTIFY_TO configured"
    if not SMTP_HOST:
        return False, "no SMTP_HOST configured"
    # From is a setting, never a guess. A default like petdoor@<somedomain>
    # sends mail as somebody else's domain, fails SPF and DKIM at any real
    # relay, and quietly lands every future alert in a spam folder — which is
    # precisely the failure a notification feature exists to prevent. Refusing
    # and naming the missing setting is the honest answer.
    sender = SMTP_FROM or SMTP_USER
    if not sender:
        return False, "no SMTP_FROM (or SMTP_USER) to send as"

    import smtplib, ssl
    from email.message import EmailMessage

    msg = EmailMessage()
    msg["Subject"] = f"[petdoor] {subject}"
    msg["From"] = sender
    msg["To"] = NOTIFY_TO
    msg.set_content(body)

    try:
        with smtplib.SMTP(SMTP_HOST, SMTP_PORT, timeout=20) as srv:
            # STARTTLS unless explicitly disabled. A relay on this host or on
            # the LAN may not offer it, and refusing to send at all would be
            # worse than a link that never leaves the machine — so this
            # downgrades when the server genuinely does not advertise it, and
            # never on a handshake failure, which is what an attack looks like.
            if SMTP_CRYPTO not in ("", "none", "off") and srv.has_extn("starttls"):
                srv.starttls(context=ssl.create_default_context())
                srv.ehlo()
            if SMTP_USER and SMTP_PASSWORD:
                srv.login(SMTP_USER, SMTP_PASSWORD)
            srv.send_message(msg)
        return True, None
    except Exception as e:
        # The relay's own rejection text is usually the actionable part
        # ("application-specific password required"), so keep it.
        return False, f"{type(e).__name__}: {e}"


def notify_command(device, command, who, source):
    """Tell somebody a consequential command was queued for the door."""
    if not notify_worthy(command) or not NOTIFY_TO:
        return
    when = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S %Z").strip()
    body = (
        f"{command}\n\n"
        f"  door    : {device}\n"
        f"  queued  : {when}\n"
        f"  from    : {source}\n"
        f"  by      : {who or 'not recorded'}\n\n"
        "The door collects queued commands when it next calls in, usually\n"
        "within five minutes, and sooner if the collar is away. Until then it\n"
        "can still be cancelled from the Controls tab.\n\n"
        "https://petdoor.aspl.net/dashboard\n"
    )
    ok, why = send_notification(f"{command} queued for {device}", body)
    if not ok:
        # Worth a log line rather than silence: a notification that never
        # arrives is indistinguishable from nothing having happened.
        sys.stderr.write(f"  NOTIFY FAILED for {command!r}: {why}\n")


def queue_command(device, command):
    """Queue one command line. Returns (ok, message)."""
    command = command.strip()
    if not command:
        return False, "empty command"
    verb = command.split()[0].lower()
    if verb in FORBIDDEN:
        return False, (f"'{verb}' is not remotely settable — it is part of the "
                       "channel this command travels over")
    if verb not in VALID_VERBS:
        return False, f"unknown command '{verb}'; known: {', '.join(VALID_VERBS)}"
    with db() as conn:
        conn.execute("INSERT INTO commands(device,command,queued) VALUES(?,?,?)",
                     (device, command, int(time.time())))
    return True, f"queued for {device}: {command}"


def pending_commands(device):
    with db() as conn:
        return [dict(r) for r in conn.execute(
            "SELECT id,command FROM commands WHERE device=? AND delivered IS NULL "
            "ORDER BY id", (device,))]


def mark_delivered(ids):
    if not ids:
        return
    with db() as conn:
        conn.executemany("UPDATE commands SET delivered=? WHERE id=?",
                         [(int(time.time()), i) for i in ids])


def record_ack(device, text):
    """Attach the door's report to the most recently delivered commands."""
    if not text:
        return
    with db() as conn:
        conn.execute(
            "UPDATE commands SET ack=? WHERE device=? AND delivered IS NOT NULL "
            "AND ack IS NULL", (text[:200], device))


def _ensure_column(conn, table, column, decl):
    """Add a column to an existing database. Older installs predate these."""
    have = {r["name"] for r in conn.execute(f"PRAGMA table_info({table})")}
    if column not in have:
        conn.execute(f"ALTER TABLE {table} ADD COLUMN {column} {decl}")


def migrate():
    with db() as conn:
        _ensure_column(conn, "devices", "status", "TEXT")
        _ensure_column(conn, "devices", "door_ip", "TEXT")
        _ensure_column(conn, "devices", "remote", "INTEGER")
        # The door's current settings, as key=value text. NULL until a door
        # running firmware new enough to send X-PetDoor-Config calls in; the
        # settings form shows "not reported yet" rather than guessing.
        _ensure_column(conn, "devices", "config", "TEXT")
        _ensure_column(conn, "firmware", "git", "TEXT")


def get_key():
    with db() as conn:
        row = conn.execute("SELECT v FROM settings WHERE k='shared_key'").fetchone()
        return row["v"] if row else ""


def set_key(key):
    with db() as conn:
        conn.execute("INSERT INTO settings(k,v) VALUES('shared_key',?) "
                     "ON CONFLICT(k) DO UPDATE SET v=excluded.v", (key,))


def note_firmware(device, version, build, boots, git=None):
    """Record a firmware change, once, the first time we see it.

    Called on every upload; almost always a no-op. The comparison is against
    the LAST row rather than any row, so a deliberate rollback to a previous
    build is recorded as its own event rather than silently ignored — a
    rollback is exactly the thing you want to see in a timeline.
    """
    if not version and not build:
        return
    with db() as conn:
        last = conn.execute(
            "SELECT version,build FROM firmware WHERE device=? "
            "ORDER BY id DESC LIMIT 1", (device,)).fetchone()
        if last and last["version"] == version and last["build"] == build:
            return
        # An OTA window open in the last ten minutes is the strong hint that
        # this arrived over the air rather than down a cable. Not proof — the
        # window could have been opened and not used — so the column says
        # "ota?" rather than "ota".
        recent = conn.execute(
            "SELECT 1 FROM commands WHERE device=? AND command='ota' "
            "AND delivered IS NOT NULL AND delivered > ? LIMIT 1",
            (device, int(time.time()) - 600)).fetchone()
        conn.execute(
            "INSERT INTO firmware(device,version,build,git,first_seen,boots,via)"
            " VALUES(?,?,?,?,?,?,?)",
            (device, version, build, git, int(time.time()), boots,
             "ota?" if recent else "cable"))
    sys.stderr.write(f"  FIRMWARE CHANGE {device}: v{version} {build}\n")


def note_device(device, version, build, boots, ip, git=None):
    note_firmware(device, version, build, boots, git)
    with db() as conn:
        conn.execute(
            "INSERT INTO devices(device,version,build,boots,last_seen,last_ip)"
            " VALUES(?,?,?,?,?,?)"
            " ON CONFLICT(device) DO UPDATE SET"
            "   version=excluded.version, build=excluded.build,"
            "   boots=excluded.boots, last_seen=excluded.last_seen,"
            "   last_ip=excluded.last_ip",
            (device, version, build, boots, int(time.time()), ip))


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

    with db() as conn:
        devs = conn.execute("SELECT * FROM devices ORDER BY device").fetchall()
    if devs:
        bits = []
        for d in devs:
            age = int((time.time() - (d["last_seen"] or 0)) / 60)
            bits.append(f'{html.escape(d["device"])} v{html.escape(d["version"] or "?")} '
                        f'&middot; boot #{d["boots"] or 0} &middot; {age} min ago')
        cards += ('<div class="card"><div class="k">Doors</div>'
                  '<div style="font-size:12.5px;margin-top:4px;line-height:1.6">'
                  + "<br>".join(bits) + "</div></div>")

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

    # ------------------------------------------------------------------
    # PUBLIC vs PRIVATE
    #
    # This server does no authentication of its own. It splits the routes so a
    # reverse proxy in front of it can, because "when does this door open and
    # close" is a detailed record of when a house is occupied and empty.
    #
    #   PUBLIC   /            project page, no data
    #            /demo        sample dashboard, synthetic data baked in
    #            /images/*    photographs used by that page
    #            /health      so uptime monitoring does not need a credential
    #
    #   PRIVATE  /dashboard    the analytics
    #            /api/events   the raw log the analytics are built from
    #            /api/command  POST, queues a command — OPENS THE DOOR
    #            /table        plain-HTML fallback view
    #            /export.csv   the whole log as a file
    #
    #   SIGNED   /ingest       POST only, HMAC over timestamp + body
    #
    # /api/command is under /api/ deliberately: every proxy rule in the docs
    # already matches /api/*, so it is covered the moment it exists rather than
    # waiting for each operator to notice a new path. It is also off unless the
    # server was started with --allow-web-control.
    #
    # Protect the private set at the proxy. Leaving them open publishes your
    # household routine to anyone who finds the hostname. See
    # docs/WEB-DASHBOARD.md for worked nginx, Caddy and Apache rules.
    # ------------------------------------------------------------------
    def _serve_file(self, name, ctype):
        page = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
        if os.path.exists(page):
            with open(page, "rb") as fh:
                return self._send(200, fh.read(), ctype)
        return None

    def do_GET(self):
        # Match on the path alone. Exact-matching self.path would 404 on
        # anything carrying a query string — a cache-buster, a share link with
        # UTM parameters, a proxy that appends its own.
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            sent = self._serve_file("public.html", "text/html; charset=utf-8")
            if sent is not None:
                return sent
            # No public page deployed: fall through to the dashboard rather than
            # serving nothing, so an existing single-page install keeps working.
            sent = self._serve_file("dashboard.html", "text/html; charset=utf-8")
            if sent is not None:
                return sent
            return self._send(200, render(), "text/html; charset=utf-8")
        if path in ("/demo", "/demo.html"):
            # PUBLIC. Self-contained: the sample data is baked into the page, so
            # it never touches /api/events and can be shown to anyone.
            sent = self._serve_file("demo.html", "text/html; charset=utf-8")
            if sent is not None:
                return sent
            return self._send(404, "no sample page deployed")
        if path in ("/dashboard", "/dashboard.html"):
            sent = self._serve_file("dashboard.html", "text/html; charset=utf-8")
            if sent is not None:
                return sent
            return self._send(200, render(), "text/html; charset=utf-8")
        if path.startswith("/images/"):
            # Static, read-only, and strictly from the images directory beside
            # this script. basename() strips any traversal attempt outright.
            name = os.path.basename(path[len("/images/"):])
            root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "images")
            path = os.path.join(root, name)
            ext = os.path.splitext(name)[1].lower()
            types = {".jpeg": "image/jpeg", ".jpg": "image/jpeg", ".png": "image/png",
                     ".svg": "image/svg+xml", ".webp": "image/webp"}
            if name and ext in types and os.path.isfile(path):
                with open(path, "rb") as fh:
                    return self._send(200, fh.read(), types[ext])
            return self._send(404, "not found")
        if path.startswith("/api/events"):
            with db() as conn:
                rows = conn.execute("SELECT device,epoch,uptime,boot,type,detail,rssi "
                                    "FROM events ORDER BY epoch, boot, uptime").fetchall()
                devs = conn.execute("SELECT * FROM devices").fetchall()
            with db() as conn:
                # Config changes, so the dashboard can mark WHEN the door's
                # behaviour was altered against the behaviour itself. A shift in
                # the daily rhythm means something different if you changed the
                # exit threshold that morning.
                cmds = conn.execute(
                    "SELECT device,command,queued,delivered,ack FROM commands "
                    "WHERE delivered IS NOT NULL ORDER BY delivered DESC LIMIT 60"
                ).fetchall()
                # Still waiting for the door to call in. The control panel shows
                # these so a button press does not simply vanish for five
                # minutes with nothing to show it was registered.
                pend = conn.execute(
                    "SELECT device,command,queued FROM commands "
                    "WHERE delivered IS NULL ORDER BY id"
                ).fetchall()
                fw = conn.execute(
                    "SELECT device,version,build,first_seen,boots,via FROM firmware "
                    "ORDER BY first_seen DESC LIMIT 20"
                ).fetchall()
            return self._send(200, json.dumps({"events": [dict(r) for r in rows],
                                               "devices": [dict(d) for d in devs],
                                               "commands": [dict(c) for c in cmds],
                                               "pending": [dict(p) for p in pend],
                                               "firmware": [dict(f) for f in fw],
                                               "control": bool(ALLOW_WEB_CONTROL)}),
                              "application/json; charset=utf-8")
        if path.startswith("/table"):
            return self._send(200, render(), "text/html; charset=utf-8")
        if path.startswith("/export.csv"):
            with db() as conn:
                rows = conn.execute("SELECT * FROM events ORDER BY epoch, boot, uptime").fetchall()
            csv = "epoch,uptime_s,boot,event,detail,rssi\n" + "".join(
                f'{r["epoch"]},{r["uptime"]},{r["boot"]},{r["type"]},{r["detail"]},{r["rssi"]}\n'
                for r in rows)
            return self._send(200, csv, "text/csv; charset=utf-8")
        if path == "/health":
            return self._send(200, json.dumps({"ok": True}), "application/json")
        self._send(404, "not found")

    def _csrf_problem(self):
        """Why this POST should not be honoured, or None if it looks fine.

        The proxy in front of this server decides WHO you are, using a cookie.
        A cookie is attached by the browser to any request to this origin —
        including one started by a page on a completely different site. So
        authentication alone does not mean the *user* asked for this.

        Two checks, neither of which a cross-origin page can satisfy:

          * A custom request header. Browsers refuse to send one cross-origin
            without first asking permission via a CORS preflight, and this
            server answers no preflight, so the request is never made.
          * Origin must match Host, when the browser sends an Origin at all.

        Neither is a substitute for the proxy's authentication. They assume the
        request is already authenticated and stop a *different site* from
        riding that authentication.
        """
        if self.headers.get("X-PetDoor-Control") != "1":
            return "missing X-PetDoor-Control header"
        origin = self.headers.get("Origin")
        if origin:
            if urlparse(origin).netloc != (self.headers.get("Host") or ""):
                return f"cross-origin POST from {urlparse(origin).netloc}"
        return None

    def _json(self, code, payload):
        return self._send(code, json.dumps(payload) + "\n",
                          "application/json; charset=utf-8")

    def _handle_command(self):
        """PRIVATE. Queue one command from the dashboard's control panel.

        Lives under /api/ on purpose: every protection rule in
        docs/WEB-DASHBOARD.md already matches /api/*, so an existing deployment
        that followed those instructions covers this route the day it appears.
        A new top-level path would have been exposed until each operator
        noticed and updated their proxy.
        """
        if not ALLOW_WEB_CONTROL:
            return self._json(403, {"ok": False, "error":
                "web control is off; start the server with --allow-web-control"})

        bad = self._csrf_problem()
        if bad:
            self.log_message("CONTROL REJECTED from %s: %s", self.client_address[0], bad)
            return self._json(403, {"ok": False, "error": f"rejected: {bad}"})

        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0 or length > 2000:
            return self._json(400, {"ok": False, "error": "empty or oversized body"})
        try:
            payload = json.loads(self.rfile.read(length))
            # Lowercased, not just whitespace-normalised. Every web verb and
            # every argument they take is lowercase ASCII, and the firmware
            # compares them with strcmp — so "door OPEN" would pass the check
            # below and then be refused by the door, which is the worst of both
            # worlds: the panel says queued, the door says no.
            command = " ".join(str(payload["command"]).split()).lower()[:120]
            device = str(payload.get("device") or "")[:32]
        except Exception:
            return self._json(400, {"ok": False, "error": 'expected {"command": "..."}'})

        ok, why = web_command_allowed(command)
        if not ok:
            return self._json(400, {"ok": False, "error": why})

        # A few commands lose state or take the door off the air for a minute.
        # The panel asks first; this makes the asking load-bearing rather than
        # decorative, so the same guard applies to a hand-crafted request.
        if web_command_needs_confirm(command) and payload.get("confirm") is not True:
            return self._json(400, {"ok": False, "confirm_required": True,
                                    "error": f"'{command.split()[0]}' needs confirming"})

        if not device:
            with db() as conn:
                known = [r["device"] for r in conn.execute("SELECT device FROM devices")]
            if len(known) == 1:
                device = known[0]
            elif not known:
                return self._json(400, {"ok": False, "error":
                    "no door has ever uploaded, so there is nothing to queue for"})
            else:
                return self._json(400, {"ok": False, "error":
                    "several doors are known; say which"})

        # A double tap on a phone is one intent, not two. Without this, two
        # `door open` commands queue and the door pulses the relay twice —
        # which on a controller that toggles is an open followed by a stop.
        with db() as conn:
            dup = conn.execute("SELECT id FROM commands WHERE device=? AND command=? "
                               "AND delivered IS NULL", (device, command)).fetchone()
        if dup:
            return self._json(200, {"ok": True, "queued": False,
                                    "message": f"already waiting for {device}"})

        ok, msg = queue_command(device, command)
        # Logged whether or not it was accepted: this is the audit trail for a
        # door that can now be opened from a browser.
        self.log_message("CONTROL from %s: %s -> %s", self.client_address[0], command, msg)
        if ok:
            # Caddy strips any client-supplied identity headers BEFORE
            # authenticating and then sets these from oauth2-proxy's response,
            # so they are the proxy's word rather than the caller's. Anything
            # reaching this port without going through Caddy could forge them —
            # which is the same trust boundary the whole private half rests on.
            who = (self.headers.get("X-Auth-Request-Email")
                   or self.headers.get("X-Auth-Request-User") or "")
            notify_command(device, command, who, f"the dashboard ({self.client_address[0]})")
        return self._json(200 if ok else 400, {"ok": ok, "queued": ok, "message": msg})

    def do_DELETE(self):
        """PRIVATE. Drop whatever has not reached the door yet.

        The reason this exists: a queued command waits for the door's next
        check-in, which can be five minutes away. That delay is usually a
        nuisance, but here it is a gift — it means a mistaken tap is still
        recallable. Better than a confirmation dialog on every press, which
        would add friction to the one action people actually want (open the
        door, from the garden, with cold hands).
        """
        if urlparse(self.path).path != "/api/command":
            return self._send(404, "not found")
        if not ALLOW_WEB_CONTROL:
            return self._json(403, {"ok": False, "error": "web control is off"})
        bad = self._csrf_problem()
        if bad:
            self.log_message("CONTROL REJECTED from %s: %s", self.client_address[0], bad)
            return self._json(403, {"ok": False, "error": f"rejected: {bad}"})
        # Scoped to one door when the panel says which. Without this a Cancel
        # pressed on a page showing the back door would also empty the front
        # door's queue, which the person pressing it has no way to see.
        qs = parse_qs(urlparse(self.path).query)
        device = (qs.get("device") or [""])[0][:32]
        with db() as conn:
            if device:
                n = conn.execute("DELETE FROM commands WHERE delivered IS NULL "
                                 "AND device=?", (device,)).rowcount
            else:
                n = conn.execute("DELETE FROM commands WHERE delivered IS NULL").rowcount
        self.log_message("CONTROL from %s: cancelled %d undelivered%s",
                         self.client_address[0], n, f" for {device}" if device else "")
        return self._json(200, {"ok": True, "cancelled": n,
                                "message": f"cancelled {n} command{'' if n == 1 else 's'}"})

    def do_POST(self):
        if urlparse(self.path).path == "/api/command":
            return self._handle_command()
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
        version = (self.headers.get("X-PetDoor-Version") or "")[:32]
        build = (self.headers.get("X-PetDoor-Build") or "")[:40]
        try:
            boots = int(self.headers.get("X-PetDoor-Boot") or 0)
        except ValueError:
            boots = 0
        # A discovery-table dump, not events. Filed separately so the CSV
        # parser never has to guess what it is looking at.
        if self.headers.get("X-PetDoor-Kind") == "scan":
            text = body.decode("utf-8", "replace")[:20000]
            with db() as conn:
                conn.execute("INSERT INTO scans(device,epoch,text) VALUES(?,?,?)",
                             (device, int(time.time()), text))
                # Two is plenty: the latest, and the one before it to compare.
                conn.execute(
                    "DELETE FROM scans WHERE device=? AND epoch NOT IN "
                    "(SELECT epoch FROM scans WHERE device=? ORDER BY epoch DESC LIMIT 2)",
                    (device, device))
            self.log_message("%s: discovery table stored (%d bytes)", device, len(text))
            return self._send(200, json.dumps({"scan": "stored"}) + "\n",
                              "application/json")

        rows = parse_csv(body.decode("utf-8", "replace"))
        added = store(device, rows)
        # The door's OWN address, not the proxy's. self.client_address is
        # whatever last hop connected — behind a reverse proxy that is the
        # proxy, which is no use for pushing an update to.
        door_ip = self.headers.get("X-PetDoor-IP") or self.client_address[0]
        note_device(device, version, build, boots, door_ip,
                    (self.headers.get("X-PetDoor-Git") or "")[:40] or None)
        with db() as conn:
            conn.execute(
                "UPDATE devices SET status=?, config=?, door_ip=?, remote=? "
                "WHERE device=?",
                (self.headers.get("X-PetDoor-Status"),
                 self.headers.get("X-PetDoor-Config"), door_ip,
                 1 if self.headers.get("X-PetDoor-Remote") == "1" else 0, device))
        self.log_message("%s v%s boot#%d: %d events, %d new (%s)",
                         device, version or "?", boots, len(rows), added, reason)

        # What the door made of whatever we sent it last time.
        record_ack(device, self.headers.get("X-PetDoor-Ack"))

        # ------------------------------------------------------------------
        # The reply is the only channel we have to a door behind NAT: it calls
        # us, we never call it. So anything queued rides back on this response.
        #
        # It is SIGNED with the same key the upload was, because uploads are
        # plain HTTP by design — a TLS handshake costs the ESP32 more heap than
        # it has. Without a signature, anyone on the path could retune the door
        # or raise its radio at will. The door verifies before obeying, and
        # ignores the reply entirely when no key is configured.
        #
        # A door running firmware without REMOTE_CONFIG never sends
        # X-PetDoor-Remote and never reads this; queued commands simply wait,
        # which is why --commands shows whether a door is listening.
        # ------------------------------------------------------------------
        listening = self.headers.get("X-PetDoor-Remote") == "1"
        pend = pending_commands(device) if listening else []
        key = get_key()

        if pend and not key:
            # Refuse rather than send orders nobody can authenticate.
            self.log_message("%s: %d command(s) withheld — no shared key to sign with",
                             device, len(pend))
            pend = []

        if pend:
            reply = "".join(c["command"] + "\n" for c in pend)
            # The door sends a fresh nonce with every upload and folds it into
            # the signature it expects back. Without that the HMAC would prove
            # only that we once said these bytes, not that we said them now —
            # and replies travel over plain HTTP, so a recorded "door open"
            # could be played back at will. Signing the nonce binds the reply
            # to this one request.
            #
            # A door that sends no nonce is running firmware from before this
            # existed; it would reject the reply anyway, so say so rather than
            # sending something that cannot be verified.
            nonce = self.headers.get("X-PetDoor-Nonce")
            if not nonce:
                self.log_message("%s: %d command(s) withheld — door sent no nonce "
                                 "(firmware predates replay protection)",
                                 device, len(pend))
                return self._send(200,
                                  json.dumps({"received": len(rows), "new": added}) + "\n",
                                  "application/json")
            ts = str(int(time.time()))
            signed = (ts + "\n" + nonce + "\n").encode() + reply.encode()
            sig = hmac.new(key.encode(), signed, hashlib.sha256).hexdigest()
            # Only now, once the reply can actually be signed and sent.
            mark_delivered([c["id"] for c in pend])
            self.log_message("%s: sent %d command(s)", device, len(pend))
            body_b = reply.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body_b)))
            self.send_header("X-PetDoor-Timestamp", ts)
            self.send_header("X-PetDoor-Signature", sig)
            self.end_headers()
            return self.wfile.write(body_b)

        return self._send(200, json.dumps({"received": len(rows), "new": added}) + "\n",
                          "application/json")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    global ALLOW_WEB_CONTROL
    ap = argparse.ArgumentParser(description="PetDoor log server")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--init", action="store_true", help="create the database and a key")
    ap.add_argument("--show-key", action="store_true", help="print the key the door must use")
    ap.add_argument("--rotate-key", action="store_true", help="issue a new key")
    ap.add_argument("--set-key", metavar="KEY", help="set a specific key")
    ap.add_argument("--no-key", action="store_true", help="accept unsigned uploads")
    ap.add_argument("--allow-web-control", action="store_true",
                    help="let the private dashboard queue open/close/lock/unlock. "
                         "Only with authentication in front of /api/* — see "
                         "docs/WEB-DASHBOARD.md")
    ap.add_argument("--queue", nargs="+", metavar="WORD",
                    help="queue a command for the door, delivered on its next upload "
                         "(e.g. --queue pulse 500)")
    ap.add_argument("--device", default=None,
                    help="which door to queue for (default: the only one known)")
    ap.add_argument("--commands", action="store_true",
                    help="show queued, delivered and acknowledged commands")
    ap.add_argument("--clear-commands", action="store_true",
                    help="drop commands that have not been delivered yet")
    ap.add_argument("--firmware", action="store_true",
                    help="show every firmware a door has run, newest first")
    ap.add_argument("--doors", action="store_true",
                    help="show each door: firmware, address to push to, and what it sees")
    ap.add_argument("--scan", action="store_true",
                    help="show the last discovery table a door uploaded")
    args = ap.parse_args()

    init_db()
    migrate()

    if args.init:
        if not get_key():
            set_key(secrets.token_hex(16))
        print(f"database : {os.path.abspath(DB_PATH)}")
        print(f"key      : {get_key()}")
        print("\nPut this in petdoor/secrets.h:")
        print(f'  #define LOG_SHARED_KEY "{get_key()}"')
        return
    if args.firmware:
        with db() as conn:
            rows = conn.execute("SELECT * FROM firmware ORDER BY first_seen DESC").fetchall()
        if not rows:
            print("No firmware changes recorded yet.")
            print("This is populated from uploads, so it starts from the first")
            print("upload after this server was updated — not retroactively.")
            return
        print(f"{'first seen':20} {'version':9} {'commit':9} {'build':22} {'boots':>6}  how")
        for r in rows:
            when = dt.datetime.fromtimestamp(r["first_seen"]).strftime("%Y-%m-%d %H:%M:%S")
            print(f"{when:20} {r['version'] or '?':9} {(r['git'] if 'git' in r.keys() else None) or '-':9} "
                  f"{r['build'] or '?':22} {r['boots'] if r['boots'] is not None else '?':>6}  "
                  f"{r['via'] or ''}")
        return

    if args.commands or args.queue or args.clear_commands or args.doors or args.scan:
        with db() as conn:
            known = [r["device"] for r in conn.execute("SELECT device FROM devices")]
        target = args.device
        if target is None:
            if len(known) == 1:
                target = known[0]
            elif args.queue or args.clear_commands:
                if not known:
                    print("No door has ever uploaded, so there is nothing to queue for.")
                    print("Name one with --device to queue ahead of its first upload;")
                    print("it collects the commands when it calls in.")
                else:
                    print("Several doors are known; say which with --device:")
                    for d in known:
                        print("   ", d)
                raise SystemExit(1)

        if args.clear_commands:
            with db() as conn:
                n = conn.execute(
                    "DELETE FROM commands WHERE delivered IS NULL AND device=?",
                    (target,)).rowcount
            print(f"dropped {n} undelivered command(s) for {target}")

        if args.queue:
            cmd = " ".join(args.queue)
            ok, msg = queue_command(target, cmd)
            print(msg)
            if not ok:
                raise SystemExit(1)
            print("The door collects it on its next upload. Watch with --commands.")
            # The same notification as the web path. Somebody with a shell on
            # the server opening the door is no less worth telling people about
            # than somebody with a browser — arguably more.
            notify_command(target, cmd, os.environ.get("USER", ""), "the command line")

        if args.doors:
            with db() as conn:
                for d in conn.execute("SELECT * FROM devices ORDER BY device"):
                    age = int(time.time()) - (d["last_seen"] or 0)
                    print(f"  {d['device']}")
                    print(f"    firmware   : v{d['version'] or '?'}  ({d['build'] or '?'})")
                    print(f"    boots      : #{d['boots'] or 0}")
                    print(f"    last heard : {age // 60} min ago")
                    print(f"    push to    : {d['door_ip'] or '(unknown — needs newer firmware)'}")
                    print(f"    commands   : {'accepted' if d['remote'] else 'NOT SUPPORTED by this build'}")
                    if d["status"]:
                        print(f"    sees       : {d['status']}")
                    print()

        if args.scan:
            with db() as conn:
                rows = list(conn.execute(
                    "SELECT device,epoch,text FROM scans ORDER BY epoch DESC LIMIT 1"))
            if not rows:
                print("No discovery table has been uploaded.")
                print("Ask for one with:  --queue scan")
            for r in rows:
                when = dt.datetime.fromtimestamp(r["epoch"]).strftime("%Y-%m-%d %H:%M")
                print(f"{r['device']} — discovery table at {when}\n")
                print(r["text"])

        if args.commands:
            with db() as conn:
                rows = list(conn.execute(
                    "SELECT id,device,command,queued,delivered,ack FROM commands "
                    "ORDER BY id DESC LIMIT 30"))
            if not rows:
                print("no commands queued or sent")
            for r in rows:
                when = dt.datetime.fromtimestamp(r["queued"]).strftime("%Y-%m-%d %H:%M")
                if r["ack"]:
                    state = f"acked: {r['ack']}"
                elif r["delivered"]:
                    state = "delivered, awaiting the next upload for the result"
                else:
                    state = "QUEUED — waiting for the door to call in"
                print(f"  #{r['id']:<4} {when}  {r['device']:<14} {r['command']:<34} {state}")
        raise SystemExit(0)

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
    if args.allow_web_control:
        ALLOW_WEB_CONTROL = True
    if ALLOW_WEB_CONTROL:
        print(f"  control   : POST http://{args.host}:{args.port}/api/command")
        print()
        print("  !! WEB CONTROL IS ON. Anyone who can load /dashboard can open,")
        print("  !! close, lock and unlock the door. That route must have")
        print("  !! authentication in front of it — see docs/WEB-DASHBOARD.md.")
    with Server((args.host, args.port), Handler) as srv:
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped")


if __name__ == "__main__":
    main()
