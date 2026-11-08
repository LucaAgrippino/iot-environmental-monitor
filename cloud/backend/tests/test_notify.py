import hashlib
import hmac
import json
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest
from fastapi.testclient import TestClient

from iotcloud import ingest, notify
from iotcloud.api import create_app

DEV = "002d004f3442501020323848"


class Clock:
    def __init__(self, t=1_800_000_000):
        self.t = t

    def __call__(self):
        return self.t


@pytest.fixture()
def rig(db):
    sent, clock = [], Clock()
    senders = {k: (lambda cfg, m, k=k: sent.append((k, cfg, m))) for k in notify.SENDERS}
    n = notify.Notifier(db, online_window_s=150, senders=senders, clock=clock, sleep=lambda s: None)
    return n, sent, clock, db


def channel(db, kind="ntfy", events=None, name="phone", enabled=True, **cfg):
    base = {"ntfy": {"url": "http://ntfy.local", "topic": "envmon-secret"}, "webhook": {"url": "http://hook.local/x"}}[kind]
    return notify.save_channel(db, {"name": name, "kind": kind, "config": {**base, **cfg}, "events": events, "enabled": enabled})


ALARM = {"device": DEV, "kind": "alarm", "alarm_type": "HIGH", "sensor_id": "temperature", "source": "field_device",
         "measured_value_raw": 3120, "threshold_value_raw": 3000}


def test_alarm_message_uses_the_firmware_units(rig):
    n, sent, _, db = rig
    channel(db)
    n.submit(ALARM)
    n.drain()
    _, _, m = sent[0]
    assert m["title"] == f"Temperature HIGH on {DEV}" and m["priority"] == 4
    assert "31.20 °C (limit 30.00 °C)" in m["message"] and "field device" in m["message"]
    assert notify.fmt_value("humidity", 5560) == "55.6 %" and notify.fmt_value("pressure", 10132) == "1013.2 hPa"


def test_a_channel_gets_only_the_events_it_asked_for(rig):
    n, sent, _, db = rig
    channel(db, name="phone", events=["alarm"])
    channel(db, "webhook", name="mailbox", events=["ota_result"])
    channel(db, name="off", enabled=False)
    n.submit(ALARM)
    n.submit({"device": DEV, "kind": "ota", "result": "failed", "req": 7, "reason": "sig_invalid"})
    n.submit({"device": DEV, "kind": "ota", "result": "accepted", "req": 7})         # not a final result
    n.submit({"device": DEV, "kind": "telemetry", "ts": 1})
    n.drain()
    assert [(k, m["event"]) for k, _, m in sent] == [("ntfy", "alarm"), ("webhook", "ota_result")]
    assert "sig_invalid" in sent[1][2]["message"]


def test_clear_is_a_separate_event_and_off_by_default(rig):
    n, sent, _, db = rig
    channel(db)
    n.submit({**ALARM, "alarm_type": "CLEAR"})
    n.drain()
    assert sent == []
    channel(db, name="all", events=list(notify.EVENTS))
    n.submit({**ALARM, "alarm_type": "CLEAR"})
    n.drain()
    assert sent[0][2]["title"].startswith("Temperature back to normal")


def test_failures_are_retried_then_logged(rig):
    n, sent, _, db = rig
    calls, delays = [], []
    def flaky(cfg, m):
        calls.append(1)
        if len(calls) < 3:
            raise OSError("connection refused")
        sent.append(m)
    n.senders["ntfy"], n.sleep = flaky, delays.append
    channel(db)
    n.submit(ALARM); n.drain()
    assert len(calls) == 3 and delays == [1, 5] and len(sent) == 1
    n.senders["ntfy"] = lambda cfg, m: (_ for _ in ()).throw(OSError("down"))
    n.submit(ALARM); n.drain()
    log = notify.recent_log(db)
    assert [r["status"] for r in log] == ["failed", "sent"] and "OSError: down" in log[0]["error"]


