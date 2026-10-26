#!/usr/bin/env python3
"""Stream a signed OTA image to the FirmwareStore bring-up harness over the debug UART.

Counterpart of firmware/gateway/integration-tests/firmware_store/main_test_firmware_store.c.
Protocol (one command per line, then raw bytes):

  host -> "OTA <image_id hex> <file_len> <install 0|1>"
  dev  -> "NEED <offset>"     where to start (0, or the resume point after an interruption)
  host -> raw bytes from <offset> in 1024-byte blocks; the device answers each with '.'
  dev  -> "RECEIVED ...", "VERIFY ...", and with --install "INSTALL ..." / "ACTIVATE ..."

Commands run at 115 200 baud; the image itself is sent at 921 600 and the line is
switched back afterwards.

Test selection (docs/lld/middleware/firmware-store.md section 23):
  TC-HW-FS-002  --image <max-size Slot-B image>
  TC-HW-FS-003  --stop-after N, or pull the power during the transfer, then run again
  TC-HW-FS-004  any run (the VERIFY line carries the duration)
  TC-HW-FS-005  --install
  TC-HW-FS-007  --corrupt-body            (expects VERIFY err=13, HASH_MISMATCH)
  TC-HW-FS-008  --image <image linked for Slot A>   (expects VERIFY err=11, WRONG_SLOT)

Requires pyserial.
"""

import argparse
import sys
import time
import zlib

import serial

COMMAND_BAUD = 115200
TRANSFER_BAUD = 921600
BLOCK = 1024
LINE_TIMEOUT_S = 30.0
BLOCK_TIMEOUT_S = 10.0
HEADER_SIZE = 128
BAUD_SETTLE_S = 0.05


def read_line(port, timeout_s):
    """Return the next line from the device, or None on timeout."""
    port.timeout = timeout_s
    raw = port.readline()
    return raw.decode("ascii", errors="replace").strip() if raw else None


def wait_for(port, prefix, timeout_s=LINE_TIMEOUT_S):
    """Print device lines until one starts with prefix; return it."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        line = read_line(port, 1.0)
        if line:
            print("dev:", line)
            if line.startswith(prefix):
                return line
    sys.exit("timed out waiting for %r" % prefix)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--image", required=True, help="signed image from tools/ota/make_image.py")
    parser.add_argument("--image-id", type=lambda s: int(s, 16), default=None,
                        help="hex id of this upload (default: CRC-32 of the file)")
    parser.add_argument("--install", action="store_true",
                        help="also run install_begin/install_step and activate")
    parser.add_argument("--corrupt-body", action="store_true",
                        help="flip one body byte before sending (TC-HW-FS-007)")
    parser.add_argument("--stop-after", type=int, default=None, metavar="N",
                        help="stop sending after N bytes (emulates an interrupted transfer)")
    args = parser.parse_args()

    with open(args.image, "rb") as f:
        data = bytearray(f.read())
    if len(data) <= HEADER_SIZE:
        sys.exit("image is shorter than its header")
    if args.corrupt_body:
        data[HEADER_SIZE + len(data) // 2] ^= 0x01

    image_id = args.image_id if args.image_id is not None else zlib.crc32(bytes(data)) & 0xFFFFFFFE
    if image_id == 0xFFFFFFFF:
        image_id = 1

    port = serial.Serial(args.port, COMMAND_BAUD, timeout=1)
    port.reset_input_buffer()
    # The harness may have booted before we opened the port: an empty line makes it say READY.
    port.write(b"\n")
    wait_for(port, "READY")

    command = "OTA %X %d %d\n" % (image_id, len(data), 1 if args.install else 0)
    print("host:", command.strip())
    port.write(command.encode("ascii"))

    need = wait_for(port, "NEED")
    offset = int(need.split()[1])
    print("host: device wants bytes from offset %d of %d" % (offset, len(data)))

    time.sleep(BAUD_SETTLE_S)
    port.baudrate = TRANSFER_BAUD
    sent = offset
    started = time.monotonic()
    for pos in range(offset, len(data), BLOCK):
        if args.stop_after is not None and pos >= args.stop_after:
            print("host: stopping after %d bytes as asked" % pos)
            port.baudrate = COMMAND_BAUD
            return
        port.write(bytes(data[pos:pos + BLOCK]))
        ack = port.read(1)
        if ack != b".":
            sys.exit("no ack for the block at offset %d (got %r)" % (pos, ack))
        sent = min(pos + BLOCK, len(data))
    elapsed = time.monotonic() - started
    print("host: sent %d bytes in %.1f s" % (sent - offset, elapsed))

    time.sleep(BAUD_SETTLE_S)
    port.baudrate = COMMAND_BAUD
    wait_for(port, "READY", timeout_s=120.0)


if __name__ == "__main__":
    main()
