"""Robot library: control of the two boards over SWD (openocd) and the build scripts.

Keywords are plain functions of the class; Robot turns `flash_fd` into `Flash Fd`. Serial numbers and
console ports are the bench's (docs/test/verification-plan.md section 4).
"""
import glob
import json
import os
import re
import shutil
import subprocess
import time

from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FD_SERIAL = "066BFF303435554157025015"
GW_SERIAL = "0670FF575157877567154553"
FD_CONSOLE = "/dev/serial/by-id/usb-STMicroelectronics_STM32_STLink_%s-if02" % FD_SERIAL
GW_CONSOLE = "/dev/serial/by-id/usb-STMicroelectronics_STM32_STLink_%s-if02" % GW_SERIAL
FD_ELF = os.path.join(ROOT, "firmware/field-device/Debug/app/field_device_app.elf")
GW_ELF = os.path.join(ROOT, "firmware/gateway/Debug/app/gateway_app.elf")
# The Field Device's independent watchdog (10 s) would reset the board during a long debug halt: DBGMCU_APB1_FZ
# bit 12 stops it while the core is halted (reset by a power-on only, set again before every halt).
FD_FREEZE_WDG = "mmw 0xE0042008 0x1000 0"
FB_BASE = 0xC0000000
FB_W, FB_H = 800, 480
TARGET = {"FD": ("stm32f4x", FD_SERIAL), "GW": ("stm32l4x", GW_SERIAL)}


def _openocd():
    if os.environ.get("VERIFY_OPENOCD"):            # the Raspberry Pi: openocd from the distribution
        return os.environ["VERIFY_OPENOCD"]
    return glob.glob("/opt/st/stm32cubeide_*/plugins/com.st.stm32cube.ide.mcu.externaltools.openocd.linux64_*/tools/bin/openocd")[0]


def _scripts():
    if os.environ.get("VERIFY_OPENOCD"):
        return "/usr/share/openocd/scripts"
    return glob.glob("/opt/st/stm32cubeide_*/plugins/com.st.stm32cube.ide.mcu.debug.openocd_*/resources/openocd/st_scripts")[0]


def _run_openocd(board, commands, timeout=60):
    target, serial = TARGET[board]
    if os.environ.get("VERIFY_OPENOCD"):
        iface = ["-f", "interface/stlink.cfg", "-c", "adapter serial %s" % serial]
    else:
        iface = ["-f", "interface/stlink-dap.cfg", "-c", "adapter serial %s" % serial, "-c", "transport select dapdirect_swd"]
    cmd = [_openocd(), "-s", _scripts(), *iface, "-f", "target/%s.cfg" % target, "-c", "adapter speed 2000",
           "-c", "init; %s; shutdown" % commands]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r.stdout + r.stderr


