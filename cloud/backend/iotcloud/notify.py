"""Alerts: turn what happens (an alarm, a device going silent, an OTA result) into a message on the operator's phone,
by e-mail or to another system (the AWS counterpart: SNS topics with subscriptions, and CloudWatch alarms).

    event ──> which channels want it? ──> queue ──> worker thread: send (3 tries) ──> notify_log

- A channel is one destination: ntfy (push to the phone app), webhook (JSON POST, signed), or e-mail (SMTP).
  Each channel lists the events it wants, so the phone can get alarms while the mailbox gets only OTA results.
- Sending never happens on the MQTT thread: a slow server must not delay the ingest of the next reading.
- A channel that sends more than RATE_LIMIT messages in a minute drops the rest (logged as "suppressed"): a fault
  storm should not bury the phone, or get the sender blocked.
- "Device offline" has no message to react to, so a watcher compares each device's last message with the online
  window. It tells you once when the device goes quiet and once when it returns, not every 15 seconds.
Secrets (ntfy token, webhook secret, SMTP password) are stored in the database and never returned by the API.
"""
import hashlib
import hmac
import json
import logging
import queue
import re
import smtplib
import ssl
import threading
import time
import urllib.error
import urllib.request
from collections import deque
from email.message import EmailMessage

log = logging.getLogger("iotcloud.notify")

EVENTS = {
    "alarm": "an alarm is raised (HIGH or LOW)",
    "alarm_clear": "an alarm is cleared",
    "device_offline": "a device stops sending",
    "device_online": "a device is back",
    "ota_result": "an OTA update ends (success, failed, rolled back, rejected)",
}
DEFAULT_EVENTS = ("alarm", "device_offline", "ota_result")
OTA_TERMINAL = ("success", "failed", "rolled_back", "rejected")
RATE_LIMIT = 20            # messages per channel per minute
ATTEMPTS, RETRY_DELAYS = 3, (1, 5)
SECRET_FIELDS = {"ntfy": ("token",), "webhook": ("secret",), "email": ("password",)}
MASK = "********"
TOPIC_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")


class NotifyError(ValueError):
    pass


# -- messages --------------------------------------------------------------------------------------------------------
# The firmware sends alarm values in register units: 0.01 degC, 0.01 %RH, 0.1 hPa.
UNITS = {"temperature": (100, "°C", 2), "humidity": (100, "%", 1), "pressure": (10, "hPa", 1)}


def fmt_value(sensor, raw):
    div, unit, nd = UNITS.get(sensor, (1, "", 0))
    return f"{raw / div:.{nd}f} {unit}".strip() if isinstance(raw, (int, float)) else "?"


def classify(event: dict):
    """The notification kind of a hub event, or None when it is not something to tell anyone."""
    k = event.get("kind")
    if k == "alarm":
        return "alarm_clear" if event.get("alarm_type") == "CLEAR" else "alarm"
    if k == "ota" and event.get("result") in OTA_TERMINAL:
        return "ota_result"
    return k if k in ("device_offline", "device_online") else None


def describe(kind: str, event: dict, name: str) -> dict:
    """{"title", "message", "priority" 1-5, "tags"}: ntfy's model, which the other channels reuse."""
    if kind in ("alarm", "alarm_clear"):
        s = event.get("sensor_id", "sensor")
        meas, lim = fmt_value(s, event.get("measured_value_raw")), fmt_value(s, event.get("threshold_value_raw"))
        src = "field device" if event.get("source") == "field_device" else "gateway"
        if kind == "alarm":
            return {"title": f"{s.capitalize()} {event.get('alarm_type')} on {name}", "priority": 4,
                    "message": f"{meas} (limit {lim}), measured by the {src}", "tags": ["warning"]}
        return {"title": f"{s.capitalize()} back to normal on {name}", "priority": 3,
                "message": f"{meas}, measured by the {src}", "tags": ["white_check_mark"]}
    if kind == "device_offline":
        return {"title": f"{name} stopped sending", "priority": 4, "tags": ["red_circle"],
                "message": f"Nothing received for {event.get('silent_s', '?')} s"}
    if kind == "device_online":
        down = event.get("down_s")
        return {"title": f"{name} is back", "priority": 3, "tags": ["green_circle"],
                "message": f"Sending again after {down // 60} min" if isinstance(down, int) else "Sending again"}
    res = event.get("result")
    detail = f" ({event['reason']})" if event.get("reason") else ""
    return {"title": f"Firmware update on {name}: {res}", "priority": 3 if res == "success" else 4,
            "tags": ["rocket"] if res == "success" else ["x"], "message": f"Update job {event.get('req', '?')}: {res}{detail}"}


