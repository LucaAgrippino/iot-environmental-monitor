#!/usr/bin/env python3
"""Validation helper: one broker outage for the disconnected-case scenarios (UC-09..UC-14 E1, docs/test/validation-plan.md).

Stops the Mosquitto broker on the Pi, lets the Gateway buffer for OUTAGE seconds, raises and clears a temperature alarm on the Field
Device and sends a read request while it is down, starts the broker again and reports what the cloud received afterwards.
The broker is always started again, also on an error. Needs VAL_PASS. usage: outage.py [OUTAGE_SECONDS]
"""
import os, subprocess, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "libraries"))
from ValCloud import ValCloud
from ValBoards import ValBoards

PI = "luca@192.168.0.209"
OUTAGE = int(sys.argv[1]) if len(sys.argv) > 1 else 240


def log(msg):
    print("%s  %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def pi(cmd):
    return subprocess.run(["ssh", "-o", "LogLevel=ERROR", PI, cmd], capture_output=True, text=True, timeout=60)


c = ValCloud()
b = ValBoards()
assert c.login() == 200
b.install_helpers()
t_stop = None
try:
    log("device view before: online, latest ts %s" % c.device_view()["latest"]["ts"])
    r = pi("sudo systemctl stop mosquitto && systemctl is-active mosquitto")
    t_stop = int(time.time())
    log("broker stopped (%s)" % r.stdout.strip().replace("\n", " "))
    time.sleep(45)
    log("UC-14 E1: operator asks for a reading while the gateway is disconnected")
    try:
        st, body = c.send("control", {"command": "read"})
        log("  API answer: %s %s" % (st, body))
        if st == 202:
            row = c.command_result(body["id"], timeout=30)
            log("  command row after 30 s: status=%s %s" % (row["status"], {k: row.get(k) for k in ("reason", "detail", "error") if row.get(k)}))
    except Exception as e:  # the answer is the observation
        log("  API error: %r" % (e,))
    while time.time() < t_stop + 90:
        time.sleep(2)
    log("alarm raised on the field device (threshold 20.00)")
    b.fd_set_config("temp-alarm-high", 2000)
    time.sleep(20)
    log("alarm cleared (threshold 42.00)")
    b.fd_set_config("temp-alarm-high", 4200)
    while time.time() < t_stop + OUTAGE:
        time.sleep(2)
finally:
    r = pi("sudo systemctl start mosquitto; sleep 2; systemctl is-active mosquitto")
    t_up = int(time.time())
    log("broker started again (%s), outage %d s" % (r.stdout.strip().replace("\n", " "), t_up - (t_stop or t_up)))

log("waiting for the gateway to reconnect and flush")
deadline = time.time() + 300
while time.time() < deadline:
    try:
        if c.device_view()["latest"]["ts"] > t_up:
            break
    except Exception:
        pass
    time.sleep(5)
log("telemetry after the restart seen; letting the backlog drain for 90 s")
time.sleep(90)

for kind in ("telemetry", "health", "alarms"):
    rows = c.history(kind, since=t_stop - 30, limit=500)
    key = "recv_ts"
    inside = [x for x in rows if t_stop <= x["ts"] <= t_up]
    has_recv = bool(rows) and all(key in x for x in rows)  # health rows carry no recv_ts
    late = [x for x in inside if x.get(key, 0) >= t_up] if has_recv else None
    log("%s: %d rows since the stop, %d with a device time inside the outage, received after the restart: %s" % (kind, len(rows), len(inside), len(late) if has_recv else "n/a"))
    if has_recv:
        by_recv = sorted(rows, key=lambda x: x[key])
        ts_in_recv_order = [x["ts"] for x in by_recv]
        log("  chronological when read in arrival order: %s" % (ts_in_recv_order == sorted(ts_in_recv_order)))
    stamps = [x["ts"] for x in rows if t_stop <= x["ts"] <= t_up]
    gaps = [b - a for a, b in zip(stamps, stamps[1:])]
    log("  device time of the first row inside the outage: +%s s after the stop; largest gap inside: %s s" % (stamps[0] - t_stop if stamps else "n/a", max(gaps) if gaps else "n/a"))
    if kind == "alarms":
        for x in rows:
            log("  alarm row: ts=%s recv=%s %s" % (x["ts"], x.get(key), {k: x[k] for k in x if k in ("alarm_type", "sensor", "state", "event")}))
    elif inside:
        log("  first/last device time in the outage: %s .. %s (outage %s .. %s)" % (inside[0]["ts"], inside[-1]["ts"], t_stop, t_up))
log("done")
