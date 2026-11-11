"""Robot library: a passive listener on the RS-485 line (the Raspberry Pi's CH340 dongle, wired in parallel with the two boards).

It only listens, so the Gateway and the Field Device keep talking. Frames are split on a silence of 4 ms (3.5 characters at 9600 baud);
the CRC-16 of each frame is checked, and a frame with a bad CRC is kept but marked.
"""
import os
import time

import serial

PORT = os.environ.get("VERIFY_RS485_PORT", "/dev/ttyUSB0")
BAUD = int(os.environ.get("VERIFY_RS485_BAUD", "9600"))


def _crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


class Rs485Probe:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self):
        self.frames = []   # dicts: t (s from capture start), data (bytes), crc_ok (bool)

    def start_capture_bus(self, seconds):
        """Listen in a background thread (so the test can act on the boards meanwhile); read the result with `wait_capture_bus`."""
        import threading
        self._t = threading.Thread(target=self.capture_bus, args=(seconds,), daemon=True)
        self._t.start()

    def wait_capture_bus(self):
        self._t.join()
        return len(self.frames)

    def capture_bus(self, seconds):
        """Listen for `seconds`; the frames are kept for the keywords below. Returns the number of frames."""
        gap = 0.004
        port = serial.Serial(PORT, BAUD, timeout=0.001)
        port.reset_input_buffer()
        start = time.monotonic()
        end = start + float(seconds)
        cur, last = bytearray(), None
        self.frames = []

        def flush():
            if len(cur) >= 4:
                data = bytes(cur)
                self.frames.append(dict(t=first - start, data=data, crc_ok=_crc16(data[:-2]) == (data[-2] | data[-1] << 8)))
            cur.clear()

        first = 0.0
        while time.monotonic() < end:
            b = port.read(64)
            now = time.monotonic()
            if b:
                if last is not None and now - last > gap and cur:
                    flush()
                if not cur:
                    first = now
                cur.extend(b)
                last = now
            elif cur and last is not None and now - last > gap:
                flush()
        flush()
        port.close()
        return len(self.frames)

    def frame_count(self):
        return len(self.frames)

    def function_codes_seen(self, slave=1):
        """Sorted function codes of the valid requests and replies of a slave on the line (exception replies have bit 7 set)."""
        return sorted({f["data"][1] & 0x7F for f in self.frames if f["crc_ok"] and f["data"][0] == int(slave)})

    def request_times(self, slave=1, function=4):
        """Start times of the frames of one slave and function code that look like a request (8 bytes, valid CRC)."""
        return [(f["t"], f["data"]) for f in self.frames
                if f["crc_ok"] and len(f["data"]) == 8 and f["data"][0] == int(slave) and f["data"][1] == int(function)]

    def reply_count(self, slave=1):
        """Valid frames of the slave that are not 8-byte requests (replies)."""
        return sum(1 for f in self.frames if f["crc_ok"] and f["data"][0] == int(slave) and len(f["data"]) != 8)

    def retry_groups(self, slave=1, function=4, max_gap=0.4):
        """Groups of identical requests sent one after the other with less than `max_gap` s between them.
        Returns a list of (count, median interval in seconds) per group."""
        by_data = {}
        for t, d in self.request_times(slave, function):
            by_data.setdefault(d, []).append(t)
        out = []
        for ts in by_data.values():
            group = [ts[0]]
            for t in ts[1:] + [None]:
                if t is not None and t - group[-1] < float(max_gap):
                    group.append(t)
                    continue
                iv = sorted(b - a for a, b in zip(group, group[1:]))
                out.append((len(group), iv[len(iv) // 2] if iv else 0.0))
                group = [t] if t is not None else []
        return out

    def dominant_request_spacing(self, slave=1):
        """Median spacing (s) of the most frequent request string of the slave: how often the master repeats it."""
        by = {}
        for f in self.frames:
            if f["crc_ok"] and len(f["data"]) == 8 and f["data"][0] == int(slave):
                by.setdefault(f["data"], []).append(f["t"])
        if not by:
            raise AssertionError("no request to slave %s seen" % slave)
        ts = max(by.values(), key=len)
        iv = sorted(b - a for a, b in zip(ts, ts[1:]))
        return iv[len(iv) // 2]

    def request_bursts(self, slave=1, span=1.0):
        """For each request string of the slave: runs of occurrences that follow each other within 0.4 s and span at most `span` s.
        Returns (count, duration) per run, for every string except the most frequent one (the regular poll)."""
        by = {}
        for f in self.frames:
            if f["crc_ok"] and len(f["data"]) == 8 and f["data"][0] == int(slave):
                by.setdefault(f["data"], []).append(f["t"])
        if len(by) < 2:
            return []
        regular = max(by, key=lambda k: len(by[k]))
        out = []
        for k, ts in by.items():
            if k == regular:
                continue
            run = [ts[0]]
            for t in ts[1:] + [None]:
                if t is not None and t - run[-1] < 0.4:
                    run.append(t)
                    continue
                out.append((len(run), round(run[-1] - run[0], 2)))
                run = [t] if t is not None else []
        return out
