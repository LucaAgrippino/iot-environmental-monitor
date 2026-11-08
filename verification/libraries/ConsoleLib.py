"""Robot library: the serial consoles. The Field Device console is a plain line CLI (115200), the Gateway
console a three-panel screen (460800): both are read as text with the escape codes removed, and searches ignore
white space because the Gateway repaints between characters.
"""
import os
import re
import termios
import time

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Z0-9]|\x1b[=>]")
BAUD = {"FD": 115200, "GW": 460800}
_rate = {115200: termios.B115200, 460800: termios.B460800}


class ConsoleLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self):
        self.fd = None
        self.buf = ""
        self.board = None

    def open_console(self, board, port):
        """Open the console of FD or GW."""
        self.close_console()
        self.board = board.upper()
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = a[3] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[4] = a[5] = _rate[BAUD[self.board]]
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        self.buf = ""
        self._pump(0.3)

    def close_console(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def _pump(self, seconds):
        end = time.time() + float(seconds)
        while time.time() < end:
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                data = b""
            if data:
                self.buf += ANSI.sub("", data.decode("latin1"))
            else:
                time.sleep(0.02)

    def send_command(self, line, wait=1.0):
        """Send one command line and collect the answer for `wait` seconds; returns the text."""
        self.buf = ""
        os.write(self.fd, (line + "\r").encode())
        self._pump(wait)
        return self.buf

    def console_should_contain(self, text, timeout=0.0):
        """Fail unless the text (white space ignored) appears in what was collected, waiting up to `timeout`."""
        end = time.time() + float(timeout)
        while True:
            if "".join(text.split()) in "".join(self.buf.split()):
                return
            if time.time() >= end:
                raise AssertionError("console did not show %r; last output: %r" % (text, " ".join(self.buf.split())[-300:]))
            self._pump(0.2)

    def console_should_not_contain(self, text):
        if "".join(text.split()) in "".join(self.buf.split()):
            raise AssertionError("console shows %r" % text)

    def command_should_answer(self, line, expect, wait=1.5, tries=3):
        """Send a command and require `expect` in the answer; the Gateway console drops input while the board
        is busy, so the command is repeated a few times (only use it for idempotent commands)."""
        for _ in range(int(tries)):
            self.send_command(line, wait)
            if "".join(expect.split()) in "".join(self.buf.split()):
                return self.buf
        raise AssertionError("%r did not answer %r: %r" % (line, expect, " ".join(self.buf.split())[-300:]))

    def console_value(self, pattern):
        """First group of a regular expression in the collected text (white space normalised to single spaces)."""
        m = re.search(pattern, " ".join(self.buf.split()))
        if not m:
            raise AssertionError("pattern %r not found in %r" % (pattern, " ".join(self.buf.split())[-300:]))
        return m.group(1)

    def get_console_text(self):
        return self.buf

    def wait_for_boot(self, seconds=45):
        """Wait until the console answers (the Gateway console is silent for a while after a reset)."""
        end = time.time() + float(seconds)
        while time.time() < end:
            self.send_command("", 0.8)
            if self.board == "GW" and "envmon>" in self.buf:
                return
            if self.board == "FD" and ("fd>" in self.buf or "help" in self.buf.lower()):
                return
            if self.board == "FD":
                # the image without the display prints no prompt for an empty line: ask for something
                self.send_command("help", 0.8)
                if "Available commands" in self.buf:
                    return
        raise AssertionError("%s console did not come up" % self.board)
