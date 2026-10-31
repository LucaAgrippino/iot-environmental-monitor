#!/usr/bin/env python3
"""Passive timeline of the two-board test: the Modbus bus and the MQTT alarm topic on ONE clock.

Why one tool: REQ-NF-113 (alarm within 500 ms) brackets the Field Device setting the alarm flag to the
broker's PUBACK (sequence-diagrams.md SD-05). Neither board logs that with millisecond resolution, so this
tool puts both observable ends on the laptop's clock:

  [BUS]  every Modbus RTU frame seen by a USB-RS-485 dongle that only LISTENS (never transmits), with its
         CRC checked, and an ALARM_FLAGS change when a read of the FD input register 0x0031 shows one;
  [MQTT] every message on the alarm topic, stamped when the laptop's subscriber receives it. The broker
         sends the PUBACK to the Gateway at the same moment, so this is the end of the budget.

When an ALARM_FLAGS change is followed by an alarm message it prints the time between them. That is the
part of the budget the Gateway controls; the Field Device's own detection-to-flag time and the poll period
before the first read are NOT visible here (the Field Device logs nothing when it raises an alarm).

usage:  two_board_monitor.py --serial /dev/ttyUSB0 [--baud 9600]
                             [--mqtt-host H --cafile CA --cert OPERATOR.crt --key OPERATOR.key]
                             [--topic 'dt/iotmonitor/+/alarms']
        two_board_monitor.py --selftest
Needs pyserial; paho-mqtt only for the MQTT half.
"""

import argparse
import struct
import sys
import threading
import time

ALARM_FLAGS_REG = 0x0031  # FD input register (modbus-register-map.md 6.2)
FRAME_GAP_S = 0.006       # a pause longer than this ends a frame (3.5 characters is 3.65 ms at 9600)


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def add_crc(frame):
    return frame + struct.pack("<H", crc16(frame))


class FrameDecoder:
    """Turns complete frames into text lines and ALARM_FLAGS change events."""

    def __init__(self):
        self.last_request = None   # (slave, function, start, count)
        self.alarm_flags = None

    def decode(self, frame, t):
        """Return (lines, events). events are ('alarm_flags', old, new, t)."""
        if len(frame) < 4:
            return ["short frame %s" % frame.hex(" ")], []
        if crc16(frame[:-2]) != struct.unpack("<H", frame[-2:])[0]:
            return ["BAD CRC  %s" % frame.hex(" ")], []
        slave, fc, body = frame[0], frame[1], frame[2:-2]
        if fc & 0x80:
            return ["slave %d exception fc=0x%02X code=0x%02X" % (slave, fc & 0x7F, body[0])], []
        if fc in (3, 4) and len(frame) == 8:                      # request: start, count
            start, count = struct.unpack(">HH", body)
            self.last_request = (slave, fc, start, count)
            return ["slave %d request FC%02d  start=0x%04X count=%d" % (slave, fc, start, count)], []
        if fc in (3, 4) and body and len(body) == 1 + body[0]:    # response to the last request
            data = body[1:]
            regs = [struct.unpack(">H", data[i:i + 2])[0] for i in range(0, len(data) - 1, 2)]
            events = []
            text = "slave %d response FC%02d  %s" % (slave, fc, " ".join("%04X" % r for r in regs))
            req = self.last_request
            if req and req[0] == slave and req[1] == 4 and req[2] <= ALARM_FLAGS_REG < req[2] + len(regs):
                flags = regs[ALARM_FLAGS_REG - req[2]]
                text += "   ALARM_FLAGS=0x%04X" % flags
                if self.alarm_flags is not None and flags != self.alarm_flags:
                    events.append(("alarm_flags", self.alarm_flags, flags, t))
                self.alarm_flags = flags
            return [text], events
        if fc == 6 and len(frame) == 8:
            reg, val = struct.unpack(">HH", body)
            return ["slave %d FC06 write  reg=0x%04X value=0x%04X" % (slave, reg, val)], []
        if fc == 16:
            if len(frame) == 8:                                   # response: start, count
                start, count = struct.unpack(">HH", body)
                return ["slave %d FC16 response  start=0x%04X count=%d" % (slave, start, count)], []
            start, count = struct.unpack(">HH", body[:4])
            vals = [struct.unpack(">H", body[5 + i:7 + i])[0] for i in range(0, body[4] - 1, 2)]
            return ["slave %d FC16 write  start=0x%04X count=%d values=%s" % (
                slave, start, count, " ".join("%04X" % v for v in vals))], []
        return ["slave %d fc=0x%02X %s" % (slave, fc, body.hex(" "))], []


