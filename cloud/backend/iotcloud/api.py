"""The HTTP API the web app uses (the AWS counterpart: API Gateway + Lambda)."""
import csv
import io
import json
import time
from contextlib import asynccontextmanager

import asyncio
from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import Response, StreamingResponse

from . import config, mqtt
from .db import TABLES, Database
from .live import Hub

EXPORT_COLUMNS = {
    "telemetry": ["ts", "recv_ts", "sync_state", "temperature_deci_c", "humidity_pct", "pressure_hpa", "fd_online",
                  "fd_temperature_deci_c", "fd_humidity_pct", "fd_pressure_hpa"],
    "health": ["ts", "recv_ts", "raw"],
    "alarms": ["ts", "recv_ts", "sensor_id", "alarm_type", "measured_value_raw", "threshold_value_raw", "source"],
    "events": ["recv_ts", "kind", "req", "result", "payload"],
}
TIME_COLUMN = {"telemetry": "ts", "health": "ts", "alarms": "ts", "events": "recv_ts"}


def create_app(settings: config.Settings | None = None, db: Database | None = None, start_mqtt: bool = True) -> FastAPI:
    settings = settings or config.load()
    db = db or Database(settings.db_path)
    hub = Hub()

    @asynccontextmanager
    async def lifespan(app):
        hub.bind(asyncio.get_running_loop())
        client = mqtt.start(settings, db, hub) if start_mqtt else None
        yield
        if client:
            client.loop_stop()
            client.disconnect()

    app = FastAPI(title="iotcloud", lifespan=lifespan)
    app.state.db, app.state.hub, app.state.settings = db, hub, settings

    def device_row(r: dict, now: int) -> dict:
        last = db.query("SELECT * FROM telemetry WHERE device=? ORDER BY ts DESC LIMIT 1", (r["id"],))
        health = json.loads(r["last_health"]) if r.get("last_health") else None
        return {
            "id": r["id"], "name": r.get("name"), "first_seen": r["first_seen"], "last_seen": r["last_seen"],
            "online": now - r["last_seen"] <= settings.online_window_s, "fw_version": r.get("fw_version"),
            "latest": {k: v for k, v in last[0].items() if k != "raw"} if last else None,
            "health": health,
            "active_alarms": db.query(
                "SELECT sensor_id, alarm_type, ts FROM alarms a WHERE device=? AND alarm_type!='CLEAR' AND NOT EXISTS ("
                " SELECT 1 FROM alarms b WHERE b.device=a.device AND b.sensor_id=a.sensor_id AND b.id>a.id"
                " AND b.alarm_type='CLEAR')", (r["id"],)),
        }

    @app.get("/api/healthz")
    def healthz():
        return {"ok": True}

    @app.get("/api/devices")
    def devices():
        now = int(time.time())
        return [device_row(r, now) for r in db.query("SELECT * FROM devices ORDER BY id")]

    @app.get("/api/devices/{device}")
    def device(device: str):
        rows = db.query("SELECT * FROM devices WHERE id=?", (device,))
        if not rows:
            raise HTTPException(404, "unknown device")
        return device_row(rows[0], int(time.time()))

    def history(table: str, device: str, since: int, until: int | None, limit: int):
        col = TIME_COLUMN[table]
        sql = f"SELECT * FROM {table} WHERE device=? AND {col}>=?" + (f" AND {col}<=?" if until else "")
        args = (device, since) + ((until,) if until else ())
        # newest first from the table, returned oldest first so a chart can draw it as it is
        rows = db.query(sql + f" ORDER BY {col} DESC LIMIT ?", args + (limit,))
        return list(reversed(rows))

    @app.get("/api/devices/{device}/telemetry")
    def telemetry(device: str, since: int = 0, until: int | None = None, limit: int = Query(1000, le=20000)):
        return [{k: v for k, v in r.items() if k != "raw"} for r in history("telemetry", device, since, until, limit)]

    @app.get("/api/devices/{device}/health")
    def health(device: str, since: int = 0, until: int | None = None, limit: int = Query(200, le=5000)):
        return [{**json.loads(r["raw"]), "ts": r["ts"]} for r in history("health", device, since, until, limit)]

    @app.get("/api/devices/{device}/alarms")
    def alarms(device: str, since: int = 0, until: int | None = None, limit: int = Query(200, le=5000)):
        return history("alarms", device, since, until, limit)

    @app.get("/api/devices/{device}/events")
    def events(device: str, since: int = 0, until: int | None = None, limit: int = Query(200, le=5000)):
        return [{**r, "payload": json.loads(r["payload"])} for r in history("events", device, since, until, limit)]

    @app.get("/api/devices/{device}/export")
    def export(device: str, kind: str, format: str = "csv", since: int = 0, until: int | None = None):
        """Download what the cloud holds about a device (the 'logs' of this system, see cloud/README.md)."""
        if kind not in TABLES:
            raise HTTPException(400, f"kind must be one of {sorted(TABLES)}")
        if format not in ("csv", "json"):
            raise HTTPException(400, "format must be csv or json")
        rows = history(kind, device, since, until, 1_000_000)
        name = f"{device}-{kind}-{int(time.time())}.{format}"
        headers = {"Content-Disposition": f'attachment; filename="{name}"'}
        if format == "json":
            return Response(json.dumps(rows), media_type="application/json", headers=headers)
        buf = io.StringIO()
        w = csv.DictWriter(buf, fieldnames=EXPORT_COLUMNS[kind], extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
        return Response(buf.getvalue(), media_type="text/csv", headers=headers)

    @app.get("/api/stream")
    async def stream():
        return StreamingResponse(hub.stream(), media_type="text/event-stream",
                                 headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})

    return app


def app():  # uvicorn --factory iotcloud.api:app
    return create_app()
