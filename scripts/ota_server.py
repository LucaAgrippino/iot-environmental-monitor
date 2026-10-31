#!/usr/bin/env python3
"""ota_server.py - the cloud side of a Gateway OTA update, over MQTT (TC-SYS-OTA-nnn).

Plays the operator and the file server of docs/lld/application/update-service-lld.md section 6:

    cmd/iotmonitor/<id>/ota         JSON command   {"job":42,"size_a":N,"size_b":N}     (QoS 1)
    dt/iotmonitor/<id>/ota/request  JSON request   {"job":42,"slot":"B","offset":N,"len":N}  (QoS 0)
    cmd/iotmonitor/<id>/ota/data    binary chunk   job u32 LE | offset u32 LE | bytes    (QoS 0)
    dt/iotmonitor/<id>/ota/result   JSON report    {"event":"ota","result":...}          (QoS 1)

The device pulls the image: it requests each chunk and this server answers from the signed image
file for the slot it asked for (tools/ota/make_image.py output, one per slot). The run ends at the
first terminal report (success, failed, rolled_back, rejected); "accepted" and the activate reset
in between are normal, and the device's reconnect after the reboot is waited for.

    # TC-SYS-OTA-001: a valid update over the bring-up broker
    ota_server.py --host 192.168.0.218 --device-id <serial> --image-a a.ota --image-b b.ota --job 42 \\
        --cafile local-test/mosquitto-broker/ca.crt --cert firmware/gateway/certs/client.crt \\
        --key firmware/gateway/certs/client.key --expect success

    # TC-SYS-OTA-003: the broker's answers stop for two minutes mid-download
    ota_server.py ... --silent-at 65536 --silent-seconds 120

    # TC-SYS-OTA-005: an image signed with another key is just another --image-b file
    ota_server.py ... --expect failed

Authentication of the operator is the broker's job: see tools/ota/README.md.
"""

import argparse
import json
import re
import ssl
import struct
import sys
import threading
import time

import paho.mqtt.client as mqtt

CHUNK_MAX = 1024
HEADER_SIZE = 128
BODY_BYTE_FLIPPED = HEADER_SIZE + 1500
TERMINAL = ("success", "failed", "rolled_back", "rejected")

PREFIX_CMD = "cmd/iotmonitor/"
PREFIX_DT = "dt/iotmonitor/"


# ---------------------------------------------------------------------------
# Wire contract (pure functions: no network, easy to check)
# ---------------------------------------------------------------------------

def topics(device_id):
    return {
        "command": f"{PREFIX_CMD}{device_id}/ota",
        "data": f"{PREFIX_CMD}{device_id}/ota/data",
        "request": f"{PREFIX_DT}{device_id}/ota/request",
        "result": f"{PREFIX_DT}{device_id}/ota/result",
    }


def build_command(job, size_a, size_b):
    return json.dumps({"job": job, "size_a": size_a, "size_b": size_b}, separators=(",", ":"))


def frame_chunk(job, offset, data):
    """The /ota/data payload: job and offset as unsigned 32-bit little-endian, then the bytes."""
    if not 0 < len(data) <= CHUNK_MAX:
        raise ValueError(f"a chunk carries 1..{CHUNK_MAX} bytes, got {len(data)}")
    return struct.pack("<II", job, offset) + data


def parse_request(payload):
    """Return (job, slot, offset, length) from a /ota/request payload, or None if malformed."""
    try:
        obj = json.loads(payload)
        job, slot, off, length = int(obj["job"]), obj["slot"], int(obj["offset"]), int(obj["len"])
    except (ValueError, KeyError, TypeError):
        return None
    if slot not in ("A", "B") or not 0 < length <= CHUNK_MAX or off < 0:
        return None
    return job, slot, off, length


def parse_report(payload):
    try:
        obj = json.loads(payload)
    except ValueError:
        return None
    return obj if isinstance(obj, dict) and obj.get("event") == "ota" else None


