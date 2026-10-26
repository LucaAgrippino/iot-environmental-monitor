#!/usr/bin/env python3
"""Measure how quickly the console TUI answers a key, over the real serial port.

For each case it types keys and reports (a) the time until the menu highlight is on the right item
and (b) the time until the panel has finished drawing (the line goes quiet). Needs `pyte`.

    python3 scripts/console_latency.py [/dev/ttyACM0] [baud]
"""

import os
import select
import sys
import termios
import time

import pyte

sys.path.insert(0, os.path.dirname(__file__))
import console_bench as cb  # noqa: E402



def timed(con, keys, want_row, want_title, quiet=0.12, limit=6.0):
    """Send keys; return (seconds until the highlight shows on menu row `want_row`, seconds until
    `want_title` is on screen, seconds until the stream goes quiet, bytes received)."""
    con.pump(0.5)
    before = len(con.raw)
    os.write(con.fd, keys.encode())
    t0 = time.time()
    t_hl = t_title = None
    last = t0
    while time.time() - t0 < limit:
        ready, _, _ = select.select([con.fd], [], [], 0.01)
        if ready:
            data = os.read(con.fd, 4096)
            con.raw += data
            con.stream.feed(con.decoder.decode(data))
            last = time.time()
            if t_hl is None and "▸" in con.screen.display[want_row][:30]:
                t_hl = last - t0
            if t_title is None and want_title in con.text():
                t_title = last - t0
        elif time.time() - last > quiet and (t_hl is not None or time.time() - t0 > 1.0):
            break
    return t_hl, t_title, last - t0, len(con.raw) - before


def fmt(v):
    return "  n/a " if v is None else "%5.0f ms" % (v * 1000)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    con = cb.Console(cb.open_port(port))
    con.send("\x0c", 2.5)
    con.send("\x1b", 1.0)
    con.send("\x1b", 1.0)
    con.send("1", 2.0)

    # Menu rows on screen: item i is on row 11 + i.
    cases = [
        ("one step: Graph -> System status", "\x1b[B", 12, "SYSTEM STATUS"),
        ("one step: System status -> Alarms", "\x1b[B", 13, "ACTIVE ALARMS"),
        ("jump: 7 (Firmware)", "7", 17, "FIRMWARE"),
        ("one step: wrap Firmware -> Graph", "\x1b[B", 11, "LIVE"),
        ("held key: 6 steps at once", "\x1b[B" * 6, 17, "FIRMWARE"),
        ("held key: 6 steps back up", "\x1b[A" * 6, 11, "LIVE"),
        ("jump: 4 (Thresholds)", "4", 14, "THRESHOLDS"),
        ("jump: 1 (Graph)", "1", 11, "LIVE"),
    ]
    print("%-36s %10s %10s %10s %7s" % ("case", "highlight", "panel", "quiet", "bytes"))
    for name, keys, row, title in cases:
        hl, tt, quiet, n = timed(con, keys, row, title)
        print("%-36s %s %s %s %7d" % (name, fmt(hl), fmt(tt), fmt(quiet), n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