# -- senders: (config, message-dict) -> None, raise on failure ---------------------------------------------------------
def _post(url, data: bytes, headers: dict, timeout=10):
    req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:  # noqa: S310 - http(s) only, checked when the channel is saved
        r.read(1024)


def send_ntfy(cfg: dict, m: dict):
    body = json.dumps({"topic": cfg["topic"], "title": m["title"], "message": m["message"], "priority": m["priority"],
                       "tags": m["tags"]}).encode()
    headers = {"Content-Type": "application/json"}
    if cfg.get("token"):
        headers["Authorization"] = f"Bearer {cfg['token']}"
    _post(cfg["url"].rstrip("/") + "/", body, headers)


def send_webhook(cfg: dict, m: dict):
    body = json.dumps({k: m[k] for k in ("event", "device", "title", "message", "priority", "ts", "data") if k in m}).encode()
    headers = {"Content-Type": "application/json"}
    if cfg.get("secret"):  # the receiver checks that the message is ours: HMAC-SHA256 of the body
        headers["X-Signature"] = "sha256=" + hmac.new(cfg["secret"].encode(), body, hashlib.sha256).hexdigest()
    _post(cfg["url"], body, headers)


def send_email(cfg: dict, m: dict):
    msg = EmailMessage()
    msg["Subject"], msg["From"], msg["To"] = f"[Environmental Monitor] {m['title']}", cfg["from"], ", ".join(cfg["to"])
    msg.set_content(f"{m['message']}\n\nDevice: {m.get('device', '')}\nEvent: {m.get('event', '')}\n")
    sec = cfg.get("security", "starttls")
    if sec == "ssl":
        smtp = smtplib.SMTP_SSL(cfg["host"], cfg["port"], timeout=15, context=ssl.create_default_context())
    else:
        smtp = smtplib.SMTP(cfg["host"], cfg["port"], timeout=15)
    with smtp:
        if sec == "starttls":
            smtp.starttls(context=ssl.create_default_context())
        if cfg.get("username"):
            smtp.login(cfg["username"], cfg.get("password", ""))
        smtp.send_message(msg)


SENDERS = {"ntfy": send_ntfy, "webhook": send_webhook, "email": send_email}


# -- channels ----------------------------------------------------------------------------------------------------------
def _url(v, what):
    if not isinstance(v, str) or not re.match(r"^https?://[^\s/]+", v):
        raise NotifyError(f"{what}: a http:// or https:// address")
    return v


def validate(kind: str, cfg: dict) -> dict:
    if kind not in SENDERS:
        raise NotifyError(f"kind must be one of {', '.join(SENDERS)}")
    if not isinstance(cfg, dict):
        raise NotifyError("config must be an object")
    if kind == "ntfy":
        out = {"url": _url(cfg.get("url"), "url"), "topic": cfg.get("topic", "")}
        if not TOPIC_RE.match(str(out["topic"])):
            raise NotifyError("topic: letters, digits, - and _ (a long random one is the only secret of an open ntfy topic)")
        if cfg.get("token"):
            out["token"] = str(cfg["token"])
    elif kind == "webhook":
        out = {"url": _url(cfg.get("url"), "url")}
        if cfg.get("secret"):
            out["secret"] = str(cfg["secret"])
    else:
        to = cfg.get("to")
        if isinstance(to, str):
            to = [a.strip() for a in to.split(",") if a.strip()]
        if not cfg.get("host") or not to or not all("@" in a for a in to) or "@" not in str(cfg.get("from", "")):
            raise NotifyError("email needs host, from and to (addresses)")
        sec = cfg.get("security", "starttls")
        if sec not in ("starttls", "ssl", "none"):
            raise NotifyError("security: starttls, ssl or none")
        try:
            port = int(cfg.get("port") or {"ssl": 465, "starttls": 587, "none": 25}[sec])
        except (TypeError, ValueError):
            raise NotifyError("port must be a number")
        out = {"host": str(cfg["host"]), "port": port, "security": sec, "from": cfg["from"], "to": to}
        if cfg.get("username"):
            out.update(username=str(cfg["username"]), password=str(cfg.get("password", "")))
    return out


def check_events(events) -> list:
    events = list(DEFAULT_EVENTS) if events is None else events
    if not isinstance(events, list) or not events or any(e not in EVENTS for e in events):
        raise NotifyError(f"events: a non-empty list of {', '.join(EVENTS)}")
    return events


