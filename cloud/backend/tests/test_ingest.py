import json

from iotcloud import ingest

DEV = "002d004f3442501020323848"


def pub(db, suffix, msg, now=1_800_000_000, root="iotmonitor"):
    return ingest.handle(db, root, f"dt/{root}/{DEV}/{suffix}", json.dumps(msg).encode(), now)


COMMON = {"schema_version": "1.1", "device_id": DEV, "timestamp": 1_799_999_990, "sync_state": "synchronised"}


def test_topic_parsing():
    assert ingest.parse_topic("iotmonitor", f"dt/iotmonitor/{DEV}/ota/result") == (DEV, "ota/result")
    assert ingest.parse_topic("iotmonitor", f"cmd/iotmonitor/{DEV}/config") is None  # commands are ours, not the device's
    assert ingest.parse_topic("iotmonitor", f"dt/other/{DEV}/telemetry") is None
    assert ingest.parse_topic("iotmonitor", f"dt/iotmonitor/{DEV}/nonsense") is None


def test_telemetry_stored_with_field_device(db):
    ev = pub(db, "telemetry", {**COMMON, "temperature_deci_c": 231, "humidity_pct": 45, "pressure_hpa": 1013,
                               "accel_x_mg": None, "field_device": {"online": True, "temperature_deci_c": 225,
                                                                    "humidity_pct": None, "pressure_hpa": 1012}})
    assert ev == {"device": DEV, "kind": "telemetry", "ts": 1_799_999_990}
    row = db.query("SELECT * FROM telemetry")[0]
    assert (row["temperature_deci_c"], row["humidity_pct"], row["fd_temperature_deci_c"], row["fd_humidity_pct"],
            row["fd_online"]) == (231, 45, 225, None, 1)  # a null sensor stays null, not zero
    assert db.query("SELECT id, last_seen FROM devices") == [{"id": DEV, "last_seen": 1_800_000_000}]


def test_unsynchronised_device_time_is_replaced_by_reception_time(db):
    pub(db, "telemetry", {**COMMON, "sync_state": "unsynchronised", "timestamp": 12345, "temperature_deci_c": 200})
    assert db.query("SELECT ts, recv_ts FROM telemetry") == [{"ts": 1_800_000_000, "recv_ts": 1_800_000_000}]


def test_health_updates_the_registry(db):
    pub(db, "health", {**COMMON, "fw_version": "1.2.3", "uptime_s": 99, "cloud_connected": True})
    d = db.query("SELECT fw_version, last_health FROM devices")[0]
    assert d["fw_version"] == "1.2.3" and json.loads(d["last_health"])["uptime_s"] == 99


def test_alarm_stored(db):
    ev = pub(db, "alarms", {**COMMON, "sensor_id": "temperature", "alarm_type": "HIGH", "measured_value_raw": 6100,
                            "threshold_value_raw": 6000, "source": "field_device"})
    assert ev["kind"] == "alarm" and ev["alarm_type"] == "HIGH"
    assert db.query("SELECT alarm_type, threshold_value_raw, source FROM alarms") == [
        {"alarm_type": "HIGH", "threshold_value_raw": 6000, "source": "field_device"}]


def test_results_go_to_events_with_request_id(db):
    pub(db, "config/result", {**COMMON, "event": "config", "req": 7, "result": "applied"})
    pub(db, "control/result", {**COMMON, "event": "control", "command": "read", "req": 8, "result": "accepted"})
    pub(db, "ota/result", {**COMMON, "event": "ota", "job": 42, "result": "success", "version": "1.2.4"})
    rows = db.query("SELECT kind, req, result FROM events ORDER BY id")
    assert rows == [{"kind": "config", "req": 7, "result": "applied"}, {"kind": "control", "req": 8, "result": "accepted"},
                    {"kind": "ota", "req": 42, "result": "success"}]


def test_ota_chunk_requests_are_not_stored(db):
    ev = pub(db, "ota/request", {**COMMON, "job": 42, "slot": "B", "offset": 0, "len": 1024})
    assert ev["kind"] == "ota_request" and db.query("SELECT COUNT(*) AS n FROM events")[0]["n"] == 0


def test_garbage_is_logged_not_fatal(db):
    ev = ingest.handle(db, "iotmonitor", f"dt/iotmonitor/{DEV}/telemetry", b"{not json", 1_800_000_000)
    assert ev["kind"] == "bad_payload"
    assert db.query("SELECT kind FROM events") == [{"kind": "bad_payload"}]


def test_a_replayed_older_health_message_does_not_set_the_registry_back(db):
    pub(db, "health", {**COMMON, "timestamp": 2000, "fw_version": "1.0.1", "uptime_s": 50})
    pub(db, "health", {**COMMON, "timestamp": 1000, "fw_version": "1.0.0", "uptime_s": 900})  # replayed, older
    d = db.query("SELECT fw_version, last_health FROM devices")[0]
    assert d["fw_version"] == "1.0.1" and json.loads(d["last_health"])["uptime_s"] == 50
    assert db.query("SELECT COUNT(*) AS n FROM health")[0]["n"] == 2  # the old one is still in the history
    pub(db, "health", {**COMMON, "timestamp": 3000, "fw_version": "1.0.2", "uptime_s": 80})  # newer still applies
    assert db.query("SELECT fw_version FROM devices")[0]["fw_version"] == "1.0.2"
