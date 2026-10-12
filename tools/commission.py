#!/usr/bin/env python3
"""Functional commissioning test for a PetDoor, over the serial console.

Walks every function the door has, says what it is about to test, and either
checks the answer itself or asks you — because half of what this door does is
a sound or a flash of light, and no amount of software can hear a buzzer.

    tools/commission.py                       # all of it, with prompts
    tools/commission.py --port /dev/cu.X      # a different adapter
    tools/commission.py --no-move             # skip everything that drives the motor
    tools/commission.py --only lock,mute      # just these groups
    tools/commission.py --list                # what groups exist

WHY THIS EXISTS, AND WHY IT IS NOT A UNIT TEST
----------------------------------------------
tests/host/ covers the pure logic and tests/server/ covers the alerting. What
neither can reach is the half of this project that is a relay, a reed, a magnet
and a buzzer. Those have been tested by hand, from memory, differently each
time — and the morning that cost most was a closed reed that made when pressed
by hand and not when driven by the motor, which nobody thought to check because
nobody had a list.

This is the list.

RUN IT WITH A CABLE ATTACHED. The console is immediate and gives the full
command set; the remote path takes up to five minutes per command, which turns
a twenty-step suite into an afternoon. Two checks (mute, unmute) have no console
key at all and are therefore prompted rather than driven — see MUTE below.

SAFETY
------
Movement tests drive a real motor on a real door. They are quarantined behind
their own confirmation, they never run under --no-move, and the suite restores
the door to automatic control when it finishes or is interrupted. Do not run the
movement group with an animal in the doorway.
"""

import argparse
import os
import re
import subprocess
import sys
import time

# ---------------------------------------------------------------------------
# The console, held open for the whole run
# ---------------------------------------------------------------------------
#
# ONE file descriptor for the entire suite, which is not an optimisation.
# macOS resets termios when the last descriptor closes, so a stty followed by a
# separate read silently reverts to 9600 and returns garbage — a whole session
# was once lost to a "silent board" that was only being listened to at the
# wrong baud rate. And there is no flow control, so anything the door prints
# while no descriptor is open is simply gone: a travel verdict that lands
# between two steps would vanish. Hence one descriptor, opened once.


class Console:
    def __init__(self, port, verbose=False):
        self.port = port
        self.verbose = verbose
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        # min 0 so a read on a quiet port returns instead of blocking forever;
        # time 10 is a one-second ceiling per read. Raising min to buffer
        # fuller reads makes every read block until the port speaks again,
        # which hangs the moment the output stops.
        subprocess.run(
            ["stty", "-f", port, "115200", "cs8", "-parenb", "-cstopb",
             "-crtscts", "min", "0", "time", "10"],
            check=True)
        self.buf = ""

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass

    def drain(self, seconds=1.0):
        """Read and discard, to start a step from a known-quiet point."""
        self.read_for(seconds)
        self.buf = ""

    def read_for(self, seconds):
        """Read for a WALL-CLOCK duration and return what arrived.

        Paced by the clock, never by a loop count. This console emits
        [wifi]/[fix] lines every few seconds, so a read that finds data returns
        at once and only a silent read burns its timeout — which means N
        iterations is emphatically not N seconds. A run of five calibration
        passes was once desynchronised exactly this way.
        """
        out = []
        end = time.time() + seconds
        while time.time() < end:
            try:
                chunk = os.read(self.fd, 8192)
            except OSError:
                break
            if chunk:
                text = chunk.decode("utf-8", "replace")
                out.append(text)
                if self.verbose:
                    sys.stdout.write(text)
                    sys.stdout.flush()
        got = "".join(out)
        self.buf += got
        return got

    def await_text(self, needles, seconds, quiet=False):
        """Wait up to `seconds` for any of `needles`. Returns the one seen, or None.

        Everything read along the way stays in self.buf, so a later check can
        still find a line that arrived while we were waiting for something else.
        """
        if isinstance(needles, str):
            needles = [needles]
        end = time.time() + seconds
        seen = self.buf
        while time.time() < end:
            for n in needles:
                if n in seen:
                    return n
            seen += self.read_for(0.5)
        for n in needles:
            if n in seen:
                return n
        return None

    def key(self, ch, settle=1.5):
        """Send ONE top-level key, with NO carriage return.

        Top-level keys are single characters and several of them move the door:
        the 'o' in a word typed at the top level will pulse the OPEN relay. A
        carriage return after a SUBMENU key is worse than useless — the CR is
        the empty line that exits the submenu, so 'w\\r' opens the timing menu
        and immediately cancels it, and everything typed next is interpreted as
        top-level keystrokes. That is how `vibration 33` once became 'r' then
        't' on a live door.
        """
        os.write(self.fd, ch.encode())
        return self.read_for(settle)

    def line(self, text, settle=2.0):
        """Send a LINE, which is only valid inside a submenu."""
        os.write(self.fd, (text + "\r").encode())
        return self.read_for(settle)

    def status(self, seconds=6.0):
        """Send 's' and parse the status block into a dict.

        's' is the one harmless key, and its output identifies which level the
        console is at — which is why every step that sends text confirms with
        it first.
        """
        self.buf = ""
        os.write(self.fd, b"s")
        text = ""
        end = time.time() + seconds
        while time.time() < end:
            text += self.read_for(0.5)
            if "---- status ----" in text and "free heap" in text:
                break
        fields, flags = parse_status(text)
        return fields, flags, text


# ---------------------------------------------------------------------------
# Parsing, kept out of the Console so it can be tested with no board attached
# ---------------------------------------------------------------------------
#
# Worth separating for a dull reason: the one time this script is most needed is
# when the door is misbehaving, which is exactly when the serial line may be the
# thing that is broken. --selftest exercises these against captured blocks so a
# parser bug cannot masquerade as a hardware fault.


def parse_status(text):
    """(fields, flags) from a status block. Tolerant of partial captures."""
    fields = {}
    flags = []
    for raw in text.splitlines():
        ln = raw.rstrip()
        m = re.match(r"\s{2}([A-Za-z][A-Za-z0-9 /]*?)\s*:\s*(.*)$", ln)
        if m:
            fields[m.group(1).strip()] = m.group(2).strip()
        # The block also carries bare state banners with no colon value.
        for word in ("LOCKED", "MANUAL", "MUTED", "MAINTENANCE"):
            if re.match(r"\s{2}%s\s" % word, ln):
                flags.append(word)
    return fields, flags


def switches_from(sw):
    """(open_pin, open_made, closed_pin, closed_made) from the switches value."""
    o = re.search(r"open GPIO (\d+)\s+(MADE|open)", sw or "")
    c = re.search(r"closed GPIO (\d+)\s+(MADE|open)", sw or "")
    return (o.group(1) if o else None, bool(o and o.group(2) == "MADE"),
            c.group(1) if c else None, bool(c and c.group(2) == "MADE"))


