#!/usr/bin/env python3
"""Host tests for the log server's alerting rules.

These exist because the alerting is state-machine logic held in dictionaries —
"have I already mailed about this outage?", "has this door been quiet long
enough?" — and the failure modes are silent in both directions. Too eager and
the one message that mattered is buried; too shy and the door that stopped
reporting is never mentioned at all.

No framework and no network: send_notification is replaced with a recorder, and
the database is a throwaway file. Run with  tests/server/run.sh
"""
import os
import re
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER_DIR = os.path.join(HERE, "..", "..", "tools", "logserver")

# A scratch database, chosen before the module is imported — it reads this at
# import time to decide where the file lives.
_tmp = tempfile.mkdtemp()
os.environ["PETDOOR_DB"] = os.path.join(_tmp, "test.sqlite3")
os.environ["PETDOOR_NOTIFY_TO"] = "tests@example.invalid"
os.environ["PETDOOR_NOTIFY_COOLDOWN_S"] = "3600"
os.environ["PETDOOR_STALE_AFTER_S"] = "7200"

sys.path.insert(0, SERVER_DIR)
import importlib.util
spec = importlib.util.spec_from_file_location(
    "petdoorlog", os.path.join(SERVER_DIR, "petdoor-logserver.py"))
srv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(srv)

FAILS = 0
CHECKS = 0


def check(cond, what):
    global FAILS, CHECKS
    CHECKS += 1
    if not cond:
        FAILS += 1
        print(f"  FAIL  {what}")


# Replace the mailer with a recorder. Nothing here should ever open a socket.
SENT = []
srv.send_notification = lambda subject, **kw: (SENT.append((subject, kw)) or (True, "ok"))


def seed_device(name, last_seen):
    with srv.db() as conn:
        conn.execute(
            "INSERT INTO devices(device,version,build,boots,last_seen,last_ip)"
            " VALUES(?,?,?,?,?,?)"
            " ON CONFLICT(device) DO UPDATE SET last_seen=excluded.last_seen",
            (name, "v1.1.0", "test", 1, last_seen, "10.0.0.1"))


def test_stale_door_alerts_once_then_recovers():
    SENT.clear()
    srv._stale_alerted.clear()
    now = int(time.time())

    seed_device("quiet-door", now - 3 * 3600)      # 3h: past the 2h threshold
    srv.check_stale_doors()
    check(len(SENT) == 1, "a door quiet for 3h produces one alert")
    check("stopped reporting" in SENT[0][0], "the subject names the problem")

    # The fault persists. It must NOT mail again every five minutes.
    srv.check_stale_doors()
    srv.check_stale_doors()
    check(len(SENT) == 1, "a continuing outage does not re-mail on every pass")

    # It comes back.
    seed_device("quiet-door", now)
    srv.check_stale_doors()
    check(len(SENT) == 2, "the door returning sends exactly one recovery mail")
    check("reporting again" in SENT[1][0], "the recovery subject says so")

    # And a second outage later must alert again, not be suppressed forever.
    seed_device("quiet-door", now - 3 * 3600)
    srv.check_stale_doors()
    check(len(SENT) == 3, "a SECOND outage alerts again after a recovery")


def test_healthy_and_unknown_doors_stay_silent():
    SENT.clear()
    srv._stale_alerted.clear()
    now = int(time.time())

    seed_device("busy-door", now - 60)             # a minute ago: fine
    srv.check_stale_doors()
    check(not any("busy-door" in s for s, _ in SENT), "a door heard a minute ago is not alerted")

    # 1h59m: still inside the window. The boundary matters — a door that misses
    # one 30-minute heartbeat must not raise an alarm.
    seed_device("borderline", now - (2 * 3600 - 60))
    srv.check_stale_doors()
    check(not any("borderline" in s for s, _ in SENT),
          "just inside the threshold does not alert")

    # A door that has NEVER reported has no outage to report.
    seed_device("never-seen", 0)
    srv.check_stale_doors()
    check(not any("never-seen" in s for s, _ in SENT),
          "a door that never reported is not treated as having gone quiet")