class Boards:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    # ---- flashing ---------------------------------------------------------------------------
    def flash_fd(self, gui=True):
        """Build and flash the Field Device product image (with the display stack by default)."""
        env = dict(os.environ, ST_SERIAL=FD_SERIAL, **({"GUI": "1"} if str(gui).lower() in ("true", "1", "yes") else {}))
        r = subprocess.run(["bash", os.path.join(ROOT, "scripts/build-fd-app.sh"), "--flash"], env=env,
                           capture_output=True, text=True, timeout=900, cwd=ROOT)
        if "Verified OK" not in r.stdout + r.stderr:
            raise AssertionError("Field Device flash failed:\n" + (r.stdout + r.stderr)[-800:])

    def flash_gw(self):
        """Build and flash the Gateway product image."""
        env = dict(os.environ, ST_SERIAL=GW_SERIAL)
        r = subprocess.run(["bash", os.path.join(ROOT, "scripts/build-gateway-app.sh"), "--flash"], env=env,
                           capture_output=True, text=True, timeout=900, cwd=ROOT)
        if "Verified OK" not in r.stdout + r.stderr:
            raise AssertionError("Gateway flash failed:\n" + (r.stdout + r.stderr)[-800:])

    # ---- SWD --------------------------------------------------------------------------------
    def reset_board(self, board):
        """Reset the board and let it run (a soft reset through SYSRESETREQ)."""
        _run_openocd(board.upper(), "reset run")

    def hold_board_in_reset(self, board):
        """Halt the board in reset: it goes silent on the bus, like an unplugged cable."""
        _run_openocd(board.upper(), "reset halt")

    def release_board(self, board):
        """Let a board held in reset run again."""
        _run_openocd(board.upper(), "reset run")

    def read_u32(self, board, address):
        """Read one 32-bit word from the running or halted target."""
        out = _run_openocd(board.upper(), "echo VAL=[read_memory %s 32 1]" % address)
        m = re.search(r"VAL=(0x[0-9a-fA-F]+)", out)
        if not m:
            raise AssertionError("cannot read %s: %s" % (address, out[-200:]))
        return int(m.group(1), 16)

    def read_gpio_odr(self, board, port_base):
        """Output data register of a GPIO port (LED pins)."""
        return self.read_u32(board, hex(int(port_base, 0) + 0x14))

    def symbol_address(self, board, symbol):
        """Address of a (static) symbol, from the ELF with gdb."""
        elf = FD_ELF if board.upper() == "FD" else GW_ELF
        out = subprocess.run(["gdb-multiarch", "-batch", "-ex", "p/x (unsigned)&%s" % symbol, elf],
                             capture_output=True, text=True, timeout=60).stdout
        m = re.search(r"= (0x[0-9a-f]+)", out)
        if not m:
            raise AssertionError("symbol %s not found" % symbol)
        return int(m.group(1), 16)

    def fd_symbol(self, key):
        """An address (or value) from fd-symbols.json, written by scripts/gen-verify-symbols.py for the image that is flashed."""
        path = os.environ.get("VERIFY_SYMBOLS", os.path.join(ROOT, "verification/build/fd-symbols.json"))
        if not os.path.exists(path):
            raise AssertionError("no symbol file %s: run scripts/gen-verify-symbols.py on the PC and copy it here" % path)
        value = json.load(open(path))[key]
        return int(value, 0) if isinstance(value, str) else int(value)

    def write_u8(self, board, address, value):
        """Write one byte to the running target (a debugger write, as select_fd_screen does for a halfword)."""
        _run_openocd(board.upper(), "mwb 0x%x %d" % (int(address), int(value)))

    def select_fd_screen(self, index):
        """Ask LcdUi for a screen (0 Sensors, 1 Trend, 2 Status, 3 Alarms, 4 Config) through its request field.

        The address of the field comes from gdb when it is installed (the PC) and otherwise from the JSON file
        that scripts/gen-verify-symbols.py wrote on the PC for the image that is flashed (the Raspberry Pi has no
        gdb and no firmware tree); VERIFY_SYMBOLS overrides the file name."""
        _run_openocd("FD", "mwh 0x%x %d" % (self._requested_screen_address(), int(index) + 1))

    def _requested_screen_address(self):
        path = os.environ.get("VERIFY_SYMBOLS", os.path.join(ROOT, "verification/build/fd-symbols.json"))
        if shutil.which("gdb-multiarch"):
            base = self.symbol_address("FD", "'lcd_ui.c'::s_ui")
            off = int(subprocess.run(["gdb-multiarch", "-batch", "-ex", "p/x &((struct lcd_ui*)0)->requested_screen", FD_ELF],
                                     capture_output=True, text=True, timeout=60).stdout.split("= ")[-1].strip(), 16)
            return base + off
        if not os.path.exists(path):
            raise AssertionError("no gdb-multiarch and no symbol file %s: run scripts/gen-verify-symbols.py on the PC" % path)
        return int(json.load(open(path))["requested_screen_address"], 16)

    def capture_after_reset(self, path, delay_ms):
        """Reset the Field Device, let it run `delay_ms`, halt it and save the framebuffer as a PNG (splash timing)."""
        raw = path + ".bin"
        for attempt in range(3):
            _run_openocd("FD", "reset run; %s; sleep %d; halt; dump_image %s 0x%x %d; resume" % (FD_FREEZE_WDG, int(delay_ms), raw, FB_BASE, FB_W * FB_H * 4), timeout=90)
            try:
                return self._save_png(raw, path)
            except (IOError, FileNotFoundError):
                time.sleep(1)
        raise AssertionError("the framebuffer could not be captured after the reset")

    def _save_png(self, raw, path):
        data = open(raw, "rb").read()
        os.remove(raw)
        if len(data) != FB_W * FB_H * 4:
            raise IOError("framebuffer dump is %d bytes, expected %d" % (len(data), FB_W * FB_H * 4))
        img = Image.new("RGB", (FB_W, FB_H))
        img.frombytes(b"".join(bytes((data[i + 2], data[i + 1], data[i])) for i in range(0, len(data), 4)))
        img.save(path)
        return path

    def capture_framebuffer(self, path, settle=0.0):
        """Halt the Field Device, dump the SDRAM framebuffer and save it as a PNG (halting avoids tearing)."""
        if float(settle):
            time.sleep(float(settle))
        raw = path + ".bin"
        for attempt in range(3):
            _run_openocd("FD", "%s; halt; dump_image %s 0x%x %d; resume" % (FD_FREEZE_WDG, raw, FB_BASE, FB_W * FB_H * 4), timeout=90)
            try:
                return self._save_png(raw, path)
            except (IOError, FileNotFoundError):
                time.sleep(1)
        raise AssertionError("the framebuffer could not be captured")