# ---------------------------------------------------------------------------
# Result bookkeeping
# ---------------------------------------------------------------------------

PASS, FAIL, SKIP, WARN = "PASS", "FAIL", "SKIP", "WARN"
results = []
measurements = []          # (label, value) worth reading back as numbers
_order = []                # groups in the order they actually ran


def record(group, name, outcome, detail=""):
    results.append((group, name, outcome, detail))
    if group not in _order:
        _order.append(group)
    mark = {PASS: "  ok  ", FAIL: " FAIL ", SKIP: " skip ", WARN: " warn "}[outcome]
    print(f"   [{mark}] {name}" + (f" — {detail}" if detail else ""))


def measure(label, value):
    """A number worth collecting into the summary rather than only printing."""
    measurements.append((label, value))


def say(text):
    print(f"\n=== {text}")


def ask(question):
    """A yes/no the software cannot answer for itself."""
    while True:
        a = input(f"   ?  {question} [y/n/s=skip] ").strip().lower()
        if a in ("y", "yes"):
            return True
        if a in ("n", "no"):
            return False
        if a in ("s", "skip"):
            return None


def pause(text):
    input(f"   >  {text} (press Enter) ")


# ---------------------------------------------------------------------------
# Groups
# ---------------------------------------------------------------------------


def group_console(con, args):
    say("CONSOLE — is the door talking at all?")
    fields, flags, text = con.status()
    if "---- status ----" not in text:
        record("console", "console responds to 's'", FAIL,
               "no status block; check the TTL wiring and that TX/RX are not swapped")
        return False
    record("console", "console responds to 's'", PASS)
    fw = fields.get("firmware", "?")
    record("console", "firmware reported", PASS if fw != "?" else FAIL, fw)
    record("console", "BLE stack", PASS, fields.get("ble stack", "?"))
    boot = fields.get("boot", "?")
    record("console", "boot counter", PASS, boot)
    if "BROWNOUT" in text or "brownout" in text:
        record("console", "no brownout banner", FAIL,
               "the supply sagged: thicker cable, stronger 5V, or feed the "
               "relay coils separately with a common ground")
    return True


def group_sensors(con, args):
    say("SENSORS — reeds and vibration, with the door left alone")
    fields, flags, text = con.status()

    sw = fields.get("switches")
    if not sw:
        record("sensors", "limit switches configured", WARN,
               "none fitted — the door is open loop and only knows what it commanded")
    else:
        record("sensors", "limit switches configured", PASS, sw)
        pos = fields.get("position", "?")
        if pos.startswith("UNKNOWN"):
            record("sensors", "position is measurable", FAIL,
                   "neither switch is made. Either a reed/magnet is detached, "
                   "or the panel is parked between the ends. At rest those are "
                   "indistinguishable — run the 'reeds' group, which puts the "
                   "panel on each stop by hand and names the faulty one")
        else:
            record("sensors", "position is measurable", PASS, pos)
        # A reed that reads MADE at both ends at once is physically impossible.
        if "MADE" in sw and sw.count("MADE") > 1:
            record("sensors", "switches not contradicting", FAIL,
                   "both made at once: a shorted wire, a stuck switch or a stray magnet")
        else:
            record("sensors", "switches not contradicting", PASS)

    vib = fields.get("vibration")
    if not vib:
        record("sensors", "vibration sensor configured", WARN, "none fitted")
    else:
        record("sensors", "vibration sensor configured", PASS, vib)
        # Quiet AT REST is the whole question. A sensor firing with the door
        # still is not merely noisy: the wake probe reads it as "already
        # moving" and suppresses the real press, so the door stops responding.
        m = re.search(r"(\d+)\s+edges", vib)
        before = int(m.group(1)) if m else None
        if before is None:
            record("sensors", "vibration quiet at rest", WARN, "could not read the edge count")
        else:
            print("      watching the edge count for 12 s with the door untouched...")
            con.read_for(12.0)
            f2, _fl, _t = con.status()
            m2 = re.search(r"(\d+)\s+edges", f2.get("vibration", ""))
            after = int(m2.group(1)) if m2 else before
            delta = after - before
            if delta == 0:
                record("sensors", "vibration quiet at rest", PASS, "0 new edges in 12 s")
            elif delta < 50:
                record("sensors", "vibration quiet at rest", WARN,
                       f"{delta} edges in 12 s — slight; watch for SF_VIBRATION_NOISY")
            else:
                record("sensors", "vibration quiet at rest", FAIL,
                       f"{delta} edges in 12 s with the door still. Back off the "
                       "sensitivity screw, or move it — a sensor on the frame "
                       "feels the world, not the door")

    if "SENSOR" in text and "fault" in text:
        record("sensors", "no latched sensor fault", FAIL,
               "see the !! SENSOR line in 's'")
    else:
        record("sensors", "no latched sensor fault", PASS)


def _read_switches(con):
    """(open_pin, open_made, closed_pin, closed_made) from the status block.

    The status line reads e.g.
        switches     : open GPIO 32 MADE, closed GPIO 25 open
    """
    fields, _flags, _t = con.status()
    sw = fields.get("switches", "")
    op, om, cp, cm = switches_from(sw)
    return op, om, cp, cm, sw