def test_stall_cooldown_but_gave_up_always():
    SENT.clear()
    srv._notified_at.clear()

    def row(kind, detail=1, uptime=100, boot=1):
        return {"type": kind, "detail": detail, "uptime": uptime, "boot": boot,
                "epoch": 0, "rssi": -60, "src": 0}

    # A dead reed stalls every close attempt. The first earns a mail; the rest
    # must be suppressed, or the GAVE_UP that follows is buried under them.
    srv.notify_events("d", [row("STALLED", 2)])
    check(len(SENT) == 1, "the first stall mails")
    srv.notify_events("d", [row("STALLED", 2), row("STALLED", 2)])
    check(len(SENT) == 1, "further stalls within the cooldown are suppressed")

    # GAVE_UP is terminal and rare: it is never rate limited, even arriving
    # immediately after stalls that were.
    srv.notify_events("d", [row("GAVE_UP", 3)])
    check(len(SENT) == 2, "gave-up mails even though stalls were suppressed")
    check("staying OPEN" in SENT[1][0], "the gave-up subject leads with the state")
    srv.notify_events("d", [row("GAVE_UP", 3)])
    check(len(SENT) == 3, "a second gave-up mails again — it is never suppressed")

    # The cooldown is per door, not global.
    srv.notify_events("other-door", [row("STALLED", 2)])
    check(len(SENT) == 4, "a different door's stall is not suppressed by the first")


def seed_event(device, boot, uptime, etype, detail, rssi=-70, src=0):
    with srv.db() as conn:
        conn.execute(
            "INSERT OR REPLACE INTO events"
            "(device,epoch,uptime,boot,type,detail,rssi,received,src)"
            " VALUES(?,?,?,?,?,?,?,?,?)",
            (device, int(time.time()), uptime, boot, etype, detail, rssi,
             int(time.time()), src))


def test_inferred_movement_never_renders_as_measured():
    """Invariant 20: an inference and a measurement are different claims.

    LOG_UNCOMMANDED's detail is a DoorState when a limit switch measured the
    door's arrival, and the same value plus 10 when it was only inferred from
    how long the vibration sensor felt it move. Rendering the two identically is
    how an inference quietly becomes a measurement — which is the failure this
    guards, and it is silent: the page still looks right.
    """
    seed_device("render-door", int(time.time()))
    # A measured close, and an inferred close that took 11.2 s.
    seed_event("render-door", 7, 100, "UNCOMMANDED", 2)
    seed_event("render-door", 7, 200, "UNCOMMANDED", 12, src=11229)
    page = srv.render()

    check("a limit switch saw it" in page,
          "a measured UNCOMMANDED says a switch saw it")
    check("inferred from 11.2 s of movement" in page,
          "an inferred UNCOMMANDED says so, with the duration it was inferred from")
    # Both must still name the end the door reached, and neither may be
    # rendered as the literal detail number.
    check(page.count("the door moved to closed and nothing commanded it") == 2,
          "both rows name the end the door reached")
    check("state 12" not in page,
          "detail 12 is decoded as an inferred close, not shown as a raw state")


def test_an_inferred_open_is_still_an_open():
    """11 is an inferred OPEN. Off-by-one here would report the wrong end."""
    seed_device("render-door2", int(time.time()))
    seed_event("render-door2", 9, 100, "UNCOMMANDED", 11, src=10203)
    page = srv.render()
    check("the door moved to open and nothing commanded it" in page,
          "detail 11 renders as an inferred OPEN, not a closed")
    check("state 11" not in page, "detail 11 is decoded, not shown raw")


def test_a_door_movement_says_what_asked_for_it():
    """OPEN and CLOSE spend `detail` on the ActuationSource, and nothing read it.

    The Detail column was blank for every door movement, so a log full of
    correctly attributed beacon travels looked as though nothing had attributed
    them at all — the rows were right and the page still looked plausible, which
    is the worst way for this to fail. Reported from a live door whose beacon
    opens were all present in the database and all untagged on the page.
    """
    seed_device("src-door", int(time.time()))
    seed_event("src-door", 11, 100, "OPEN", 0)    # SRC_BEACON
    seed_event("src-door", 11, 200, "CLOSE", 1)   # SRC_MANUAL, the console
    seed_event("src-door", 11, 300, "OPEN", 2)    # SRC_REMOTE, a queued command
    page = srv.render()

    # Match the cell, not the word: "beacon" alone also appears in
    # "beacon battery" and "beacon found" elsewhere on the page.
    check(">beacon</td>" in page, "a beacon-driven open is tagged as the beacon")
    check(">console</td>" in page, "a console-driven close is tagged as the console")
    check(">network</td>" in page, "a queued remote command is tagged as the network")
    check("source 0" not in page, "the source is decoded, not shown as a raw number")


