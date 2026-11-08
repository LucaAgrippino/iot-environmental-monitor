#!/usr/bin/env python3
"""A simulated Gateway: publishes telemetry, health and alarms in the firmware's schema 1.1 and answers the
/config and /control commands the way CloudPublisher does. For developing the cloud side without the boards.

    device_sim.py --device-id sim-0001 --fast          # a message every few seconds instead of every minute
Not simulated yet: OTA (the cloud side of it is not written yet).
"""
import argparse
import json
import struct
import zlib
import random
import ssl
import time

import paho.mqtt.client as mqtt

REMOTE_KEYS = {  # key -> (min, max): the Tier 2 keys of cloud_publisher_json.c, with plausible ranges
    "polling_interval_ms": (100, 10000), "modbus_poll_period_ms": (100, 60000),
    "telemetry_interval_ms": (1000, 3_600_000), "health_interval_ms": (1000, 3_600_000),
    "temp_alarm_high": (-4000, 8500), "temp_alarm_low": (-4000, 8500), "temp_hysteresis": (0, 1000),
    "humidity_alarm_high": (0, 10000), "humidity_alarm_low": (0, 10000), "humidity_hysteresis": (0, 1000),
    "pressure_alarm_high": (3000, 11000), "pressure_alarm_low": (3000, 11000), "pressure_hysteresis": (0, 100),
}
LOCAL_KEYS = {"filter_alpha", "modbus_slave_addr", "mqtt_broker", "mqtt_port", "ntp_servers", "ntp_interval_min"}