def group_reeds(con, args):
    """WHICH reed is broken — by hand, so the motor is not part of the answer.

    At rest this is unanswerable: two reeds both reading "open" means either a
    dead reed or a panel parked between the ends, and nothing in the firmware
    can distinguish those. The only way is to put the panel somewhere KNOWN and
    ask the switch what it thinks.

    Doing it by hand rather than with the motor is the point. On 2026-10-10 the
    closed reed made perfectly when the panel was pressed home and never made
    when the motor drove it — so the reed was fine and the fault was where the
    motor parks the panel. A test that used the motor would have blamed the
    reed; this one tells the two apart.
    """
    say("REEDS — which switch, tested by hand so the motor is not involved")
    o_pin, _om, c_pin, _cm, sw = _read_switches(con)
    if not o_pin and not c_pin:
        record("reeds", "limit switches configured", SKIP,
               f"none fitted or unparseable: {sw!r}")
        return
    print(f"      open reed on GPIO {o_pin}, closed reed on GPIO {c_pin}")
    print("      Move the panel BY HAND for each step. Do not use the motor.")

    verdict = {}
    for end, pin, label in (("open", o_pin, "OPEN"), ("closed", c_pin, "CLOSED")):
        if not pin:
            record("reeds", f"{end} reed (not fitted)", SKIP,
                   "single-switch builds are legal")
            continue
        go = ask(f"Push the panel fully onto its {label} stop and hold it there. Ready?")
        if go is None or go is False:
            record("reeds", f"{end} reed makes at its stop", SKIP)
            continue
        _op, om, _cp, cm, sw2 = _read_switches(con)
        mine, theirs = (om, cm) if end == "open" else (cm, om)
        other = "closed" if end == "open" else "open"
        if mine and not theirs:
            verdict[end] = PASS
            record("reeds", f"{end} reed makes at its stop", PASS,
                   f"GPIO {pin} MADE with the panel {label}")
        elif mine and theirs:
            verdict[end] = FAIL
            record("reeds", f"{end} reed makes at its stop", FAIL,
                   f"BOTH switches made at once ({sw2}). Physically impossible: "
                   "a shorted wire, a stuck switch, or a stray magnet")
        else:
            verdict[end] = FAIL
            record("reeds", f"{end} reed makes at its stop", FAIL,
                   f"GPIO {pin} is NOT MADE with the panel held {label}. "
                   f"THIS IS THE {label} REED. Check its connector first, then "
                   "the magnet-to-reed gap with the panel against the stop — "
                   "the capture range is narrow")
        if theirs and not mine:
            record("reeds", f"{other} reed not stuck", FAIL,
                   f"the {other} switch reads MADE while the panel is {label}: "
                   f"its magnet or wiring is holding it on")

    bad = [e for e, v in verdict.items() if v == FAIL]
    if not verdict:
        pass
    elif not bad:
        print("\n      >> Both reeds are good. If a travel still STALLS, the fault")
        print("         is WHERE THE MOTOR PARKS THE PANEL, not the sensor —")
        print("         move the magnet to make where the motor actually stops.")
    else:
        print(f"\n      >> FAULTY REED(S): {', '.join(e.upper() for e in bad)}")
        print("         A bad CLOSED reed is the worse half: every close STALLS,")
        print("         which fails the door OPEN by design, so the door stands")
        print("         open and an animal cannot be shut in.")


def group_beeps(con, args):
    say("BUZZER and LED — the parts only a person can check")
    fields, _flags, _t = con.status()
    if not fields.get("buzzer"):
        record("beeps", "buzzer configured", SKIP, "none fitted")
    else:
        record("beeps", "buzzer configured", PASS, fields["buzzer"])
        # 'w' ALONE, then confirm the header came back, then the line. The
        # documented dance, and the only safe one.
        out = con.key("w", settle=2.5)
        if "dwell / timing" not in out:
            record("beeps", "timing menu opens", FAIL, "no menu header; aborting this group")
            return
        record("beeps", "timing menu opens", PASS)
        con.line("beep", settle=3.0)
        heard = ask("Did you hear THREE even beeps?")
        record("beeps", "buzzer sounds", PASS if heard else (SKIP if heard is None else FAIL))
        con.line("led", settle=3.0)
        seen = ask("Did you see THREE flashes of the status LED?")
        record("beeps", "status LED flashes", PASS if seen else (SKIP if seen is None else FAIL))
        con.line("q", settle=1.5)          # leave the submenu deliberately
        f2, _fl, t2 = con.status()
        record("beeps", "returned to the top level",
               PASS if "---- status ----" in t2 else FAIL)


def group_lock(con, args):
    say("LOCK / UNLOCK — the beacon may not open the door")
    con.key("k", settle=2.0)
    _f, flags, text = con.status()
    if "LOCKED" in flags or "LOCKED" in text:
        record("lock", "'k' locks", PASS)
    else:
        record("lock", "'k' locks", FAIL, "no LOCKED banner in 's'")
    heard = ask("Did you hear the lock acknowledgement (two short low beeps)?")
    record("lock", "lock is audible", PASS if heard else (SKIP if heard is None else WARN),
           "" if heard else "the ack exists so a remote command is not silent")

    con.key("K", settle=2.0)
    _f2, flags2, text2 = con.status()
    if "LOCKED" not in flags2 and "LOCKED" not in text2:
        record("lock", "'K' unlocks", PASS)
    else:
        record("lock", "'K' unlocks", FAIL, "still LOCKED after 'K'")


def group_manual(con, args):
    say("MANUAL OVERRIDE — a fact about a person, and it must be releasable")
    # Release first, so the group starts from a known state whatever the door
    # was left in. 'O' is the ONLY console release there is, which is the bug
    # this group exists to catch: for a while clearOverride() was reachable
    # from the network verbs and from nowhere else.
    con.key("O", settle=2.0)
    _f, flags, text = con.status()
    if "MANUAL" in flags or "MANUAL " in text:
        record("manual", "'O' releases manual control", FAIL,
               "still MANUAL after 'O' — the console cannot hand the door back")
    else:
        record("manual", "'O' releases manual control", PASS)

    if args.no_move:
        record("manual", "manual control is SET by a console move", SKIP,
               "needs --no-move off: setting it requires moving the door")
        return
    # Setting it requires a real actuation, so it lives with the movement
    # tests; see group_movement, which checks the flag after an open.


def group_mute(con, args):
    say("MUTE — portal-only, so this one is prompted rather than driven")
    print("      There is deliberately no console key for mute: it was added as")
    print("      a dashboard control. So this step asks you to send it.")
    _f, flags, text = con.status()
    was_muted = "MUTED" in flags or "MUTED" in text
    if was_muted:
        record("mute", "starting state", WARN, "already MUTED — will restore to unmuted")

    go = ask("Send `mute` from the dashboard now. Ready to check?")
    if go is None:
        record("mute", "mute takes effect", SKIP)
    else:
        found = None
        print("      waiting up to 6 min for the door to collect the command...")
        end = time.time() + 360
        while time.time() < end and not found:
            con.read_for(5.0)
            _f2, fl2, t2 = con.status()
            if "MUTED" in fl2 or "MUTED" in t2:
                found = True
        record("mute", "mute takes effect", PASS if found else FAIL,
               "status shows MUTED" if found else "no MUTED banner within 6 min")
        if found:
            ok = ask("A FAULT sound should still be audible when muted. "
                     "Routine sounds off, alarms on — does that match the "
                     "acknowledgement you just heard (or did not hear)?")
            record("mute", "mute spares the alarms", PASS if ok else
                   (SKIP if ok is None else WARN),
                   "" if ok else "faults and CHIME_TEST are deliberately not routine")

    back = ask("Now send `unmute`. Ready to check?")
    if back is None:
        record("mute", "unmute restores sound", SKIP)
        return
    cleared = None
    end = time.time() + 360
    while time.time() < end and not cleared:
        con.read_for(5.0)
        _f3, fl3, t3 = con.status()
        if "MUTED" not in fl3 and "MUTED" not in t3:
            cleared = True
    record("mute", "unmute restores sound", PASS if cleared else FAIL)


