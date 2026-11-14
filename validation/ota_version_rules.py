#!/usr/bin/env python3
"""Validation helper: the anti-rollback rule on the real Gateway (security review S-4, FS-D20).

Signs the bodies of the firmware that runs now again with other version numbers and sends them as updates:
  1. an OLDER version  -> must be refused, reason version_not_newer
  2. the SAME version  -> must be refused, reason version_not_newer
  3. a NEWER version   -> must install (success) and the Gateway must report it
Each refused update downloads the whole image first (about 3 minutes) and is refused at the verify step; nothing is installed.
Needs VAL_PASS. usage: ota_version_rules.py BODY_DIR RUNNING_PATCH     (RUNNING_PATCH: the patch number of the running 1.1.<n>)
"""
import json, os, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "libraries"))
from ValCloud import ValCloud

ROOT = os.path.abspath(os.path.join(HERE, ".."))
KEY = os.path.expanduser("~/keys/ota_release.pem")
OUT = os.path.join(HERE, "build", "ota_version_rules.log")


def log(msg):
    line = "%s  %s" % (time.strftime("%H:%M:%S"), msg)
    print(line, flush=True)
    with open(OUT, "a") as f:
        f.write(line + "\n")


def sign(body_dir, patch, tmp):
    os.makedirs(tmp, exist_ok=True)
    for s, S in (("a", "A"), ("b", "B")):
        subprocess.run(["python3", os.path.join(ROOT, "tools", "ota", "make_image.py"), "sign", "--body", os.path.join(body_dir, "gateway_slot_%s.bin" % s),
                        "--slot", S, "--version", "1.1.%d" % patch, "--private", KEY, "--out", os.path.join(tmp, "gateway_slot_%s.ota" % s)],
                       check=True, capture_output=True)


def run_round(c, body_dir, patch, label):
    tmp = os.path.join(HERE, "build", "version_rule_images")
    sign(body_dir, patch, tmp)
    for n in ("a", "b"):
        c.delete_image("val-v-%s" % n)
        assert c.upload_image("val-v-%s" % n, os.path.join(tmp, "gateway_slot_%s.ota" % n)) == 201
    for j in c.active_ota_jobs():
        c.cancel_ota(j)
    st, job = c.start_ota("val-v-a", "val-v-b")
    assert st == 202, (st, job)
    t0 = time.time()
    while time.time() - t0 < 900:
        row = c.ota_job(job["id"])
        if row["state"] not in ("sent", "running"):
            break
        time.sleep(5)
    else:
        c.cancel_ota(job["id"])
        row = c.ota_job(job["id"])
    detail = json.loads(row["detail"]) if row.get("detail") else {}
    log("%s: version 1.1.%d -> job %s, reason %r, version reported by the Gateway %r (%d s)"
        % (label, patch, row["state"], detail.get("reason"), detail.get("version"), time.time() - t0))
    for n in ("a", "b"):
        c.delete_image("val-v-%s" % n)
    return row["state"], detail.get("reason")


def main():
    body_dir, running = sys.argv[1], int(sys.argv[2])
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").close()
    c = ValCloud()
    assert c.login() == 200
    log("running firmware %s" % c.firmware_version())
    results = []
    for label, patch, want_state, want_reason in (("older", running - 16, "failed", "version_not_newer"), ("same", running, "failed", "version_not_newer"),
                                                  ("newer", running + 1, "success", None)):
        state, reason = run_round(c, body_dir, patch, label)
        ok = state == want_state and (want_reason is None or reason == want_reason)
        results.append(ok)
        log("  -> %s" % ("as expected" if ok else "NOT AS EXPECTED (wanted %s %s)" % (want_state, want_reason or "")))
        if label == "newer" and state == "success":
            deadline = time.time() + 420
            while time.time() < deadline:
                d = c.device_view()
                if d["online"] and d["fw_version"] == "1.1.%d" % patch and d["health"]["lifecycle_state"] == "operational":
                    break
                time.sleep(10)
            log("  Gateway now reports %s" % c.firmware_version())
        time.sleep(15)
    log("done: %s" % ("all three as expected" if all(results) else "SOME NOT AS EXPECTED"))


main()
