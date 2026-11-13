#!/usr/bin/env python3
"""Write verification/build/fd-symbols.json for the Field Device image that is flashed.

The Raspberry Pi bench has no gdb and no firmware tree, so the address that Boards.select_fd_screen needs
(LcdUi's `requested_screen` field) is resolved here, on the PC, from the ELF, and copied to the Pi next to the
suites. Run it again after every Field Device GUI build.

usage: scripts/gen-verify-symbols.py [ELF] [OUT.json]
"""
import json
import os
import re
import subprocess
import sys

root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
elf = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "firmware/field-device/Debug/app/field_device_app.elf")
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "verification/build/fd-symbols.json")


def gdb(expr):
    text = subprocess.run(["gdb-multiarch", "-batch", "-ex", expr, elf], capture_output=True, text=True, timeout=60).stdout
    m = re.search(r"= (0x[0-9a-f]+)", text)
    if not m:
        sys.exit("gdb could not evaluate %s on %s" % (expr, elf))
    return int(m.group(1), 16)


base = gdb("p/x (unsigned)&'lcd_ui.c'::s_ui")
offset = gdb("p/x &((struct lcd_ui*)0)->requested_screen")
os.makedirs(os.path.dirname(out), exist_ok=True)
json.dump({"elf": os.path.basename(elf), "requested_screen_address": hex(base + offset)}, open(out, "w"), indent=2)
print("requested_screen at", hex(base + offset), "->", out)
