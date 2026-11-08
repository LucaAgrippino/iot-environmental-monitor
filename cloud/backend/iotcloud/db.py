"""SQLite storage. One connection, guarded by a lock: the MQTT thread writes, the API threads read.

Tables (the AWS counterpart in brackets):
  devices    one row per device seen                      [IoT Core thing registry]
  telemetry  sensor readings                              [Timestream / DynamoDB]
  health     device health reports                        [Timestream / DynamoDB]
  alarms     alarm events                                 [Timestream + SNS source]
  events     everything else: command results, OTA        [CloudWatch Logs / Jobs history]
  commands   config / control commands and their outcome  [IoT Device Shadow / Jobs]
  images     firmware images the operator uploaded        [S3 bucket]
  ota_jobs   one OTA update of one device                 [IoT Jobs]
  users, sessions   who may log in, and who is logged in  [Cognito]
"""
import json
import sqlite3
import threading

SCHEMA = """
CREATE TABLE IF NOT EXISTS devices (
  id TEXT PRIMARY KEY, name TEXT, first_seen INTEGER NOT NULL, last_seen INTEGER NOT NULL,
  fw_version TEXT, last_health TEXT);
CREATE TABLE IF NOT EXISTS telemetry (
  id INTEGER PRIMARY KEY, device TEXT NOT NULL, ts INTEGER NOT NULL, recv_ts INTEGER NOT NULL, sync_state TEXT,
  temperature_deci_c INTEGER, humidity_pct INTEGER, pressure_hpa INTEGER,
  fd_online INTEGER, fd_temperature_deci_c INTEGER, fd_humidity_pct INTEGER, fd_pressure_hpa INTEGER, raw TEXT);
CREATE INDEX IF NOT EXISTS telemetry_dev_ts ON telemetry(device, ts);
CREATE TABLE IF NOT EXISTS health (
  id INTEGER PRIMARY KEY, device TEXT NOT NULL, ts INTEGER NOT NULL, recv_ts INTEGER NOT NULL, raw TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS health_dev_ts ON health(device, ts);
CREATE TABLE IF NOT EXISTS alarms (
  id INTEGER PRIMARY KEY, device TEXT NOT NULL, ts INTEGER NOT NULL, recv_ts INTEGER NOT NULL, sync_state TEXT,
  sensor_id TEXT, alarm_type TEXT, measured_value_raw INTEGER, threshold_value_raw INTEGER, source TEXT);
CREATE INDEX IF NOT EXISTS alarms_dev_ts ON alarms(device, ts);
CREATE TABLE IF NOT EXISTS events (
  id INTEGER PRIMARY KEY, device TEXT NOT NULL, recv_ts INTEGER NOT NULL, kind TEXT NOT NULL,
  req INTEGER, result TEXT, payload TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS events_dev_ts ON events(device, recv_ts);
CREATE TABLE IF NOT EXISTS commands (
  id INTEGER PRIMARY KEY, device TEXT NOT NULL, kind TEXT NOT NULL, body TEXT NOT NULL,
  created INTEGER NOT NULL, status TEXT NOT NULL, reason TEXT, finished INTEGER);
CREATE INDEX IF NOT EXISTS commands_dev ON commands(device, created);
CREATE TABLE IF NOT EXISTS images (
  name TEXT PRIMARY KEY, size INTEGER NOT NULL, sha256 TEXT NOT NULL, uploaded INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS ota_jobs (
  id INTEGER PRIMARY KEY, device TEXT NOT NULL, image_a TEXT NOT NULL, image_b TEXT NOT NULL,
  size_a INTEGER NOT NULL, size_b INTEGER NOT NULL, created INTEGER NOT NULL, updated INTEGER NOT NULL,
  state TEXT NOT NULL, sent_bytes INTEGER NOT NULL DEFAULT 0, detail TEXT);
CREATE INDEX IF NOT EXISTS ota_jobs_dev ON ota_jobs(device, created);
CREATE TABLE IF NOT EXISTS users (
  name TEXT PRIMARY KEY, pw_hash TEXT NOT NULL, role TEXT NOT NULL, created INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS sessions (
  token_hash TEXT PRIMARY KEY, user TEXT NOT NULL, created INTEGER NOT NULL, expires INTEGER NOT NULL);
"""
# Tables of the cloud's own state (the AWS counterparts: IoT Jobs, S3 for the images)


TABLES = {"telemetry", "health", "alarms", "events"}


class Database:
    def __init__(self, path: str):
        self._lock = threading.RLock()
        self._c = sqlite3.connect(path, check_same_thread=False)
        self._c.row_factory = sqlite3.Row
        with self._lock:
            self._c.execute("PRAGMA journal_mode=WAL")
            self._c.executescript(SCHEMA)

    def execute(self, sql, args=()):
        with self._lock:
            cur = self._c.execute(sql, args)
            self._c.commit()
            return cur.lastrowid

    def query(self, sql, args=()):
        with self._lock:
            return [dict(r) for r in self._c.execute(sql, args).fetchall()]

    def touch_device(self, device: str, now: int, fw_version: str | None = None, health: dict | None = None):
        with self._lock:
            self._c.execute(
                "INSERT INTO devices(id, first_seen, last_seen) VALUES(?,?,?) "
                "ON CONFLICT(id) DO UPDATE SET last_seen=excluded.last_seen",
                (device, now, now))
            if fw_version:
                self._c.execute("UPDATE devices SET fw_version=? WHERE id=?", (fw_version, device))
            if health is not None:
                self._c.execute("UPDATE devices SET last_health=? WHERE id=?", (json.dumps(health), device))
            self._c.commit()
