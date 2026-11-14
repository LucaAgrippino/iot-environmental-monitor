#!/usr/bin/env python3
"""Validation helper: a series of interrupted OTA downloads on the real Gateway (finding F-14, UC-18 E2).

Each round signs the same firmware bodies again with the next version (so no rebuild is needed), uploads them, starts an update job and,
when the cloud has served TRIGGER bytes, stops the Mosquitto broker on the Pi for OUTAGE seconds. It then waits for the job to end and for
the Gateway to report the new version, and writes one line per round. The broker is always started again, also on an error. A job that does
not end within JOB_TIMEOUT seconds is cancelled and noted as HUNG (the failure that F-14 describes); the series goes on.
Needs VAL_PASS. usage: ota_series.py BODY_DIR FIRST_VERSION_PATCH
   BODY_DIR  a release directory with gateway_slot_a.bin and gateway_slot_b.bin (the firmware that runs now)
   FIRST_VERSION_PATCH  the patch number of the first new version (1.1.<n>), higher than the one running
"""
import os, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "libraries"))
from ValCloud import ValCloud
from ValBoards import ValBoards

ROOT = os.path.abspath(os.path.join(HERE, ".."))
KEY = os.path.expanduser("~/keys/ota_release.pem")
MAKE_IMAGE = os.path.join(ROOT, "tools", "ota", "make_image.py")
# (bytes served when the broker is cut, seconds it stays down)
ROUNDS = [(30000, 25), (70000, 40), (100000, 25), (130000, 15), (160000, 25), (190000, 40), (220000, 25), (250000, 15), (280000, 25), (310000, 40)]
JOB_TIMEOUT = 900
OUT = os.path.join(HERE, "build", "ota_series.log")


def log(msg):
    line = "%s  %s" % (time.strftime("%H:%M:%S"), msg)
    print(line, flush=True)
    with open(OUT, "a") as f:
        f.write(line + "\n")


def sign(body_dir, patch, tmp):
    os.makedirs(tmp, exist_ok=True)
    for s, S in (("a", "A"), ("b", "B")):
        subprocess.run(["python3", MAKE_IMAGE, "sign", "--body", os.path.join(body_dir, "gateway_slot_%s.bin" % s), "--slot", S,
                        "--version", "1.1.%d" % patch, "--private", KEY, "--out", os.path.join(tmp, "gateway_slot_%s.ota" % s)],
                       check=True, capture_output=True)


def main():
    body_dir, first = sys.argv[1], int(sys.argv[2])
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").close()
    c, b = ValCloud(), ValBoards()
    assert c.login() == 200
    log("series start: running firmware %s, %d rounds" % (c.firmware_version(), len(ROUNDS)))
    hung = failed = ok = 0
    try:
        for i, (trigger, outage) in enumerate(ROUNDS):
            patch = first + i
            version = "1.1.%d" % patch
            before = c.firmware_version()
            tmp = os.path.join(HERE, "build", "series_images")
            sign(body_dir, patch, tmp)
            for n in ("a", "b"):
                c.delete_image("val-s-%s" % n)
                assert c.upload_image("val-s-%s" % n, os.path.join(tmp, "gateway_slot_%s.ota" % n)) == 201
            for j in c.active_ota_jobs():
                c.cancel_ota(j)
            st, job = c.start_ota("val-s-a", "val-s-b")
            if st != 202:
                log("round %d: could not start the job: %s %s" % (i + 1, st, job))
                break
            t0, cut_at, state, cut_done = time.time(), None, "sent", False
            while time.time() - t0 < JOB_TIMEOUT:
                row = c.ota_job(job["id"])
                state = row["state"]
                if state not in ("sent", "running"):
                    break
                if not cut_done and row["sent_bytes"] >= trigger:
                    cut_at = row["sent_bytes"]
                    log("round %d: cutting the broker for %d s at %d bytes" % (i + 1, outage, cut_at))
                    b.pi_shell("sudo systemctl stop mosquitto; sleep %d; sudo systemctl start mosquitto" % outage, timeout=outage + 90)
                    cut_done = True
                time.sleep(2)
            else:
                state = "HUNG"
            secs = int(time.time() - t0)
            if state == "HUNG":
                row = c.ota_job(job["id"])
                c.cancel_ota(job["id"])
                hung += 1
            # the new version should be running a few minutes after a success; wait for the Gateway either way
            fw, d, deadline = None, {}, time.time() + 420
            while time.time() < deadline:
                try:
                    d = c.device_view()
                    fw = d["fw_version"]
                    if d["online"] and (fw == version or state in ("failed", "HUNG", "cancelled", "rejected")) and d["health"]["lifecycle_state"] == "operational":
                        break
                except Exception:
                    pass
                time.sleep(10)
            if state == "success" and fw == version:
                ok += 1
            elif state != "HUNG":
                failed += 1
            log("round %d: version %s, cut at %s of %d bytes for %d s -> job %s after %d s; Gateway reports %s (was %s)"
                % (i + 1, version, cut_at, row["size_b"], outage, state, secs, fw, before))
            for n in ("a", "b"):
                c.delete_image("val-s-%s" % n)
            if fw is None or not d.get("online"):
                log("the Gateway is not reporting: the series stops here")
                break
            if state in ("HUNG", "failed") and fw != version:
                body = d["health"]["lifecycle_state"]
                log("  (Gateway stayed on %s, lifecycle %s)" % (fw, body))
            time.sleep(20)
    finally:
        r = b.pi_shell("sudo systemctl start mosquitto; sleep 1; systemctl is-active mosquitto")
        log("broker: %s" % r.strip())
        log("series end: %d succeeded, %d ended in another state, %d hung" % (ok, failed, hung))


main()
