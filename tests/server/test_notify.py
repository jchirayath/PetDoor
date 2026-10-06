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


def main():
    srv.init_db()
    print("log server — alerting tests")
    test_stale_door_alerts_once_then_recovers()
    test_healthy_and_unknown_doors_stay_silent()
    test_stall_cooldown_but_gave_up_always()
    test_inferred_movement_never_renders_as_measured()
    test_an_inferred_open_is_still_an_open()
    print(f"{'FAILED' if FAILS else 'ok    '}  {CHECKS} checks, {FAILS} failed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
