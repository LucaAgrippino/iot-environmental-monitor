#!/usr/bin/env python3
"""Drive the Gateway console TUI over the ST-Link virtual COM port and check what it shows.

Sends keystrokes to /dev/ttyACM0 (460800 8N1 by default; CONSOLE_BAUD=... to override), renders the returned ANSI stream with the `pyte`
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


def wait_for_menu(con, timeout=25.0):
    """After a reset the board needs a few seconds before the console runs: nudge with Ctrl-L until
    the menu is on screen."""
    end = time.time() + timeout
    while time.time() < end:
        con.send("\x0c", 1.5)
        if con.has("MAIN MENU"):
            return True
    return False


def wait_for_text(con, text, timeout=12.0):
    """Wait until `text` is on screen. The console is the lowest-priority task, so for a few
    seconds after boot (WiFi association, TLS handshake) it can answer slowly."""
    end = time.time() + timeout
    while time.time() < end:
        if con.has(text):
            return True
        con.pump(0.25)
    return con.has(text)


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
    con.send("\x1b", 1.0)  # leave any panel a previous run left focused ...
    con.send("\x1b", 1.0)
    con.send("1", 1.5)     # ... and start on the live graph
    con.send("\x0c", 2.0)
    con.pump(1.0)
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
    check("read-only panels (thresholds) do not take focus on Tab", con.has("focus MENU"))
    con.send("\r", 0.8)
    check("... nor on Enter", con.has("focus MENU"))
    check("... and the menu has no \u25ba marker on the read-only items",
          not any("\u25ba" in l for l in con.screen.display[13:18] if "Thresholds" in l or "Network" in l))
    con.send("1", 0.8)
    con.send("\t", 0.8)
    check("IT-GCS-005 Tab moves focus to the graph", con.has("focus PANEL"))
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

    # Firmware panel: actions without the console
    con.send("7", 0.8)
    con.send("\r", 0.8)
    check("firmware panel takes focus on Enter", con.has("focus PANEL") and con.has("Check for update") and con.has("Reboot gateway"))
    con.send("\r", 0.8)
    check("firmware: check for update reports 'not available'", con.has("no update source"))
    con.send("\x1b[B", 0.6)
    con.send("\r", 0.8)
    check("firmware: Enter on Reboot asks first", con.has("Reboot the gateway now? [y/N]"))
    con.send("n", 0.8)
    check("firmware: n cancels, the board keeps running", not con.has("[y/N]") and not (b"\x1b[?1049l" in con.raw[-400:]))
    con.send("\x1b", 0.6)

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

    # IT-GCS-021..023 and the banner dots: link status against reality (WiFi joined, broker up,
    # no Field Device attached)
    con.send("clear\r", 0.6)
    con.send("wifi status\r", 0.5)
    wait_for_text(con, "rssi", 30.0)  # the link is still coming up for ~40 s after a reset
    m = re.search(r"rssi\s+(-\d+) dBm", con.text())
    check("IT-GCS-021 wifi status: connected with a plausible RSSI", con.has("● connected") and bool(m) and -95 <= int(m.group(1)) <= -20)
    con.send("clear\r", 0.6)
    con.send("mqtt status\r", 0.5)
    wait_for_text(con, ":8883", 5.0)
    for _ in range(12):  # TLS handshake can still be running just after boot
        if con.has("● connected"):
            break
        con.send("clear\r", 0.4)
        con.send("mqtt status\r", 2.0)
    check("IT-GCS-022 mqtt status: connected to the broker", con.has("● connected") and con.has(":8883"))
    con.send("clear\r", 0.6)
    con.send("modbus status\r", 1.2)
    check("IT-GCS-023 modbus status: offline (no Field Device)", con.has("● offline"))
    banner = con.screen.display[4]
    dots = [i for i, ch in enumerate(banner) if ch == "\u25cf"]
    cols = [con.cell(4, i).fg for i in dots]
    check("banner dots: wifi and mqtt green, modbus red", len(dots) == 3 and cols[0] == "6fbf8e" and cols[1] == "6fbf8e" and cols[2] == "cc6666", str(cols))
    con.send("clear\r", 0.6)
    con.send("selftest\r", 1.5)
    check("selftest comms fails only because Modbus is offline", con.has("sensors   PASS") and con.has("comms     FAIL"))

    # GCS-O7: paging
    con.send("clear\r", 0.6)
    con.send("help\r", 1.5)
    check("help tail is visible, header scrolled out", con.has("restart the gateway") and not con.has("Available commands"))
    for _ in range(8):
        con.send("\x1b[5~", 0.25)
    con.pump(0.5)
    check("GCS-O7 PgUp reveals the top of the help list", con.has("Available commands") and con.has("more"))
    for _ in range(8):
        con.send("\x1b[6~", 0.25)
    con.pump(0.5)
    check("GCS-O7 PgDn returns to the live view", con.has("restart the gateway") and not con.has("more"))

    # IT-GCS-028: the cursor blinks while the panel has focus
    seen = set()
    end = time.time() + 3.0
    while time.time() < end:
        con.pump(0.2)
        seen.add("\u2588" in "".join(con.screen.display[18]))
    check("IT-GCS-028 cursor blinks", seen == {True, False}, str(seen))

    # IT-GCS-025/026: a terminal that is too small, then big enough
    con.send("\x1b[8;24;80t", 1.5)
    check("IT-GCS-025 small terminal gets only the resize notice", con.has("Resize the terminal") and not con.has("MAIN MENU"))
    con.send("\x1b[8;40;120t", 2.5)
    check("IT-GCS-026 big enough again: the UI returns", con.has("MAIN MENU") and not con.has("Resize the terminal"))
    con.send("6\r", 0.8)

    # IT-GCS-015: provisioning commit, answered y, applied
    con.send("clear\r", 0.6)
    con.send("prov set mqtt-endpoint 192.168.0.218\r", 0.8)
    con.send("prov set modbus-addr 9\r", 0.8)
    con.send("prov commit\r", 1.0)
    check("IT-GCS-015 commit shows the staged values and asks", con.has("Apply? [y/N]") or con.has("modbus-addr = 9"))
    con.send("y", 3.0)
    check("IT-GCS-015 y applies provisioning", con.has("provisioning applied"))

    # IT-GCS-017: the prompt times out after 30 s and discards
    con.send("clear\r", 0.6)
    con.send("prov set modbus-addr 10\r", 0.8)
    con.send("prov commit\r", 1.0)
    check("IT-GCS-017 prompt is up", con.has("Apply? [y/N]"))
    con.pump(31.5)
    check("IT-GCS-017 prompt timed out and discarded", con.has("timed out") and not con.has("Apply? [y/N]"))

    # IT-GCS-016: n discards
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
    check("the console is back after the reboot", wait_for_menu(con))
    con.send("6", 0.8)
    con.send("\r", 0.8)
    con.send("thresholds\r", 0.5)
    check("persistence: t.hi 31.0 survived the reset", wait_for_text(con, "31.0"))
    show(con, "after reboot: thresholds")
    con.send("clear\r", 0.6)
    con.send("mqtt status\r", 0.5)
    check("IT-GCS-015 the committed broker survived the reset", wait_for_text(con, "192.168.0.218:8883"))

    # put the default back
    con.send("set t.hi 40\r", 0.5)
    check("restored t.hi 40", wait_for_text(con, "applied"))

    # reboot from the firmware panel, no console involved
    con.send("\x1b", 0.6)
    con.send("7", 0.8)
    con.send("\r", 0.8)
    con.send("\x1b[B", 0.6)
    con.send("\r", 0.8)
    mark = len(con.raw)
    con.send("y", 0.5)
    con.pump(4.0)
    check("firmware: y reboots the board from the panel", b"\x1b[?1049l" in con.raw[mark:])
    check("firmware: the console is back after the panel reboot", wait_for_menu(con))

    bad = [n for n, ok in results if not ok]
    print("\n%d checks, %d failed" % (len(results), len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
