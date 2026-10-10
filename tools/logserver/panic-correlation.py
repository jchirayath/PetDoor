#!/usr/bin/env python3
# Counts the panic/radio-up correlation the parking lot describes. Read-only.
# Run inside the log-server container, where the database lives:
#   sudo docker cp panic-correlation.py podcast-petdoor-1:/tmp/
#   sudo docker exec -i podcast-petdoor-1 python3 -I /tmp/panic-correlation.py
# The September panics are a DIFFERENT fault (Bluedroid heap exhaustion), so
# read only ~#1036 and later. See docs/PARKING_LOT.md.
import sqlite3, datetime
c = sqlite3.connect("/data/petdoor.sqlite3")
c.row_factory = sqlite3.Row
def ts(e): return datetime.datetime.utcfromtimestamp(e).strftime("%m-%d %H:%M")

# Boot N's BOOT row reports why boot N-1 ended. detail is the reset reason.
boots = {}
for r in c.execute("SELECT boot, MIN(epoch) lo, MAX(epoch) hi, COUNT(*) n "
                   "FROM events WHERE device='back-door' AND epoch>1000000000 "
                   "GROUP BY boot ORDER BY boot"):
    boots[r["boot"]] = dict(lo=r["lo"], hi=r["hi"], n=r["n"])

print("=== reset reasons reported at each boot (detail=4 is ESP_RST_PANIC) ===")
panic_terminated = []
for r in c.execute("SELECT boot, detail, epoch, uptime FROM events "
                   "WHERE device='back-door' AND type='BOOT' ORDER BY boot"):
    flag = ""
    if r["detail"] == 4:
        panic_terminated.append(r["boot"] - 1)
        flag = "  <== boot %d died of PANIC" % (r["boot"] - 1)
    print("  boot #%d  reset=%d  %s%s" % (r["boot"], r["detail"], ts(r["epoch"]), flag))

print()
print("=== for each PANIC-terminated boot: was OTA or a MAINT window in it? ===")
print("  %-7s %-13s %-9s %-7s %s" % ("boot", "span (UTC)", "duration", "events", "radio activity in that boot"))
for b in panic_terminated:
    if b not in boots:
        print("  #%-6d (no events uploaded for this boot)" % b); continue
    i = boots[b]
    dur = i["hi"] - i["lo"]
    acts = []
    for q in c.execute("SELECT command, delivered FROM commands WHERE device='back-door' "
                       "AND delivered IS NOT NULL AND delivered BETWEEN ? AND ? ORDER BY delivered",
                       (i["lo"] - 60, i["hi"] + 60)):
        acts.append("%s@%s" % (q["command"].split()[0], ts(q["delivered"])))
    for q in c.execute("SELECT type, epoch, uptime FROM events WHERE device='back-door' "
                       "AND boot=? AND type IN ('MAINT','CONSOLE') ORDER BY uptime", (b,)):
        acts.append("%s@%s" % (q["type"], ts(q["epoch"])))
    print("  #%-6d %-13s %-9s %-7d %s" % (b, ts(i["lo"]), "%.1f h" % (dur/3600.0),
                                          i["n"], ", ".join(acts) if acts else "NONE FOUND"))

print()
print("=== control group: boots that did NOT panic, and their radio activity ===")
clean = [b for b in sorted(boots) if b not in panic_terminated]
for b in clean[-12:]:
    i = boots[b]
    dur = i["hi"] - i["lo"]
    n_ota = c.execute("SELECT COUNT(*) FROM commands WHERE device='back-door' AND command LIKE 'ota%' "
                      "AND delivered BETWEEN ? AND ?", (i["lo"]-60, i["hi"]+60)).fetchone()[0]
    n_maint = c.execute("SELECT COUNT(*) FROM events WHERE device='back-door' AND boot=? "
                        "AND type='MAINT'", (b,)).fetchone()[0]
    print("  #%-6d %-13s %-9s ota=%d maint=%d" % (b, ts(i["lo"]), "%.1f h" % (dur/3600.0), n_ota, n_maint))