def group_movement(con, args):
    say("MOVEMENT — this drives the motor")
    if args.no_move:
        record("movement", "whole group", SKIP, "--no-move")
        return
    print("      The door will OPEN and then CLOSE. A travel takes about twelve")
    print("      seconds and resolves asynchronously: the request prints first,")
    print("      the verdict up to ~15 s later.")
    go = ask("Is the doorway CLEAR of animals, hands and obstructions?")
    if not go:
        record("movement", "whole group", SKIP, "doorway not confirmed clear")
        return

    for key, verb, arrived in (("o", "OPEN", "OPEN — ARRIVED"),
                               ("x", "CLOSE", "CLOSED — ARRIVED")):
        con.buf = ""
        con.key(key, settle=2.0)
        hit = con.await_text([arrived, "STALLED", "NEVER MOVED"], 40.0)
        if hit == arrived:
            m = re.search(r"in (\d+) ms", con.buf)
            record("movement", f"{verb} arrives", PASS,
                   f"verified by the limit switch in {m.group(1)} ms" if m else "verified")
            if m and "within a second of the" in con.buf:
                record("movement", f"{verb} has deadline margin", WARN,
                       "the door says it only just made it — run 'calibrate'")
        elif hit == "STALLED":
            record("movement", f"{verb} arrives", FAIL,
                   "STALLED: it started and never arrived. If the door clearly "
                   "ran its full travel, suspect the magnet at that end rather "
                   "than the motor — the capture range is narrow")
        elif hit == "NEVER MOVED":
            record("movement", f"{verb} arrives", FAIL,
                   "nothing moved at all: the controller swallowed the press, or "
                   "the door is jammed. Try a longer 'pulse'")
        else:
            record("movement", f"{verb} arrives", FAIL, "no verdict within 40 s")

        if key == "o":
            # An actuation at the console IS a person taking control, and the
            # flag must appear. This is the other half of group_manual.
            _f, flags, text = con.status()
            if "MANUAL" in flags or "MANUAL " in text:
                record("movement", "a console move takes MANUAL control", PASS)
            else:
                record("movement", "a console move takes MANUAL control", FAIL,
                       "no MANUAL banner after opening by hand")

    heard = ask("Did each movement beep, and did you hear a different pattern "
                "for the console than you would for the collar?")
    record("movement", "movement is audible", PASS if heard else
           (SKIP if heard is None else WARN))


def group_beacon(con, args):
    say("BEACON — the thing the whole door is for")
    fields, _flags, _t = con.status()
    radio = fields.get("radio", "")
    m = re.search(r"(\d+) adverts", radio)
    if m and int(m.group(1)) > 0:
        record("beacon", "radio is scanning", PASS, f"{m.group(1)} adverts seen")
    else:
        record("beacon", "radio is scanning", FAIL, radio or "no radio line")
    if "0 samples dropped" in radio:
        record("beacon", "no samples dropped", PASS)
    elif "samples dropped" in radio:
        record("beacon", "no samples dropped", WARN, radio)

    tgt = fields.get("target", "")
    if "never been heard" in fields.get("rssi", ""):
        record("beacon", "target beacon heard", FAIL,
               f"{tgt} has never been heard this boot. The door refuses to "
               "actuate until it hears the collar once, so this must pass")
    else:
        record("beacon", "target beacon heard", PASS, fields.get("rssi", ""))

    gap = fields.get("worst gap", "")
    if "EXCEEDS" in gap:
        record("beacon", "sample gaps within tolerance", WARN,
               f"{gap} — at this distance that is expected; re-check with the "
               "collar at its normal range, because a close dwell below the "
               "worst gap WILL close on a routine radio dropout")
    else:
        record("beacon", "sample gaps within tolerance", PASS, gap)

    if args.no_move:
        record("beacon", "collar opens the door", SKIP, "--no-move")
        return
    go = ask("Bring the collar to the door to test a beacon-driven OPEN. Ready?")
    if go is None or go is False:
        record("beacon", "collar opens the door", SKIP)
        return
    # Automatic control has to be live for this to mean anything.
    con.key("O", settle=1.5)
    con.buf = ""
    print("      waiting up to 60 s for presence and an open...")
    hit = con.await_text(["the collar is here", "presence] PRESENT"], 60.0)
    record("beacon", "collar is detected", PASS if hit else FAIL,
           "" if hit else "no presence within 60 s — check thresholds with 't'")
    if hit:
        arrived = con.await_text(["OPEN — ARRIVED", "STALLED", "NEVER MOVED"], 45.0)
        record("beacon", "collar opens the door",
               PASS if arrived == "OPEN — ARRIVED" else FAIL, arrived or "no verdict")


def _travel(con, key, arrived, label, group, note=""):
    """Drive one leg and report. Returns the measured ms, or None."""
    con.buf = ""
    con.key(key, settle=2.0)
    hit = con.await_text([arrived, "STALLED", "NEVER MOVED"], 45.0)
    if hit == arrived:
        m = re.search(r"in (\d+) ms", con.buf)
        ms = int(m.group(1)) if m else None
        tight = "within a second of the" in con.buf
        if ms:
            measure(f"{group}/{label}", f"{ms} ms")
        record(group, f"{label} arrives", PASS,
               (f"{ms} ms" if ms else "verified") + (" — ONLY JUST" if tight else ""))
        if tight:
            record(group, f"{label} has deadline margin", WARN,
                   "the door says it only just made it; run 'calibrate'")
        return ms
    if hit == "STALLED":
        record(group, f"{label} arrives", FAIL,
               note or "STALLED: it started and never arrived")
    elif hit == "NEVER MOVED":
        record(group, f"{label} arrives", FAIL,
               "nothing moved at all — the press was swallowed, or it is jammed")
    else:
        record(group, f"{label} arrives", FAIL, "no verdict within 45 s")
    return None


def group_cycles(con, args):
    """Several full cycles, because one of anything proves nothing here.

    Repeatability is the number that matters, not a single travel. The
    reference door measured a close at 15,192 ms and then 11,901 ms minutes
    apart on the same orientation — a 3.3 s spread against a documented
    repeatability of 175 ms. One cycle would have reported either figure with a
    straight face.
    """
    say(f"CYCLES — {args.cycles} full open/close cycles, timed")
    if args.no_move:
        record("cycles", "whole group", SKIP, "--no-move")
        return
    if not ask("Is the doorway CLEAR of animals, hands and obstructions?"):
        record("cycles", "whole group", SKIP, "doorway not confirmed clear")
        return

    opens, closes = [], []
    for n in range(1, args.cycles + 1):
        print(f"      cycle {n} of {args.cycles}")
        o = _travel(con, "o", "OPEN — ARRIVED", f"cycle {n} open", "cycles")
        c = _travel(con, "x", "CLOSED — ARRIVED", f"cycle {n} close", "cycles")
        if o:
            opens.append(o)
        if c:
            closes.append(c)

    for label, vals in (("open", opens), ("close", closes)):
        if len(vals) < 2:
            record("cycles", f"{label} repeatability", SKIP,
                   f"only {len(vals)} good measurement(s)")
            continue
        spread = max(vals) - min(vals)
        mean = sum(vals) // len(vals)
        detail = f"mean {mean} ms, spread {spread} ms over {len(vals)} ({min(vals)}-{max(vals)})"
        measure(f"{label} mean", f"{mean} ms")
        measure(f"{label} spread", f"{spread} ms over {len(vals)} cycles")
        # A door that wanders by seconds blunts stall detection, because the
        # deadline has to be set wide enough for the slowest travel.
        if spread > 1500:
            record("cycles", f"{label} repeatability", FAIL, detail +
                   " — this door is ERRATIC. A travel time that wanders by "
                   "seconds forces a wide deadline, which is what blunts stall "
                   "detection. Suspect friction or a magnet near its capture limit")
        elif spread > 700:
            record("cycles", f"{label} repeatability", WARN, detail)
        else:
            record("cycles", f"{label} repeatability", PASS, detail)


