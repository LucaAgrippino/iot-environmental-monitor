#!/usr/bin/env python3
"""Write verification/build/fd-symbols.json for the Field Device image that is flashed.

The addresses: LcdUi's `requested_screen` (select a screen) and `tick_count` (refresh rate), and HealthMonitor's lifecycle state
(forcing the fault state, REQ-LD-270).

The Raspberry Pi bench has no gdb and no firmware tree, so the address that Boards.select_fd_screen needs
(LcdUi's `requested_screen` field) is resolved here, on the PC, from the ELF, and copied to the Pi next to the
suites. Run it again after every Field Device GUI build.

usage: scripts/gen-verify-symbols.py [FD_ELF] [OUT.json] [GW_ELF]
  GW_ELF is the Gateway image that is flashed (for example ~/rel-final/1.0.1/gateway_slot_a.elf): the longest sensor cycle
  of the Gateway (REQ-NF-100) is read from its address.
"""
import json
import os
import re
import subprocess
import sys

root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
elf = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "firmware/field-device/Debug/app/field_device_app.elf")
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "verification/build/fd-symbols.json")


gw_elf = sys.argv[3] if len(sys.argv) > 3 else None


def gdb(expr, image=None):
    text = subprocess.run(["gdb-multiarch", "-batch", "-ex", expr, image or elf], capture_output=True, text=True, timeout=60).stdout
    m = re.search(r"= (0x[0-9a-f]+)", text)
    if not m:
        sys.exit("gdb could not evaluate %s on %s" % (expr, elf))
    return int(m.group(1), 16)


base = gdb("p/x (unsigned)&'lcd_ui.c'::s_ui")
offset = gdb("p/x &((struct lcd_ui*)0)->requested_screen")
tick = gdb("p/x &((struct lcd_ui*)0)->tick_count")
hm_state = gdb("p/x (unsigned)&'health_monitor.c'::s_hm.snapshot.lifecycle_state")
hm_size = gdb("p/x (unsigned)sizeof('health_monitor.c'::s_hm.snapshot.lifecycle_state)")
faulted = gdb("p/x (unsigned)LIFECYCLE_STATE_FAULTED")
fd_cycle = gdb("p/x (unsigned)&g_fd_sensor_cycle_us_max")
gw_cycle = gdb("p/x (unsigned)&g_gw_sensor_cycle_ms_max", gw_elf) if gw_elf else None
os.makedirs(os.path.dirname(out), exist_ok=True)
json.dump({"elf": os.path.basename(elf),
           "requested_screen_address": hex(base + offset),      # LcdUi: request a screen (select_fd_screen)
           "ui_tick_count_address": hex(base + tick),            # LcdUi: lcd_ui_tick() calls (REQ-NF-108)
           "hm_lifecycle_state_address": hex(hm_state),          # HealthMonitor: the lifecycle state it shows on the LEDs
           "hm_lifecycle_state_size": hm_size,
           "lifecycle_faulted": faulted,
           "fd_sensor_cycle_us_max_address": hex(fd_cycle),     # SensorTask of the Field Device: longest cycle, us (REQ-NF-100)
           **({"gw_sensor_cycle_ms_max_address": hex(gw_cycle)} if gw_cycle is not None else {})},  # Gateway: longest cycle, ms
          open(out, "w"), indent=2)
print("requested_screen at", hex(base + offset), "tick_count at", hex(base + tick), "hm state at", hex(hm_state), "->", out)