def test_a_storm_is_cut_off_per_channel(rig):
    n, sent, _, db = rig
    channel(db)
    for _ in range(notify.RATE_LIMIT + 5):
        n.submit(ALARM)
    n.drain()
    assert len(sent) == notify.RATE_LIMIT
    assert [r["status"] for r in notify.recent_log(db)].count("suppressed") == 5


def test_offline_is_told_once_and_the_return_once(rig):
    n, sent, clock, db = rig
    channel(db, events=["device_offline", "device_online"])
    ingest.handle(db, "iotmonitor", f"dt/iotmonitor/{DEV}/health", b"{}", clock.t)
    clock.t += 100
    n.tick(); n.drain()
    assert sent == []                                  # still inside the grace period after the start
    clock.t += 200
    n.tick(); clock.t += 15; n.tick(); n.drain()       # silent for 300 s, checked twice
    assert [m["event"] for _, _, m in sent] == ["device_offline"]
    clock.t += 600
    n.submit({"device": DEV, "kind": "telemetry"}); n.submit({"device": DEV, "kind": "telemetry"}); n.drain()
    assert [m["event"] for _, _, m in sent] == ["device_offline", "device_online"]
    assert "after 15 min" in sent[1][2]["message"]


def test_offline_state_survives_a_restart(rig):
    n, sent, clock, db = rig
    channel(db, events=["device_offline"])
    ingest.handle(db, "iotmonitor", f"dt/iotmonitor/{DEV}/health", b"{}", clock.t)
    clock.t += 1000
    n.tick(); n.drain()
    assert len(sent) == 1
    again = notify.Notifier(db, 150, senders=n.senders, clock=clock, sleep=lambda s: None)
    clock.t += 1000
    again.tick(); again.drain()
    assert len(sent) == 1                              # already told: not told again after a restart


# -- channels --------------------------------------------------------------------------------------------------------------
def test_secrets_are_masked_and_kept_on_update(rig):
    _, _, _, db = rig
    ch = channel(db, token="tk_secret")
    assert ch["config"]["token"] == notify.MASK and "tk_secret" not in json.dumps(notify.list_channels(db))
    notify.save_channel(db, {"config": {"url": "http://ntfy.local", "topic": "envmon-secret", "token": notify.MASK}}, ch["id"])
    assert json.loads(db.query("SELECT config FROM notify_channels")[0]["config"])["token"] == "tk_secret"


def test_channel_validation(rig):
    _, _, _, db = rig
    bad = [{"name": "x", "kind": "ntfy", "config": {"url": "ftp://x", "topic": "t"}},
           {"name": "x", "kind": "ntfy", "config": {"url": "http://x", "topic": "bad topic!"}},
           {"name": "x", "kind": "webhook", "config": {"url": "javascript:alert(1)"}},
           {"name": "x", "kind": "email", "config": {"host": "smtp", "from": "a@b", "to": "nobody"}},
           {"name": "x", "kind": "sms", "config": {}},
           {"name": "", "kind": "webhook", "config": {"url": "http://x"}},
           {"name": "x", "kind": "webhook", "config": {"url": "http://x"}, "events": ["nonsense"]}]
    for b in bad:
        with pytest.raises(notify.NotifyError):
            notify.save_channel(db, b)
    channel(db, "webhook", name="dup")
    with pytest.raises(notify.NotifyError):
        channel(db, "webhook", name="dup")


# -- the real senders against local servers ---------------------------------------------------------------------------------
class Capture(BaseHTTPRequestHandler):
    got = []

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        Capture.got.append((self.path, dict(self.headers), body))
        self.send_response(200); self.end_headers(); self.wfile.write(b"ok")

    def log_message(self, *a):
        pass


@pytest.fixture()
def http():
    Capture.got = []
    srv = HTTPServer(("127.0.0.1", 0), Capture)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{srv.server_port}"
    srv.shutdown()


M = {"title": "Temperature HIGH on d", "message": "31.20 °C (limit 30.00 °C)", "priority": 4, "tags": ["warning"], "event": "alarm",
     "device": "d", "ts": 1, "data": {"x": 1}}