def group_uncommanded(con, args):
    """LOCK the door, then move it by hand. Both halves are the test.

    A lock stops the BEACON opening the door; it must not stop the door
    noticing that somebody moved it. And a hand on the door is treated as that
    person taking control, which is the behaviour that makes a held door stay
    where it was left.
    """
    say("UNCOMMANDED — locked, then moved BY HAND")
    if args.no_move:
        print("      (--no-move only skips the MOTOR; you move this one yourself)")

    fields, _fl, _t = con.status()
    start = fields.get("position", "?")
    if start.startswith("UNKNOWN"):
        record("uncommanded", "starts from a known end", SKIP,
               "the panel is between the ends; put it on a stop first")
        return
    record("uncommanded", "starts from a known end", PASS, start)

    con.key("k", settle=2.0)
    _f, flags, text = con.status()
    if not ("LOCKED" in flags or "LOCKED" in text):
        record("uncommanded", "door is locked for the test", FAIL, "'k' did not take")
        return
    record("uncommanded", "door is locked for the test", PASS)

    other = "CLOSED" if start.startswith("OPEN") else "OPEN"
    go = ask(f"Now move the panel BY HAND fully to its {other} stop. "
             f"Take a few seconds over it. Done?")
    if go is None or go is False:
        con.key("K", settle=1.5)
        record("uncommanded", "hand move is detected", SKIP)
        return

    con.buf = ""
    hit = con.await_text(["NOTHING commanded it", "uncommanded",
                          "moved to", "[vib]"], 30.0)
    if hit:
        record("uncommanded", "hand move is detected", PASS,
               "the door noticed and said so")
    else:
        record("uncommanded", "hand move is detected", FAIL,
               "nothing on the console. With no reed at that end the move can "
               "only be INFERRED from vibration, and only if it took between "
               "the band shown in 's' — a quick shove is invisible to both paths")

    f2, flags2, text2 = con.status()
    record("uncommanded", "lock did not suppress detection",
           PASS if hit else FAIL,
           "a lock gates the BEACON, never the door's own senses")
    if "MANUAL" in flags2 or "MANUAL " in text2:
        record("uncommanded", "a hand takes MANUAL control", PASS)
    else:
        record("uncommanded", "a hand takes MANUAL control", WARN,
               "no MANUAL banner. Expected for a MEASURED move; an INFERRED one "
               "deliberately does not seize the door, because acting on a guess "
               "would make it inert for good")
    newpos = f2.get("position", "?")
    record("uncommanded", "position followed the panel",
           PASS if not newpos.startswith("UNKNOWN") else WARN, newpos)

    con.key("K", settle=1.5)
    _f3, flags3, text3 = con.status()
    record("uncommanded", "unlocked again",
           PASS if not ("LOCKED" in flags3 or "LOCKED" in text3) else FAIL)


def group_obstruction(con, args):
    """Block the door and find out what it does. The point of the project.

    A stalled close must FAIL OPEN — reversed, not retried — because a door
    stopped partway shut is exactly when something may be under it. This is
    invariant 14, it is the behaviour an animal's safety rests on, and it has
    never been tested by anything but accident.
    """
    say("OBSTRUCTION — a blocked close must FAIL OPEN")
    if args.no_move:
        record("obstruction", "whole group", SKIP, "--no-move")
        return
    print("      !! Use a SOLID OBJECT. Never a hand, an arm or a foot.")
    print("      !! The motor will drive against it for the full travel time.")
    print("      !! Something soft and rigid, placed where the panel closes.")
    go = ask("Open the door, place an obstruction in the doorway, and stand clear. Ready?")
    if go is None or go is False:
        record("obstruction", "whole group", SKIP, "not confirmed")
        return

    # Make sure we are starting from open, so the close has somewhere to go.
    fields, _fl, _t = con.status()
    if not fields.get("position", "").startswith("OPEN"):
        _travel(con, "o", "OPEN — ARRIVED", "open before the test", "obstruction")

    con.buf = ""
    con.key("x", settle=2.0)
    hit = con.await_text(["STALLED", "NEVER MOVED", "CLOSED — ARRIVED"], 45.0)
    if hit == "CLOSED — ARRIVED":
        record("obstruction", "the obstruction was detected", FAIL,
               "the door closed anyway — the obstruction was not in its path, "
               "or it is strong enough to push through. Re-site it and re-run")
        return
    if hit == "NEVER MOVED":
        record("obstruction", "the obstruction was detected", PASS,
               "NEVER MOVED — nothing moved, so nothing is trapped")
    elif hit == "STALLED":
        record("obstruction", "the obstruction was detected", PASS,
               "STALLED — it started and never arrived")
    else:
        record("obstruction", "the obstruction was detected", FAIL,
               "no verdict within 45 s")
        return

    if hit == "STALLED":
        # The safety behaviour itself.
        if "being reopened" in con.buf or "It was CLOSING" in con.buf:
            record("obstruction", "a stalled close FAILS OPEN", PASS,
                   "the door said it was reopening")
        else:
            record("obstruction", "a stalled close FAILS OPEN", FAIL,
                   "no sign of a reversal — invariant 14 is not holding")
        back = con.await_text(["OPEN — ARRIVED"], 45.0)
        record("obstruction", "the reversal reaches OPEN",
               PASS if back else FAIL,
               "" if back else "it reversed but never got back to open")
        f2, _fl2, t2 = con.status()
        if "retrying in" in t2 or f2.get("retrying in"):
            record("obstruction", "a retry is scheduled", PASS,
                   f2.get("retrying in", "see 's'"))
        elif "GIVEN UP" in t2 or f2.get("gave up"):
            record("obstruction", "attempts are capped", PASS,
                   "it has given up and is staying open, which is the designed end")
        else:
            record("obstruction", "a retry is scheduled", WARN,
                   "no retry and no give-up state visible")

    pause("Now REMOVE the obstruction and stand clear")
    ok = _travel(con, "x", "CLOSED — ARRIVED", "close after clearing", "obstruction",
                 note="still stalling with the obstruction removed — this is no "
                      "longer about the obstruction")
    record("obstruction", "recovers once cleared", PASS if ok else FAIL)


