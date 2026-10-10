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
import threading
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
# The SMTP settings deliberately share the names the web host already uses for its
# other services, so this reuses one set of credentials rather than adding a
# second copy of the same secret to go stale independently.
NOTIFY_TO = os.environ.get("PETDOOR_NOTIFY_TO", "").strip()
SMTP_HOST = os.environ.get("SMTP_HOST", "").strip()
SMTP_PORT = int(os.environ.get("SMTP_PORT", "587") or 587)
SMTP_USER = os.environ.get("SMTP_USER", "").strip()
SMTP_PASSWORD = os.environ.get("SMTP_PASSWORD", "")
SMTP_FROM = os.environ.get("SMTP_FROM", "").strip()
# Where the "Open the dashboard" button in notification mail points. No default
# on purpose: this is a public project, and a hard-coded host would send every
# installation's mail pointing at somebody else's door. Unset means no button.
DASHBOARD_URL = os.environ.get("PETDOOR_DASHBOARD_URL", "").strip()
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
NOTIFY_VERBS = ("door", "lock", "unlock", "defaults", "macs", "reboot", "ota",
                # A maintenance window stops the door guarding the animal and
                # opens a console on it. Both halves deserve an email.
                "maint")
NOTIFY_DOOR_ARGS = ("open",)   # `door close`/`auto` are the safe direction

EVENT_LABEL = {
    "OPEN": "came in", "CLOSE": "went out", "BOOT": "restarted",
    "REFUSED": "refused", "FIX_GOT": "beacon found", "FIX_LOST": "beacon lost",
    "STALLED": "did not complete its travel",
    "MAINT": "maintenance mode",
    # The position hold: the door pinned open or closed with the automatic
    # path inert. Unlike maintenance this does not expire, so it is the one
    # state here that can be months old and still true.
    "MANUAL": "under manual control",
    "CONSOLE": "network console",
    "NO_MOVE": "did not move at all",
    # The door's own controller has modes of its own and has been watched
    # driving the door with nothing commanding it. Only visible once limit
    # switches are fitted, which is why this event is newer than the rest.
    "UNCOMMANDED": "moved, not by PetDoor",
    "RETRY": "pressed again",
    # The one entry here that is a request rather than a record: close attempts
    # are exhausted and the door is staying open until a person deals with it.
    "GAVE_UP": "GAVE UP closing",
    "WAKE": "woke the controller",
    # The beacon's own battery, from its Eddystone-TLM frames. Worth surfacing
    # because of what a flat one does: the door refuses to act until it has
    # heard the collar once since boot, so a dead beacon does not shut the door
    # — it stops the door working, quietly, with the animal outside.
    "BEACON_LOW": "beacon battery",
    # A sensor disagreed with the other one badly enough to be called broken.
    # Distinct from STALLED/NO_MOVE, which say the DOOR misbehaved: this says
    # the thing watching the door is lying, which is worse because a stall is
    # visible and a dead sensor is not.
    "SENSOR_FAULT": "sensor fault",
    # The same sensor telling the truth again. Logged so the history does not
    # show faults arriving and never leaving — read a week later, a fault the
    # door shrugged off would otherwise look identical to one still live.
    "SENSOR_OK": "sensor recovered",
}

# petdoor/eventlog.h : enum SensorFault. Each is diagnosed by cross-checking
# one sensor against the other, so each names a PART rather than a symptom.
def sensor_fault_evidence(detail, src):
    """What the spare field means for this fault code, in words.

    `src` is one integer shared by every event type, and SENSOR_FAULT does not
    even use it consistently: for the vibration faults it is an edge count, but
    for SF_REED_LOST (6) it is WHICH END the door was sitting at. Rendering it
    as "1 vibration edges" is the kind of wrong that sends someone to inspect
    the wrong sensor, so the meaning is decided in one place and used by both
    the email and the table.
    """
    if not src:
        return None
    if detail == 6:
        return {1: "the door was sitting OPEN", 2: "the door was sitting CLOSED"}.get(
            src, f"end code {src}")
    return f"{src} vibration edges"


SENSOR_FAULT = {
    1: ("both limit switches made at once",
        "a shorted wire, a stuck switch, or a stray magnet"),
    2: ("the vibration sensor felt nothing during a VERIFIED travel",
        "the door moved and the sensor did not notice — deaf, unplugged, "
        "or come off the door"),
    3: ("the vibration sensor is firing with the door standing still",
        "sensitivity screw too far in, or mounted where it feels the world "
        "rather than the door"),
    5: ("the vibration sensor produced no edges at all during an actuation",
        "unplugged, a broken wire, or no power — even a badly mounted sensor "
        "hears the relay click, so zero means no signal path"),
    4: ("the door ran a full travel and never arrived",
        "the limit switch at that end is not making — most likely a magnet "
        "that has come off or drifted out of its narrow capture range"),
    6: ("a limit switch at the end the door is sitting at is not making",
        "the door has been at rest at that end for minutes and the switch "
        "fitted there reports nothing — check the connector first, then the "
        "magnet-to-reed gap with the door against its stop. Nothing else "
        "notices this one: it takes neither both switches at once nor a "
        "travel that fails, so it stays invisible until the next actuation"),
}

# What a door actuation's `detail` means: ActuationSource in petdoor/door.h.
ACTUATION_SOURCE = {0: "the collar", 1: "the console", 2: "the dashboard",
                    3: "a safety reversal"}

# Flags packed into an actuation's spare field: LogActuationFlags in
# petdoor/eventlog.h. "verified" is the one worth reading — without it the
# entry records only that the door was COMMANDED somewhere.
ACTUATION_FLAGS = ((0x0001, "verified by a switch"), (0x0002, "after a wake press"),
                   (0x0004, "needed a repeat press"), (0x0008, "safety reversal"))
RESET_REASON = {1: "power-on", 3: "software", 4: "panic", 5: "interrupt watchdog",
                6: "task watchdog", 7: "watchdog", 9: "BROWNOUT"}

# What asked the door to move. This is OPEN's and CLOSE's `detail`, and it is the
# ActuationSource enum in petdoor/door.h — keep the two in step, and note that
# adding a value must not re-label history already in the database.
#
# 3 is not an ordinary movement: the firmware reverses a stalled close by itself,
# so a "fail-safe reversal" in this column is the visible trace of a close that
# did not complete. Rendered loudly for that reason.
# HOW manual control was taken: OverrideSource in petdoor/eventlog.h, carried
# in a MANUAL event's spare column.
#
# 0 means NOT RECORDED, not "unknown source" — every MANUAL row written before
# the firmware carried this has 0 there, and rendering it as a real source
# would invent a fact about history. Same rule as the actuation details below.
OVERRIDE_SOURCE = {
    1: ("at the console", "somebody standing at the door with a cable"),
    2: ("from the dashboard", "a command sent over the network"),
    3: ("BY HAND — the door was physically moved",
        "a travel nothing commanded: the door is no longer where it was left. "
        "The limit switches and the vibration sensor saw it move; nothing "
        "asked it to"),
    4: ("by a position hold", "`lock open` or `lock close`"),
}