class Monitor:
    def __init__(self, out=print):
        self.dec = FrameDecoder()
        self.out = out
        self.flag_change_t = None
        self.lock = threading.Lock()

    @staticmethod
    def stamp(t):
        return time.strftime("%H:%M:%S", time.localtime(t)) + ".%03d" % int((t % 1) * 1000)

    def bus_frame(self, frame, t):
        lines, events = self.dec.decode(frame, t)
        with self.lock:
            for line in lines:
                self.out("%s [BUS]  %s" % (self.stamp(t), line))
            for _, old, new, when in events:
                self.out("%s [BUS]  >>> ALARM_FLAGS 0x%04X -> 0x%04X (first read showing it)" % (self.stamp(when), old, new))
                if new != 0:
                    self.flag_change_t = when

    def mqtt_message(self, topic, payload, t):
        with self.lock:
            self.out("%s [MQTT] %s  %s" % (self.stamp(t), topic, payload[:200]))
            if self.flag_change_t is not None:
                self.out("%s [MQTT] >>> %.0f ms from the bus read that showed the alarm to the broker receiving it"
                         " (REQ-NF-113 budget is 500 ms from the FD setting the flag)" % (
                             self.stamp(t), (t - self.flag_change_t) * 1000.0))
                self.flag_change_t = None


def run_serial(mon, port, baud):
    import serial
    ser = serial.Serial(port, baud, timeout=0.001)
    buf, last = bytearray(), 0.0
    while True:
        data = ser.read(256)
        now = time.time()
        if data:
            buf += data
            last = now
        elif buf and now - last > FRAME_GAP_S:
            mon.bus_frame(bytes(buf), last)
            buf.clear()


def run_mqtt(mon, args):
    import ssl
    import paho.mqtt.client as mqtt
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="two-board-monitor")
    except AttributeError:  # paho 1.x
        client = mqtt.Client(client_id="two-board-monitor")
    client.tls_set(ca_certs=args.cafile, certfile=args.cert, keyfile=args.key, tls_version=ssl.PROTOCOL_TLS_CLIENT)
    if args.no_hostname_check:
        client.tls_insecure_set(True)
    client.on_connect = lambda c, *a: c.subscribe(args.topic, 1)
    client.on_message = lambda c, u, m: mon.mqtt_message(m.topic, m.payload.decode("latin-1"), time.time())
    client.connect(args.mqtt_host, args.port, keepalive=30)
    client.loop_start()


def selftest():
    lines = []
    mon = Monitor(out=lines.append)
    req = add_crc(bytes([1, 4]) + struct.pack(">HH", 0x0030, 2))
    quiet = add_crc(bytes([1, 4, 4]) + struct.pack(">HH", 1, 0x0000))
    alarm = add_crc(bytes([1, 4, 4]) + struct.pack(">HH", 1, 0x0001))
    t0 = 1000.0
    for frame, t in ((req, t0), (quiet, t0 + 0.02), (req, t0 + 1.0), (alarm, t0 + 1.02)):
        mon.bus_frame(frame, t)
    assert any("ALARM_FLAGS 0x0000 -> 0x0001" in ln for ln in lines), lines
    mon.mqtt_message("dt/iotmonitor/x/alarms", "{}", t0 + 1.02 + 0.180)
    assert any(" 180 ms" in ln for ln in lines), lines
    bad = bytearray(alarm)
    bad[-1] ^= 1
    assert "BAD CRC" in FrameDecoder().decode(bytes(bad), 0)[0][0]
    write = add_crc(bytes([1, 16]) + struct.pack(">HHB", 0x0210, 2, 4) + struct.pack(">HH", 0x6B00, 0x1234))
    assert "values=6B00 1234" in FrameDecoder().decode(write, 0)[0][0]
    exc = add_crc(bytes([1, 0x90, 3]))
    assert "exception" in FrameDecoder().decode(exc, 0)[0][0]
    print("selftest ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--serial", help="the USB-RS-485 dongle, listen only")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--mqtt-host")
    ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--cafile")
    ap.add_argument("--cert")
    ap.add_argument("--key")
    ap.add_argument("--topic", default="dt/iotmonitor/+/alarms")
    ap.add_argument("--no-hostname-check", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        selftest()
        return 0
    if not args.serial:
        ap.error("--serial is required")
    mon = Monitor()
    if args.mqtt_host:
        run_mqtt(mon, args)
    print("listening on %s at %d baud (never transmitting)" % (args.serial, args.baud), flush=True)
    try:
        run_serial(mon, args.serial, args.baud)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
