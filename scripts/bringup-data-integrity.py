#!/usr/bin/env python3
"""TC-HW-CP-010 (TC-HW-WIFITASK-data-integrity) — laptop side.

Publishes N patterned payloads to the gateway bring-up's config command
topic; firmware/gateway/integration-tests/cloud_publisher/
main_test_cloud_publisher.c's bringup_msg_cb() verifies every byte and the
sequence and logs a PASS/FAIL summary on the UART.

Payload: "DI:<seq:04d>:<total:04d>:<len:05d>:" (19 bytes) + len bytes of
'A'..'Z' repeating. Sizes are chosen to sit on both sides of the
ISM43362's 1460-byte R0 chunk and to span several chunks, so a stream-
cursor bug in wifitask_try_recv() (duplicate/dropped bytes at chunk
boundaries) shows up as CORRUPT rather than hiding inside one chunk.

Usage: scripts/bringup-data-integrity.py [--host IP] [--client-id ID] [--gap S]
"""
import argparse
import pathlib
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
CERTS = REPO / "local-test" / "mosquitto-broker"
HEADER_LEN = 19
# data lengths (excluding the 19-byte header); total payload = 19 + len
SIZES = [13, 300, 1000, 1400, 1441, 1442, 1500, 2000, 2919, 2922, 3500, 4000,
         13, 1441, 1442, 4000, 300, 2922, 13, 1000]


def payload(seq: int, total: int, length: int) -> bytes:
    hdr = f"DI:{seq:04d}:{total:04d}:{length:05d}:".encode()
    assert len(hdr) == HEADER_LEN
    body = bytes((ord("A") + (i % 26)) for i in range(length))
    return hdr + body


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--client-id", default="gw-cp-bringup-001")
    ap.add_argument("--gap", type=float, default=1.5, help="seconds between publishes")
    args = ap.parse_args()
    topic = f"cmd/iotmonitor/{args.client_id}/config"
    total = len(SIZES)
    for seq, length in enumerate(SIZES):
        data = payload(seq, total, length)
        cmd = ["mosquitto_pub", "-h", args.host, "-p", "8883",
               "--cafile", str(CERTS / "ca.crt"), "--cert", str(CERTS / "client.crt"),
               "--key", str(CERTS / "client.key"), "-q", "1", "-t", topic, "-s"]
        r = subprocess.run(cmd, input=data, capture_output=True)
        if r.returncode != 0:
            print(f"seq {seq}: mosquitto_pub failed: {r.stderr.decode().strip()}", file=sys.stderr)
            return 1
        print(f"seq {seq:2d}/{total}: {len(data):5d} bytes published")
        time.sleep(args.gap)
    print("done — read the TC-HW-CP-010 summary on the board's UART")
    return 0


if __name__ == "__main__":
    sys.exit(main())
