"""The HTTP API the web app uses (the AWS counterpart: API Gateway + Lambda)."""
import csv
import io
import json
import time
from contextlib import asynccontextmanager

import asyncio
from fastapi import FastAPI, HTTPException, Query, Request
from fastapi.responses import Response, StreamingResponse

from . import commands, config, mqtt, ota
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


def create_app(settings: config.Settings | None = None, db: Database | None = None, start_mqtt: bool = True,
               publisher=None) -> FastAPI:
    settings = settings or config.load()
    db = db or Database(settings.db_path)
    hub = Hub()
    link = {"publish": publisher}  # publish(topic, payload, qos); set by the MQTT client once it exists

    @asynccontextmanager
    async def lifespan(app):
        hub.bind(asyncio.get_running_loop())
        client = mqtt.start(settings, db, hub) if start_mqtt else None
        if client:
            link["publish"] = lambda topic, payload, qos: client.publish(topic, payload, qos)
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

    def require_device(device: str):
        if not db.query("SELECT 1 FROM devices WHERE id=?", (device,)):
            raise HTTPException(404, "unknown device")

    def publish():
        if link["publish"] is None:
            raise HTTPException(503, "not connected to the broker")
        return link["publish"]

    # -- commands -------------------------------------------------------------------------------------------------
    async def json_body(request: Request):
        try:
            return await request.json()
        except ValueError:
            raise HTTPException(400, "body must be JSON")

    @app.post("/api/devices/{device}/config", status_code=202)
    async def send_config(device: str, request: Request):
        """Change remote-writable settings, e.g. {"telemetry_interval_ms": 30000}. The result arrives as an event."""
        require_device(device)
        try:
            return commands.send(db, publish(), settings.topic_root, device, "config", await json_body(request))
        except commands.BadCommand as e:
            raise HTTPException(400, str(e))

    @app.post("/api/devices/{device}/control", status_code=202)
    async def send_control(device: str, request: Request):
        """{"command": "read" | "restart" | "restart_confirm" | "reset_metrics"}"""
        require_device(device)
        try:
            return commands.send(db, publish(), settings.topic_root, device, "control", await json_body(request))
        except commands.BadCommand as e:
            raise HTTPException(400, str(e))

    @app.get("/api/devices/{device}/commands")
    def list_commands(device: str, limit: int = Query(50, le=500)):
        return commands.list_for(db, device, limit, timeout_s=settings.command_timeout_s)

    @app.get("/api/commands/{cid}")
    def get_command(cid: int):
        row = commands.get(db, cid, timeout_s=settings.command_timeout_s)
        if not row:
            raise HTTPException(404, "unknown command")
        return row

    # -- firmware images and OTA ----------------------------------------------------------------------------------
    @app.get("/api/images")
    def images():
        return ota.list_images(db)

    @app.put("/api/images/{name}", status_code=201)
    async def upload_image(name: str, request: Request):
        """Raw body = the .ota file (tools/ota/make_image.py output). One file per flash slot."""
        try:
            return ota.save_image(db, settings.image_dir, name, await request.body())
        except ota.OtaError as e:
            raise HTTPException(400, str(e))

    @app.delete("/api/images/{name}", status_code=204)
    def delete_image(name: str):
        try:
            if not ota.delete_image(db, settings.image_dir, name):
                raise HTTPException(404, "unknown image")
        except ota.OtaError as e:
            raise HTTPException(409, str(e))

    @app.post("/api/devices/{device}/ota", status_code=202)
    async def start_ota(device: str, request: Request):
        """{"image_a": "<name>", "image_b": "<name>"}: starts an update; the device pulls the chunks."""
        require_device(device)
        body = await json_body(request)
        try:
            return ota.start_job(db, publish(), settings.topic_root, device, str(body.get("image_a")), str(body.get("image_b")))
        except ota.OtaError as e:
            raise HTTPException(409 if "progress" in str(e) else 400, str(e))

    @app.get("/api/devices/{device}/ota")
    def list_ota(device: str, limit: int = Query(20, le=200)):
        return ota.list_jobs(db, device, limit)

    @app.get("/api/ota/{job}")
    def get_ota(job: int):
        row = ota.get_job(db, job)
        if not row:
            raise HTTPException(404, "unknown job")
        return row

    @app.post("/api/ota/{job}/cancel")
    def cancel_ota(job: int):
        if not ota.cancel_job(db, job):
            raise HTTPException(409, "the job is not running")
        return ota.get_job(db, job)

    @app.get("/api/stream")
    async def stream():
        return StreamingResponse(hub.stream(), media_type="text/event-stream",
                                 headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})

    return app


def app():  # uvicorn --factory iotcloud.api:app
    return create_app()
