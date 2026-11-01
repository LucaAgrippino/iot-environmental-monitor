#!/usr/bin/env python3
"""Drive the Gateway three-panel console over the ST-Link virtual COM port and check what it shows.

Sends keystrokes to /dev/ttyACM0 (460800 8N1 by default; CONSOLE_BAUD=... to override), renders the returned ANSI stream with the `pyte`
terminal emulator (pip install pyte) and asserts on the screen text. Use with the
Gateway application image, board connected, nothing else
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

COLS, ROWS = 100, 36  # the firmware default layout; `term <rows> <cols>` changes it
HEADER, CENTRE, SOUTH, PROMPT = (0, 2), (3, 24), (25, 35), 35  # 0-based pyte rows, inclusive-exclusive end for slices
results = []


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0  # iflag
    attrs[1] = 0  # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0  # lflag
    attrs[4] = attrs[5] = getattr(termios, "B" + os.environ.get("CONSOLE_BAUD", "460800"))  # = CONSOLE_LINK_BAUD
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

    def rows(self, lo, hi):
        """Text of the 0-based screen rows lo..hi-1."""
        return "\n".join(line.rstrip() for line in self.screen.display[lo:hi])

    def centre(self):
        return self.rows(3, 24)

    def south(self):
        return self.rows(25, 35)

    def prompt(self):
        return self.screen.display[35].rstrip()


def wait_for_layout(con, timeout=25.0):
    """After a reset the board needs a few seconds before the console runs: nudge with Ctrl-L until
    the header is on screen."""
    end = time.time() + timeout
    while time.time() < end:
        con.send("\x0c", 1.5)
        if "ENVMON GATEWAY" in con.rows(0, 1):
            return True
    return False


def wait_for_text(con, text, timeout=12.0):
    """Wait until `text` is in the command panel. The console is the lowest-priority task, so for a
    few seconds after boot (WiFi association, TLS handshake) it can answer slowly."""
    end = time.time() + timeout
    while time.time() < end:
        if text in con.south():
            return True
        con.pump(0.25)
    return text in con.south()


def check(name, ok, detail=""):
    results.append((name, ok))
    print(("PASS  " if ok else "FAIL  ") + name + (("  - " + detail) if (detail and not ok) else ""))


def show(con, title):
    if not os.environ.get("VERBOSE"):
        return
    print("\n--- " + title + " " + "-" * (70 - len(title)))
    print(con.text())
    print("-" * 75 + "\n")


def cmd(con, line, wait=0.8):
    """Clear the command panel, run one line, return what the panel shows."""
    con.send("clear\r", 0.5)
    con.send(line + "\r", wait)
    return con.south()


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    con = Console(open_port(port))

    # The board drew its layout before this script attached; Ctrl-L repaints everything.
    con.send("\x1b", 0.5)  # drop anything a previous session left in the line
    con.send("\x0c", 2.5)
    con.pump(1.0)
    show(con, "boot")
    check("PN-001 north panel: product, firmware, uptime, alarms", "ENVMON GATEWAY" in con.rows(0, 1) and "fw " in con.rows(0, 1) and "up " in con.rows(0, 1))
    check("PN-002 north panel: WiFi, MQTT, Modbus, sensors, RSSI", all(w in con.rows(1, 2) for w in ("WiFi", "MQTT", "Modbus", "sensors", "dBm")))
    check("PN-003 rule under the header, divider above the command panel", set(con.screen.display[2].strip()) == {"-"} and "console" in con.screen.display[24], repr(con.screen.display[2][:20]) + " / " + repr(con.screen.display[24][:30]))
    check("PN-004 prompt on the last row", con.prompt().startswith("envmon>"))

    # Typing goes to the prompt row and the cursor follows it.
    con.send("sta", 0.4)
    check("PN-005 typed text appears at the prompt", con.prompt() == "envmon> sta")
    check("PN-006 the cursor sits at the end of the typed text", (con.screen.cursor.y, con.screen.cursor.x) == (PROMPT, len("envmon> sta")), str((con.screen.cursor.y, con.screen.cursor.x)))
    con.send("\x7f", 0.4)
    check("PN-007 backspace", con.prompt() == "envmon> st")
    con.send("\x1b", 0.4)
    check("PN-008 Escape clears the line", con.prompt() == "envmon>")

    # Commands answer in the south panel and never disturb the others.
    head_before = con.rows(0, 3)
    out = cmd(con, "version")
    check("PN-010 the command is echoed in the south panel", "envmon> version" in out)
    check("PN-011 the answer follows the echo", "fw " in out and "STM32L475" in out)
    check("PN-012 the north panel is untouched by a command", con.rows(0, 1).split("up")[0] == head_before.split("\n")[0].split("up")[0])
    out = cmd(con, "help", 1.5)
    check("PN-013 a long answer scrolls inside the south panel only", "restart the gateway" in out and "ENVMON GATEWAY" in con.rows(0, 1) and "console" in con.screen.display[24])
    out = cmd(con, "bogus")
    check("PN-014 unknown command", "unknown command" in out)
    out = cmd(con, "sensors", 1.0)
    m = re.search(r"temp\s+(-?\d+\.\d+)", out)
    check("PN-015 sensors: plausible temperature", bool(m) and 5.0 < float(m.group(1)) < 45.0, "no value" if not m else m.group(1))
    con.send("\x1b[A", 0.4)
    check("PN-016 Up recalls the last command", con.prompt() == "envmon> sensors")
    con.send("\x1b", 0.4)

    # Logs scroll in the centre panel, the prompt stays put.
    con.send("clear\r", 0.5)
    before = con.centre()
    con.send("typ", 0.2)
    deadline = time.time() + 20
    while con.centre() == before and time.time() < deadline:
        con.pump(0.25)
    check("PN-020 log lines arrive in the centre panel", con.centre() != before and "[" in con.centre())
    check("PN-021 a log line never lands in the command panel or on the prompt", "[INFO" not in con.south() and "[INFO" not in con.prompt())
    check("PN-022 typing survived the log line", con.prompt() == "envmon> typ" and (con.screen.cursor.y, con.screen.cursor.x) == (PROMPT, len("envmon> typ")))
    con.send("\x1b", 0.4)

    # Key latency: the echo of a keystroke.
    worst = 0.0
    for _ in range(10):
        t0 = time.time()
        os.write(con.fd, b"x")
        while "envmon> x" != con.prompt() and time.time() - t0 < 2.0:
            con.pump(0.005)
        worst = max(worst, time.time() - t0)
        con.send("\x1b", 0.2)
    check("PN-023 key echo latency under 400 ms (worst %.0f ms; median is ~55)" % (worst * 1000), worst < 0.4)

    # Redraw and resize.
    con.send("\x0c", 1.5)
    check("PN-030 Ctrl-L repaints the layout", "ENVMON GATEWAY" in con.rows(0, 1) and con.prompt().startswith("envmon>"))
    out = cmd(con, "term")
    check("PN-031 term reports the layout size", "layout 36 rows x 100 cols" in out)
    out = cmd(con, "term 5 5")
    check("PN-032 term refuses a size out of range", "size out of range" in out)

    # Thresholds, selftest, profiles.
    out = cmd(con, "set t.hi 31")
    check("PN-040 set applies", "applied" in out, out)
    out = cmd(con, "thresholds")
    check("PN-041 thresholds shows the new limit", "31.0" in out, out)
    out = cmd(con, "set t.hi abc")
    check("PN-042 bad value is refused", "[ERR]" in out)
    out = cmd(con, "selftest", 1.5)
    check("PN-043 selftest prints per-subsystem results", "sensors" in out and "overall" in out)
    out = cmd(con, "profiles list", 1.0)
    check("PN-044 the default profile is listed", "slave   1   map 1   vendor 0x1A45" in out)
    out = cmd(con, "profiles add addr=5 map=1 vendor=0x1A45 desc=Boiler room", 3.0)  # a flash save: ~0.8 s
    check("PN-045 profiles add saves a profile", "profile for slave 5 saved" in out)
    out = cmd(con, "profiles add addr=7 map=1 vendor=1 desc=a b c d e")
    check("PN-046 a line with too many words is refused, not truncated", "too many words" in out)
    out = cmd(con, "profiles remove 5", 3.0)
    check("PN-047 profiles remove", "profile for slave 5 removed" in out)
    out = cmd(con, "prov set modbus-addr 300")
    check("PN-048 modbus-addr 300 is refused", "[ERR]" in out)

    # Links against reality (WiFi joined, broker up, no Field Device attached).
    con.send("clear\r", 0.5)
    con.send("wifi status\r", 0.5)
    wait_for_text(con, "rssi", 30.0)  # the link is still coming up for ~40 s after a reset
    m = re.search(r"rssi\s+(-\d+) dBm", con.south())
    check("PN-050 wifi status: connected with a plausible RSSI", "\u25cf connected" in con.south() and bool(m) and -95 <= int(m.group(1)) <= -20, con.south())
    for _ in range(12):  # the TLS handshake can still be running just after boot
        out = cmd(con, "mqtt status", 2.0)
        if "\u25cf connected" in out:
            break
    check("PN-051 mqtt status: connected to the broker", "\u25cf connected" in out and ":8883" in out)
    out = cmd(con, "modbus status", 1.2)
    check("PN-052 modbus status: offline (no Field Device)", "\u25cf offline" in out)
    row2 = con.screen.display[1]
    dots = [i for i, ch in enumerate(row2) if ch == "\u25cf"]
    check("PN-053 header dots: WiFi and MQTT up, Modbus not", len(dots) == 3 and con.cell(1, dots[0]).fg != con.cell(1, dots[2]).fg, str([con.cell(1, i).fg for i in dots]))

    # Provisioning: confirm, decline, timeout.
    cmd(con, "prov set mqtt-endpoint 192.168.0.218")
    con.send("prov set modbus-addr 9\r", 0.8)
    con.send("prov commit\r", 1.0)
    check("PN-060 commit shows the staged values and the prompt asks y/N", "modbus-addr = 9" in con.south() and con.prompt().startswith("[y/N]"))
    con.send("y", 3.0)
    check("PN-061 y applies provisioning and the normal prompt returns", "provisioning applied" in con.south() and con.prompt().startswith("envmon>"))
    cmd(con, "prov set modbus-addr 10")
    con.send("prov commit\r", 1.0)
    check("PN-062 the prompt asks again", con.prompt().startswith("[y/N]"))
    con.pump(31.5)
    check("PN-063 the question times out and discards", "timed out" in con.south() and con.prompt().startswith("envmon>"))
    cmd(con, "prov set modbus-addr 9")
    con.send("prov commit\r", 1.0)
    con.send("n", 1.0)
    check("PN-064 n discards", "Discarded" in con.south() and con.prompt().startswith("envmon>"))

    # Reboot: the limit set earlier must survive.
    con.send("clear\r", 0.5)
    mark = len(con.raw)
    con.send("reboot\r", 0.5)
    con.pump(4.0)
    check("PN-070 reboot gives the terminal back (scroll region reset)", b"\x1b[r" in con.raw[mark:])
    check("PN-071 the layout is back after the reboot", wait_for_layout(con))
    # The layout is up well before Lifecycle has loaded the stored configuration: ask again.
    out = ""
    for _ in range(20):
        out = cmd(con, "thresholds", 1.5)
        if "31.0" in out:
            break
        con.pump(2.0)
    check("PN-072 persistence: t.hi 31.0 survived the reset", "31.0" in out, out)
    out = cmd(con, "mqtt status")
    check("PN-073 the committed broker survived the reset", "192.168.0.218:8883" in out, out)
    out = cmd(con, "set t.hi 40")
    check("PN-074 restored t.hi 40", wait_for_text(con, "applied"))

    bad = [n for n, ok in results if not ok]
    print("\n%d checks, %d failed" % (len(results), len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