def override_source(src):
    """(what, why) for a MANUAL event's spare column, or None if not recorded."""
    if not src:
        return None
    return OVERRIDE_SOURCE.get(src)


ACT_SOURCE = {0: "beacon", 1: "console", 2: "network",
              3: "fail-safe reversal"}

# Which of those mean the door FELL OVER, as opposed to being restarted on
# purpose. 1 (power-on) and 3 (software) are ordinary — a software restart is
# exactly what an OTA push does, and power-on is a plug. Everything else is the
# door dying and coming back, and every one of them used to be silent: a door
# panicked opening an OTA window and the only way to find out was to go and look.
ABNORMAL_RESET = {4, 5, 6, 7, 9}

RESET_ADVICE = {
    4: "a crash. The backtrace only ever exists on the serial console, so it is "
       "gone unless a cable was attached. Compare maxalloc against heap in the "
       "status line: a large gap means the heap is fragmented, which takes a door "
       "out while every other number still looks healthy.",
    5: "an interrupt watchdog — something blocked with interrupts disabled.",
    6: "a task watchdog — a task stopped yielding.",
    7: "a watchdog reset.",
    9: "the supply sagged. Check the PSU and anything sharing it with the motor; "
       "a relay coil on a marginal supply is the classic cause.",
}


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
            -- The event's one spare integer. 0 for most rows. Named for its
            -- first use and since outgrown it, which is worth knowing before
            -- reading a value here: CONSOLE rows put the last octet of the
            -- connecting address in it, SENSOR_FAULT the evidence that raised
            -- the fault, BEACON_LOW the millivolts, and an inferred
            -- UNCOMMANDED the milliseconds of movement it was inferred from.
            -- Read it according to `type` and never on its own.
            -- Deliberately not part of the primary key: it says something
            -- ABOUT the event, it does not identify it.
            src      INTEGER NOT NULL DEFAULT 0,
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
               "travel", "buzzer", "beep", "sensors", "upload", "maint",
               "schedule", "vibration", "led", "wake", "retry", "calibrate",
    "mute", "unmute",
)

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
    # A bare `lock` stops the collar OPENING the door. `lock open` / `lock close`
    # pin the door to a position and never expire. One verb, optional argument,
    # so `need` stays 0 and the validator only constrains the argument if given.
    "lock":       ([_word("open", "close")], 0, None, False),
    "unlock":     ([], 0, None, False),
    # Silences the routine sounds only; faults still ring. Neither needs
    # confirming: both are instantly reversible and neither moves the door.
    "mute":       ([], 0, None, False),
    "unmute":     ([], 0, None, False),
    "beep":       ([], 0, None, False),
    "scan":       ([], 0, None, False),
    "resetstats": ([], 0, None, False),
    # Needs confirming because for its duration the door deliberately ignores
    # the collar — an animal outside cannot let itself in. The firmware bounds
    # the window; this bounds the surprise.
    # Minutes, up to MAINT_MAX_MS (480 h = 28,800 min). The firmware REFUSES a
    # longer window rather than shortening it, so this range has to match the
    # firmware's or the panel starts offering windows that come back refused.
    "maint":      ([_whole(1, 28800, " min")], 0, None, True),
    # Confirmed, because a window that is wrong locks an animal out overnight
    # and nobody finds out until morning. Arguments are free-form
    # ("add 22:00-06:00 Mon-Fri"), so the firmware does the parsing — which it
    # must anyway, since anyone can run a server.
    "schedule":   ([], 0, None, True),
    "vibration":  ([_whole(0, 39, "")], 1, None, False),

    # --- detection ---------------------------------------------------------
    "thresholds": ([_whole(-120, 0, " dBm"), _whole(-120, 0, " dBm")], 2,
                   _enter_above_exit, False),
    "filter":     ([_odd(1, 15), _frac(0.01, 1.0)], 2, None, False),
    "openfilter": ([_odd(1, 15), _frac(0.01, 1.0)], 2, None, False),

    # --- timing ------------------------------------------------------------
    "dwell":      ([_whole(100, 600000, " ms")] * 3, 3, _lockout_below_close, False),
    # One value sets both directions; two set them separately. They are not
    # equal on a mounted door — gravity assists the close and opposes the open.
    "travel":     ([_whole(0, 120000, " ms"), _whole(0, 120000, " ms")], 1, None, False),
    # How long an idle vendor controller is assumed to need a wake press before
    # it will listen. 0 turns the wake press off.
    "wake":       ([_whole(0, 3600000, " ms")], 1, None, False),
    # After a close that stalled: how long to wait, and how many attempts
    # before the door stays open and says so. `retry clear` resets a door that
    # has already given up, for somebody who has just cleared the obstruction.
    "retry":      ([_whole(1, 1440, " min"), _whole(1, 10)], 2, None, False),
    # Times a travel in each direction and adopts the result. Confirmed,
    # because it drives the door twice, on purpose, while somebody may be
    # standing at it — and it refuses unless a maintenance window is open.
    "calibrate":  ([], 0, None, True),
    "led":        ([], 0, None, False),

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
    if not spec:
        return False
    # `lock open` / `lock close` pin the door and never expire, so they must be
    # confirmed even if the request is crafted by hand. A BARE `lock` only stops
    # the collar opening the door and has never needed confirming — marking the
    # whole verb would put a dialog on the ordinary lock and, worse, refuse the
    # existing Lock button, which sends none. So this one asks about the
    # argument rather than the verb.
    if parts[0].lower() == "lock" and len(parts) > 1:
        return True
    return bool(spec[3])


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

    # "maint off" ends the window; "maint" alone takes the firmware's default
    # duration; "maint <minutes>" names one and is range-checked below.
    # The schedule sub-verbs carry their own arguments; the door validates
    # them and reports what it did.
    if verb == "schedule":
        return (True, "")

    if verb == "vibration" and args and args[0].lower() in ("off", "none"):
        return (True, "") if len(args) == 1 else (False, "'vibration off' takes nothing else")

    # "retry clear" resets a door that has given up closing, for somebody who
    # has just cleared whatever was in the way. A word where a number belongs,
    # so it has to be taken before the range checks run.
    if verb == "retry" and args and args[0].lower() == "clear":
        return (True, "") if len(args) == 1 else (False, "'retry clear' takes nothing else")

    if verb == "maint" and args and args[0].lower() in ("off", "on"):
        return (True, "") if len(args) == 1 else (False, f"'maint {args[0].lower()}' takes nothing else")

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


