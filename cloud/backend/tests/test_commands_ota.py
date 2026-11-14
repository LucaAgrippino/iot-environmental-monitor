import json
import struct

import pytest
from fastapi.testclient import TestClient

from iotcloud import ingest, ota
from iotcloud.api import create_app
from iotcloud.config import Settings

DEV = "002d004f3442501020323848"


class Bus:
    """Stands in for the MQTT client: records what the cloud publishes."""
    def __init__(self):
        self.sent = []

    def __call__(self, topic, payload, qos):
        self.sent.append((topic, payload, qos))


@pytest.fixture()
def env(tmp_path, db):
    settings = Settings(db_path=str(tmp_path / "x.sqlite3"), image_dir=str(tmp_path / "img"), command_timeout_s=60, web_dir="", auth_required=False)
    bus = Bus()
    import time
    ingest.handle(db, "iotmonitor", f"dt/iotmonitor/{DEV}/health", b'{"fw_version":"0.9.0"}', int(time.time()))  # registers the device, heard just now
    return TestClient(create_app(settings, db, start_mqtt=False, publisher=bus)), bus, db, settings


def device_says(db, suffix, **msg):
    ingest.handle(db, "iotmonitor", f"dt/iotmonitor/{DEV}/{suffix}", json.dumps(msg).encode())


def test_config_command_is_tracked_until_the_device_answers(env):
    c, bus, db, _ = env
    r = c.post(f"/api/devices/{DEV}/config", json={"telemetry_interval_ms": 30000})
    assert r.status_code == 202 and r.json()["status"] == "pending"
    topic, payload, qos = bus.sent[0]
    assert topic == f"cmd/iotmonitor/{DEV}/config" and qos == 1
    assert json.loads(payload) == {"telemetry_interval_ms": 30000, "req": r.json()["id"]}
    device_says(db, "config/result", event="config", req=r.json()["id"], result="applied")
    assert c.get(f"/api/commands/{r.json()['id']}").json()["status"] == "applied"


def test_rejected_command_keeps_the_reason(env):
    c, bus, db, _ = env
    cid = c.post(f"/api/devices/{DEV}/config", json={"polling_interval_ms": 5}).json()["id"]
    device_says(db, "config/result", event="config", req=cid, result="rejected", reason="not_remote", key="polling_interval_ms")
    row = c.get(f"/api/devices/{DEV}/commands").json()[0]
    assert row["status"] == "rejected" and row["reason"] == "not_remote"


def test_unanswered_command_times_out_and_can_still_complete(env, monkeypatch):
    import time
    c, bus, db, _ = env
    cid = c.post(f"/api/devices/{DEV}/control", json={"command": "read"}).json()["id"]
    monkeypatch.setattr(time, "time", lambda: 10**10)
    assert c.get(f"/api/commands/{cid}").json()["status"] == "timeout"
    device_says(db, "control/result", event="control", command="read", req=cid, result="accepted")
    assert c.get(f"/api/commands/{cid}").json()["status"] == "accepted"


def test_bad_commands_are_refused_before_publishing(env):
    c, bus, _, _ = env
    assert c.post(f"/api/devices/{DEV}/control", json={"command": "format"}).status_code == 400
    assert c.post(f"/api/devices/{DEV}/config", json={}).status_code == 400
    assert c.post(f"/api/devices/{DEV}/config", json={"x": "1"}).status_code == 400
    assert c.post(f"/api/devices/{DEV}/config", json={"req": 1}).status_code == 400
    assert c.post("/api/devices/nobody/control", json={"command": "read"}).status_code == 404
    assert bus.sent == []


def test_field_device_commands_are_forwarded_and_their_result_is_tracked(env):
    """fd_ack_alarm / fd_reset_metrics go to the Gateway, which writes them to the Field Device and reports its answer."""
    c, bus, db, _ = env
    for name in ("fd_ack_alarm", "fd_reset_metrics"):
        r = c.post(f"/api/devices/{DEV}/control", json={"command": name})
        assert r.status_code == 202
        topic, payload, qos = bus.sent[-1]
        assert topic == f"cmd/iotmonitor/{DEV}/control" and qos == 1
        assert json.loads(payload) == {"command": name, "req": r.json()["id"]}
        device_says(db, "control/result", event="control", command=name, req=r.json()["id"], result="rejected", reason="field_device_unreachable")
        done = c.get(f"/api/commands/{r.json()['id']}").json()
        assert done["status"] == "rejected" and "field_device_unreachable" in json.dumps(done)


