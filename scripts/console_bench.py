#!/usr/bin/env python3
"""Drive the Gateway console TUI over the ST-Link virtual COM port and check what it shows.

Sends keystrokes to /dev/ttyACM0 (115200 8N1), renders the returned ANSI stream with the `pyte`
terminal emulator (pip install pyte) and asserts on the screen text. Use with the
integration-tests/console_service image (see its header comment), board connected, nothing else
reading the port.

    python3 scripts/console_bench.py [/dev/ttyACM0]

Exit status 0 only if every check passes.
"""

import codecs
import os
import re
import select
import sys
import termios
import time

import pyte

COLS, ROWS = 88, 30
results = []


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0  # iflag
    attrs[1] = 0  # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0  # lflag
    attrs[4] = attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


class Console:
    def __init__(self, fd):
        self.fd = fd
        self.screen = pyte.Screen(COLS, ROWS)
        self.stream = pyte.Stream(self.screen)
        self.raw = b""
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")  # glyphs may span reads

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if ready:
                data = os.read(self.fd, 4096)
                self.raw += data
                self.stream.feed(self.decoder.decode(data))

    def send(self, text, wait=0.6):
        os.write(self.fd, text.encode())
        self.pump(wait)

    def text(self):
        return "\n".join(line.rstrip() for line in self.screen.display)

    def has(self, needle):
        return needle in self.text()

    def cell(self, row, col):
        return self.screen.buffer[row][col]


def check(name, ok, detail=""):
    results.append((name, ok))
    print(("PASS  " if ok else "FAIL  ") + name + (("  - " + detail) if (detail and not ok) else ""))