def test_ntfy_posts_json_with_utf8_and_a_token(http):
    notify.send_ntfy({"url": http, "topic": "envmon-secret", "token": "tk_1"}, M)
    path, headers, body = Capture.got[0]
    assert json.loads(body)["topic"] == "envmon-secret" and "°C" in json.loads(body)["message"]
    assert headers["Authorization"] == "Bearer tk_1" and json.loads(body)["priority"] == 4


def test_webhook_is_signed(http):
    notify.send_webhook({"url": http + "/hook", "secret": "s3cret"}, M)
    path, headers, body = Capture.got[0]
    assert path == "/hook" and json.loads(body)["event"] == "alarm"
    assert headers["X-Signature"] == "sha256=" + hmac.new(b"s3cret", body, hashlib.sha256).hexdigest()


def test_email_uses_starttls_login_and_the_addresses(monkeypatch):
    log = []

    class FakeSMTP:
        def __init__(self, host, port, timeout=None): log.append(("connect", host, port))
        def __enter__(self): return self
        def __exit__(self, *a): pass
        def starttls(self, context=None): log.append("starttls")
        def login(self, u, p): log.append(("login", u, p))
        def send_message(self, msg): log.append(("send", msg["To"], msg["Subject"]))

    monkeypatch.setattr(notify.smtplib, "SMTP", FakeSMTP)
    notify.send_email({"host": "smtp.x", "port": 587, "security": "starttls", "from": "env@x", "to": ["me@x", "you@x"],
                       "username": "u", "password": "p"}, M)
    assert log == [("connect", "smtp.x", 587), "starttls", ("login", "u", "p"),
                   ("send", "me@x, you@x", "[Environmental Monitor] Temperature HIGH on d")]


# -- the API ------------------------------------------------------------------------------------------------------------------
def test_api_channels_test_button_and_log(rig, settings):
    n, sent, _, db = rig
    c = TestClient(create_app(settings, db, start_mqtt=False, notifier=n))
    r = c.post("/api/notify/channels", json={"name": "phone", "kind": "ntfy", "config": {"url": "http://n.local", "topic": "abc", "token": "tk"}})
    assert r.status_code == 201 and r.json()["config"]["token"] == notify.MASK and r.json()["events"] == list(notify.DEFAULT_EVENTS)
    cid = r.json()["id"]
    assert c.post("/api/notify/channels", json={"name": "phone", "kind": "ntfy", "config": {}}).status_code == 400
    assert c.post(f"/api/notify/channels/{cid}/test").json() == {"status": "sent", "error": None}
    n.senders["ntfy"] = lambda cfg, m: (_ for _ in ()).throw(OSError("no route"))
    assert c.post(f"/api/notify/channels/{cid}/test").json()["status"] == "failed"
    assert [x["status"] for x in c.get("/api/notify/log").json()] == ["failed", "sent"]
    assert c.put(f"/api/notify/channels/{cid}", json={"events": ["alarm", "alarm_clear"]}).json()["events"] == ["alarm", "alarm_clear"]
    assert len(c.get("/api/notify/events").json()) == len(notify.EVENTS)
    assert c.delete(f"/api/notify/channels/{cid}").status_code == 204
    assert c.delete(f"/api/notify/channels/{cid}").status_code == 404
    assert c.post("/api/notify/channels/99/test").status_code == 400


def test_a_viewer_cannot_touch_the_alerts(rig, settings):
    from dataclasses import replace
    from iotcloud import auth
    n, _, _, db = rig
    auth.add_user(db, "guest", "a long enough password", "viewer")
    c = TestClient(create_app(replace(settings, auth_required=True), db, start_mqtt=False, notifier=n))
    assert c.post("/api/auth/login", json={"username": "guest", "password": "a long enough password"}).status_code == 200
    assert c.get("/api/notify/channels").status_code == 200
    assert c.post("/api/notify/channels", json={"name": "x", "kind": "webhook", "config": {"url": "http://x"}}).status_code == 403
    assert c.post("/api/notify/channels/1/test").status_code == 403
