#!/usr/bin/env python3
"""ota_uart_server.py - host side of the UpdateService bench harness (TC-HW-US-nnn).

Plays the cloud for integration-tests/update_service/main_test_update_service.c over the debug
UART: it sends the OTA command, answers the device's chunk requests from a signed image file
(tools/ota/make_image.py output), and prints the device's reports and events. Options bend the
server so that the failure cases of docs/lld/application/update-service-lld.md 23.1 can be run.

    # TC-HW-US-001: a valid Slot-B image, nothing bent
    ota_uart_server.py --image-a slot_a.ota --image-b slot_b.ota --job 42

    # TC-HW-US-002: stop answering at 64 KB for 30 s (the device must give up after 3 retries)
    ota_uart_server.py ... --silent-at 65536 --silent-seconds 30

    # TC-HW-US-003: link reported down at 64 KB for 120 s, then up
    ota_uart_server.py ... --link-down-at 65536 --link-down-seconds 120

    # TC-HW-US-005: one flipped body byte;  TC-HW-US-007: a second command mid-download
    ota_uart_server.py ... --corrupt-body
    ota_uart_server.py ... --second-command-at 65536

    # TC-HW-US-009 / -010: seed a TRIAL_RUNNING / ROLLED_BACK record and let the board reset
    ota_uart_server.py --seed T       ota_uart_server.py --seed R

The wire protocol is in the header of the harness. The exit code is 0 when the run ended in the
expected terminal report (default: any terminal report; --expect names one).
"""

import argparse
import re
import sys
import time

import serial

CHUNK_MAX = 1024
HEADER_SIZE = 128
BODY_BYTE_FLIPPED = HEADER_SIZE + 1500  # inside the body of any image over 1.6 KB

TERMINAL = ("success", "failed", "rolled_back", "rejected")


def read_image(path):
    with open(path, "rb") as f:
        return f.read()


