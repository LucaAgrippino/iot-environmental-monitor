"""Robot library: the Remote Operator's view of the local cloud (iotcloud on the Raspberry Pi), through its HTTP API.

Settings (environment): VAL_HOST (default envmon.lan), VAL_IP (the Pi, default 192.168.0.209), VAL_USER (default
validator), VAL_PASS (no default: never written to a file).
"""
import json
import os
import socket
import time

import requests
import urllib3

urllib3.disable_warnings()
HOST = os.environ.get("VAL_HOST", "envmon.lan")
IP = os.environ.get("VAL_IP", "192.168.0.209")
_real = socket.getaddrinfo


def _resolve(host, *a, **k):  # the same as curl --resolve: the Caddy site is named envmon.lan, the Pi has no DNS entry
    return _real(IP if host == HOST else host, *a, **k)


socket.getaddrinfo = _resolve


class ValCloud:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self):
        self.s = requests.Session()
        self.s.verify = False
        self.base = "https://%s/api" % HOST
        self.device = None

    # ---- session --------------------------------------------------------------------------------------
    def login(self, user=None, password=None):
        r = self.s.post(self.base + "/auth/login", json={"username": user or os.environ.get("VAL_USER", "validator"),
                                                         "password": password or os.environ["VAL_PASS"]})
        return r.status_code

    def login_status(self, user, password):
        """HTTP status of a login attempt with these credentials, on a throw-away session."""
        return requests.post(self.base + "/auth/login", json={"username": user, "password": password}, verify=False).status_code

    def api_status_without_login(self, path):
        return requests.get(self.base + path, verify=False).status_code

    def device_id(self):
        if not self.device:
            self.device = self.get("/devices")[0]["id"]
        return self.device

    # ---- reads ----------------------------------------------------------------------------------------
    def get(self, path, **params):
        r = self.s.get(self.base + path, params=params)
        r.raise_for_status()
        return r.json()

    def device_view(self):
        return self.get("/devices/" + self.device_id())

    def history(self, kind, since=0, limit=200):
        return self.get("/devices/%s/%s" % (self.device_id(), kind), since=int(since), limit=int(limit))

    def raw_telemetry(self, since):
        """The stored JSON payloads of the telemetry since `since` (the export keeps the 'raw' column)."""
        r = self.s.get(self.base + "/devices/%s/export" % self.device_id(),
                       params={"kind": "telemetry", "format": "json", "since": int(since)})
        r.raise_for_status()
        return [json.loads(x["raw"]) for x in r.json()]

    def now(self):
        return int(time.time())

    # ---- firmware update (UC-18, UC-20) ------------------------------------------------------------------
    def upload_image(self, name, path):
        """PUT one signed .ota file into the cloud's image store; returns the HTTP status."""
        with open(path, "rb") as f:
            r = self.s.put("%s/images/%s" % (self.base, name), data=f.read(), headers={"Content-Type": "application/octet-stream"})
        return r.status_code

    def delete_image(self, name):
        return self.s.delete("%s/images/%s" % (self.base, name)).status_code

    def start_ota(self, image_a, image_b):
        """Start an update job for the device; returns (HTTP status, job row or error text)."""
        r = self.s.post("%s/devices/%s/ota" % (self.base, self.device_id()), json={"image_a": image_a, "image_b": image_b})
        try:
            return r.status_code, r.json()
        except ValueError:
            return r.status_code, r.text

    def ota_job(self, job):
        return self.get("/ota/%d" % int(job))

    def cancel_ota(self, job):
        """Stop serving a job (used to clean up a job started for a device that is not there)."""
        return self.s.post("%s/ota/%d/cancel" % (self.base, int(job))).status_code

    def active_ota_jobs(self):
        """Ids of the device's jobs that are still sent or running."""
        return [j["id"] for j in self.get("/devices/%s/ota" % self.device_id()) if j["state"] in ("sent", "running")]

    def firmware_version(self):
        return self.device_view().get("fw_version")

    # ---- commands -------------------------------------------------------------------------------------
    def send(self, kind, body):
        """POST a command; returns (http status, row or error text)."""
        r = self.s.post("%s/devices/%s/%s" % (self.base, self.device_id(), kind), json=body)
        try:
            return r.status_code, r.json()
        except ValueError:
            return r.status_code, r.text

    def command_result(self, cid, timeout=30):
        """Wait until the command row leaves 'pending'; returns the row."""
        end = time.time() + float(timeout)
        row = None
        while time.time() < end:
            row = self.get("/commands/%d" % int(cid))
            if row["status"] != "pending":
                return row
            time.sleep(0.5)
        return row

    def wait_for(self, kind, since, predicate_key, predicate_value, timeout=30):
        """Wait for a row of `kind` after `since` whose field `predicate_key` equals `predicate_value`; returns it."""
        end = time.time() + float(timeout)
        while time.time() < end:
            for row in self.history(kind, since=since):
                if str(row.get(predicate_key)) == str(predicate_value):
                    return row
            time.sleep(1)
        raise AssertionError("no %s row with %s=%s within %s s" % (kind, predicate_key, predicate_value, timeout))

    def wait_for_telemetry_after(self, t0, timeout=90):
        end = time.time() + float(timeout)
        while time.time() < end:
            rows = [r for r in self.history("telemetry", since=int(t0) - 1) if r["recv_ts"] >= int(t0)]
            if rows:
                return rows[-1]
            time.sleep(2)
        raise AssertionError("no telemetry received within %s s" % timeout)

    def wait_until_offline_then_online(self, offline_timeout=60, online_timeout=240):
        """Used after a restart: the Gateway must come back and publish again. Returns the seconds it took."""
        t0 = time.time()
        start = self.device_view()["last_seen"]
        end = time.time() + float(online_timeout)
        while time.time() < end:
            if self.device_view()["last_seen"] > start + 5 and self.history("health", since=int(t0) - 5):
                return time.time() - t0
            time.sleep(3)
        raise AssertionError("the Gateway did not publish again within %s s" % online_timeout)

    def boot_time(self):
        """Unix time at which the Gateway last started, from its newest health message."""
        h = self.history("health", since=int(time.time()) - 120)[-1]
        return int(h["timestamp"]) - int(h["uptime_s"])
