"""Commands to a device: the request id travels in the message, the device echoes it in its result.

The flow (the AWS counterpart is a Device Shadow desired-state update, or an IoT Job for one-off commands):
  API call -> row in `commands` (status "pending") -> MQTT cmd/<root>/<device>/{config,control} at QoS 1
  device answers dt/<root>/<device>/{config,control}/result with the same `req` -> ingest.on_result sets the status.
A command with no answer within the timeout shows as "timeout" (the device may be offline; the broker keeps a QoS 1
message for a persistent session, so it can still execute later: the status then moves to the real result).
"""
import json
import time

PROFILE_COMMANDS = ("profile_add", "profile_remove")
CONTROL_COMMANDS = ("read", "restart", "restart_confirm", "reset_metrics", "fd_ack_alarm", "fd_reset_metrics") + PROFILE_COMMANDS
MAX_CONFIG_KEYS = 8  # the firmware rejects more than eight keys in one message


class BadCommand(ValueError):
    pass


def validate_config(body) -> dict:
    """Shape only: which keys and values are valid is the device's decision, and its answer is shown to the operator."""
    if not isinstance(body, dict) or not 1 <= len(body) <= MAX_CONFIG_KEYS:
        raise BadCommand(f"send an object with 1..{MAX_CONFIG_KEYS} keys")
    for k, v in body.items():
        if k == "req" or not isinstance(k, str):
            raise BadCommand("'req' is set by the cloud")
        if not isinstance(v, int) or isinstance(v, bool):
            raise BadCommand(f"{k}: values are integers (fixed-point units, see the register map)")
    return body


def _int_member(body: dict, key: str, low: int, high: int) -> int:
    v = body.get(key)
    if not isinstance(v, int) or isinstance(v, bool) or not low <= v <= high:
        raise BadCommand(f"{key} must be an integer {low}..{high}")
    return v


def validate_control(body) -> dict:
    """The message the device gets: the command, and for the profile commands the members the firmware reads
    (the ranges are the firmware's: slave 1..247, map and vendor 1..65535, a label of up to 32 plain characters)."""
    cmd = body.get("command") if isinstance(body, dict) else None
    if cmd not in CONTROL_COMMANDS:
        raise BadCommand(f"command must be one of {', '.join(CONTROL_COMMANDS)}")
    out = {"command": cmd}
    if cmd in PROFILE_COMMANDS:
        out["slave"] = _int_member(body, "slave", 1, 247)
        if cmd == "profile_add":
            out["map"] = _int_member(body, "map", 1, 65535)
            out["vendor"] = _int_member(body, "vendor", 1, 65535)
            desc = body.get("desc")
            if desc is not None:
                if not isinstance(desc, str) or not 1 <= len(desc) <= 32 or any(c in '"\\' or ord(c) < 32 or ord(c) > 126 for c in desc):
                    raise BadCommand("desc must be 1..32 plain ASCII characters, no quote or backslash")
                out["desc"] = desc
    return out


def send(db, publish, root: str, device: str, kind: str, body: dict, now: int | None = None) -> dict:
    """Record the command, publish it, return its row. `publish(topic, payload, qos)` is the MQTT client's."""
    now = int(time.time()) if now is None else now
    if kind == "config":
        payload_body = validate_config(body)
    elif kind == "control":
        payload_body = validate_control(body)
    else:
        raise BadCommand("kind must be config or control")
    cid = db.execute("INSERT INTO commands(device, kind, body, created, status) VALUES(?,?,?,?,?)",
                     (device, kind, json.dumps(payload_body), now, "pending"))
    publish(f"cmd/{root}/{device}/{kind}", json.dumps({**payload_body, "req": cid}, separators=(",", ":")), 1)
    return get(db, cid, now=now)


def on_result(db, device: str, msg: dict, now: int):
    req = msg.get("req")
    if not isinstance(req, int) or isinstance(req, bool):
        return  # a command that did not come from the cloud (the console, a script)
    status = msg.get("result")
    if status not in ("applied", "accepted", "rejected"):
        status = "failed"
    db.execute("UPDATE commands SET status=?, reason=?, finished=? WHERE id=? AND device=?",
               (status, msg.get("reason") or msg.get("key"), now, req, device))


def _present(row: dict, now: int, timeout_s: int) -> dict:
    row = {**row, "body": json.loads(row["body"])}
    if row["status"] == "pending" and now - row["created"] > timeout_s:
        row["status"] = "timeout"
    return row


def get(db, cid: int, now: int | None = None, timeout_s: int = 60):
    rows = db.query("SELECT * FROM commands WHERE id=?", (cid,))
    return _present(rows[0], int(time.time()) if now is None else now, timeout_s) if rows else None


def list_for(db, device: str, limit: int = 50, now: int | None = None, timeout_s: int = 60):
    now = int(time.time()) if now is None else now
    rows = db.query("SELECT * FROM commands WHERE device=? ORDER BY id DESC LIMIT ?", (device, limit))
    return [_present(r, now, timeout_s) for r in rows]
