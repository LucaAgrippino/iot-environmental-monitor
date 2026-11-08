import json

from fastapi.testclient import TestClient

from iotcloud import ingest
from iotcloud.api import create_app
from iotcloud.db import Database

DEV = "002d004f3442501020323848"
COMMON = {"schema_version": "1.1", "device_id": DEV, "sync_state": "synchronised"}


def put(db, suffix, ts, **kw):
    ingest.handle(db, "iotmonitor", f"dt/iotmonitor/{DEV}/{suffix}", json.dumps({**COMMON, "timestamp": ts, **kw}).encode(), ts)


def client(settings, db, now):
    return TestClient(create_app(settings, db, start_mqtt=False))


def test_device_list_with_online_status_and_active_alarm(settings, db, monkeypatch):
    import time
    now = 1_800_000_000
    monkeypatch.setattr(time, "time", lambda: now + 60)
    put(db, "telemetry", now, temperature_deci_c=231, humidity_pct=45, pressure_hpa=1013)
    put(db, "alarms", now, sensor_id="temperature", alarm_type="HIGH", measured_value_raw=6100, threshold_value_raw=6000, source="gateway")
    d = client(settings, db, now).get("/api/devices").json()[0]
    assert d["id"] == DEV and d["online"] is True and d["latest"]["temperature_deci_c"] == 231
    assert d["active_alarms"] == [{"sensor_id": "temperature", "alarm_type": "HIGH", "ts": now}]
    monkeypatch.setattr(time, "time", lambda: now + 1000)
    assert client(settings, db, now).get("/api/devices").json()[0]["online"] is False  # nothing heard for too long


def test_clear_removes_the_active_alarm(settings, db):
    put(db, "alarms", 1_800_000_000, sensor_id="temperature", alarm_type="HIGH", measured_value_raw=1, threshold_value_raw=0, source="gateway")
    put(db, "alarms", 1_800_000_100, sensor_id="temperature", alarm_type="CLEAR", measured_value_raw=1, threshold_value_raw=0, source="gateway")
    assert client(settings, db, 0).get(f"/api/devices/{DEV}").json()["active_alarms"] == []


def test_history_is_oldest_first_and_filterable(settings, db):
    for i in range(5):
        put(db, "telemetry", 1_800_000_000 + i * 60, temperature_deci_c=200 + i)
    c = client(settings, db, 0)
    rows = c.get(f"/api/devices/{DEV}/telemetry").json()
    assert [r["temperature_deci_c"] for r in rows] == [200, 201, 202, 203, 204]
    rows = c.get(f"/api/devices/{DEV}/telemetry", params={"since": 1_800_000_120, "limit": 2}).json()
    assert [r["temperature_deci_c"] for r in rows] == [203, 204]  # the newest two within the window


def test_export_csv_and_json(settings, db):
    put(db, "telemetry", 1_800_000_000, temperature_deci_c=231)
    c = client(settings, db, 0)
    r = c.get(f"/api/devices/{DEV}/export", params={"kind": "telemetry"})
    assert r.headers["content-type"].startswith("text/csv") and "attachment" in r.headers["content-disposition"]
    lines = r.text.strip().splitlines()
    assert lines[0].startswith("ts,recv_ts,sync_state,temperature_deci_c") and ",231," in lines[1]
    assert c.get(f"/api/devices/{DEV}/export", params={"kind": "telemetry", "format": "json"}).json()[0]["temperature_deci_c"] == 231
    assert c.get(f"/api/devices/{DEV}/export", params={"kind": "users"}).status_code == 400


def test_unknown_device_is_404(settings, db):
    assert client(settings, db, 0).get("/api/devices/nope").status_code == 404


def test_web_app_is_served_without_hiding_the_api(settings, db, tmp_path):
    from dataclasses import replace
    (tmp_path / "index.html").write_text("<title>app</title>")
    c = TestClient(create_app(replace(settings, web_dir=str(tmp_path)), db, start_mqtt=False))
    assert "<title>app</title>" in c.get("/").text
    assert c.get("/api/healthz").json() == {"ok": True}


def test_missing_tls_files_do_not_take_the_service_down(settings, db, caplog):
    """A restart loop hides the cause: the service stays up, says which file is missing, and commands answer 503."""
    import logging
    from dataclasses import replace
    s = replace(settings, mqtt_cafile="/nonexistent/ca.crt", mqtt_cert="/nonexistent/c.crt", mqtt_key="/nonexistent/c.key")
    ingest.handle(db, "iotmonitor", f"dt/iotmonitor/{DEV}/health", b"{}", 1)
    with caplog.at_level(logging.ERROR, logger="iotcloud.mqtt"):
        with TestClient(create_app(s, db, start_mqtt=True)) as c:
            assert c.get("/api/healthz").json() == {"ok": True}
            assert c.get("/api/devices").status_code == 200
            assert c.post(f"/api/devices/{DEV}/control", json={"command": "read"}).status_code == 503
    assert "CA certificate not found: /nonexistent/ca.crt" in caplog.text