# --------------------------------------------------------------------- email
#
# One template for every message the server sends, so a new notification cannot
# arrive looking like it came from a different system. Three rules shape it:
#
#   * TABLES AND INLINE STYLES, not a stylesheet. Mail clients are not browsers
#     — Outlook renders through Word, Gmail strips <style> blocks, and anything
#     relying on flexbox or classes degrades into unstyled text.
#   * EVERY MESSAGE ALSO GOES AS PLAIN TEXT. Built from the same arguments, so
#     the two cannot drift. A text part is what a watch, a screen reader and a
#     spam filter all read, and a message with only an HTML part looks like
#     bulk mail.
#   * THE LOGO IS ATTACHED, NOT LINKED. Most clients block remote images by
#     default, and a blocked logo is worse than no logo — it leaves a broken
#     frame at the top of an alert about somebody's door.

EMAIL_INK = "#17212B"
EMAIL_MUTED = "#5A6673"
EMAIL_RULE = "#DFE5EB"
EMAIL_SUNK = "#F1F4F7"
# The dashboard's own palette, so mail and portal are recognisably one thing.
EMAIL_ACCENT = {
    "calm": "#2A9D8F",    # teal: something happened, nothing is wrong
    "door": "#E9A23B",    # amber: the door was asked to move
    "warn": "#E9A23B",    # amber: act within days; nothing is broken yet
    "alert": "#C4453B",   # red: somebody should look at this today
}
# "door" and "warn" are the same amber on purpose. They mean different things —
# one is an actuation, the other is a battery going flat — and keeping separate
# keys means either can be recoloured without dragging the other with it. An
# unknown accent falls back to "calm" silently, which is why a new one is added
# here rather than passed in hopefully: a low battery rendered teal reads as
# "nothing is wrong".
LOGO_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "petdoor-logo.png")


def _email_text(title, lede, rows, note, cta_url):
    """The plain-text half. Same arguments as the HTML, so they stay in step."""
    out = [title, "=" * len(title), ""]
    if lede:
        out += [lede, ""]
    if rows:
        width = max(len(k) for k, _ in rows)
        out += ["  %-*s : %s" % (width, k, v) for k, v in rows] + [""]
    if note:
        out += [note, ""]
    if cta_url:
        out += [cta_url, ""]
    out += ["--", "PetDoor. Not a security device: Bluetooth advertisements are",
            "unauthenticated, so the door opens for anything broadcasting the",
            "beacon's address."]
    return "\n".join(out)


def _email_html(title, lede, rows, note, cta_url, cta_label, accent):
    colour = EMAIL_ACCENT.get(accent, EMAIL_ACCENT["calm"])
    esc = lambda t: (str(t).replace("&", "&amp;").replace("<", "&lt;")
                     .replace(">", "&gt;").replace('"', "&quot;"))
    font = ("-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,"
            "Arial,sans-serif")

    row_html = ""
    for k, v in rows or []:
        row_html += (
            '<tr>'
            '<td style="padding:7px 14px 7px 0;color:%s;font:13px %s;'
            'white-space:nowrap;vertical-align:top">%s</td>'
            '<td style="padding:7px 0;color:%s;font:600 13px %s;'
            'vertical-align:top">%s</td></tr>'
            % (EMAIL_MUTED, font, esc(k), EMAIL_INK, font, esc(v)))
    table = ("" if not row_html else
             '<table role="presentation" cellpadding="0" cellspacing="0" '
             'style="width:100%%;margin:4px 0 20px;background:%s;'
             'border-radius:8px;padding:6px 16px"><tbody>%s</tbody></table>'
             % (EMAIL_SUNK, row_html))

    button = ("" if not cta_url else
              '<table role="presentation" cellpadding="0" cellspacing="0" '
              'style="margin:4px 0 8px"><tr><td style="background:%s;'
              'border-radius:7px"><a href="%s" style="display:inline-block;'
              'padding:10px 20px;color:#ffffff;font:600 14px %s;'
              'text-decoration:none">%s</a></td></tr></table>'
              % (colour, esc(cta_url), font, esc(cta_label or "Open the dashboard")))

    return """<!doctype html>
<html><body style="margin:0;padding:0;background:#EEF1F4">
<table role="presentation" cellpadding="0" cellspacing="0" style="width:100%%;background:#EEF1F4">
<tr><td align="center" style="padding:24px 12px">
  <table role="presentation" cellpadding="0" cellspacing="0" width="560"
         style="width:560px;max-width:100%%;background:#FFFFFF;border-radius:12px;
                border:1px solid %(rule)s;overflow:hidden">
    <tr><td style="height:4px;background:%(colour)s;font-size:0;line-height:0">&nbsp;</td></tr>
    <tr><td style="padding:20px 28px 0">
      <table role="presentation" cellpadding="0" cellspacing="0"><tr>
        <td style="vertical-align:middle;padding-right:11px">
          <img src="cid:petdoorlogo" width="34" height="34" alt=""
               style="display:block;border:0"></td>
        <td style="vertical-align:middle;color:%(ink)s;font:700 17px %(font)s;
                   letter-spacing:-0.2px">PetDoor</td>
      </tr></table>
    </td></tr>
    <tr><td style="padding:18px 28px 26px">
      <div style="color:%(ink)s;font:700 20px %(font)s;line-height:1.3;
                  margin:0 0 10px">%(title)s</div>
      %(lede)s
      %(table)s
      %(note)s
      %(button)s
    </td></tr>
    <tr><td style="padding:16px 28px 22px;border-top:1px solid %(rule)s;
                   background:#FBFCFD">
      <div style="color:%(muted)s;font:12px %(font)s;line-height:1.55">
        <b>Not a security device.</b> Bluetooth advertisements are unauthenticated,
        so the door opens for anything broadcasting the beacon's address.
      </div>
    </td></tr>
  </table>
</td></tr></table>
</body></html>""" % {
        "rule": EMAIL_RULE, "colour": colour, "ink": EMAIL_INK,
        "muted": EMAIL_MUTED, "font": font, "title": esc(title),
        "lede": ("" if not lede else
                 '<p style="margin:0 0 16px;color:%s;font:15px %s;line-height:1.55">%s</p>'
                 % (EMAIL_INK, font, esc(lede))),
        "table": table,
        "note": ("" if not note else
                 '<p style="margin:0 0 18px;color:%s;font:13px %s;line-height:1.6">%s</p>'
                 % (EMAIL_MUTED, font, esc(note))),
        "button": button,
    }


