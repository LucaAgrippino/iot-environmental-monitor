#!/usr/bin/env python3
"""Provision a Gateway over its serial console: WiFi credentials and the X.509 identity.

Sends `prov set wifi-ssid`, `prov set wifi-pass`, `prov commit` (answering the confirmation), then the
CA, the client certificate and the private key as `prov cert add` chunks and `prov cert end`. PEM files
are converted to DER here, because the device stores DER (REQ-LI-040, REQ-LI-060, REQ-NF-302).

    python3 scripts/provision_device.py --port /dev/ttyACM1 --ssid MyNet --password secret-pass \\
        --ca ca.crt --cert client.crt --key client.key
    python3 scripts/provision_device.py --port /dev/ttyACM1 --endpoint broker.example.org
    python3 scripts/provision_device.py --port /dev/ttyACM1 --status
    python3 scripts/provision_device.py --port /dev/ttyACM1 --clear

The Gateway console runs at 460800 baud (CONSOLE_BAUD to override). Exit status 0 only if every step answered with its success line.
"""
import argparse
import base64
import os
import re
import sys
import termios
import time


ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Z0-9]|\x1b[=>]")

CHUNK_CHARS = 96  # base64 characters per command: 72 bytes, a multiple of 4 characters, under the 120 limit


def open_port(path, baud):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    rate = getattr(termios, "B%d" % baud)
    attrs[4] = attrs[5] = rate
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


class Console:
    """Collects what the console prints after each command, with the escape codes removed."""

    def __init__(self, fd):
        self.fd = fd
        self.buf = ""

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                data = b""
            if data:
                self.buf += ANSI.sub("", data.decode("latin1"))
            else:
                time.sleep(0.02)

    def send(self, line, wait=0.5):
        self.buf = ""
        os.write(self.fd, (line + "\r").encode())
        self.pump(wait)

    def text(self):
        return self.buf

    def has(self, needle):
        # the log panel repaints between characters: compare without whitespace
        return "".join(needle.split()) in "".join(self.buf.split())


def pem_to_der(path):
    data = open(path).read()
    m = re.search(r"-----BEGIN [A-Z ]+-----(.*?)-----END [A-Z ]+-----", data, re.S)
    if not m:
        sys.exit("%s: no PEM block found" % path)
    return base64.b64decode("".join(m.group(1).split()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--ssid")
    ap.add_argument("--password")
    ap.add_argument("--endpoint", help="MQTT broker host name or IPv4 address")
    ap.add_argument("--ca")
    ap.add_argument("--cert")
    ap.add_argument("--key")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--clear", action="store_true")
    ap.add_argument("--run", action="append", default=[], help="run a console command and print what it shows (repeatable)")
    a = ap.parse_args()

    fd = open_port(a.port, int(os.environ.get("CONSOLE_BAUD", "460800")))
    os.set_blocking(fd, False)
    con = Console(fd)
    con.pump(0.5)
    ok = True

    def step(line, expect, wait=0.6, tries=3):
        nonlocal ok
        good = False
        for _ in range(tries):
            con.send(line, wait)
            good = con.has(expect)
            if good:
                break
        shown = line if "wifi-pass" not in line else "prov set wifi-pass ********"
        print("%-4s %s" % ("ok" if good else "FAIL", shown if len(shown) < 70 else shown[:67] + "..."))
        ok = ok and good

    if a.clear:
        step("prov cert clear", "removed", 1.5)
    if (a.ssid and a.password) or a.endpoint:
        if a.ssid and a.password:
            step("prov set wifi-ssid %s" % a.ssid, "staged wifi-ssid")
            step("prov set wifi-pass %s" % a.password, "staged wifi-pass")
        if a.endpoint:
            step("prov set mqtt-endpoint %s" % a.endpoint, "staged mqtt-endpoint")
        con.send("prov commit", 0.8)
        step("y", "provisioning applied", 3.0, tries=1)
    for name, path in (("ca", a.ca), ("client", a.cert), ("key", a.key)):
        if not path:
            continue
        der = pem_to_der(path)
        b64 = base64.b64encode(der).decode()
        stored = False
        for attempt in range(1, 4):
            # The console task has the lowest priority and drops input while the board associates or
            # handshakes, so every chunk is acknowledged with its running byte count and a mismatch
            # restarts the item.
            con.send("prov cert reset", 0.6)
            sent = 0
            clean = True
            for i in range(0, len(b64), CHUNK_CHARS):
                chunk = b64[i:i + CHUNK_CHARS]
                sent += len(base64.b64decode(chunk))
                got = False
                for _ in range(4):
                    con.send("prov cert add %s %s" % (name, chunk), 0.1)
                    # wait for the acknowledgement before deciding anything: a slow answer must not
                    # be mistaken for a lost chunk (a resend would double it)
                    for _ in range(40):
                        if con.has("%s: %d bytes so far" % (name, sent)) or con.has("bytes so far"):
                            break
                        con.pump(0.1)
                    got = con.has("%s: %d bytes so far" % (name, sent))
                    if got or con.has("bytes so far"):
                        break  # acknowledged, or the count is wrong (restart the item)
                if not got:
                    clean = False
                    break
            if not clean:
                print("     %s: restart (attempt %d)" % (name, attempt))
                continue
            con.send("prov cert end %s" % name, 2.5)
            if con.has("stored"):
                stored = True
                break
            print("     console said: " + " ".join(con.text().split())[:160])
        print("%-4s prov cert %s (%d bytes)" % ("ok" if stored else "FAIL", name, len(der)))
        ok = ok and stored
    con.send("prov cert status", 0.8)
    for line in con.text().replace("\r", "").split("\n"):
        if ("wifi" in line and ("stored" in line or "provisioned" in line)) or "identity" in line:
            print("    " + " ".join(line.split()))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