def test_a_failsafe_reversal_is_not_rendered_as_an_ordinary_open():
    """SRC_FAILSAFE means a close STALLED and the firmware reversed itself.

    Same reasoning as invariant 20: it arrives as an OPEN like any other, and
    rendering it identically to a beacon open hides the only visible trace that
    a close did not complete.
    """
    seed_device("failsafe-door", int(time.time()))
    seed_event("failsafe-door", 12, 100, "OPEN", 3)
    page = srv.render()
    check("fail-safe reversal" in page,
          "detail 3 names itself a fail-safe reversal, not a plain open")
    check('var(--bad)">fail-safe reversal' in page,
          "and is rendered loudly, because it means a close stalled")


def test_act_source_covers_every_source_the_firmware_can_send():
    """ACT_SOURCE must stay in step with ActuationSource in petdoor/door.h.

    The bug this guards is additive: someone adds SRC_SCHEDULE to the firmware,
    the door starts sending it, and the dashboard quietly renders "source 4"
    forever. Parsing the header rather than restating it is the point — a copy
    of the enum here could drift exactly as the renderer did.
    """
    hdr = os.path.join(HERE, "..", "..", "petdoor", "door.h")
    with open(hdr, encoding="utf-8") as fh:
        found = {int(m.group(2)): m.group(1)
                 for m in re.finditer(r"\bSRC_([A-Z]+)\s*=\s*(\d+)", fh.read())}
    check(len(found) >= 4,
          f"found the ActuationSource enum in door.h (got {sorted(found)})")
    missing = sorted(v for v in found if v not in srv.ACT_SOURCE)
    check(not missing,
          f"ACT_SOURCE covers every SRC_* value (missing {missing})")


def test_a_held_closed_door_is_rendered_as_what_it_costs():
    """HOLD detail 2 pins the door shut and never expires.

    Rendering it as a neutral state change would be the worst outcome here: a
    maintenance window reads the same way and ends by itself, while this does
    not. The row has to say what it costs, not what it is called.
    """
    seed_device("hold-door", int(time.time()))
    seed_event("hold-door", 20, 100, "HOLD", 2)
    seed_event("hold-door", 20, 200, "HOLD", 1)
    seed_event("hold-door", 20, 300, "HOLD", 0)
    page = srv.render()
    check("an animal outside cannot get in" in page,
          "a held-CLOSED row says an animal cannot get in")
    check("held OPEN" in page, "a held-OPEN row names itself")
    check("decides for itself again" in page, "a release says automation is back")
    check("detail 2" not in page, "the hold detail is decoded, not shown raw")


def test_held_closed_mails_on_arrival_and_held_open_is_quieter():
    """Held closed is the one state whose cost lands on the animal."""
    SENT.clear()
    seed_device("hold-mail", int(time.time()))
    srv.notify_events("hold-mail", [
        {"type": "HOLD", "detail": 2, "uptime": 10, "boot": 3, "rssi": -60},
    ])
    check(any("HELD CLOSED" in sub for sub, _ in SENT),
          "being held closed sends a mail naming it")

    SENT.clear()
    srv.notify_events("hold-mail", [
        {"type": "HOLD", "detail": 1, "uptime": 20, "boot": 3, "rssi": -60},
    ])
    check(any("held OPEN" in sub for sub, _ in SENT),
          "being held open also mails, as a notice")


