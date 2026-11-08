"""Turn one MQTT message into stored rows. Pure functions on (database, topic, payload): no broker needed to test.

This is the "rules engine" step: validate, store, and say what happened (the returned event feeds the live stream).
Topics follow the AWS IoT convention the firmware uses: dt/<root>/<device>/... from the device, cmd/<root>/<device>/... to it.
"""
import json
import time

from . import commands, ota

DEVICE_TOPICS = {
    "telemetry", "health", "alarms",
    "ota/result", "ota/request", "config/result", "control/result",
}


def parse_topic(root: str, topic: str):
    """dt/<root>/<device>/<suffix> -> (device, suffix), or None for any other topic."""
    parts = topic.split("/", 3)
    if len(parts) != 4 or parts[0] != "dt" or parts[1] != root or not parts[2]:
        return None
    return (parts[2], parts[3]) if parts[3] in DEVICE_TOPICS else None


def _ts(msg: dict, now: int) -> int:
    """Device time when the device is synchronised with NTP, otherwise the time of reception."""
    if msg.get("sync_state") == "synchronised" and isinstance(msg.get("timestamp"), int):
        return msg["timestamp"]
    return now


def _opt(d, key):
    v = d.get(key) if isinstance(d, dict) else None
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) else None


def handle(db, root: str, topic: str, payload: bytes, now: int | None = None):
    """Store one message. Returns an event dict for the live stream, or None if the message is ignored."""
    now = int(time.time()) if now is None else now
    parsed = parse_topic(root, topic)
    if parsed is None:
        return None
    device, kind = parsed
    try:
        msg = json.loads(payload)
        if not isinstance(msg, dict):
            raise ValueError("not an object")
    except ValueError:
        db.execute("INSERT INTO events(device, recv_ts, kind, payload) VALUES(?,?,?,?)",
                   (device, now, "bad_payload", json.dumps({"topic": topic, "bytes": len(payload)})))
        return {"device": device, "kind": "bad_payload", "ts": now}

    ts = _ts(msg, now)
    db.touch_device(device, now)

    if kind == "telemetry":
        fd = msg.get("field_device") or {}
        db.execute(
            "INSERT INTO telemetry(device, ts, recv_ts, sync_state, temperature_deci_c, humidity_pct, pressure_hpa,"
            " fd_online, fd_temperature_deci_c, fd_humidity_pct, fd_pressure_hpa, raw) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
            (device, ts, now, msg.get("sync_state"), _opt(msg, "temperature_deci_c"), _opt(msg, "humidity_pct"),
             _opt(msg, "pressure_hpa"), int(bool(fd.get("online"))) if fd else None,
             _opt(fd, "temperature_deci_c"), _opt(fd, "humidity_pct"), _opt(fd, "pressure_hpa"),
             json.dumps(msg)))
        return {"device": device, "kind": "telemetry", "ts": ts}

    if kind == "health":
        db.execute("INSERT INTO health(device, ts, recv_ts, raw) VALUES(?,?,?,?)", (device, ts, now, json.dumps(msg)))
        db.touch_device(device, now, fw_version=msg.get("fw_version"), health=msg)
        return {"device": device, "kind": "health", "ts": ts}

    if kind == "alarms":
        db.execute(
            "INSERT INTO alarms(device, ts, recv_ts, sync_state, sensor_id, alarm_type, measured_value_raw,"
            " threshold_value_raw, source) VALUES(?,?,?,?,?,?,?,?,?)",
            (device, ts, now, msg.get("sync_state"), msg.get("sensor_id"), msg.get("alarm_type"),
             _opt(msg, "measured_value_raw"), _opt(msg, "threshold_value_raw"), msg.get("source")))
        return {"device": device, "kind": "alarm", "ts": ts, "alarm_type": msg.get("alarm_type"),
                "sensor_id": msg.get("sensor_id"), "source": msg.get("source"),
                "measured_value_raw": _opt(msg, "measured_value_raw"), "threshold_value_raw": _opt(msg, "threshold_value_raw")}

    if kind == "ota/request":  # one per chunk: too many to store, the OTA service answers them
        return {"device": device, "kind": "ota_request", "ts": now}

    if kind == "config/result" or kind == "control/result":
        commands.on_result(db, device, msg, now)
    if kind == "ota/result":
        ota.on_result(db, device, msg, now)
    name = {"ota/result": "ota", "config/result": "config", "control/result": "control"}[kind]
    db.execute("INSERT INTO events(device, recv_ts, kind, req, result, payload) VALUES(?,?,?,?,?,?)",
               (device, now, name, msg.get("req") if isinstance(msg.get("req"), int) else msg.get("job"),
                msg.get("result"), json.dumps(msg)))
    return {"device": device, "kind": name, "ts": now, "result": msg.get("result"),
            "req": msg.get("req") if name != "ota" else msg.get("job"), "reason": msg.get("reason")}
