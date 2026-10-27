#!/usr/bin/env python3
"""make_slot_descriptor.py - build the 32-byte on-chip slot descriptor for a slot image.

The descriptor is what FirmwareStore writes once a slot is completely installed and checked, and
what the bootloader verifies before it jumps (docs/hld/flash-partition-layout.md 7.1). This tool
produces the same bytes for an image that was programmed by other means (SWD, a bench), so the
bootloader can be exercised without a full OTA install.

Usage:
    make_slot_descriptor.py slot_app_a.bin --slot A --version 0x01000000 -o desc_a.bin
    make_slot_descriptor.py slot_app_b.bin --slot B --version 0x02000000 --bad-body-crc -o d.bin

The CRC is CRC-32/ISO-HDLC (zlib.crc32), the routine shared with ConfigStore.
"""

import argparse
import struct
import sys
import zlib

DESC_MAGIC = 0x46574453
SLOT_BASE = {"A": 0x08006000, "B": 0x08080000}
SLOT_SIZE = 491520


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="slot image (.bin) as programmed at the slot base")
    ap.add_argument("--slot", choices=("A", "B"), required=True)
    ap.add_argument("--version", type=lambda s: int(s, 0), default=0x01000000)
    ap.add_argument("--bad-body-crc", action="store_true", help="flip a bit of body_crc32 (negative test)")
    ap.add_argument("--bad-desc-crc", action="store_true", help="flip a bit of desc_crc32 (negative test)")
    ap.add_argument("--load-addr", type=lambda s: int(s, 0), help="override load_addr (negative test)")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    body = open(args.image, "rb").read()
    if not 0 < len(body) <= SLOT_SIZE:
        print(f"image length {len(body)} does not fit a slot", file=sys.stderr)
        return 1

    body_crc = zlib.crc32(body) & 0xFFFFFFFF
    if args.bad_body_crc:
        body_crc ^= 1

    load = SLOT_BASE[args.slot] if args.load_addr is None else args.load_addr
    head = struct.pack("<IIIIII", DESC_MAGIC, len(body), args.version, load, body_crc, 0xFFFFFFFF)
    desc_crc = zlib.crc32(head) & 0xFFFFFFFF
    if args.bad_desc_crc:
        desc_crc ^= 1

    open(args.output, "wb").write(head + struct.pack("<II", desc_crc, 0xFFFFFFFF))
    print(f"{args.output}: slot {args.slot} len={len(body)} version={args.version:#010x} body_crc={body_crc:#010x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