def test_profile_commands_carry_their_members_and_are_validated(env):
    c, bus, db, _ = env
    r = c.post(f"/api/devices/{DEV}/control", json={"command": "profile_add", "slave": 17, "map": 1, "vendor": 4660, "desc": "verif"})
    assert r.status_code == 202
    sent = json.loads(bus.sent[-1][1])
    assert sent == {"command": "profile_add", "slave": 17, "map": 1, "vendor": 4660, "desc": "verif", "req": r.json()["id"]}
    assert len(bus.sent[-1][1]) <= 160 - 20  # the firmware reads at most 160 characters; "req" is the cloud's
    r = c.post(f"/api/devices/{DEV}/control", json={"command": "profile_remove", "slave": 17})
    assert r.status_code == 202 and json.loads(bus.sent[-1][1]) == {"command": "profile_remove", "slave": 17, "req": r.json()["id"]}
    n = len(bus.sent)
    for bad in ({"command": "profile_add", "slave": 0, "map": 1, "vendor": 1},
                {"command": "profile_add", "slave": 248, "map": 1, "vendor": 1},
                {"command": "profile_add", "slave": 9, "map": 0, "vendor": 1},
                {"command": "profile_add", "slave": 9, "map": 1},
                {"command": "profile_add", "slave": 9, "map": 1, "vendor": 70000},
                {"command": "profile_add", "slave": 9, "map": 1, "vendor": 1, "desc": 'a"b'},
                {"command": "profile_add", "slave": 9, "map": 1, "vendor": 1, "desc": "x" * 33},
                {"command": "profile_add", "slave": True, "map": 1, "vendor": 1},
                {"command": "profile_remove"}):
        assert c.post(f"/api/devices/{DEV}/control", json=bad).status_code == 400, bad
    assert len(bus.sent) == n  # nothing bad was published


def test_no_broker_means_503(tmp_path, db):
    c = TestClient(create_app(Settings(db_path="x", image_dir=str(tmp_path), auth_required=False), db, start_mqtt=False))
    ingest.handle(db, "iotmonitor", f"dt/iotmonitor/{DEV}/health", b"{}", 1)
    assert c.post(f"/api/devices/{DEV}/control", json={"command": "read"}).status_code == 503


def test_wire_contract_matches_the_reference_server():
    assert ota.build_command(42, 491648, 491600) == '{"job":42,"size_a":491648,"size_b":491600}'
    assert ota.frame_chunk(42, 4096, b"\xAB\xCD") == bytes([42, 0, 0, 0, 0, 16, 0, 0, 0xAB, 0xCD])
    assert ota.parse_request(b'{"job":42,"slot":"B","offset":4096,"len":1024}') == (42, "B", 4096, 1024)
    for bad in (b"{}", b"nope", b'{"job":1,"slot":"C","offset":0,"len":1}', b'{"job":1,"slot":"A","offset":0,"len":2000}'):
        assert ota.parse_request(bad) is None


def upload(c, name, data):
    r = c.put(f"/api/images/{name}", content=data)
    assert r.status_code == 201, r.text
    return r.json()


