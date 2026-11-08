"""Robot library: sample both boards every N seconds during an endurance run and judge the samples."""
import csv
import os
import re
import time

from ConsoleLib import ConsoleLib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FD_PORT = "/dev/serial/by-id/usb-STMicroelectronics_STM32_STLink_066BFF303435554157025015-if02"
GW_PORT = "/dev/serial/by-id/usb-STMicroelectronics_STM32_STLink_0670FF575157877567154553-if02"


def _num(pattern, text):
    m = re.search(pattern, " ".join(text.split()))
    return int(m.group(1)) if m else -1


def _uptime_s(text):
    t = " ".join(text.split())
    m = re.search(r"Uptime (\d+)d (\d+):(\d+):(\d+)", t)          # Gateway
    if m:
        d, h, mi, s = map(int, m.groups())
        return d * 86400 + h * 3600 + mi * 60 + s
    return _num(r"Uptime: (\d+) s", t)                           # Field Device


class Soak:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self):
        self.rows = []

    def _sample_board(self, board, port, command, retries=3):
        c = ConsoleLib()
        c.open_console(board, port)
        try:
            for _ in range(retries):
                text = c.send_command(command, 2.5)
                if _uptime_s(text) >= 0:
                    return text
        finally:
            c.close_console()
        return ""

    def run_soak(self, seconds, interval, csv_path):
        """Sample both boards every `interval` seconds for `seconds`; write the samples to a CSV file."""
        end = time.time() + float(seconds)
        self.rows = []
        with open(csv_path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["time", "gw_uptime", "gw_modbus_ok", "gw_modbus_timeouts", "gw_mqtt_sent", "gw_mqtt_failed",
                        "gw_buffer", "fd_uptime", "fd_frames", "fd_crc_errors", "fd_sensor_failures"])
            while True:
                gw = self._sample_board("GW", GW_PORT, "status")
                fd = self._sample_board("FD", FD_PORT, "status")
                row = [int(time.time()), _uptime_s(gw), _num(r"Modbus \S+ \w+ (\d+) ok", gw), _num(r"/ (\d+) timeouts", gw),
                       _num(r"MQTT \S+ \w+ (\d+) sent", gw), _num(r"/ (\d+) failed", gw), _num(r"Buffer (\d+) entries", gw),
                       _uptime_s(fd), _num(r"Modbus frames: (\d+)", fd), _num(r"Modbus CRC errors: (\d+)", fd),
                       _num(r"Sensor failures: (\d+)", fd)]
                self.rows.append(row)
                w.writerow(row)
                f.flush()
                if time.time() >= end:
                    break
                time.sleep(max(0.0, float(interval) - 12))   # the two samples take about 12 s

    def soak_resets(self):
        """Number of times an uptime went backwards: (gateway resets, field device resets)."""
        def resets(col):
            vals = [r[col] for r in self.rows if r[col] >= 0]
            return sum(1 for a, b in zip(vals, vals[1:]) if b < a)
        return resets(1), resets(7)

    def soak_summary(self):
        ok = [r for r in self.rows if r[1] >= 0 and r[7] >= 0]
        if len(ok) < 2:
            raise AssertionError("fewer than two complete samples")
        a, b = ok[0], ok[-1]
        d = lambda i: b[i] - a[i]
        return dict(samples=len(self.rows), seconds=b[0] - a[0], gw_ok=d(2), gw_timeouts=d(3), mqtt_sent=d(4),
                    mqtt_failed=d(5), fd_frames=d(8), fd_crc=d(9), fd_sensor_failures=d(10), buffer_end=b[6])