def group_logging(con, args):
    """Did any of the above reach the log, and does it leave the door?

    The recurring failure in this project is not a door that misbehaves, it is
    a door that knew something and told nobody. So the suite checks its own
    footprints.
    """
    say("LOGGING — did the door record what just happened, and can it send it")
    con.buf = ""
    os.write(con.fd, b"l")
    text = ""
    end = time.time() + 20.0
    while time.time() < end:
        text += con.read_for(0.5)
        if "---- event log" in text and re.search(r"\n\s*\n", text[-80:] or ""):
            break
    if "---- event log" not in text:
        record("logging", "event log dumps", FAIL, "no log header from 'l'")
        return
    m = re.search(r"event log \((\d+)/(\d+)\)", text)
    record("logging", "event log dumps", PASS,
           f"{m.group(1)} of {m.group(2)} entries" if m else "dumped")

    seen = set(re.findall(
        r"\b(BOOT|OPEN|CLOSE|REFUSED|STALLED|UNCOMMANDED|RETRY|GAVE_UP|WAKE|"
        r"SENSOR_FAULT|SENSOR_OK|MAINT|MANUAL|CONSOLE|NO_MOVE|FIX_LOST|FIX_GOT)\b",
        text))
    record("logging", "log carries event types", PASS if seen else FAIL,
           ", ".join(sorted(seen)) if seen else "none recognised")
    # The suite has just opened and closed the door at least twice; if none of
    # that is in the ring, the ring is not recording what the door does.
    did_move = {"OPEN", "CLOSE"} & seen
    if args.no_move:
        record("logging", "this run's movements are in the log", SKIP, "--no-move")
    else:
        record("logging", "this run's movements are in the log",
               PASS if did_move else FAIL,
               ", ".join(sorted(did_move)) if did_move else
               "no OPEN/CLOSE in the ring after moving the door")

    con.buf = ""
    os.write(con.fd, b"u")
    # Wait ONLY for the terminal outcomes. "radio up" is printed first and
    # would short-circuit the wait — await_text checks its needles in order on
    # every pass, so a needle that arrives early beats one that matters more.
    # It is still useful afterwards, to tell "the radio tried and the upload
    # did not finish" from "nothing happened at all".
    hit = con.await_text(["uploaded", "no WIFI_SSID", "not compiled in"], 120.0)
    if hit == "uploaded":
        m2 = re.search(r"uploaded (\d+) events", con.buf)
        record("logging", "the log reaches the server", PASS,
               f"{m2.group(1)} events" if m2 else "upload reported")
    elif hit:
        record("logging", "the log reaches the server", SKIP,
               f"WiFi is not configured on this door ({hit})")
    elif "radio up" in con.buf:
        record("logging", "the log reaches the server", FAIL,
               "the radio came up and no upload completed within 120 s — it is "
               "reaching the air but not the server. Check the endpoint and the "
               "shared key; 's' shows the last HTTP response")
    else:
        record("logging", "the log reaches the server", FAIL,
               "the radio never even came up within 120 s. The ring is durable, "
               "so nothing is lost — but nothing remote can see it either")


GROUPS = [
    ("console", group_console, "does the door talk, and what is it running"),
    ("sensors", group_sensors, "reeds and vibration, at rest"),
    ("reeds", group_reeds, "WHICH reed is faulty — hand-moved, prompted"),
    ("beeps", group_beeps, "buzzer and LED — prompted"),
    ("lock", group_lock, "lock and unlock"),
    ("manual", group_manual, "manual override, and that it can be released"),
    ("movement", group_movement, "one open and close, driving the motor"),
    ("cycles", group_cycles, "several timed cycles, and repeatability"),
    ("uncommanded", group_uncommanded, "LOCKED, then moved by hand — prompted"),
    ("obstruction", group_obstruction, "a blocked close must FAIL OPEN — prompted"),
    ("beacon", group_beacon, "scanning, and a collar-driven open"),
    # Late, because it is the slowest: the door collects a dashboard command on
    # its own upload cadence, which ran to minutes, and there is no console key
    # for mute to shortcut it with.
    ("mute", group_mute, "mute and unmute — prompted, portal-only"),
    # Last, because it checks the footprints of everything above.
    ("logging", group_logging, "did the door record it, and can it send it"),
]

# What the operator has to have to hand, per group. Printed up front, because
# discovering you need a solid object halfway through a motor test is how
# somebody ends up using their hand.
NEEDS = {
    "reeds": "to move the panel onto each stop BY HAND",
    "beeps": "to hear a buzzer and see an LED",
    "uncommanded": "to move the panel BY HAND while the door is locked",
    "obstruction": "a SOLID OBJECT to block the doorway — never a hand",
    "beacon": "the collar/beacon, to bring near the door",
    "mute": "the dashboard open, to send `mute` and `unmute` (slow: minutes)",
}


def entry_state(con):
    """What the door was like BEFORE the suite touched it.

    Captured so restore() can put it back, rather than imposing a state. The
    first version of this script forced unlock-and-automatic on the way out,
    which is wrong in a way that matters: a door is often held deliberately —
    while a reed is being repaired, or a run is being cleaned — and a
    diagnostic that silently hands it back to the collar has changed the thing
    somebody was relying on. "Leave it as you found it" beats "leave it as the
    script thinks best".
    """
    _f, flags, text = con.status()
    return {
        "locked": "LOCKED" in flags or "LOCKED" in text,
        "manual": "MANUAL" in flags or "MANUAL " in text,
        "muted": "MUTED" in flags or "MUTED" in text,
    }