# ---------------------------------------------------------------------------

class Server:
    def __init__(self, args):
        self.args = args
        self.t = topics(args.device_id)
        self.images = {}
        for slot, path in (("A", args.image_a), ("B", args.image_b)):
            if path:
                data = open(path, "rb").read()
                if args.corrupt_body and len(data) > BODY_BYTE_FLIPPED:
                    buf = bytearray(data)
                    buf[BODY_BYTE_FLIPPED] ^= 0x01
                    data = bytes(buf)
                self.images[slot] = data
        self.requests = 0
        self.chunks = 0
        self.chunk_bytes = 0
        self.first_chunk = None
        self.last_chunk = None
        self.silent_until = 0.0
        self.silent_done = False
        self.done = threading.Event()
        self.result = None
        self.t0 = time.time()
        self.lock = threading.Lock()

        try:
            self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=args.client_id)
        except AttributeError:  # paho 1.x
            self.client = mqtt.Client(client_id=args.client_id)
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        if args.cafile:
            self.client.tls_set(ca_certs=args.cafile, certfile=args.cert, keyfile=args.key,
                                tls_version=ssl.PROTOCOL_TLS_CLIENT)
            if args.no_hostname_check:
                self.client.tls_insecure_set(True)

    def log(self, text):
        print(f"[{time.time() - self.t0:7.2f}s] {text}", flush=True)

    # -- MQTT callbacks (names and signatures differ between paho 1.x and 2.x; *a absorbs both)
    def on_connect(self, client, userdata, flags, rc, *a):
        self.log(f"connected to the broker (rc={rc})")
        client.subscribe([(self.t["request"], 0), (self.t["result"], 1)])
        if self.args.job is not None:
            size_a = len(self.images.get("A", b""))
            size_b = len(self.images.get("B", b""))
            cmd = build_command(self.args.job, size_a, size_b)
            self.log(f"> {self.t['command']}  {cmd}")
            client.publish(self.t["command"], cmd, qos=1)

    def on_message(self, client, userdata, msg):
        if msg.topic == self.t["request"]:
            self.serve(msg.payload)
        elif msg.topic == self.t["result"]:
            self.report(msg.payload)

    def serve(self, payload):
        req = parse_request(payload)
        if req is None:
            self.log(f"! malformed request {payload!r}")
            return
        job, slot, offset, length = req
        a = self.args
        with self.lock:
            self.requests += 1
            n = self.requests
        image = self.images.get(slot)
        if image is None:
            self.log(f"! no image for slot {slot}")
            return

        if a.silent_at is not None and not self.silent_done and offset >= a.silent_at:
            self.silent_done = True
            self.silent_until = time.time() + a.silent_seconds
            self.log(f"going silent for {a.silent_seconds}s at offset {offset}")
        if time.time() < self.silent_until:
            return
        if a.drop_requests and n <= a.drop_requests:
            self.log(f"dropping request {n}")
            return

        data = image[offset:offset + length]
        if not data:
            self.log(f"! offset {offset} is past the end of the slot-{slot} image")
            return
        self.client.publish(self.t["data"], frame_chunk(job, offset, data), qos=0)
        now = time.time()
        with self.lock:
            if self.first_chunk is None:
                self.first_chunk = now
            self.last_chunk = now
            self.chunks += 1
            self.chunk_bytes += len(data)
        if offset % (64 * 1024) == 0:
            self.log(f"< slot {slot} offset {offset}  ({100 * offset // len(image)}%)")

    def report(self, payload):
        rep = parse_report(payload)
        self.log(f"< result {payload.decode('latin-1')}")
        if rep and rep.get("result") in TERMINAL:
            self.result = rep["result"]
            self.done.set()

    def summary(self):
        """US-O1: the real download rate, from the first chunk served to the last."""
        with self.lock:
            chunks, size = self.chunks, self.chunk_bytes
            span = (self.last_chunk - self.first_chunk) if self.first_chunk is not None else 0.0
        if chunks and span > 0:
            self.log(f"served {chunks} chunks, {size} bytes in {span:.1f}s: "
                     f"{size / span / 1024:.1f} KiB/s, {span / chunks * 1000:.0f} ms per chunk")

    def run(self):
        a = self.args
        self.client.connect(a.host, a.port, keepalive=30)
        self.client.loop_start()
        finished = self.done.wait(a.timeout)
        self.client.loop_stop()
        self.client.disconnect()
        self.summary()
        if not finished:
            self.log("timeout")
            return 1
        ok = a.expect is None or self.result == a.expect
        self.log(f"result: {self.result}  expected: {a.expect or 'any'}  -> {'PASS' if ok else 'FAIL'}")
        return 0 if ok else 1