def test_a_still_held_closed_door_keeps_being_announced():
    """A state that cannot expire needs something that does not depend on an
    event arriving to re-announce it. Driven off the status line, because the
    HOLD event may be weeks old by the time it matters."""
    SENT.clear()
    with srv.db() as conn:
        conn.execute(
            "INSERT INTO devices(device,version,build,boots,last_seen,last_ip,status)"
            " VALUES(?,?,?,?,?,?,?)"
            " ON CONFLICT(device) DO UPDATE SET status=excluded.status",
            ("still-held", "v1.1.0", "t", 1, int(time.time()), "10.0.0.9",
             "door=CLOSED locked=0 maint=0 sfault=0 hold=2"))
    srv.check_held_closed_doors()
    check(any("STILL held closed" in sub for sub, _ in SENT),
          "a door still reporting hold=2 is re-announced")

    # And the cooldown applies, or this would mail on every sweep.
    SENT.clear()
    srv.check_held_closed_doors()
    check(not SENT, "the reminder respects the cooldown rather than every sweep")


def test_the_reminder_does_not_fire_on_a_door_that_is_not_held():
    """`hold=2` must not be matched inside another field. sfault=2 is the
    obvious collision, and it means something entirely different."""
    SENT.clear()
    with srv.db() as conn:
        conn.execute(
            "INSERT INTO devices(device,version,build,boots,last_seen,last_ip,status)"
            " VALUES(?,?,?,?,?,?,?)"
            " ON CONFLICT(device) DO UPDATE SET status=excluded.status",
            ("not-held", "v1.1.0", "t", 1, int(time.time()), "10.0.0.8",
             "door=CLOSED locked=0 maint=0 sfault=2 hold=0"))
    srv.check_held_closed_doors()
    check(not any("not-held" in sub for sub, _ in SENT),
          "sfault=2 with hold=0 does not trigger the held-closed reminder")


def test_fault_evidence_is_read_according_to_its_fault():
    """The spare field is one integer shared by every event type, and
    SENSOR_FAULT does not use it consistently: an edge count for the vibration
    faults, WHICH END for SF_REED_LOST. Labelling 1 as "1 vibration edges" would
    send someone to inspect the wrong sensor."""
    f = srv.sensor_fault_evidence
    check(f(3, 9655) == "9655 vibration edges",
          "a vibration fault's spare field is an edge count")
    check(f(6, 1) == "the door was sitting OPEN",
          "SF_REED_LOST detail 1 is read as the open end, not as one edge")
    check(f(6, 2) == "the door was sitting CLOSED",
          "SF_REED_LOST detail 2 is read as the closed end")
    check("edge" not in (f(6, 2) or ""),
          "a reed fault is never described in edges")
    check(f(3, 0) is None and f(6, 0) is None,
          "an absent spare field reads as nothing, not as zero edges")


def test_a_lost_reed_is_rendered_and_explained():
    seed_device("reed-door", int(time.time()))
    seed_event("reed-door", 11, 100, "SENSOR_FAULT", 6, src=2)
    page = srv.render()
    check("a limit switch at the end the door is sitting at is not making" in page,
          "fault 6 has a human description rather than a bare code")
    check("the door was sitting CLOSED" in page,
          "and says which end it was at")
    check("fault 6" not in page, "fault 6 is not rendered as an unrecognised code")


def test_a_crashed_door_emails_but_a_normal_restart_does_not():
    """A panic was silent until now, which is how one went unnoticed for an hour.

    The line to walk carefully: an OTA push restarts the door on purpose (reset
    3), and so does a plug (reset 1). Emailing those would mean an email per
    flash, which is how an alert becomes noise and then becomes a filter.
    """
    SENT.clear()
    seed_device("crash-door", int(time.time()))

    srv.notify_events("crash-door", [
        {"type": "BOOT", "detail": 3, "boot": 50, "uptime": 0, "src": 0, "epoch": 0}])
    check(len(SENT) == 0, "a software restart (an OTA push) sends nothing")

    srv.notify_events("crash-door", [
        {"type": "BOOT", "detail": 1, "boot": 51, "uptime": 0, "src": 0, "epoch": 0}])
    check(len(SENT) == 0, "a power-on sends nothing")

    srv.notify_events("crash-door", [
        {"type": "BOOT", "detail": 4, "boot": 52, "uptime": 0, "src": 0, "epoch": 0}])
    check(len(SENT) == 1, "a PANIC emails")
    check("panic" in SENT[-1][0].lower(), "and says so in the subject")

    # Not rate limited, on purpose: the history that forced the NimBLE default
    # was three consecutive panics, and a cooldown would have hidden two.
    srv.notify_events("crash-door", [
        {"type": "BOOT", "detail": 4, "boot": 53, "uptime": 0, "src": 0, "epoch": 0}])
    check(len(SENT) == 2, "a second consecutive panic is NOT suppressed")

    srv.notify_events("crash-door", [
        {"type": "BOOT", "detail": 9, "boot": 54, "uptime": 0, "src": 0, "epoch": 0}])
    check(len(SENT) == 3, "a brownout emails too")

    # Every abnormal reason must colour as alert. An unknown accent falls back to
    # teal — "something happened, nothing is wrong" — which is the wrong thing to
    # say about a door that fell over.
    for code in sorted(srv.ABNORMAL_RESET):
        check(code in srv.RESET_ADVICE, f"reset {code} has advice, not 'unknown'")
        check(code in srv.RESET_REASON, f"reset {code} has a human name")