def present(row: dict) -> dict:
    """A channel as the API shows it: secrets replaced by a mask."""
    cfg = json.loads(row["config"])
    for f in SECRET_FIELDS[row["kind"]]:
        if f in cfg:
            cfg[f] = MASK
    return {"id": row["id"], "name": row["name"], "kind": row["kind"], "config": cfg, "events": json.loads(row["events"]),
            "enabled": bool(row["enabled"])}


def save_channel(db, body: dict, channel_id: int | None = None, now: int | None = None) -> dict:
    kind = body.get("kind")
    old = None
    if channel_id is not None:
        rows = db.query("SELECT * FROM notify_channels WHERE id=?", (channel_id,))
        if not rows:
            raise NotifyError("no such channel")
        old = rows[0]
        kind = old["kind"]  # a channel's kind does not change; make a new one
    name = str(body.get("name") or (old or {}).get("name") or "").strip()
    if not name or len(name) > 64:
        raise NotifyError("name: 1 to 64 characters")
    raw_cfg = dict(body.get("config") or {})
    if old and "config" not in body:  # e.g. only the events or the enabled flag changed
        raw_cfg = json.loads(old["config"])
    if old:  # a secret left out, or sent back as the mask, keeps its stored value
        stored = json.loads(old["config"])
        for f in SECRET_FIELDS[kind]:
            if raw_cfg.get(f) in (None, MASK) and f in stored:
                raw_cfg[f] = stored[f]
    cfg = validate(kind, raw_cfg)
    events = check_events(body.get("events", json.loads(old["events"]) if old else None))
    enabled = int(bool(body.get("enabled", old["enabled"] if old else True)))
    try:
        if old:
            db.execute("UPDATE notify_channels SET name=?, config=?, events=?, enabled=? WHERE id=?",
                       (name, json.dumps(cfg), json.dumps(events), enabled, channel_id))
        else:
            channel_id = db.execute("INSERT INTO notify_channels(name, kind, config, events, enabled, created) VALUES(?,?,?,?,?,?)",
                                    (name, kind, json.dumps(cfg), json.dumps(events), enabled,
                                     int(time.time()) if now is None else now))
    except Exception as e:  # the only constraint is the unique name
        if "UNIQUE" in str(e):
            raise NotifyError("a channel with this name exists")
        raise
    return present(db.query("SELECT * FROM notify_channels WHERE id=?", (channel_id,))[0])


def list_channels(db):
    return [present(r) for r in db.query("SELECT * FROM notify_channels ORDER BY id")]


def delete_channel(db, channel_id: int) -> bool:
    existed = bool(db.query("SELECT 1 FROM notify_channels WHERE id=?", (channel_id,)))
    db.execute("DELETE FROM notify_channels WHERE id=?", (channel_id,))
    return existed