def test_full_ota_flow(env):
    c, bus, db, settings = env
    img_a, img_b = bytes(range(256)) * 10, bytes(reversed(range(256))) * 9
    upload(c, "fw-a.ota", img_a)
    upload(c, "fw-b.ota", img_b)
    job = c.post(f"/api/devices/{DEV}/ota", json={"image_a": "fw-a.ota", "image_b": "fw-b.ota"}).json()
    assert job["state"] == "sent" and (job["size_a"], job["size_b"]) == (len(img_a), len(img_b))
    topic, payload, qos = bus.sent[-1]
    assert topic == f"cmd/iotmonitor/{DEV}/ota" and json.loads(payload) == {"job": job["id"], "size_a": 2560, "size_b": 2304}

    # the device pulls slot B in 1000-byte chunks
    received, off = b"", 0
    while off < len(img_b):
        req = json.dumps({"job": job["id"], "slot": "B", "offset": off, "len": 1000}).encode()
        sent = ota.on_request(db, bus, settings.image_dir, "iotmonitor", DEV, req)
        assert sent is not None
        t, frame, q = bus.sent[-1]
        assert t == f"cmd/iotmonitor/{DEV}/ota/data" and q == 0
        j, o = struct.unpack("<II", frame[:8])
        assert (j, o) == (job["id"], off)
        received += frame[8:]
        off += len(frame) - 8
    assert received == img_b
    got = c.get(f"/api/ota/{job['id']}").json()
    assert got["state"] == "running" and got["sent_bytes"] == len(img_b)

    device_says(db, "ota/result", event="ota", job=job["id"], result="accepted")
    assert c.get(f"/api/ota/{job['id']}").json()["state"] == "running"
    device_says(db, "ota/result", event="ota", job=job["id"], result="success")
    assert c.get(f"/api/ota/{job['id']}").json()["state"] == "success"
    # finished: a late request gets nothing, and a new job may start
    assert ota.on_request(db, bus, settings.image_dir, "iotmonitor", DEV, req) is None
    assert c.post(f"/api/devices/{DEV}/ota", json={"image_a": "fw-a.ota", "image_b": "fw-b.ota"}).status_code == 202


def test_ota_guards(env):
    c, bus, db, settings = env
    upload(c, "a.ota", b"x" * 100)
    assert c.post(f"/api/devices/{DEV}/ota", json={"image_a": "a.ota", "image_b": "missing"}).status_code == 400
    first = c.post(f"/api/devices/{DEV}/ota", json={"image_a": "a.ota", "image_b": "a.ota"}).json()
    assert c.post(f"/api/devices/{DEV}/ota", json={"image_a": "a.ota", "image_b": "a.ota"}).status_code == 409  # one at a time
    assert c.delete("/api/images/a.ota").status_code == 409  # in use
    # a request for another device's job, or after cancel, is not served
    req = json.dumps({"job": first["id"], "slot": "A", "offset": 0, "len": 10}).encode()
    assert ota.on_request(db, bus, settings.image_dir, "iotmonitor", "other-device", req) is None
    assert c.post(f"/api/ota/{first['id']}/cancel").json()["state"] == "cancelled"
    assert ota.on_request(db, bus, settings.image_dir, "iotmonitor", DEV, req) is None
    assert c.delete("/api/images/a.ota").status_code == 204
    assert c.put("/api/images/..%2Fevil", content=b"x").status_code in (400, 404)
    assert c.put("/api/images/empty.ota", content=b"").status_code == 400



def test_a_command_for_a_device_that_is_not_reporting_is_discarded_not_queued(env, monkeypatch):
    """UC-14 E1 / UC-19 E1 (validation finding F-10): nothing is published and no command row is left to run later."""
    import time
    c, bus, db, settings = env
    real = time.time
    monkeypatch.setattr(time, "time", lambda: real() + settings.online_window_s + 60)  # nothing heard for too long
    for kind, body in (("control", {"command": "read"}), ("config", {"telemetry_interval_ms": 30000})):
        r = c.post(f"/api/devices/{DEV}/{kind}", json=body)
        assert r.status_code == 409 and "offline" in r.json()["detail"], r.text
    assert bus.sent == []
    assert db.query("SELECT COUNT(*) AS n FROM commands")[0]["n"] == 0


def test_a_command_is_refused_while_the_broker_link_is_down_instead_of_being_queued_in_the_client(env, monkeypatch):
    """With the broker down the MQTT client queues a publish and sends it when the link returns, so a command ran
    minutes later (validation finding F-10): the API refuses it before a row is made."""
    from iotcloud import mqtt as mqtt_mod

    class Client:
        """The part of the paho client the API uses; the link is down."""
        def __init__(self, bus):
            self.publish = bus

        def is_connected(self):
            return False

        def loop_stop(self):
            pass

        def disconnect(self):
            pass

    _, bus, db, settings = env
    monkeypatch.setattr(mqtt_mod, "start", lambda *a, **k: Client(bus))
    with TestClient(create_app(settings, db, start_mqtt=True)) as c:
        r = c.post(f"/api/devices/{DEV}/control", json={"command": "read"})
    assert r.status_code == 503 and "not sent" in r.json()["detail"]
    assert bus.sent == []
    assert db.query("SELECT COUNT(*) AS n FROM commands")[0]["n"] == 0