def test_a_recovery_only_mails_if_the_fault_did():
    """Being told a door is broken and never told it recovered is how somebody
    drives out to a coop for nothing. But a sensor that flaps must not send a
    stream of good news either, so the all-clear is gated on the alarm."""
    SENT.clear()
    srv._fault_alerted.clear()
    seed_device("ok-door", int(time.time()))

    def ev(kind, detail):
        return {"type": kind, "detail": detail, "uptime": 10, "boot": 1,
                "epoch": 0, "rssi": -60, "src": 0}

    # A recovery with no preceding fault mail says nothing.
    srv.notify_events("ok-door", [ev("SENSOR_OK", 3)])
    check(len(SENT) == 0, "an unheralded recovery sends nothing")

    srv.notify_events("ok-door", [ev("SENSOR_FAULT", 3)])
    check(len(SENT) == 1, "the fault mails")
    srv.notify_events("ok-door", [ev("SENSOR_OK", 3)])
    check(len(SENT) == 2, "and now the recovery mails")
    check("recovered" in SENT[-1][0], "the recovery subject says so")
    check(SENT[-1][1].get("accent") == "calm",
          "a recovery is calm, not an alert — it must not read as a new problem")

    # The flag is consumed, so a second recovery for the same code is silent.
    srv.notify_events("ok-door", [ev("SENSOR_OK", 3)])
    check(len(SENT) == 2, "a repeated recovery does not re-mail")

    # A different fault code is tracked separately.
    srv.notify_events("ok-door", [ev("SENSOR_FAULT", 6)])
    srv.notify_events("ok-door", [ev("SENSOR_OK", 3)])
    check(len(SENT) == 3, "fault 6's mail does not entitle fault 3 to an all-clear")

    check("SENSOR_OK" in srv.EVENT_LABEL, "SENSOR_OK has a human label")


def main():
    srv.init_db()
    # The real server runs migrate() at startup, and some columns the tests
    # exercise (devices.status, which carries the uploaded status line) only
    # exist after it. Without this the suite tests a schema no door ever meets.
    srv.migrate()
    print("log server — alerting tests")
    test_stale_door_alerts_once_then_recovers()
    test_healthy_and_unknown_doors_stay_silent()
    test_stall_cooldown_but_gave_up_always()
    test_inferred_movement_never_renders_as_measured()
    test_an_inferred_open_is_still_an_open()
    test_a_door_movement_says_what_asked_for_it()
    test_a_failsafe_reversal_is_not_rendered_as_an_ordinary_open()
    test_act_source_covers_every_source_the_firmware_can_send()
    test_a_held_closed_door_is_rendered_as_what_it_costs()
    test_held_closed_mails_on_arrival_and_held_open_is_quieter()
    test_a_still_held_closed_door_keeps_being_announced()
    test_the_reminder_does_not_fire_on_a_door_that_is_not_held()
    test_fault_evidence_is_read_according_to_its_fault()
    test_a_lost_reed_is_rendered_and_explained()
    test_a_crashed_door_emails_but_a_normal_restart_does_not()
    test_a_recovery_only_mails_if_the_fault_did()
    print(f"{'FAILED' if FAILS else 'ok    '}  {CHECKS} checks, {FAILS} failed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