def restore(con, before):
    """Put the door back the way it was found.

    Runs on the way out however the suite ended, Ctrl-C included. The states
    this suite sets all survive a reboot, and held CLOSED is the one that can
    shut an animal out indefinitely — so leaving one set by accident makes the
    door worse than not running the suite at all.
    """
    say("RESTORING — putting the door back as it was found")
    if before is None:
        print("   entry state was never captured (the console was not reachable);")
        print("   leaving the door alone rather than guessing at it.")
        return
    try:
        _f0, flags0, text0 = con.status()
        now = {
            "locked": "LOCKED" in flags0 or "LOCKED" in text0,
            "manual": "MANUAL" in flags0 or "MANUAL " in text0,
            "muted": "MUTED" in flags0 or "MUTED" in text0,
        }
        # Lock: 'k' sets, 'K' clears.
        if now["locked"] != before["locked"]:
            con.key("k" if before["locked"] else "K", settle=1.5)
        # Manual: only releasable from here. SETTING it needs an actuation, so
        # a door that arrived held and was released by a test cannot be put
        # back without moving it — say so rather than moving a door to tidy up.
        if now["manual"] and not before["manual"]:
            con.key("O", settle=1.5)
        _f, flags, text = con.status()
        still_locked = "LOCKED" in flags or "LOCKED" in text
        still_manual = "MANUAL" in flags or "MANUAL " in text
        still_muted = "MUTED" in flags or "MUTED" in text
        ok = True
        if still_locked != before["locked"]:
            ok = False
            print(f"   !! lock is {'ON' if still_locked else 'OFF'} and was "
                  f"{'ON' if before['locked'] else 'OFF'} — fix with "
                  f"{'`lock`' if before['locked'] else '`unlock`'}")
        if still_manual != before["manual"]:
            ok = False
            if before["manual"]:
                print("   !! the door arrived under MANUAL control and is now "
                      "automatic. Re-setting it would mean MOVING the door, so "
                      "this suite will not do it for you — open or close it by "
                      "hand, or from the dashboard, to put it back.")
            else:
                print("   !! the door is under MANUAL control and did not start "
                      "that way — send `door auto`.")
        if still_muted != before["muted"]:
            ok = False
            print(f"   !! the door is {'MUTED' if still_muted else 'UNMUTED'} and "
                  f"did not start that way — send "
                  f"{'`unmute`' if still_muted else '`mute`'} from the dashboard.")
        if ok:
            bits = []
            bits.append("LOCKED" if before["locked"] else "unlocked")
            bits.append("under MANUAL control" if before["manual"] else "automatic")
            if before["muted"]:
                bits.append("MUTED")
            print("   back as found: " + ", ".join(bits))
    except OSError as exc:
        print(f"   !! could not restore over the console: {exc}")


# Real captures from the reference door, kept verbatim. A parser tested against
# invented text proves only that the author and the parser agree.
SAMPLE_CLOSED = """---- status ----
  target       : MAC ac:23:3f:25:17:0a
  uptime       : 2887 s
  door         : CLOSED
  relay pulse  : 1500 ms
  door travel  : open 12308 ms, close 25000 ms configured
  last verified: open 10820 ms, close 15192 ms (measured, this boot)
  position     : CLOSED (measured)
  switches     : open GPIO 32 open, closed GPIO 25 MADE
  buzzer       : GPIO 27, passive, active HIGH
  MANUAL       : a person moved this door. Nothing automatic
                 will open or close it, in either direction,
  !!             The door is CLOSED, so an animal outside cannot get in.
  vibration    : GPIO 33, 225615 edges since boot (pull-up on)
  firmware     : v1.1.0 (built Oct 10 2026 10:38:05)
  ble stack    : NimBLE
  boot         : #1061, last reset: software restart
  free heap    : 127252 bytes (low-water 69208)
"""

SAMPLE_BETWEEN = """---- status ----
  door         : CLOSED
  position     : UNKNOWN (measured)  << between the two switches
  switches     : open GPIO 32 open, closed GPIO 25 open
  radio        : 171529 adverts, last 0 ms ago, 0 samples dropped
  worst gap    : 117391 ms between samples  << EXCEEDS SAMPLE_MAX_AGE_MS
  rssi         : beacon has never been heard
  vibration    : GPIO 33, 286 edges since boot (pull-up on)
  LOCKED       : the beacon cannot open this door ('K' to unlock)
  free heap    : 126644 bytes (low-water 67016)
"""

SAMPLE_OPEN = """---- status ----
  door         : OPEN
  position     : OPEN (measured)
  switches     : open GPIO 32 MADE, closed GPIO 25 open
  free heap    : 1 bytes (low-water 1)
"""