def selftest():
    """Check the wire contract without a broker."""
    t = topics("abc")
    assert t["command"] == "cmd/iotmonitor/abc/ota" and t["data"] == "cmd/iotmonitor/abc/ota/data"
    assert t["request"] == "dt/iotmonitor/abc/ota/request" and t["result"] == "dt/iotmonitor/abc/ota/result"
    assert build_command(42, 491648, 491600) == '{"job":42,"size_a":491648,"size_b":491600}'
    frame = frame_chunk(42, 4096, b"\xAB\xCD")
    assert frame == bytes([42, 0, 0, 0, 0, 16, 0, 0, 0xAB, 0xCD]), frame
    assert len(frame_chunk(1, 0, bytes(1024))) == 8 + 1024
    for bad in (b"", bytes(1025)):
        try:
            frame_chunk(1, 0, bad)
        except ValueError:
            pass
        else:
            raise AssertionError("an out-of-range chunk was framed")
    assert parse_request(b'{"job":42,"slot":"B","offset":4096,"len":1024}') == (42, "B", 4096, 1024)
    for bad in (b"{}", b"nope", b'{"job":1,"slot":"C","offset":0,"len":1}',
                b'{"job":1,"slot":"A","offset":0,"len":0}', b'{"job":1,"slot":"A","offset":0,"len":2000}'):
        assert parse_request(bad) is None, bad
    rep = parse_report(b'{"event":"ota","job":42,"result":"failed","reason":"hash_mismatch"}')
    assert rep["result"] == "failed" and parse_report(b'{"event":"x"}') is None
    print("selftest ok")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true", help="check the wire contract and exit")
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--device-id", help="the device serial (24 hex characters)")
    ap.add_argument("--client-id", default="ota-operator")
    ap.add_argument("--cafile", help="CA certificate; give it (with --cert and --key) to use TLS")
    ap.add_argument("--cert", help="operator client certificate")
    ap.add_argument("--key", help="operator client key")
    ap.add_argument("--no-hostname-check", action="store_true", help="bring-up broker with a bare IP")
    ap.add_argument("--image-a", help="signed image linked for Slot A (.ota)")
    ap.add_argument("--image-b", help="signed image linked for Slot B (.ota)")
    ap.add_argument("--job", type=lambda s: int(s, 0), help="job id; sends the command")
    ap.add_argument("--silent-at", type=int, help="stop answering from this offset")
    ap.add_argument("--silent-seconds", type=float, default=120.0)
    ap.add_argument("--drop-requests", type=int, default=0, help="ignore the first N requests")
    ap.add_argument("--corrupt-body", action="store_true", help="flip one body byte in the images")
    ap.add_argument("--expect", choices=TERMINAL, help="terminal report that means PASS")
    ap.add_argument("--timeout", type=float, default=900.0)
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.device_id:
        ap.error("--device-id is required")
    if args.cafile and not (args.cert and args.key):
        ap.error("--cafile needs --cert and --key (the broker requires a client certificate)")
    return Server(args).run()


if __name__ == "__main__":
    sys.exit(main())
