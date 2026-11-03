#!/usr/bin/env python3
"""REQ-NF-113 alarm latency on the two-board bench, both halves in one run.

Raises and clears a Field Device temperature alarm N times from the Field Device console (lowering
`temp-alarm-high`, then restoring it) and measures, on the clock of the machine that runs
scripts/two_board_monitor.py (the Raspberry Pi with the RS-485 dongle):

  gap      Field Device flag set -> first Modbus read that shows it. The Field Device logs
           `alarm raised ...` when it sets the flag; this machine timestamps that line on arrival,
           so the figure is slightly LOW by the log's own delay (a few ms to tens of ms).
  publish  that first bus read -> the alarm message reaching the broker (two_board_monitor.py).
           Only an alarm of the right kind (HIGH/LOW for a raise, CLEAR for a clear) with a
           timestamp within 10 s of the read ends a measurement, so alarms replayed from the
           Gateway's store-and-forward ring do not count.

Before a run, let the Gateway finish any replay (watch the broker: no alarm PUBLISH for 30 s) and
power-cycle it if the Wi-Fi module has been up for hours: both change the numbers a lot.

usage:  alarm_latency_bench.py --cycles 10 --out DIR
        [--fd-console /dev/ttyACM0] [--pi luca@192.168.0.209] [--broker 192.168.0.218]
        [--pi-dir bench] [--settle 15]
Needs pyserial here; the Pi needs two_board_monitor.py, certs/ and a venv with pyserial + paho-mqtt
under <pi-dir> (see docs/test/two-board-integration.md).
"""

import argparse
import os
import re
import statistics
import subprocess
import sys
import time


def pi_clock(pi):
    """(offset, tz) of the Pi: its clock minus ours in seconds, and its UTC offset in seconds.

    One persistent ssh session, so the round trip is a few ms and the best of 40 samples is good to
    about +-4 ms (a fresh ssh per sample costs hundreds of ms and is useless)."""
    tz = subprocess.run(["ssh", "-o", "BatchMode=yes", pi, "date +%z"], capture_output=True, text=True).stdout.strip()
    tz_s = (1 if tz[0] == "+" else -1) * (int(tz[1:3]) * 3600 + int(tz[3:5]) * 60)
    proc = subprocess.Popen(
        ["ssh", "-o", "BatchMode=yes", pi, "python3 -u -c \"import sys,time\nfor l in sys.stdin: print(time.time())\""],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    time.sleep(1.5)
    best = None
    for _ in range(40):
        t0 = time.time()
        proc.stdin.write("x\n")
        proc.stdin.flush()
        v = float(proc.stdout.readline())
        t1 = time.time()
        if best is None or t1 - t0 < best[0]:
            best = (t1 - t0, v - (t0 + t1) / 2)
        time.sleep(0.05)
    proc.terminate()
    return best[1], tz_s


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cycles", type=int, default=10)
    ap.add_argument("--out", required=True, help="directory for the raw monitor log")
    ap.add_argument("--fd-console", default="/dev/ttyACM0")
    ap.add_argument("--pi", default="luca@192.168.0.209")
    ap.add_argument("--broker", default="192.168.0.218")
    ap.add_argument("--pi-dir", default="bench")
    ap.add_argument("--settle", type=float, default=15.0)
    args = ap.parse_args()

    import serial
    os.makedirs(args.out, exist_ok=True)
    offset, tz_s = pi_clock(args.pi)
    print("Pi clock is %+.0f ms from ours, UTC%+d h" % (offset * 1000, tz_s // 3600), flush=True)

    d = args.pi_dir
    remote = ("timeout %d ~/pymodbus-venv/bin/python -u ~/%s/two_board_monitor.py --serial /dev/ttyUSB0 "
              "--mqtt-host %s --cafile ~/%s/certs/ca.crt --cert ~/%s/certs/operator.crt "
              "--key ~/%s/certs/operator.key --topic 'dt/iotmonitor/#'") % (
                  int(args.cycles * 23 + args.settle + 30), args.broker, d, d, d, d)
    mon = subprocess.Popen(["ssh", "-o", "BatchMode=yes", args.pi, remote], stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, text=True)

    ser = serial.Serial(args.fd_console, 115200, timeout=0.01)
    raised, buf = [], b""

    def pump(sec):
        nonlocal buf
        end = time.time() + sec
        while time.time() < end:
            data = ser.read(512)
            if data:
                now = time.time()
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if b"alarm raised" in line:
                        raised.append(now)

    def send(c):
        ser.write((c + "\r\n").encode())

    pump(args.settle)
    for i in range(args.cycles):
        for limit, hold in ((500, 9 + 0.7 * (i % 3)), (4000, 9)):
            send("config set temp-alarm-high %d" % limit)
            pump(0.6)
            send("config commit")
            pump(0.6)
            send("y")  # the console asks "Apply? [y/N]"
            pump(hold)
    out = mon.communicate()[0]
    with open(os.path.join(args.out, "monitor.log"), "w") as f:
        f.write(out)

    # --- publish: bus read -> broker, as printed by two_board_monitor.py ---------------------
    raise_ms, clear_ms = [], []
    for line in out.splitlines():
        m = re.search(r">>> (\d+) ms from the bus read that showed the (raise|clear)", line)
        if m:
            (raise_ms if m.group(2) == "raise" else clear_ms).append(int(m.group(1)))
    changes = len(re.findall(r">>> ALARM_FLAGS", out))

    # --- gap: Field Device log line -> first bus read that shows the temperature-high bit -----
    flags = []
    for line in out.splitlines():
        m = re.match(r"(\d\d):(\d\d):(\d\d)\.(\d\d\d) \[BUS\]  >>> ALARM_FLAGS 0x(\w+) -> 0x(\w+)", line)
        if m:
            h, mi, se, ms = (int(x) for x in m.groups()[:4])
            if int(m.group(6), 16) & ~int(m.group(5), 16) & 0x0002:
                flags.append(h * 3600 + mi * 60 + se + ms / 1000.0)
    gaps = []
    for t in raised:
        sod = (t + tz_s) % 86400 + offset
        nxt = [f - sod for f in flags if -0.05 <= f - sod < 2.0]
        if nxt:
            gaps.append(round(nxt[0] * 1000))

    def stats(name, v):
        if v:
            s = sorted(v)
            print("%-8s n=%-2d median=%-5d max=%-5d  within 500 ms: %d/%d   %s" % (
                name, len(s), int(statistics.median(s)), s[-1], sum(x <= 500 for x in s), len(s), v))
        else:
            print("%-8s no samples" % name)

    print("flag changes on the bus: %d (expected %d); alarms paired: %d" % (
        changes, 2 * args.cycles, len(raise_ms) + len(clear_ms)))
    stats("raise", raise_ms)
    stats("clear", clear_ms)
    stats("gap", gaps)
    return 0


if __name__ == "__main__":
    sys.exit(main())