def show(con, title):
    if not os.environ.get("VERBOSE"):
        return
    print("\n--- " + title + " " + "-" * (70 - len(title)))
    print(con.text())
    print("-" * 75 + "\n")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    con = Console(open_port(port))

    # IT-GCS-001: the board drew its first frame before this script attached, so a terminal that
    # joins late only sees changes. Ctrl-L repaints the whole screen.
    con.send("\x0c", 2.5)
    con.send("1", 1.5)
    con.pump(2.0)
    show(con, "boot / graph")
    check("IT-GCS-001 TUI renders: banner, menu, chart", con.has("ENVIRONMENTAL GATEWAY") and con.has("MAIN MENU") and con.has("LIVE · TEMPERATURE"))
    check("IT-GCS-002 banner fields", con.has("uptime") and con.has("sensors online") and con.has("wifi"))
    m = re.search(r"now\s+(-?\d+\.\d) °C", con.text())
    check("IT-GCS-027 live temperature is plausible", bool(m) and 5.0 < float(m.group(1)) < 45.0, "no value" if not m else m.group(1))

    # IT-GCS-003/004/005/006: navigation
    con.send("\x1b[B", 0.8)
    check("IT-GCS-003 arrow down selects System status", con.has("SYSTEM STATUS"))
    con.send("4", 0.8)
    check("IT-GCS-004 digit jumps to Thresholds", con.has("THRESHOLDS"))
    con.send("\t", 0.8)
    check("IT-GCS-005 Tab moves focus to the panel", con.has("focus PANEL"))
    con.send("\x1b", 0.8)
    check("IT-GCS-006 Esc returns to the menu", con.has("focus MENU"))

    # IT-GCS-007: channel cycling
    con.send("1", 0.8)
    con.send("\r", 0.8)
    con.send("\x1b[B", 1.0)
    check("IT-GCS-007 down cycles to humidity", con.has("HUMIDITY"))
    con.send("\x1b[B", 1.0)
    check("IT-GCS-007 and to pressure", con.has("PRESSURE"))
    show(con, "pressure chart")
    con.send("\x1b[B", 1.0)  # wraps back to temperature
    check("IT-GCS-007 and wraps to temperature", con.has("TEMPERATURE"))
    con.send("\x1b", 0.6)

    # IT-GCS-009..011: panels
    con.send("2", 1.2)
    check("IT-GCS-009 status panel is populated", con.has("Uptime") and con.has("Sensors") and con.has("WiFi"))
    con.send("3", 1.2)
    check("IT-GCS-010 alarms panel", con.has("ACTIVE ALARMS") and (con.has("No active alarms") or con.has("HIGH") or con.has("LOW")))
    con.send("5", 1.2)
    check("IT-GCS-011 network panel", con.has("NETWORK") and con.has("MQTT"))
    show(con, "network")

    # Console
    con.send("6", 0.6)
    con.send("\r", 0.6)
    con.send("help\r", 1.2)
    check("IT-GCS-012 help lists commands (tail of the list is visible)", con.has("reboot") and con.has("restart the gateway"))
    con.send("clear\r", 0.8)
    con.send("sensors\r", 1.2)
    show(con, "sensors")
    check("IT-GCS-013 sensors table", con.has("temp") and con.has("humid") and con.has("press") and con.has("°C"))
    con.send("clear\r", 0.6)
    con.send("read t\r", 1.0)
    check("read t", con.has("TEMPERATURE"))
    con.send("clear\r", 0.6)
    con.send("version\r", 1.0)
    check("version", con.has("STM32L475VGT6"))
    con.send("clear\r", 0.6)
    con.send("serial\r", 1.0)
    check("serial prints 24 hex digits", re.search(r"\b[0-9A-F]{24}\b", con.text()) is not None)

    # IT-GCS-014 / 008: set re-colours the chart
    con.send("clear\r", 0.6)
    con.send("set t.hi 31\r", 1.5)
    check("IT-GCS-014 set applies", con.has("applied"))
    con.send("\x1b", 0.6)
    con.send("1", 2.0)
    check("IT-GCS-008 chart shows the new limit", con.has("HI 31.0"))
    show(con, "after set t.hi 31")
    con.send("6\r", 0.8)

    # selftest, profiles, errors
    con.send("clear\r", 0.6)
    con.send("selftest\r", 1.5)
    check("IT-GCS-018 selftest prints per-subsystem results", con.has("sensors") and con.has("overall"))
    con.send("clear\r", 0.6)
    con.send("profiles list\r", 1.0)
    check("IT-GCS-019 profiles list", con.has("slave") or con.has("No profiles"))
    con.send("clear\r", 0.6)
    con.send("set t.hi abc\r", 1.0)
    check("IT-GCS-029 bad value is refused", con.has("[ERR]"))
    con.send("clear\r", 0.6)
    con.send("prov set modbus-addr 300\r", 1.0)
    check("IT-GCS-029 modbus-addr 300 is refused", con.has("[ERR]"))
    con.send("clear\r", 0.6)
    con.send("bogus\r", 1.0)
    check("unknown command", con.has("unknown command"))

    # IT-GCS-015/016: confirm flow
    con.send("clear\r", 0.6)
    con.send("prov set modbus-addr 9\r", 0.8)
    con.send("prov commit\r", 1.0)
    check("confirm prompt appears", con.has("Apply? [y/N]"))
    con.send("n", 1.0)
    check("IT-GCS-016 n discards", con.has("Discarded") and not con.has("Apply? [y/N]"))

    # persistence + IT-GCS-024: reboot, the limit set earlier must survive
    con.send("clear\r", 0.6)
    con.send("reboot\r", 0.5)
    con.pump(4.0)
    check("IT-GCS-024 reboot restores the screen setup", b"\x1b[?1049l" in con.raw)
    con.send("\x1b", 1.0)
    con.send("6", 0.8)
    con.send("\r", 0.8)
    con.send("thresholds\r", 1.5)
    show(con, "after reboot: thresholds")
    check("persistence: t.hi 31.0 survived the reset", con.has("31.0"))

    # put the default back
    con.send("set t.hi 40\r", 1.5)
    check("restored t.hi 40", con.has("applied"))

    bad = [n for n, ok in results if not ok]
    print("\n%d checks, %d failed" % (len(results), len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