def selftest():
    """Exercise the parsers with no board attached. Returns an exit code."""
    bad = 0

    def ck(cond, what):
        nonlocal bad
        print(("  ok   " if cond else " FAIL  ") + what)
        if not cond:
            bad += 1

    f, fl = parse_status(SAMPLE_CLOSED)
    ck(f.get("door") == "CLOSED", "reads the door state")
    ck(f.get("position") == "CLOSED (measured)", "reads the measured position")
    ck(f.get("ble stack") == "NimBLE", "reads a key containing a space")
    ck(f.get("last verified", "").startswith("open 10820"),
       "reads 'last verified', whose key has no padding before the colon")
    ck("MANUAL" in fl, "sees the MANUAL banner, which has no value")
    ck("LOCKED" not in fl, "does not invent a LOCKED banner")
    ck(f.get("firmware", "").startswith("v1.1.0"), "reads the firmware line")

    op, om, cp, cm = switches_from(f.get("switches"))
    ck((op, om, cp, cm) == ("32", False, "25", True),
       "closed-at-its-stop: closed MADE, open not")

    f2, fl2 = parse_status(SAMPLE_BETWEEN)
    op2, om2, cp2, cm2 = switches_from(f2.get("switches"))
    ck((om2, cm2) == (False, False), "parked between the ends: neither made")
    ck("LOCKED" in fl2, "sees the LOCKED banner")
    ck("EXCEEDS" in f2.get("worst gap", ""), "keeps the inline EXCEEDS warning")
    ck("never been heard" in f2.get("rssi", ""), "notices a beacon never heard")
    ck(re.search(r"(\d+)\s+edges", f2.get("vibration", "")).group(1) == "286",
       "reads the vibration edge count the at-rest check compares")

    f3, _fl3 = parse_status(SAMPLE_OPEN)
    op3, om3, cp3, cm3 = switches_from(f3.get("switches"))
    ck((om3, cm3) == (True, False), "open-at-its-stop: open MADE, closed not")

    # The faulty-reed logic, as group_reeds applies it, without a door.
    for end, (om_, cm_), want in (("open", (True, False), PASS),
                                  ("open", (False, False), FAIL),
                                  ("closed", (False, True), PASS),
                                  ("closed", (False, False), FAIL),
                                  ("closed", (True, True), FAIL)):
        mine, theirs = (om_, cm_) if end == "open" else (cm_, om_)
        got = PASS if (mine and not theirs) else FAIL
        ck(got == want, f"{end} reed verdict for made={mine} other={theirs} is {want}")

    ck(switches_from(None) == (None, False, None, False),
       "a missing switches line does not raise")
    ck(parse_status("") == ({}, []), "an empty capture does not raise")

    print(f"\n  {'FAILED' if bad else 'ok    '}  {bad} failure(s)")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/cu.usbserial-0001")
    ap.add_argument("--cycles", type=int, default=3,
                    help="how many open/close cycles the cycles group runs (default 3)")
    ap.add_argument("--no-move", action="store_true",
                    help="skip every test that drives the motor")
    ap.add_argument("--only", default="",
                    help="comma-separated group names (see --list)")
    ap.add_argument("--list", action="store_true", help="list the groups and exit")
    ap.add_argument("--report", metavar="FILE", default="",
                    help="also write the results to FILE, to keep or compare")
    ap.add_argument("--selftest", action="store_true",
                    help="test this script's own parsers; needs no door")
    ap.add_argument("--verbose", action="store_true",
                    help="echo the raw console traffic as it arrives")
    args = ap.parse_args()

    if args.selftest:
        print("commission.py — parser self-test (no hardware)")
        return selftest()

    if args.list:
        for name, _fn, blurb in GROUPS:
            print(f"  {name:<12} {blurb}")
        return 0

    wanted = [g.strip() for g in args.only.split(",") if g.strip()] or None
    todo = [(n, f, b) for n, f, b in GROUPS if wanted is None or n in wanted]
    if wanted:
        unknown = sorted(set(wanted) - {n for n, _f, _b in GROUPS})
        if unknown:
            print(f"unknown group(s): {', '.join(unknown)}", file=sys.stderr)
            return 2

    print("PetDoor commissioning — walks every function and says what it found")
    print("=" * 70)
    print(f"  port    : {args.port}")
    print(f"  motor   : {'SKIPPED (--no-move)' if args.no_move else 'WILL BE DRIVEN'}")
    moving = ("movement", "cycles", "obstruction", "beacon")
    n_moves = 0
    if not args.no_move:
        for name, _f, _b in todo:
            n_moves += {"movement": 2, "cycles": args.cycles * 2,
                        "obstruction": 3, "beacon": 1}.get(name, 0)
    print(f"  groups  : {len(todo)}")
    print(f"  the door will move about {n_moves} time(s)")

    # Everything the operator needs, before the first prompt rather than
    # halfway through a motor test.
    needed = [(n, NEEDS[n]) for n, _f, _b in todo if n in NEEDS]
    if needed:
        print("\n  YOU WILL NEED:")
        for name, what in needed:
            print(f"    - {what}   ({name})")
    print("\n  Steps that need you say so and wait. Answer 's' to skip any of them.")
    print("  Ctrl-C is safe at any point: the door is put back as it was found.")

    if not args.no_move and any(n in moving for n, _f, _b in todo):
        print("\n  !! THIS WILL DRIVE A REAL MOTOR ON A REAL DOOR.")
        print("  !! Keep hands, limbs and animals out of the doorway throughout.")
        if input("\n  Type 'go' to begin: ").strip().lower() != "go":
            print("  cancelled")
            return 1

    if not os.path.exists(args.port):
        print(f"\nno such port: {args.port}", file=sys.stderr)
        print("Available:", file=sys.stderr)
        for p in sorted(os.listdir("/dev")):
            if p.startswith(("cu.usb", "tty.usb")):
                print(f"  /dev/{p}", file=sys.stderr)
        return 2

    con = Console(args.port, verbose=args.verbose)
    before = None
    try:
        con.drain(1.5)
        try:
            before = entry_state(con)
            print(f"\nentry state: locked={before['locked']} "
                  f"manual={before['manual']} muted={before['muted']}")
        except OSError:
            pass
        alive = group_console(con, args) if todo and todo[0][0] == "console" else True
        for name, fn, _blurb in todo:
            if name == "console":
                continue
            if not alive:
                record(name, "whole group", SKIP, "console is not responding")
                continue
            fn(con, args)
    except KeyboardInterrupt:
        print("\n   interrupted")
    finally:
        try:
            restore(con, before)
        finally:
            con.close()

    print("\n" + "=" * 70)
    print("SUMMARY")
    print("=" * 70)
    counts = {PASS: 0, FAIL: 0, SKIP: 0, WARN: 0}
    for _g, _n, outcome, _d in results:
        counts[outcome] += 1

    # Per group, so a reader can see at a glance which part of the door is
    # unhappy rather than scrolling a flat list of checks.
    print("\n  group         ok  fail  warn  skip   verdict")
    print("  " + "-" * 60)
    for group in _order:
        rows = [r for r in results if r[0] == group]
        c = {PASS: 0, FAIL: 0, SKIP: 0, WARN: 0}
        for _g, _n, outcome, _d in rows:
            c[outcome] += 1
        verdict = ("FAILED" if c[FAIL] else
                   "check warnings" if c[WARN] else
                   "not run" if c[PASS] == 0 and c[SKIP] else "ok")
        print(f"  {group:<12} {c[PASS]:>3}  {c[FAIL]:>4}  {c[WARN]:>4}  {c[SKIP]:>4}   {verdict}")

    if measurements:
        print("\n  MEASURED")
        for label, value in measurements:
            print(f"    {label:<28} {value}")

    bad = [r for r in results if r[2] in (FAIL, WARN)]
    if bad:
        print("\n  NEEDS ATTENTION")
        for group, name, outcome, detail in bad:
            print(f"    [{outcome}] {group}/{name}")
            if detail:
                # Wrap the explanation, which is often the useful part.
                words, line = detail.split(), "          "
                for w in words:
                    if len(line) + len(w) > 76:
                        print(line)
                        line = "          "
                    line += w + " "
                print(line.rstrip())

    print(f"\n  {counts[PASS]} passed, {counts[FAIL]} failed, "
          f"{counts[WARN]} warnings, {counts[SKIP]} skipped")
    if counts[FAIL]:
        print("\n  A FAIL is a door that will not behave as documented.")
        print("  docs/DIAGNOSTICS.md has the symptom tables; the lines above say")
        print("  which part to look at first.")
    elif counts[WARN]:
        print("\n  Nothing failed. The warnings are things that work but will")
        print("  bite later — read them before mounting or leaving it unattended.")
    else:
        print("\n  Every check that ran, passed.")
    if counts[SKIP]:
        print(f"  {counts[SKIP]} check(s) were skipped — they prove nothing either way.")

    if args.report:
        try:
            with open(args.report, "w", encoding="utf-8") as fh:
                fh.write("# PetDoor commissioning report\n")
                fh.write(time.strftime("%Y-%m-%d %H:%M:%S %Z\n\n"))
                fh.write(f"port: {args.port}\n")
                for label, value in measurements:
                    fh.write(f"measured: {label} = {value}\n")
                fh.write("\n")
                for group, name, outcome, detail in results:
                    fh.write(f"{outcome:<5} {group}/{name}"
                             + (f": {detail}" if detail else "") + "\n")
                fh.write(f"\n{counts[PASS]} passed, {counts[FAIL]} failed, "
                         f"{counts[WARN]} warnings, {counts[SKIP]} skipped\n")
            print(f"\n  report written to {args.report}")
        except OSError as exc:
            print(f"\n  !! could not write {args.report}: {exc}")
    return 1 if counts[FAIL] else 0


if __name__ == "__main__":
    sys.exit(main())
