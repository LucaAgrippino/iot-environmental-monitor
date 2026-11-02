#!/usr/bin/env python3
"""Probe the Field Device's Modbus slave from a Raspberry Pi with a USB-RS485 dongle.

Needs only pyserial (sudo apt install python3-serial). Use it with the GATEWAY UNPLUGGED from the
bus, so the dongle is the only master: A and B of the dongle go to A and B of the Field Device's
RS-485 module (and GND to GND if the dongle has a GND terminal).

    python3 rpi_rs485_probe.py probe  [/dev/ttyUSB0]   ask slave 1 for registers 0x0000..0x0007
                                                       and 0x0010..0x0012; prints the reply bytes
    python3 rpi_rs485_probe.py listen [/dev/ttyUSB0]   print every byte seen on the bus (use it
                                                       with the Gateway connected and polling)

Expected from a working Field Device: 'probe' prints 8 register values (register 0 = 1, register 7 =
6725 = 0x1A45) and then temperature, humidity, pressure. In 'listen' mode the Gateway's request is
`01 04 00 10 00 03 ...` repeated about every 200 ms in bursts, and the answer starts `01 04 06`.
"""

import sys
import time

import serial

BAUD = 9600


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def request(slave, fc, start, count):
    body = bytes([slave, fc, start >> 8, start & 0xFF, count >> 8, count & 0xFF])
    crc = crc16(body)
    return body + bytes([crc & 0xFF, crc >> 8])


def hexs(b):
    return " ".join("%02X" % x for x in b) if b else "(nothing)"


def probe(port):
    for start, count in ((0x0000, 8), (0x0010, 3)):
        req = request(1, 4, start, count)
        port.reset_input_buffer()
        port.write(req)
        port.flush()
        time.sleep(0.4)
        rsp = port.read(256)
        print("request  %s" % hexs(req))
        print("response %s" % hexs(rsp))
        if len(rsp) >= 5 and rsp[0] == 1 and rsp[1] == 4 and crc16(rsp[:-2]) == (rsp[-2] | rsp[-1] << 8):
            vals = [rsp[3 + 2 * i] << 8 | rsp[4 + 2 * i] for i in range(rsp[2] // 2)]
            print("  -> OK, registers from 0x%04X: %s" % (start, vals))
        elif rsp:
            print("  -> bytes came back but not a valid reply (CRC or framing): wrong baud or noise")
        else:
            print("  -> NO REPLY")
        print()


def listen(port):
    print("listening, Ctrl-C to stop")
    last = time.time()
    line = bytearray()
    while True:
        b = port.read(1)
        now = time.time()
        if b:
            if now - last > 0.02 and line:
                print("%.3f  %s" % (last, hexs(line)))
                line.clear()
            line += b
            last = now
        elif line and now - last > 0.02:
            print("%.3f  %s" % (last, hexs(line)))
            line.clear()


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "probe"
    path = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyUSB0"
    # Open with DTR and RTS low: pyserial raises both by default, and some USB-RS485 dongles use RTS
    # to enable their transmitter, which would hold the bus driven while the port is open.
    port = serial.Serial(None, BAUD, bytesize=8, parity="N", stopbits=1, timeout=0.05)
    port.port = path
    port.dtr = False
    port.rts = False
    port.open()
    port.dtr = False
    port.rts = False
    (probe if mode == "probe" else listen)(port)


if __name__ == "__main__":
    main()
