"""Robot library: listen to a second serial console in the background while a case works on the first one.

ConsoleLib drives one console (the Gateway's); this one only records, so a case can read the Field Device log while
it acts on the Gateway or the broker."""
import os
import re
import termios
import threading
import time

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
_rate = {115200: termios.B115200, 460800: termios.B460800}


class PortCapture:
    ROBOT_LIBRARY_SCOPE = "TEST"

    def __init__(self):
        self._fd = None
        self._text = ""
        self._marks = []   # (offset in _text, time.time()) at the start of every chunk read
        self._run = False
        self._thread = None

    def start_port_capture(self, port, baud=115200):
        """Start recording what the port prints (kept from now on)."""
        self.stop_port_capture()
        self._fd = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self._fd)
        a[0] = a[1] = a[3] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[4] = a[5] = _rate[int(baud)]
        termios.tcsetattr(self._fd, termios.TCSANOW, a)
        self._text = ""
        self._marks = []
        self._run = True
        self._thread = threading.Thread(target=self._pump, daemon=True)
        self._thread.start()

    def _pump(self):
        while self._run:
            try:
                data = os.read(self._fd, 4096)
            except BlockingIOError:
                data = b""
            if data:
                self._marks.append((len(self._text), time.time()))
                self._text += ANSI.sub("", data.decode("latin1"))
            else:
                time.sleep(0.05)

    def port_capture_should_contain(self, text, timeout=0.0):
        """Wait up to `timeout` seconds for `text` to appear in what was recorded."""
        end = time.time() + float(timeout)
        while True:
            if text in self._text:
                return
            if time.time() >= end:
                raise AssertionError("%r not seen on the port; last 400 characters: %r" % (text, self._text[-400:]))
            time.sleep(0.2)

    def port_capture_time_of(self, text, after=0.0):
        """Time (seconds since the epoch, to the chunk the text arrived in) of the first occurrence of `text` seen at or after the
        time `after`. Fails when it is not in the capture."""
        start = 0
        for off, t in self._marks:
            if t >= float(after):
                start = off
                break
        else:
            raise AssertionError("nothing was captured after %s" % after)
        i = self._text.find(text, start)
        if i < 0:
            raise AssertionError("%r not in the capture; last 300 characters: %r" % (text, self._text[-300:]))
        stamp = self._marks[0][1]
        for off, t in self._marks:
            if off <= i:
                stamp = t
            else:
                break
        return stamp

    def get_port_capture(self):
        return self._text

    def stop_port_capture(self):
        self._run = False
        if self._thread is not None:
            self._thread.join(2)
            self._thread = None
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None