# -- the notifier ------------------------------------------------------------------------------------------------------
class Notifier:
    def __init__(self, db, online_window_s=150, senders=None, clock=time.time, sleep=time.sleep):
        self.db, self.window, self.senders = db, online_window_s, senders or SENDERS
        self.clock, self.sleep = clock, sleep
        self.q: queue.Queue = queue.Queue(maxsize=500)
        self._sent: dict[int, deque] = {}
        self._offline = {r["device"]: r["offline_since"] for r in db.query("SELECT * FROM device_state WHERE offline_since IS NOT NULL")}
        self._started = clock()
        self._stop = threading.Event()
        self._threads = []

    # lifecycle -------------------------------------------------------------------------------------------------------
    def start(self):
        for target in (self._worker, self._watcher):
            t = threading.Thread(target=target, daemon=True, name=f"notify-{target.__name__}")
            t.start()
            self._threads.append(t)

    def stop(self):
        self._stop.set()
        self.q.put(None)

    # intake (called from the MQTT thread: must be quick) ------------------------------------------------------------------
    def submit(self, event: dict):
        device = event.get("device")
        if device and event.get("kind") not in ("device_offline", "device_online"):
            self.seen(device)  # a real message is a sign of life; the watcher's own events are not
        kind = classify(event)
        if kind is None:
            return
        name = self._name(device)
        m = {**describe(kind, event, name), "event": kind, "device": device, "ts": int(self.clock()),
             "data": {k: v for k, v in event.items() if k not in ("kind",)}}
        for ch in self.db.query("SELECT * FROM notify_channels WHERE enabled=1"):
            if kind in json.loads(ch["events"]):
                try:
                    self.q.put_nowait((ch, m))
                except queue.Full:
                    log.warning("notification queue full: dropped %s", kind)

    def _name(self, device):
        rows = self.db.query("SELECT name FROM devices WHERE id=?", (device,)) if device else []
        return (rows[0]["name"] if rows and rows[0]["name"] else device) or "device"

    def seen(self, device: str):
        """A message arrived from this device: if we had said it was offline, say it is back."""
        since = self._offline.pop(device, None)
        if since is not None:
            self.db.execute("UPDATE device_state SET offline_since=NULL WHERE device=?", (device,))
            self.submit({"device": device, "kind": "device_online", "down_s": int(self.clock() - since)})

    def tick(self, now: float | None = None):
        """One pass of the watcher: devices silent for longer than the online window are reported, once."""
        now = self.clock() if now is None else now
        if now - self._started < self.window:  # after a restart, give the devices time to be heard again
            return
        for d in self.db.query("SELECT id, last_seen FROM devices"):
            silent = int(now - d["last_seen"])
            if silent > self.window and d["id"] not in self._offline:
                self._offline[d["id"]] = d["last_seen"]  # the silence began with the last message, not when we noticed
                self.db.execute("INSERT INTO device_state(device, offline_since) VALUES(?,?) "
                                "ON CONFLICT(device) DO UPDATE SET offline_since=excluded.offline_since", (d["id"], d["last_seen"]))
                self.submit({"device": d["id"], "kind": "device_offline", "silent_s": silent})

    # delivery --------------------------------------------------------------------------------------------------------
    def deliver(self, ch: dict, m: dict) -> tuple[str, str | None]:
        """Send with retries and write the log row. Returns (status, error)."""
        now = self.clock()
        win = self._sent.setdefault(ch["id"], deque())
        while win and now - win[0] > 60:
            win.popleft()
        if len(win) >= RATE_LIMIT:
            return self._log(ch, m, "suppressed", f"more than {RATE_LIMIT} messages in a minute")
        win.append(now)
        cfg, err = json.loads(ch["config"]), None
        for attempt in range(ATTEMPTS):
            try:
                self.senders[ch["kind"]](cfg, m)
                return self._log(ch, m, "sent", None)
            except Exception as e:  # any failure of the destination: try again, then record it
                err = f"{type(e).__name__}: {e}"[:300]
                if attempt < ATTEMPTS - 1:
                    self.sleep(RETRY_DELAYS[attempt])
        return self._log(ch, m, "failed", err)

    def _log(self, ch, m, status, error):
        self.db.execute("INSERT INTO notify_log(ts, channel_id, channel, event, device, title, status, error) VALUES(?,?,?,?,?,?,?,?)",
                        (int(self.clock()), ch["id"], ch["name"], m.get("event"), m.get("device"), m["title"], status, error))
        self.db.execute("DELETE FROM notify_log WHERE id < (SELECT MAX(id) FROM notify_log) - 2000")  # keep the last 2000
        return status, error

    def test(self, channel_id: int):
        rows = self.db.query("SELECT * FROM notify_channels WHERE id=?", (channel_id,))
        if not rows:
            raise NotifyError("no such channel")
        m = {"title": "Test from the Environmental Monitor", "priority": 3, "tags": ["bell"], "event": "test", "device": None,
             "message": "If you read this, the channel works.", "ts": int(self.clock())}
        cfg, err = json.loads(rows[0]["config"]), None
        try:  # one try, answered at once: the person pressing the button wants to see the error
            self.senders[rows[0]["kind"]](cfg, m)
            status = "sent"
        except Exception as e:
            status, err = "failed", f"{type(e).__name__}: {e}"[:300]
        self._log(rows[0], m, status, err)
        return {"status": status, "error": err}

    def drain(self):
        """Deliver what is queued, here and now (the tests; the worker does it in the background)."""
        while True:
            try:
                item = self.q.get_nowait()
            except queue.Empty:
                return
            if item:
                self.deliver(*item)

    def _worker(self):
        while not self._stop.is_set():
            item = self.q.get()
            if item is None:
                return
            try:
                self.deliver(*item)
            except Exception:
                log.exception("notification failed")

    def _watcher(self):
        while not self._stop.wait(15):
            try:
                self.tick()
            except Exception:
                log.exception("offline check failed")


def recent_log(db, limit=100):
    return db.query("SELECT * FROM notify_log ORDER BY id DESC LIMIT ?", (limit,))
