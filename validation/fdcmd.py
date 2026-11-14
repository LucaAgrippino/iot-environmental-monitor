#!/usr/bin/env python3
"""Send console lines to the Field Device and print what comes back (validation helper, runs on the Raspberry Pi).

usage: fdcmd.py [--wait S] "line 1" "line 2" ...   (a line "@3" waits 3 s; the port is /dev/serial/by-id/*066BFF30*-if02)
"""
import glob, os, re, sys, termios, time, select

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")


def main(argv):
    wait = 1.5
    if argv and argv[0] == "--wait":
        wait, argv = float(argv[1]), argv[2:]
    port = glob.glob("/dev/serial/by-id/*066BFF30*-if02")[0]
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)

    def drain(sec):
        out, end = b"", time.time() + sec
        while time.time() < end:
            if select.select([fd], [], [], 0.1)[0]:
                out += os.read(fd, 4096)
        return ANSI.sub(b"", out).decode(errors="replace").replace("\r", "")

    drain(0.3)
    for line in argv:
        if line.startswith("@"):
            time.sleep(float(line[1:]))
            continue
        os.write(fd, (line + "\r\n").encode())
        print("> " + line)
        print(drain(wait).rstrip())
    os.close(fd)


main(sys.argv[1:])