class Sim:
    def __init__(self, a):
        self.a, self.id, self.root = a, a.device_id, a.root
        self.cfg = {"telemetry_interval_ms": a.telemetry_s * 1000, "health_interval_ms": a.health_s * 1000,
                    "temp_alarm_high": 3000, "temp_alarm_low": 500, "temp_hysteresis": 50}
        self.t, self.h, self.p = 22.0, 45.0, 1013.0
        self.alarm_on = False
        self.boot = time.time()
        self.restart_armed_until = 0.0
        self.fw = a.fw_version
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=self.id)
        if a.cafile:
            self.c.tls_set(ca_certs=a.cafile, certfile=a.cert, keyfile=a.key, cert_reqs=ssl.CERT_REQUIRED)
        self.c.on_connect = lambda c, u, f, r, p=None: c.subscribe(
            [(f"cmd/{self.root}/{self.id}/{t}", q) for t, q in (("config", 1), ("control", 1), ("ota", 1), ("ota/data", 0))])
        self.c.on_message = self.on_message

    def common(self):
        return {"schema_version": "1.1", "device_id": self.id, "timestamp": int(time.time()), "sync_state": "synchronised"}

    def pub(self, suffix, msg, qos=0):
        self.c.publish(f"dt/{self.root}/{self.id}/{suffix}", json.dumps(msg, separators=(",", ":")), qos)

    def step(self):
        self.t += random.uniform(-0.4, 0.4) + (self.a.drift if self.a.drift else 0)
        self.h = min(95, max(10, self.h + random.uniform(-1, 1)))
        self.p += random.uniform(-0.3, 0.3)

    def telemetry(self):
        msg = {**self.common(), "temperature_deci_c": round(self.t * 10), "humidity_pct": round(self.h), "pressure_hpa": round(self.p)}
        msg.update({k: None for k in ("accel_x_mg", "accel_y_mg", "accel_z_mg", "gyro_x_mdps", "gyro_y_mdps", "gyro_z_mdps",
                                      "mag_x_mgauss", "mag_y_mgauss", "mag_z_mgauss")})
        msg["field_device"] = {"online": True, "temperature_deci_c": round(self.t * 10 - 8), "humidity_pct": round(self.h + 2),
                               "pressure_hpa": round(self.p)}
        self.pub("telemetry", msg)
        self.check_alarm()

    def check_alarm(self):
        hi, lo, hy = self.cfg["temp_alarm_high"], self.cfg["temp_alarm_low"], self.cfg["temp_hysteresis"]
        centi = round(self.t * 100)
        kind = None
        if not self.alarm_on and (centi > hi or centi < lo):
            self.alarm_on, kind = True, ("HIGH" if centi > hi else "LOW")
            self.limit = hi if centi > hi else lo
        elif self.alarm_on and lo + hy < centi < hi - hy:
            self.alarm_on, kind = False, "CLEAR"
        if kind:
            self.pub("alarms", {**self.common(), "sensor_id": "temperature", "alarm_type": kind, "measured_value_raw": centi,
                                "threshold_value_raw": getattr(self, "limit", hi), "source": "field_device"}, 1)

    def health(self):
        self.pub("health", {**self.common(), "fw_version": self.fw, "uptime_s": int(time.time() - self.boot),
                            "lifecycle_state": "OPERATIONAL", "last_reset_cause": "POWER_ON", "sensor_fail_count": 0,
                            "alarm_raise_count": 0, "config_write_failed": False, "modbus_transactions_ok": 1000,
                            "modbus_timeouts": 1, "modbus_link_online": True, "mqtt_publishes_sent": 100,
                            "mqtt_publishes_failed": 0, "mqtt_reconnect_count": 0, "wifi_rssi_dbm": -55,
                            "cloud_connected": True, "buffer_entry_count": 0, "buffer_overflow_count": 0,
                            "ntp_sync_fail_count": 0, "last_ntp_sync_epoch": int(time.time()), "cpu_load_pct": 12,
                            "arena_min_free_bytes": 9000, "free_ram_bytes": 800, "mcu_temp_valid": True, "mcu_temp_deci_c": 410,
                            "stack_free_words": [120, 80, 200]})

    def result(self, suffix, event, req, ok, reason=None, **extra):
        msg = {**self.common(), "event": event, **({"req": req} if req is not None else {}), **extra,
               "result": ok}
        if reason:
            msg["reason"] = reason
        self.pub(suffix, msg, 1)

    def on_message(self, c, u, m):
        try:
            body = json.loads(m.payload)
        except ValueError:
            body = None
        if m.topic.endswith("/ota/data"):
            return self.on_ota_data(m.payload)
        kind = m.topic.rsplit("/", 1)[1]
        if kind == "ota":
            return self.on_ota(body)
        req = body.get("req") if isinstance(body, dict) and isinstance(body.get("req"), int) else None
        if kind == "config":
            self.on_config(body, req)
        else:
            self.on_control(body, req)

    # -- OTA: pull slot B chunk by chunk, like UpdateService, then report. "--ota-fail" ends in a failure.
    def ota_report(self, result, reason=None, job=None):
        job = job if job is not None else self.ota["job"]
        msg = {**self.common(), "event": "ota", "job": job, "result": result, **({"reason": reason} if reason else {})}
        self.pub("ota/result", msg, 1)

    def ota_request(self):
        o = self.ota
        n = min(1024, o["size"] - o["offset"])
        self.pub("ota/request", {"job": o["job"], "slot": "B", "offset": o["offset"], "len": n})

    def on_ota(self, body):
        if not isinstance(body, dict) or not isinstance(body.get("job"), int) or not isinstance(body.get("size_b"), int):
            return
        self.ota = {"job": body["job"], "size": body["size_b"], "offset": 0, "crc": 0}
        self.ota_report("accepted")
        self.ota_request()

    def on_ota_data(self, payload):
        o = getattr(self, "ota", None)
        if not o or len(payload) < 9:
            return
        job, offset = struct.unpack("<II", payload[:8])
        if job != o["job"] or offset != o["offset"]:
            return  # a duplicate or stale chunk: the next request re-syncs
        o["crc"] = zlib.crc32(payload[8:], o["crc"])
        o["offset"] += len(payload) - 8
        if o["offset"] < o["size"]:
            self.ota_request()
            return
        self.ota = None
        if self.a.ota_fail:
            self.ota_report("failed", "hash_mismatch", job=o["job"])
        else:
            self.ota_report("success", job=o["job"])
            self.fw = "ota-" + format(o["crc"], "08x")  # shows in the next health report

    def on_config(self, body, req):
        if not isinstance(body, dict):
            return self.result("config/result", "config", req, "rejected", "bad_json")
        pairs = {k: v for k, v in body.items() if k != "req"}
        if not pairs or len(pairs) > 8:
            return self.result("config/result", "config", req, "rejected", "bad_json")
        for k, v in pairs.items():
            if k in LOCAL_KEYS:
                return self.result("config/result", "config", req, "rejected", "not_remote", key=k)
            if k not in REMOTE_KEYS:
                return self.result("config/result", "config", req, "rejected", "unknown_key", key=k)
            lo, hi = REMOTE_KEYS[k]
            if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
                return self.result("config/result", "config", req, "rejected", "invalid_value", key=k)
        self.cfg.update(pairs)  # all or nothing, like the firmware
        self.result("config/result", "config", req, "applied")

    def on_control(self, body, req):
        cmd = body.get("command") if isinstance(body, dict) else None
        def res(ok, reason=None):
            msg = {**self.common(), "event": "control", "command": cmd or "unknown", **({"req": req} if req is not None else {}),
                   "result": "accepted" if ok else "rejected", **({"reason": reason} if reason else {})}
            self.pub("control/result", msg, 1)
        if cmd == "read":
            self.step(); self.telemetry(); res(True)
        elif cmd == "restart":
            self.restart_armed_until = time.time() + 30; res(True)
        elif cmd == "restart_confirm":
            if time.time() < self.restart_armed_until:
                res(True); self.boot = time.time(); self.restart_armed_until = 0
            else:
                res(False, "failed")
        elif cmd == "reset_metrics":
            res(True)
        else:
            res(False, "unknown_command" if cmd else "bad_format")

    def run(self):
        self.c.connect(self.a.host, self.a.port, keepalive=30)
        self.c.loop_start()
        nt = nh = 0.0
        try:
            while True:
                now = time.time()
                if now >= nt:
                    self.step(); self.telemetry(); nt = now + self.cfg["telemetry_interval_ms"] / 1000
                if now >= nh:
                    self.health(); nh = now + self.cfg["health_interval_ms"] / 1000
                time.sleep(0.2)
        except KeyboardInterrupt:
            self.c.loop_stop(); self.c.disconnect()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="127.0.0.1"); p.add_argument("--port", type=int, default=1883)
    p.add_argument("--cafile"); p.add_argument("--cert"); p.add_argument("--key")
    p.add_argument("--device-id", default="sim-0001"); p.add_argument("--root", default="iotmonitor")
    p.add_argument("--fw-version", default="0.9.0")
    p.add_argument("--telemetry-s", type=int, default=60); p.add_argument("--health-s", type=int, default=300)
    p.add_argument("--fast", action="store_true", help="telemetry every 3 s, health every 15 s")
    p.add_argument("--drift", type=float, default=0.0, help="degrees added to the temperature each step (to provoke an alarm)")
    p.add_argument("--ota-fail", action="store_true", help="finish every OTA with a failed result")
    a = p.parse_args()
    if a.fast:
        a.telemetry_s, a.health_s = 3, 15
    Sim(a).run()


if __name__ == "__main__":
    main()