class Server:
    def __init__(self, args):
        self.args = args
        self.ser = serial.Serial(args.port, args.baud, timeout=0.05)
        self.images = {}
        if args.image_a:
            self.images["A"] = read_image(args.image_a)
        if args.image_b:
            self.images["B"] = read_image(args.image_b)
        if args.corrupt_body:
            for slot, data in self.images.items():
                buf = bytearray(data)
                buf[min(BODY_BYTE_FLIPPED, len(buf) - 1)] ^= 0x01
                self.images[slot] = bytes(buf)
        self.requests = 0
        self.silent_until = 0.0
        self.silent_done = False
        self.link_down_done = False
        self.second_done = False
        self.t0 = time.time()
        self.last_result = None
        self.second_reason = None

    def send(self, text):
        self.ser.write(text.encode("ascii") + b"\n")

    def log(self, text):
        print(f"[{time.time() - self.t0:7.2f}s] {text}", flush=True)

    def serve_request(self, job, slot, offset, length):
        a = self.args
        self.requests += 1
        image = self.images.get(slot)

        if image is None:
            self.log(f"no image for slot {slot}")
            return

        if a.silent_at is not None and not self.silent_done and offset >= a.silent_at:
            self.silent_until = time.time() + a.silent_seconds
            self.silent_done = True
            self.log(f"going silent for {a.silent_seconds}s at offset {offset}")

        if time.time() < self.silent_until:
            return

        if a.link_down_at is not None and not self.link_down_done and offset >= a.link_down_at:
            self.link_down_done = True
            self.log(f"LINK 0 for {a.link_down_seconds}s at offset {offset}")
            self.send("LINK 0")
            time.sleep(a.link_down_seconds)
            self.send("LINK 1")
            self.log("LINK 1")
            return  # the device re-requests the same offset when the link is back

        if a.second_command_at is not None and not self.second_done and offset >= a.second_command_at:
            self.second_done = True
            self.log("second command during the download")
            self.send(f"CMD {a.job + 1} {len(self.images.get('A', b''))} {len(self.images.get('B', b''))}")

        if a.drop_requests and self.requests <= a.drop_requests:
            self.log(f"dropping request {self.requests}")
            return

        data = image[offset:offset + length]
        self.send(f"DATA {job} {offset} {len(data)}")
        self.ser.write(data)

    def handle(self, line):
        self.log(f"< {line}")
        m = re.match(r"REQ (\d+) ([AB]) (\d+) (\d+)", line)
        if m:
            self.serve_request(int(m[1]), m[2], int(m[3]), int(m[4]))
            return None

        m = re.match(r"REPORT (\w+) (\S+) (\S+) ([0-9A-Fa-f]+) (\d+)", line)
        if m and m[1] == "rejected" and self.args.job is not None and m[3] == str(self.args.job + 1):
            # The answer to the second command (--second-command-at): checked at the end, and it
            # does not end the run, because the first update is still going.
            self.second_reason = m[2]
            return None
        if m and m[1] in TERMINAL:
            return m[1]

        if line.startswith("EVENT 11"):
            return "reset"
        return None

    def run(self):
        a = self.args
        self.ser.reset_input_buffer()

        if a.seed:
            self.send(f"SEED {a.seed}")
        elif a.job is not None:
            size_a = len(self.images.get("A", b""))
            size_b = len(self.images.get("B", b""))
            self.send(f"CMD {a.job} {size_a} {size_b}")
            self.log(f"CMD {a.job} size_a={size_a} size_b={size_b}")

        end = time.time() + a.timeout
        buf = b""
        seen = []

        while time.time() < end:
            buf += self.ser.read(256)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("latin-1").strip()
                if not line:
                    continue
                verdict = self.handle(line)
                if verdict:
                    seen.append(verdict)
                    # "accepted" is not terminal; after an activate the device resets: keep
                    # listening for the post-reboot report when a seed or reset was involved.
                    if verdict != "reset" or a.stop_at_reset:
                        self.last_result = verdict
                        return self.finish()

        self.log("timeout")
        return 1

    def finish(self):
        want = self.args.expect
        ok = (want is None) or (self.last_result == want)
        if self.args.second_command_at is not None:
            second_ok = self.second_reason == "busy"
            self.log(f"second command answered: {self.second_reason or 'nothing'}  "
                     f"(expected busy)  -> {'PASS' if second_ok else 'FAIL'}")
            ok = ok and second_ok
        self.log(f"result: {self.last_result}  expected: {want or 'any'}  -> {'PASS' if ok else 'FAIL'}")
        return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--image-a", help="signed image linked for Slot A (.ota)")
    ap.add_argument("--image-b", help="signed image linked for Slot B (.ota)")
    ap.add_argument("--job", type=lambda s: int(s, 0), help="job id; sends the command")
    ap.add_argument("--seed", choices=("T", "R"), help="seed a TRIAL_RUNNING / ROLLED_BACK record")
    ap.add_argument("--silent-at", type=int, help="stop answering from this offset")
    ap.add_argument("--silent-seconds", type=float, default=30.0)
    ap.add_argument("--link-down-at", type=int, help="report the link down from this offset")
    ap.add_argument("--link-down-seconds", type=float, default=120.0)
    ap.add_argument("--second-command-at", type=int, help="send a second command from this offset")
    ap.add_argument("--drop-requests", type=int, default=0, help="ignore the first N requests")
    ap.add_argument("--corrupt-body", action="store_true", help="flip one body byte in the images")
    ap.add_argument("--expect", choices=TERMINAL, help="terminal report that means PASS")
    ap.add_argument("--stop-at-reset", action="store_true", help="end the run at the activate reset")
    ap.add_argument("--timeout", type=float, default=600.0)
    args = ap.parse_args()

    if args.job is None and not args.seed:
        ap.error("give --job (send a command) or --seed")

    return Server(args).run()


if __name__ == "__main__":
    sys.exit(main())
