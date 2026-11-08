"""Over-the-air update: firmware store, jobs, and the chunk server.

The Gateway pulls its update (docs/lld/application/update-service-lld.md, section 6):
    cmd/<root>/<device>/ota          {"job":42,"size_a":N,"size_b":N}                QoS 1   we -> device
    dt/<root>/<device>/ota/request   {"job":42,"slot":"B","offset":N,"len":N}        QoS 0   device -> we
    cmd/<root>/<device>/ota/data     job u32 LE | offset u32 LE | bytes              QoS 0   we -> device
    dt/<root>/<device>/ota/result    {"event":"ota","job":42,"result":...}           QoS 1   device -> we
The device asks for one slot's image (A or B, one is linked for each flash bank), so a job holds both files.
AWS counterpart: an IoT Job whose document points to an S3 object; here the broker carries the bytes itself.
"""
import hashlib
import json
import os
import re
import struct
import time

CHUNK_MAX = 1024
TERMINAL = ("success", "failed", "rolled_back", "rejected")
ACTIVE = ("sent", "running")
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
MAX_IMAGE = 2 * 1024 * 1024  # a slot is 1 MB; this is a generous upper bound against a wrong upload


class OtaError(ValueError):
    pass


# -- wire contract (same vectors as scripts/ota_server.py --selftest) ------------------------------------------------
def build_command(job, size_a, size_b) -> str:
    return json.dumps({"job": job, "size_a": size_a, "size_b": size_b}, separators=(",", ":"))


def frame_chunk(job, offset, data: bytes) -> bytes:
    if not 0 < len(data) <= CHUNK_MAX:
        raise ValueError(f"a chunk carries 1..{CHUNK_MAX} bytes, got {len(data)}")
    return struct.pack("<II", job, offset) + data


def parse_request(payload):
    """(job, slot, offset, length) or None if malformed."""
    try:
        obj = json.loads(payload)
        job, slot, off, length = int(obj["job"]), obj["slot"], int(obj["offset"]), int(obj["len"])
    except (ValueError, KeyError, TypeError):
        return None
    if slot not in ("A", "B") or not 0 < length <= CHUNK_MAX or off < 0:
        return None
    return job, slot, off, length


# -- firmware store ----------------------------------------------------------------------------------------------------
def image_path(image_dir: str, name: str) -> str:
    if not NAME_RE.match(name):
        raise OtaError("name: letters, digits, . _ - (max 64 characters)")
    return os.path.join(image_dir, name)


def save_image(db, image_dir: str, name: str, data: bytes, now: int | None = None) -> dict:
    if not data or len(data) > MAX_IMAGE:
        raise OtaError(f"image size must be 1..{MAX_IMAGE} bytes")
    path = image_path(image_dir, name)
    os.makedirs(image_dir, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    row = (name, len(data), hashlib.sha256(data).hexdigest(), int(time.time()) if now is None else now)
    db.execute("INSERT INTO images(name, size, sha256, uploaded) VALUES(?,?,?,?) "
               "ON CONFLICT(name) DO UPDATE SET size=excluded.size, sha256=excluded.sha256, uploaded=excluded.uploaded", row)
    return dict(zip(("name", "size", "sha256", "uploaded"), row))


def list_images(db):
    return db.query("SELECT * FROM images ORDER BY uploaded DESC")


def delete_image(db, image_dir: str, name: str) -> bool:
    if db.query("SELECT 1 FROM ota_jobs WHERE state IN ('sent','running') AND (image_a=? OR image_b=?)", (name, name)):
        raise OtaError("an update that uses this image is running")
    path = image_path(image_dir, name)
    existed = bool(db.query("SELECT 1 FROM images WHERE name=?", (name,)))
    db.execute("DELETE FROM images WHERE name=?", (name,))
    if os.path.exists(path):
        os.remove(path)
    return existed


# -- jobs --------------------------------------------------------------------------------------------------------------
def start_job(db, publish, root: str, device: str, image_a: str, image_b: str, now: int | None = None) -> dict:
    now = int(time.time()) if now is None else now
    sizes = []
    for n in (image_a, image_b):
        rows = db.query("SELECT size FROM images WHERE name=?", (n,))
        if not rows:
            raise OtaError(f"no such image: {n}")
        sizes.append(rows[0]["size"])
    # A job that never gets a terminal report must not block the device for ever
    db.execute("UPDATE ota_jobs SET state='expired', updated=? WHERE device=? AND state IN ('sent','running') AND updated<?",
               (now, device, now - 600))
    if db.query("SELECT 1 FROM ota_jobs WHERE device=? AND state IN ('sent','running')", (device,)):
        raise OtaError("this device already has an update in progress (cancel it first)")
    job = db.execute("INSERT INTO ota_jobs(device, image_a, image_b, size_a, size_b, created, updated, state) "
                     "VALUES(?,?,?,?,?,?,?,?)", (device, image_a, image_b, sizes[0], sizes[1], now, now, "sent"))
    publish(f"cmd/{root}/{device}/ota", build_command(job, sizes[0], sizes[1]), 1)
    return get_job(db, job)


def get_job(db, job: int):
    rows = db.query("SELECT * FROM ota_jobs WHERE id=?", (job,))
    return rows[0] if rows else None


def list_jobs(db, device: str, limit: int = 20):
    return db.query("SELECT * FROM ota_jobs WHERE device=? ORDER BY id DESC LIMIT ?", (device, limit))


def cancel_job(db, job: int, now: int | None = None) -> bool:
    """Stop serving chunks. The device gives up by its own timeout; there is no wire message to abort it."""
    now = int(time.time()) if now is None else now
    db.execute("UPDATE ota_jobs SET state='cancelled', updated=? WHERE id=? AND state IN ('sent','running')", (now, job))
    row = get_job(db, job)
    return bool(row and row["state"] == "cancelled")


def on_request(db, publish, image_dir: str, root: str, device: str, payload: bytes, now: int | None = None):
    """Answer one chunk request. Returns the job row when a chunk was sent, else None."""
    req = parse_request(payload)
    if req is None:
        return None
    job, slot, offset, length = req
    row = get_job(db, job)
    if not row or row["device"] != device or row["state"] not in ACTIVE:
        return None
    name = row["image_a"] if slot == "A" else row["image_b"]
    try:
        with open(image_path(image_dir, name), "rb") as f:
            f.seek(offset)
            data = f.read(length)
    except OSError:
        return None
    if not data:
        return None
    publish(f"cmd/{root}/{device}/ota/data", frame_chunk(job, offset, data), 0)
    now = int(time.time()) if now is None else now
    db.execute("UPDATE ota_jobs SET state='running', updated=?, sent_bytes=MAX(sent_bytes, ?) WHERE id=?",
               (now, offset + len(data), job))
    return get_job(db, job)


def on_result(db, device: str, msg: dict, now: int):
    job = msg.get("job")
    if not isinstance(job, int) or isinstance(job, bool):
        return
    result = msg.get("result")
    if result in TERMINAL:
        db.execute("UPDATE ota_jobs SET state=?, updated=?, detail=? WHERE id=? AND device=?",
                   (result, now, json.dumps(msg), job, device))
    else:  # "accepted" and the like: the update is on its way
        db.execute("UPDATE ota_jobs SET updated=?, detail=? WHERE id=? AND device=? AND state IN ('sent','running')",
                   (now, json.dumps(msg), job, device))