def send_notification(subject, title, lede="", rows=None, note="",
                      cta_url=None, cta_label=None, accent="calm"):
    """One message, sent as both plain text and HTML. Returns (ok, reason).

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

    if cta_url is None:
        cta_url = DASHBOARD_URL
    msg = EmailMessage()
    msg["Subject"] = f"[petdoor] {subject}"
    msg["From"] = sender
    msg["To"] = NOTIFY_TO
    # Text first, HTML second: a client shows the LAST part it understands.
    msg.set_content(_email_text(title, lede, rows, note, cta_url))
    msg.add_alternative(
        _email_html(title, lede, rows, note, cta_url, cta_label, accent),
        subtype="html")
    # The logo rides inside the HTML part as cid:petdoorlogo. A missing file is
    # not worth failing a notification over; the mail simply goes without it.
    try:
        with open(LOGO_FILE, "rb") as fh:
            msg.get_payload()[1].add_related(
                fh.read(), maintype="image", subtype="png",
                cid="<petdoorlogo>", filename="petdoor.png")
    except OSError:
        pass

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
    verb = command.split()[0].lower() if command.split() else ""
    # Amber for anything that moves or unlocks the door; teal for the rest.
    accent = "door" if verb in ("door", "unlock", "maint") else "calm"
    ok, why = send_notification(
        f"{command} queued for {device}",
        title=command,
        lede="This was queued for your door, and will be applied when it next "
             "calls in.",
        rows=[("door", device),
              ("queued", dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S %Z").strip()),
              ("from", source),
              ("by", who or "not recorded")],
        note="The door collects queued commands when it next calls in, usually "
             "within five minutes, and sooner if the collar is away. Until then "
             "it can still be cancelled from the Controls tab.",
        cta_label="Open the dashboard",
        accent=accent)
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
        # Rows uploaded before the door sent a seventh CSV column keep
        # src = 0, which reads as "not reported" rather than an address.
        _ensure_column(conn, "events", "src", "INTEGER NOT NULL DEFAULT 0")


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
    fresh = []
    with db() as conn:
        for r in rows:
            cur = conn.execute(
                "INSERT OR IGNORE INTO events"
                "(device,epoch,uptime,boot,type,detail,rssi,received,src)"
                " VALUES(?,?,?,?,?,?,?,?,?)",
                (device, r["epoch"], r["uptime"], r["boot"], r["type"],
                 r["detail"], r["rssi"], now, r.get("src", 0)))
            if cur.rowcount:
                fresh.append(r)
            added += cur.rowcount
    # INSERT OR IGNORE means the door can re-upload its whole ring without repeats;
    # only genuinely new rows reach here, so this cannot mail the same failed
    # attempt twice.
    notify_events(device, fresh)
    return added


# How long to stay quiet about the same KIND of event from the same door.
#
# INSERT OR IGNORE already stops the door's repeated ring uploads from mailing
# the same row twice. This is for the other case: a genuinely new row each time,
# arriving over and over because the underlying fault persists. A dead closed-end
# reed produces a fresh STALLED on every close attempt — three per episode, every
# episode, all night — and mailing each one is how the message that mattered ends
# up buried with the rest.
#
# In memory on purpose. A restart re-arms every alert, which is the right way
# round to fail: the worst case is one extra email, not a missed one.
NOTIFY_COOLDOWN_S = int(os.environ.get("PETDOOR_NOTIFY_COOLDOWN_S", "3600"))
_notified_at = {}

# (device, fault code) -> True once a fault email has actually gone out for it.
# The recovery mail is sent only if this says somebody was told about the fault in
# the first place, which is the same shape as _stale_alerted below. Without it, a
# sensor that flaps sends a stream of good news nobody asked for.
_fault_alerted = {}


def notify_due(device, kind):
    """True if we have not mailed about this (door, kind) pair recently."""
    now = time.time()
    key = (device, kind)
    if now - _notified_at.get(key, 0) < NOTIFY_COOLDOWN_S:
        return False
    _notified_at[key] = now
    return True


# Events the door reports that are worth an email on arrival. Kept deliberately
# tiny: the door uploads its whole ring repeatedly, and a chatty rule here is
# how PetDoor mail ends up in a folder nobody opens — at which point the one
# message that mattered is lost with the rest. See notify_command().
def notify_events(device, rows):
    if not NOTIFY_TO:
        return
    for r in rows:
        # A wrong password against the network console. That port can open the
        # door, so a failed attempt is the single event here you would want to
        # hear about the same day rather than next time you open a dashboard.
        if r["type"] == "CONSOLE" and r["detail"] == 0:
            ok, why = send_notification(
                f"wrong console password on {device}",
                title="Wrong password on the door's console",
                lede="Somebody tried the door's network console and gave the "
                     "wrong password.",
                rows=[("door", device),
                      ("uptime", "%ss (boot #%s)" % (r["uptime"], r["boot"])),
                      ("from", ("an address ending .%s on your network" % r["src"])
                               if r.get("src") else
                               "not reported (door firmware predates this)")],
                note="The console only listens during a maintenance window, and "
                     "it drops a client that fails to authenticate. If you were "
                     "not calibrating the door just now, something on your "
                     "network was knocking on it.",
                cta_label="Open the dashboard",
                accent="alert")
            if not ok:
                sys.stderr.write(f"  NOTIFY FAILED for console reject: {why}\n")

        # The beacon's battery went low. The second event here that earns a mail,
        # and it earns it for an unobvious reason: a flat beacon does NOT shut
        # the door. The firmware refuses to act until it has heard the collar
        # once since boot, so a cell that dies overnight leaves the door stuck
        # wherever it was, with the animal on the wrong side and nothing
        # obviously broken. You want days of notice, and a coin cell gives them.
        #
        # Safe to mail because the firmware latches with a recovery margin and
        # INSERT OR IGNORE dedupes the ring: one crossing, one row, one message.
        elif r["type"] == "BEACON_LOW" and r["detail"] == 1:
            mv = r["src"] if "src" in r.keys() and r["src"] else None
            ok, why = send_notification(
                f"beacon battery low on {device}",
                title="The beacon's battery is going flat",
                lede="The door's beacon reported a low battery. Replace the "
                     "cell in the next few days.",
                rows=[("door", device),
                      ("battery", f"{mv} mV" if mv else "below the threshold"),
                      ("uptime", "%ss (boot #%s)" % (r["uptime"], r["boot"]))],
                note="A flat beacon does not close the door on anything — the "
                     "door will not act at all until it has heard the collar "
                     "once since starting up. What it does instead is stop "
                     "working, without looking broken, which is why this is "
                     "worth an email rather than a line in a log. A CR2032 "
                     "reads about 3000 mV fresh.",
                cta_label="Open the dashboard",
                accent="warn")
            if not ok:
                sys.stderr.write(f"  NOTIFY FAILED for beacon battery: {why}\n")

        # A sensor went bad. The third event to earn a mail, and the most
        # urgent of the three.
        #
        # Once the limit switches are fitted the firmware TRUSTS them, so a
        # switch that stops making does not degrade gracefully: every close
        # becomes a STALL, and a stalled close fails OPEN by design. The door
        # then sits open night after night, having done exactly the right thing
        # with wrong information, and nothing about it looks broken. Same shape
        # for a vibration sensor firing at idle — the wake probe reads it as
        # "already moving" and suppresses the actuating press.
        #
        # Latched in the firmware, so this is one message per failure and not
        # one per travel.
        elif r["type"] == "SENSOR_FAULT":
            what, why_txt = SENSOR_FAULT.get(
                r["detail"], (f"fault code {r['detail']}", "unrecognised"))
            ev = sensor_fault_evidence(
                r["detail"], r["src"] if "src" in r.keys() else 0)
            ok, why = send_notification(
                f"sensor fault on {device}",
                title="A door sensor has stopped telling the truth",
                lede=what.capitalize() + ".",
                rows=[("door", device),
                      ("likely cause", why_txt),
                      ("evidence", ev or "n/a"),
                      ("uptime", "%ss (boot #%s)" % (r["uptime"], r["boot"]))],
                note="This is worth looking at today. The door trusts these "
                     "sensors: a limit switch that stops making turns every "
                     "close into a stall, and a stalled close deliberately "
                     "fails OPEN — so the door ends up standing open all night "
                     "having followed its rules correctly on bad information. "
                     "The door keeps working; what it has lost is the ability "
                     "to know whether it did.",
                cta_label="Open the dashboard",
                accent="alert")
            # Record that somebody WAS told, which is what entitles the
            # recovery below to mail. A fault nobody heard about needs no
            # all-clear.
            if ok:
                _fault_alerted[(device, r["detail"])] = True
            else:
                sys.stderr.write(f"  NOTIFY FAILED for sensor fault: {why}\n")

        # The door has STOPPED TRYING TO CLOSE and is standing open until a
        # person deals with it. The most important thing this system produces:
        # every other event is a record of something that happened, and this one
        # is a request. No cooldown — it is terminal and it is rare, and a door
        # that gave up twice in one night is a door you want told about twice.
        # A door that fell over and came back.
        #
        # Deliberately NOT behind notify_due(): repeated crashes are the signal,
        # not noise. The history that forced the NimBLE default was three
        # consecutive reset=4 boots, and a cooldown would have hidden two of them
        # — which is precisely the information that identified the cause.
        elif r["type"] == "BOOT" and r["detail"] in ABNORMAL_RESET:
            reason = RESET_REASON.get(r["detail"], f'reset {r["detail"]}')
            ok, why = send_notification(
                f"{device} restarted unexpectedly ({reason})",
                title="A door fell over and came back",
                lede=f"{device} came up on boot #{r['boot']} after {reason}. It is "
                     "running again, so this is a report rather than an outage — "
                     "but nothing asked it to restart.",
                rows=[("door", device),
                      ("reset reason", reason),
                      ("uptime", "%ss (boot #%s)" % (r["uptime"], r["boot"]))],
                note=RESET_ADVICE.get(r["detail"], "Unrecognised reset reason."),
                cta_label="Open the dashboard",
                accent="alert")
            if not ok:
                sys.stderr.write(f"  NOTIFY FAILED for abnormal boot: {why}\n")

        # The same sensor telling the truth again. Only mailed if the fault
        # itself was mailed: being told a door is broken and never told it
        # recovered is how somebody drives out to a coop for nothing.
        elif (r["type"] == "SENSOR_OK"
              and _fault_alerted.pop((device, r["detail"]), False)):
            what, _why = SENSOR_FAULT.get(
                r["detail"], (f"fault code {r['detail']}", "unrecognised"))
            ok, why = send_notification(
                f"sensor recovered on {device}",
                title="That door sensor is reporting again",
                lede="It is no longer true that " + what + ".",
                rows=[("door", device),
                      ("uptime", "%ss (boot #%s)" % (r["uptime"], r["boot"]))],
                note="Nothing needs doing. This is the counterpart to the fault "
                     "mail you had earlier, sent so a door that fixed itself does "
                     "not leave you assuming it is still broken. A sensor that "
                     "recovers and fails repeatedly is worth a look even so.",
                accent="calm")
            if not ok:
                sys.stderr.write(f"  NOTIFY FAILED for sensor recovery: {why}\n")

        elif r["type"] == "UNCOMMANDED" and notify_due(device, "UNCOMMANDED"):
            # THE DOOR IS NOT WHERE IT WAS LEFT, and that deserves saying even
            # though the door also takes manual control when it happens.
            #
            # This used to be deliberately silent, on the grounds that "most are
            # a hand on the door". The hole was that enterOverride() is guarded
            # by `if (!g_override)`: a hand-move on a door ALREADY under manual
            # control writes no MANUAL event, so it produced no mail at all. On
            # 10 Oct that is exactly what happened — control had been taken at
            # the console seven minutes earlier — and the only mail that arrived
            # was a sensor fault blaming the hardware for noticing.
            #
            # Rate-limited, unlike the MANUAL mail, because this one CAN repeat
            # on its own: the vendor controller was watched leaving the open
            # limit twice, about fifteen seconds after arriving, with nothing
            # driving it.
            d = r["detail"]
            inferred = d >= 10
            where = {1: "OPEN", 2: "CLOSED"}.get(d - 10 if inferred else d,
                                                 f"state {d}")
            if inferred:
                ms = r["src"] if "src" in r.keys() and r["src"] else 0
                # Invariant 20: an inference and a measurement are not the same
                # claim, and an email that renders them identically turns one
                # into the other.
                how = ("INFERRED from %.1f s of movement — no limit switch saw "
                       "it" % (ms / 1000.0)) if ms else \
                      "INFERRED from the duration of the movement"
            else:
                how = "a limit switch MEASURED it"
            ok, why = send_notification(
                f"door moved to {where} with nothing commanding it on {device}",
                title="This door is not where it was left",
                lede=f"It moved to {where} and nothing asked it to. On this door "
                     "that means a hand: the flap is a motorised vertical panel, "
                     "so wind cannot blow it and an animal cannot push it.",
                rows=[("door", device),
                      ("now", where),
                      ("evidence", how),
                      ("uptime", "%ss (boot #%s)" % (r["uptime"], r["boot"]))],
                note="The sensors are working — this is them doing their job. "
                     "The door has also taken itself under MANUAL control, so "
                     "nothing automatic will move it until `door auto`. Further "
                     "uncommanded moves stay quiet for an hour so a controller "
                     "with a mode of its own cannot flood your inbox.",
                accent="bad")
            if not ok:
                sys.stderr.write(f"  NOTIFY FAILED for uncommanded move: {why}\n")

        elif r["type"] == "MANUAL" and r["detail"] == 1:
            # Held CLOSED earns a mail on arrival with NO cooldown, for the same
            # reason an abnormal reset does: the cost lands on an animal that
            # cannot report it, and unlike every other state this door can be
            # left in, this one never expires.
            # Say HOW, when the door told us. A door moved BY HAND is a
            # different message from one somebody opened on the dashboard, and
            # conflating them is what let a hand on the door get reported as a
            # sensor that had "stopped telling the truth".
            how = override_source(r["src"] if "src" in r.keys() else 0)
            by_hand = how is not None and r["src"] == 3
            rows = [("door", device),
                    ("uptime", "%ss (boot #%s)" % (r["uptime"], r["boot"]))]
            if how:
                rows.insert(1, ("how", how[0]))
            ok, why = send_notification(
                f"door is under MANUAL control on {device}",
                title=("This door was moved by hand"
                       if by_hand else "Somebody took control of this door"),
                lede=(("The door is no longer where it was left — a travel "
                       "nothing commanded. It is now under MANUAL control, so "
                       "nothing automatic will move it: not the collar, not the "
                       "close dwell, not the schedule. This survives a reboot.")
                      if by_hand else
                      ("Nothing automatic will open or close it — not the "
                       "collar, not the close dwell, not the schedule — and "
                       "this survives a reboot.")),
                rows=rows,
                note=(("The sensors saw this: a limit switch measured it, or "
                       "the vibration sensor felt the travel. Nothing is wrong "
                       "with the hardware. Send `door auto` to hand the door "
                       "back, and you will be reminded while it stays set.")
                      if by_hand else
                      ("Send `door auto` to hand it back. You will be reminded "
                       "while it stays set, because manual control does not end "
                       "by itself the way a maintenance window does.")),
                accent="bad")
            if not ok:
                sys.stderr.write(f"  NOTIFY FAILED for hold closed: {why}\n")

        elif (r["type"] == "MANUAL" and r["detail"] == 0
              and notify_due(device, "MANUAL_RELEASED")):
            ok, why = send_notification(
                f"manual control released on {device}",
                title="The door is back under automatic control",
                lede="The collar decides again.",
                rows=[("door", device),
                      ("uptime", "%ss (boot #%s)" % (r["uptime"], r["boot"]))],
                note="Sent so the reminders stopping is explained rather than "
                     "just noticed.")
            if not ok:
                sys.stderr.write(f"  NOTIFY FAILED for hold open: {why}\n")

        elif r["type"] == "GAVE_UP":
            ok, why = send_notification(
                f"door is staying OPEN on {device}",
                title="The door has given up closing and is standing open",
                lede=f"{r['detail']} close attempts stalled in a row, so the door "
                     "has stopped trying and is deliberately staying open.",
                rows=[("door", device),
                      ("attempts", r["detail"]),
                      ("uptime", "%ss (boot #%s)" % (r["uptime"], r["boot"]))],
                note="This is deliberate, not a crash. A door that stops partway "
                     "while closing is exactly when something may be underneath "
                     "it, so the firmware reverses and retries a few times and "
                     "then stops rather than driving onto whatever is in the way. "
                     "It will not close again by itself: clear the obstruction "
                     "and press 'x' on the console, or send `door close`. Until "
                     "then the doorway is open.",
                cta_label="Open the dashboard",
                accent="alert")
            if not ok:
                sys.stderr.write(f"  NOTIFY FAILED for gave-up: {why}\n")

        # A travel started and never arrived. Rate limited, because a persistent
        # cause produces one of these per attempt — and the attempts that follow
        # end in GAVE_UP above, which is not rate limited and says more.
        elif r["type"] == "STALLED" and notify_due(device, "STALLED"):
            going = {1: "opening", 2: "closing"}.get(r["detail"], "moving")
            ok, why = send_notification(
                f"door stalled while {going} on {device}",
                title=f"The door stalled while {going}",
                lede="It started moving and never reached the other end.",
                rows=[("door", device),
                      ("direction", going),
                      ("uptime", "%ss (boot #%s)" % (r["uptime"], r["boot"]))],
                note="An obstruction, a jam, or a limit switch that has stopped "
                     "making. A stalled CLOSE is reversed automatically and "
                     "retried after a delay; if the retries run out you will get "
                     "a second message saying the door is staying open. Further "
                     "stalls on this door are suppressed for an hour so a "
                     "persistent fault cannot bury that one.",
                cta_label="Open the dashboard",
                accent="alert")
            if not ok:
                sys.stderr.write(f"  NOTIFY FAILED for stall: {why}\n")


# ---------------------------------------------------------------- watchdog
#
# THE ONE FAILURE NOTHING ELSE CAN CATCH.
#
# Every other alert in this file is triggered by the door SENDING something. A
# door that has lost WiFi, browned out, or whose ESP32 has died sends nothing by
# definition — so the quieter it gets, the less this system has to say about it.
# Silence looked exactly like a door with nothing to report.
#
# So this watches the absence instead. It is the only check that runs without
# the door's participation, which is precisely why it is the one worth having.
#
# The threshold has to clear the door's own heartbeat with room to spare: a
# healthy door calls in every WIFI_HEARTBEAT_MS (30 minutes by default) even
# with nothing happening, and misses one now and then to a busy radio or an OTA
# window. Two hours is roughly four missed heartbeats — long enough not to cry
# wolf over a single one, short enough to tell you the same evening.
STALE_AFTER_S = int(os.environ.get("PETDOOR_STALE_AFTER_S", "7200"))
WATCHDOG_EVERY_S = 300

# device -> True once we have mailed about this outage. Cleared when it comes
# back, so a door that flaps gets one message per outage rather than per check.
_stale_alerted = {}


def check_overridden_doors():
    """Re-announce every door still pinned CLOSED.

    The arrival mail is not enough by itself. A hold does not expire, so the
    only thing between "I will release it this evening" and an animal locked out
    for a fortnight is something that keeps saying so. Driven off the uploaded
    status line rather than the event, because the event happened once and may
    be weeks old by the time it matters.
    """
    if not NOTIFY_TO:
        return
    with db() as conn:
        rows = conn.execute(
            "SELECT device, status FROM devices WHERE status IS NOT NULL").fetchall()
    for r in rows:
        # Padded on both sides so this cannot match `hold=2` inside some future
        # field, nor `sfault=2`.
        if " ovr=1" not in f' {r["status"] or ""} ':
            continue
        if not notify_due(r["device"], "MANUAL_REMINDER"):
            continue
        send_notification(
            f'door is STILL under manual control on {r["device"]}',
            title="That door is still being held by hand",
            lede="Nothing automatic will open or close it. This does not "
                 "release itself.",
            rows=[("door", r["device"])],
            note="Send `door auto` when you are done with it.",
            accent="bad")


def check_stale_doors():
    """One pass. Mails for doors that have gone quiet, and for their return."""
    if not NOTIFY_TO:
        return
    now = int(time.time())
    with db() as conn:
        rows = conn.execute(
            "SELECT device, last_seen, version FROM devices").fetchall()
    for r in rows:
        dev, seen = r["device"], r["last_seen"] or 0
        if not seen:
            continue          # never reported at all; nothing to be silent about
        quiet = now - seen
        if quiet >= STALE_AFTER_S and not _stale_alerted.get(dev):
            _stale_alerted[dev] = True
            hrs = quiet / 3600.0
            ok, why = send_notification(
                f"{dev} has stopped reporting",
                title="A door has gone quiet",
                lede=f"Nothing has been heard from {dev} for {hrs:.1f} hours.",
                rows=[("door", dev),
                      ("last heard", time.strftime("%Y-%m-%d %H:%M UTC",
                                                   time.gmtime(seen))),
                      ("firmware", r["version"] or "unknown")],
                note="A healthy door calls in on a heartbeat even when nothing "
                     "is happening, so silence this long means it is not "
                     "running, not on the network, or not reaching this server. "
                     "The door may still be working the collar perfectly well — "
                     "it decides entirely on its own and never needs this "
                     "server — but nothing can be seen or changed remotely "
                     "until it comes back. Check power first, then WiFi.",
                cta_label="Open the dashboard",
                accent="alert")
            if not ok:
                sys.stderr.write(f"  NOTIFY FAILED for stale door {dev}: {why}\n")
        elif quiet < STALE_AFTER_S and _stale_alerted.get(dev):
            _stale_alerted[dev] = False
            ok, why = send_notification(
                f"{dev} is reporting again",
                title="The door is back",
                lede=f"{dev} has started calling in again.",
                rows=[("door", dev), ("firmware", r["version"] or "unknown")],
                note="Nothing needs doing. This closes out the earlier message "
                     "about it having gone quiet.",
                cta_label="Open the dashboard",
                accent="calm")
            if not ok:
                sys.stderr.write(f"  NOTIFY FAILED for stale recovery {dev}: {why}\n")


def watchdog_loop():
    # Daemon: it must never hold the process open on shutdown. A failure in here
    # must not take the server with it either — a crashed watchdog would stop
    # the ingest endpoint, which is far worse than a missed alert.
    while True:
        time.sleep(WATCHDOG_EVERY_S)
        try:
            check_stale_doors()
            # In the same sweep, and inside the same try: a hold that cannot
            # expire is exactly the sort of thing that must not stop being
            # announced because the other check threw.
            check_overridden_doors()
        except Exception as exc:                        # noqa: BLE001
            sys.stderr.write(f"  watchdog error (continuing): {exc}\n")


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
                         "type": p[3].strip()[:16], "detail": int(p[4]), "rssi": int(p[5]),
                         # Seventh column, added later. A door on older firmware
                         # sends six, and reads as "not reported".
                         "src": int(p[6]) if len(p) > 6 else 0})
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

    cls = {"OPEN": "i", "CLOSE": "o", "REFUSED": "b", "CONSOLE": "b",
           "NO_MOVE": "b", "STALLED": "b", "UNCOMMANDED": "b", "GAVE_UP": "b",
           "RETRY": "b", "WAKE": "s", "BEACON_LOW": "b", "SENSOR_FAULT": "b",
           "SENSOR_OK": "i"}
    out = []
    for r in rows:
        when = (datetime.fromtimestamp(r["epoch"], timezone.utc).strftime("%Y-%m-%d %H:%M")
                if r["epoch"] else f'boot {r["boot"]} +{r["uptime"]}s')
        detail = ""
        if r["type"] in ("OPEN", "CLOSE"):
            # detail is the ActuationSource (petdoor/door.h). Without this the
            # Detail column was blank for every door movement, so a log full of
            # correct beacon-driven travels read as though nothing had attributed
            # them — and a fail-safe reversal after a stalled close rendered
            # identically to an ordinary open, which is the same mistake the
            # UNCOMMANDED branch below exists to avoid.
            detail = ACT_SOURCE.get(r["detail"], f'source {r["detail"]}')
            if r["detail"] == 3:
                detail = f'<strong style="color:var(--bad)">{detail}</strong>'
        elif r["type"] == "BOOT":
            detail = RESET_REASON.get(r["detail"], f'reset {r["detail"]}')
            if r["detail"] == 9:
                detail = f'<strong style="color:var(--bad)">{detail}</strong>'
        elif r["type"] == "REFUSED":
            # 1-6 are ActuationResult; 100 is the scheduled lockout, which is
            # deliberately outside that enum so adding to it cannot re-label
            # history already in the database.
            detail = {1: "already there", 2: "too soon", 3: "boot grace",
                      4: "already travelling", 5: "waiting to retry a failed close",
                      6: "gave up closing — staying open",
                      100: "scheduled lockout"}.get(
                r["detail"], f'reason {r["detail"]}')
        elif r["type"] in ("OPEN", "CLOSE"):
            # Who asked, and how much the door actually knows about what it
            # did. An entry with no "verified" is an intention, not a record.
            bits = [ACTUATION_SOURCE.get(r["detail"], f'source {r["detail"]}')]
            flags = r["src"] if "src" in r.keys() and r["src"] else 0
            bits += [name for mask, name in ACTUATION_FLAGS if flags & mask]
            if not flags & 0x0001:
                bits.append("assumed — no switch confirmed it")
            detail = html.escape(", ".join(bits))
        elif r["type"] == "GAVE_UP":
            detail = ('<strong style="color:var(--bad)">'
                      f'{r["detail"]} close attempts stalled; the door is staying OPEN'
                      "</strong>")
        elif r["type"] == "UNCOMMANDED":
            # detail is the end the door reached. 1/2 mean a limit switch
            # MEASURED it; 11/12 mean it was INFERRED from how long the
            # vibration sensor felt the door moving, on a door whose switches
            # could not see the travel. Rendered differently on purpose: an
            # inference and a measurement are not the same claim, and showing
            # them identically is how one quietly becomes the other.
            d = r["detail"]
            inferred = d >= 10
            where = {1: "open", 2: "closed"}.get(d - 10 if inferred else d,
                                                 f"state {d}")
            if inferred:
                ms = r["src"] if "src" in r.keys() and r["src"] else 0
                how = ("inferred from %.1f s of movement — no limit switch saw it"
                       % (ms / 1000.0)) if ms else \
                      "inferred from the duration of the movement"
            else:
                how = "a limit switch saw it"
            detail = ('<strong style="color:var(--bad)">'
                      f"the door moved to {where} and nothing commanded it"
                      f'</strong> <span class="muted">({how})</span>')
        elif r["type"] == "RETRY":
            detail = f'attempt {r["detail"]} — the previous press moved nothing'
        elif r["type"] == "WAKE":
            detail = ("the wake press moved the door by itself"
                      if r["detail"] == 1 else "a wake press was needed")
        elif r["type"] == "SENSOR_FAULT":
            what, why = SENSOR_FAULT.get(
                r["detail"], (f"fault {r['detail']}", "unrecognised code"))
            ev = sensor_fault_evidence(
                r["detail"], r["src"] if "src" in r.keys() else 0)
            detail = ('<strong style="color:var(--bad)">' + html.escape(what)
                      + "</strong> — " + html.escape(why)
                      + (" (" + html.escape(ev) + ")" if ev else ""))
        elif r["type"] == "SENSOR_OK":
            what, _ = SENSOR_FAULT.get(r["detail"],
                                       (f"fault {r['detail']}", "unrecognised code"))
            detail = ("recovered — no longer true that " + html.escape(what))
        elif r["type"] == "BEACON_LOW":
            # The millivolts ride in the spare field; detail is the direction.
            mv = r["src"] if "src" in r.keys() and r["src"] else None
            where = f" ({mv} mV)" if mv else ""
            if r["detail"] == 1:
                detail = ('<strong style="color:var(--bad)">'
                          f"LOW{where} — replace the beacon battery</strong>")
            else:
                detail = f"back above the threshold{where}"
        elif r["type"] == "NO_MOVE":
            # The relay fired and nothing moved. Louder than STALLED, which at
            # least means the door tried.
            detail = ('<strong style="color:var(--bad)">relay fired, door never moved</strong>')
        elif r["type"] == "MANUAL":
            if r["detail"] == 1:
                # The spare column says HOW. Rows written before the firmware
                # carried it have 0 and must stay unannotated rather than being
                # attributed to a channel that was never recorded.
                how = override_source(r["src"] if "src" in r.keys() else 0)
                detail = ('<strong style="color:var(--bad)">a person took '
                          "control — nothing automatic will move this door</strong>")
                if how:
                    detail += f' <span class="muted">({how[0]})</span>'
            else:
                detail = "released — the door decides for itself again"
        elif r["type"] == "MAINT":
            detail = {0: "expired", 1: "started", 2: "ended"}.get(
                r["detail"], f'detail {r["detail"]}')
        elif r["type"] == "CONSOLE":
            # A wrong password against a port that can open the door is the one
            # entry here worth making loud; the rest are ordinary bookkeeping.
            detail = {1: "attached", 2: "refused, already in session",
                      3: "no password given"}.get(r["detail"])
            if detail is None:
                detail = ('<strong style="color:var(--bad)">WRONG PASSWORD</strong>'
                          if r["detail"] == 0 else f'detail {r["detail"]}')
            # Rendered as ".188" rather than a bare number: it is the last
            # octet of an address, and the door cannot send the other three.
            src = r["src"] if "src" in r.keys() else 0
            if src:
                detail += f' from .{src}'
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
        # The dashboard's and the demo's CSS and JS, lifted out of the pages so a
        # Content-Security-Policy can ENFORCE rather than merely report. An
        # inline <script> cannot be allowed by a strict policy without
        # 'unsafe-inline', which would permit every injected script too and make
        # the header decorative.
        #
        # The content is byte-identical to what was inline; only the delivery
        # changed. Access follows the page it belongs to: the proxy's `/demo*`
        # prefix keeps demo.css/js public, and dashboard.css/js fall into the
        # private catch-all with /dashboard itself. Neither contains data — they
        # are the same code that is public in the GitHub repo — but there is no
        # reason to widen the split for them.
        if path in ("/dashboard.css", "/dashboard.js",
                    "/demo.css", "/demo.js"):
            name = path.lstrip("/")
            ctype = ("text/css; charset=utf-8" if name.endswith(".css")
                     else "text/javascript; charset=utf-8")
            sent = self._serve_file(name, ctype)
            if sent is not None:
                return sent
            return self._send(404, "not found")
        if path.startswith("/assets/"):
            # The shared theme and the public page's own CSS, JS and diagrams.
            # PUBLIC: the proxy lists /assets/* with the other public routes,
            # because the project page needs them before anyone has signed in.
            # Nothing here is data — it is the same code that is in the repo.
            # basename() strips traversal, and the extension list is explicit.
            name = os.path.basename(path[len("/assets/"):])
            root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets")
            full = os.path.join(root, name)
            types = {".css": "text/css; charset=utf-8",
                     ".js": "text/javascript; charset=utf-8",
                     ".svg": "image/svg+xml"}
            ext = os.path.splitext(name)[1].lower()
            if name and ext in types and os.path.isfile(full):
                with open(full, "rb") as fh:
                    return self._send(200, fh.read(), types[ext])
            return self._send(404, "not found")
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
                # src carries an inferred UNCOMMANDED's duration (ms), which the
                # dashboard needs to show an inference as one. See invariant 20.
                rows = conn.execute("SELECT device,epoch,uptime,boot,type,detail,rssi,src "
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
    # Line-buffer stdout. In a container stdout is a pipe, so Python
    # block-buffers it and the startup banner — ports, key state, the web-control
    # warning, the watchdog line — sits unflushed for hours while the request log
    # on stderr appears immediately. The result is a server that looks like it
    # never printed its configuration. It cost a deployment's worth of doubt
    # about whether the watchdog thread had started at all.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except Exception:                                   # noqa: BLE001
        pass                                            # not a tty-like stream

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
    if NOTIFY_TO:
        threading.Thread(target=watchdog_loop, daemon=True).start()
        print(f"  watchdog  : mails if a door goes quiet for "
              f"{STALE_AFTER_S // 3600}h (checked every {WATCHDOG_EVERY_S // 60}m)")
    else:
        print("  watchdog  : off (no PETDOOR_NOTIFY_TO configured)")

    with Server((args.host, args.port), Handler) as srv:
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped")


if __name__ == "__main__":
    main()
