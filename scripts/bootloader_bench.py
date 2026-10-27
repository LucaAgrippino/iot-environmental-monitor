#!/usr/bin/env python3
"""bootloader_bench.py - hardware test of the Gateway bootloader (TC-HW-BL-nnn).

Drives docs/lld/drivers/bootloader.md section 8 on a B-L475E-IOT01A:

  1. flashes bootloader.elf and the two slot images of integration-tests/bootloader/
     (scripts/build-bootloader.sh and scripts/build-bootloader-bench.sh build them),
  2. for each scenario provisions the slot descriptors and boot records through openocd,
  3. resets the board, reads the bootloader's report line and the slot image's own report from
     the debug UART, and checks them.

DESTRUCTIVE: erases the whole bootloader/metadata/Slot A/Slot B area of the board.

    scripts/bootloader_bench.py --build-dir firmware/gateway/Debug [--only BL-003 ...] [--no-flash]

Environment: OPENOCD and OPENOCD_SCRIPTS override the CubeIDE locations; the UART is
/dev/ttyACM0 unless --port says otherwise.
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile
import time

import serial

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = os.path.join(REPO, "firmware", "gateway", "gateway.cfg")
MAKE_DESC = os.path.join(REPO, "scripts", "make_slot_descriptor.py")

META = 0x08004000
DESC_A = 0x08005000
DESC_B = 0x08005800
SLOT_A = 0x08006000
SLOT_B = 0x08080000
SLOT_SIZE = 491520

CONFIRMED, PENDING, RUNNING, ROLLED_BACK = 1, 2, 3, 4
EV_INIT, EV_STARTED, EV_ABANDONED, EV_INVALID, EV_WRITE_FAILED, EV_COPY = 1, 2, 4, 8, 16, 32


def find_openocd():
    exe = os.environ.get("OPENOCD")
    scripts = os.environ.get("OPENOCD_SCRIPTS")
    if exe and scripts:
        return exe, scripts
    base = "/opt/st/stm32cubeide_*/plugins"
    exe = exe or next(iter(glob.glob(base + "/*externaltools.openocd*/tools/bin/openocd")), None)
    scripts = scripts or next(iter(glob.glob(base + "/com.st.stm32cube.ide.mcu.debug.openocd_*/resources/openocd/st_scripts")), None)
    if not exe or not scripts:
        sys.exit("openocd not found: set OPENOCD and OPENOCD_SCRIPTS")
    return exe, scripts


class Bench:
    def __init__(self, build_dir, port):
        self.build = build_dir
        self.exe, self.scripts = find_openocd()
        self.tmp = tempfile.mkdtemp(prefix="blbench-")
        self.ser = serial.Serial(port, 115200, timeout=0.1)

    # -- openocd ---------------------------------------------------------
    def oc(self, *cmds):
        args = [self.exe, "-s", self.scripts, "-f", CFG]
        for c in ("init",) + cmds + ("shutdown",):
            args += ["-c", c]
        r = subprocess.run(args, capture_output=True, text=True)
        if r.returncode != 0 or re.search(r"^Error:", r.stderr, re.M):
            tail = "\n".join(r.stderr.strip().splitlines()[-6:])
            raise RuntimeError(f"openocd failed: {cmds}\n{tail}")
        return r.stderr

    def flash_images(self):
        elf = os.path.join(self.build, "bootloader.elf")
        a = os.path.join(self.build, "slot_app_a.bin")
        b = os.path.join(self.build, "slot_app_b.bin")
        self.oc(f"program {elf} verify", f"program {a} {SLOT_A:#x} verify",
                f"program {b} {SLOT_B:#x} verify")

    # -- provisioning ----------------------------------------------------
    def desc(self, slot, image, **flags):
        out = os.path.join(self.tmp, f"desc_{slot}.bin")
        cmd = [sys.executable, MAKE_DESC, image, "--slot", slot, "--version",
               "0x01000000" if slot == "A" else "0x02000000", "-o", out]
        cmd += [f"--{k.replace('_', '-')}" for k, v in flags.items() if v]
        subprocess.run(cmd, check=True, capture_output=True)
        return out

    def padded(self, image):
        out = os.path.join(self.tmp, "padded_" + os.path.basename(image))
        data = open(image, "rb").read()
        open(out, "wb").write(data + b"\xff" * (SLOT_SIZE - len(data)))
        return out

    def provision(self, a="ok", b="ok", full_a=False):
        """Erase the metadata and descriptor pages, then write the slot descriptors.

        a, b: 'ok', 'bad_body' (descriptor body CRC wrong), 'bad_desc' (descriptor CRC wrong) or
        'none' (no descriptor).
        """
        img_a = os.path.join(self.build, "slot_app_a.bin")
        img_b = os.path.join(self.build, "slot_app_b.bin")
        cmds = ["reset halt", f"flash erase_address {META:#x} 0x2000"]
        if full_a:
            padded = self.padded(img_a)
            cmds.append(f"program {padded} {SLOT_A:#x} verify")
            img_a = padded
        for slot, mode, img, addr in (("A", a, img_a, DESC_A), ("B", b, img_b, DESC_B)):
            if mode == "none":
                continue
            d = self.desc(slot, img, bad_body_crc=(mode == "bad_body"), bad_desc_crc=(mode == "bad_desc"))
            cmds.append(f"program {d} {addr:#x} verify")
        self.oc(*cmds)

    # -- serial ----------------------------------------------------------
    def read_until(self, patterns, timeout=6.0):
        """Read the UART until one of the regexes matches the accumulated text; return the text."""
        text = ""
        end = time.time() + timeout
        while time.time() < end:
            text += self.ser.read(256).decode("latin-1")
            if any(re.search(p, text) for p in patterns):
                time.sleep(0.15)
                text += self.ser.read(1024).decode("latin-1")
                return text
        return text

    def reset(self):
        self.ser.reset_input_buffer()
        self.oc("reset run")
        return self.read_until([r"READY\r\n", r"BL HALT", r"BL FAULT"])

    def command(self, ch, wait=True):
        self.ser.reset_input_buffer()
        self.ser.write(ch.encode())
        return self.read_until([r"READY\r\n", r"\r\n"], 3.0) if wait else ""

    def settle(self, patterns=(r"READY\r\n", r"BL HALT")):
        return self.read_until(list(patterns), 6.0)


def parse(text):
    """Pull the bootloader and slot-image reports out of a capture."""
    out = {}
    m = re.search(r"BL in=(\d+) out=(\d+) bank=([AB]) gen=(\d+) rb=(\d+) ev=0x([0-9A-F]+) "
                  r"ver=0x([0-9A-F]+) len=(\d+) ms=(\d+) ecc=(\d+)", text)
    if m:
        out["bl"] = dict(zip(("inn", "out", "bank", "gen", "rb", "ev", "ver", "len", "ms", "ecc"),
                             (int(m[1]), int(m[2]), m[3], int(m[4]), int(m[5]), int(m[6], 16),
                              int(m[7], 16), int(m[8]), int(m[9]), int(m[10]))))
    m = re.search(r"BL run 0x([0-9A-F]+)", text)
    out["run"] = int(m[1], 16) if m else None
    m = re.search(r"BL HALT ([^\r\n]+)", text)
    out["halt"] = m[1] if m else None
    m = re.search(r"SLOT-APP (\w) version=0x([0-9A-F]+) vtor=0x([0-9A-F]+) cpu_init=(\d+) sws=(\d+) "
                  r"acr=0x([0-9A-F]+) ecc=(\d+)", text)
    if m:
        out["app"] = dict(slot=m[1], version=int(m[2], 16), vtor=int(m[3], 16), cpu=int(m[4]),
                          sws=int(m[5]), ecc=int(m[7]))
    recs = {}
    for r in re.finditer(r"  copy ([AB]): (\w+)(?: active=([AB]) previous=([AB]) rb=(\d+) gen=(\d+))?", text):
        recs[r[1]] = None if r[3] is None else dict(
            state=r[2], active=r[3], previous=r[4], rb=int(r[5]), gen=int(r[6]))
        if r[3] is None:
            recs[r[1]] = r[2]
    out["rec"] = recs
    return out


def selected(rec):
    cand = [v for v in rec.values() if isinstance(v, dict)]
    return max(cand, key=lambda v: v["gen"]) if cand else None


STATE = {"CONFIRMED": CONFIRMED, "TRIAL_PENDING": PENDING, "TRIAL_RUNNING": RUNNING,
         "ROLLED_BACK": ROLLED_BACK}


class Case:
    results = []

    def __init__(self, ident, title):
        self.ident, self.title, self.fails, self.notes = ident, title, [], []

    def check(self, cond, what):
        if not cond:
            self.fails.append(what)
        return cond

    def note(self, text):
        self.notes.append(text)

    def done(self):
        ok = not self.fails
        Case.results.append((self.ident, self.title, ok))
        print(f"{'PASS' if ok else 'FAIL'}  {self.ident}  {self.title}")
        for n in self.notes:
            print(f"        {n}")
        for f in self.fails:
            print(f"        FAILED: {f}")
        return ok


def expect_boot(c, p, *, inn, out, bank, run, ev=None, ev_mask=None, rb=None, gen=None):
    bl = p.get("bl")
    if not c.check(bl is not None, "no bootloader report line"):
        return
    c.check(bl["inn"] == inn, f"state_in {bl['inn']} != {inn}")
    c.check(bl["out"] == out, f"state_out {bl['out']} != {out}")
    c.check(bl["bank"] == bank, f"bank {bl['bank']} != {bank}")
    c.check(p["run"] == run, f"jump target {p['run']!r} != {run!r}")
    if ev is not None:
        c.check(bl["ev"] == ev, f"events {bl['ev']:#x} != {ev:#x}")
    if ev_mask is not None:
        c.check(bl["ev"] & ev_mask == ev_mask, f"events {bl['ev']:#x} lack {ev_mask:#x}")
    if rb is not None:
        c.check(bl["rb"] == rb, f"rollback_count {bl['rb']} != {rb}")
    if gen is not None:
        c.check(bl["gen"] == gen, f"generation {bl['gen']} != {gen}")


def expect_app(c, p, slot, state=None):
    app = p.get("app")
    if not c.check(app is not None, "slot image did not report"):
        return
    base = SLOT_A if slot == "A" else SLOT_B
    c.check(app["slot"] == slot, f"slot {app['slot']} != {slot}")
    c.check(app["vtor"] == base, f"VTOR {app['vtor']:#x} != {base:#x}")
    c.check(app["cpu"] == 0 and app["sws"] == 3, f"cpu_init={app['cpu']} sws={app['sws']} (want 0 / PLL)")
    if state is not None:
        sel = selected(p["rec"])
        c.check(sel is not None and STATE[sel["state"]] == state, f"record state {sel} != {state}")


# ---------------------------------------------------------------------------
# Scenarios
# ---------------------------------------------------------------------------

def run_all(b, only):
    def want(i):
        return not only or i in only

    # --- BL-001 / BL-010: blank metadata, the factory image ------------------
    if want("BL-001") or want("BL-010"):
        b.provision()
        p = parse(b.reset())
        c = Case("TC-HW-BL-001", "blank metadata: factory record written, Slot A runs")
        expect_boot(c, p, inn=0, out=CONFIRMED, bank="A", run=SLOT_A, ev=EV_INIT, rb=0, gen=1)
        expect_app(c, p, "A", CONFIRMED)
        sel = selected(p["rec"])
        c.check(sel and sel["gen"] == 1 and sel["previous"] == "B", f"record {sel}")
        c.done()
        c = Case("TC-HW-BL-010", "the application finds the reset clock state: cpu_init() reaches the PLL")
        app = p.get("app", {})
        c.check(app.get("cpu") == 0 and app.get("sws") == 3, f"app {app}")
        c.note("UART text arrived intact at 115200 from the 80 MHz PCLK2: baud rate is right")
        c.done()

    # --- BL-003 .. BL-005 ---------------------------------------------------
    if any(want(i) for i in ("BL-002", "BL-003", "BL-004", "BL-005", "BL-011")):
        b.provision()
        b.reset()                                  # factory record, Slot A
        b.command("p", wait=False)                 # activate B
        p = parse(b.settle())
        c = Case("TC-HW-BL-003", "TRIAL_PENDING: armed, Slot B runs the trial")
        expect_boot(c, p, inn=PENDING, out=RUNNING, bank="B", run=SLOT_B, ev=EV_STARTED, rb=0, gen=3)
        expect_app(c, p, "B", RUNNING)
        c.done()

        b.command("x", wait=False)                 # crash without confirming
        p = parse(b.settle())
        c = Case("TC-HW-BL-004", "reset during the trial: rolled back to Slot A")
        expect_boot(c, p, inn=RUNNING, out=ROLLED_BACK, bank="A", run=SLOT_A, ev=EV_ABANDONED, rb=1, gen=4)
        expect_app(c, p, "A", ROLLED_BACK)
        c.done()

        b.command("p", wait=False)                 # try B again
        b.settle()
        r = b.command("c")                         # the application confirms
        c = Case("TC-HW-BL-005", "confirmed trial stays on Slot B across resets")
        c.check("confirm: ok" in r, f"confirm reply {r!r}")
        b.command("x", wait=False)
        p = parse(b.settle())
        expect_boot(c, p, inn=CONFIRMED, out=CONFIRMED, bank="B", run=SLOT_B, ev=0, rb=1)
        expect_app(c, p, "B", CONFIRMED)
        c.done()

        b.command("p", wait=False)                 # A trial from confirmed B
        b.settle()
        b.command("r", wait=False)                 # the application's own rollback
        p = parse(b.settle())
        c = Case("TC-HW-BL-011", "application-side rollback: the bootloader boots the result, writes nothing")
        expect_boot(c, p, inn=ROLLED_BACK, out=ROLLED_BACK, bank="B", run=SLOT_B, ev=0, rb=2)
        expect_app(c, p, "B", ROLLED_BACK)
        c.done()

        b.command("p", wait=False)                 # A pending again, then a real hang
        b.settle()
        b.command("w", wait=False)                 # watchdog reset, the true crash path
        p = parse(b.settle())
        c = Case("TC-HW-BL-012", "hang during the trial: the independent watchdog resets and the bootloader rolls back")
        expect_boot(c, p, inn=RUNNING, out=ROLLED_BACK, bank="B", run=SLOT_B, ev=EV_ABANDONED, rb=3)
        expect_app(c, p, "B", ROLLED_BACK)
        c.done()

    # --- BL-006: bad body ---------------------------------------------------
    if want("BL-006"):
        b.provision(b="bad_body")
        b.reset()
        b.command("p", wait=False)
        p = parse(b.settle())
        c = Case("TC-HW-BL-006", "new image fails its body CRC: Slot A runs, ROLLED_BACK")
        expect_boot(c, p, inn=PENDING, out=ROLLED_BACK, bank="A", run=SLOT_A,
                    ev=EV_STARTED | EV_INVALID, rb=1)
        expect_app(c, p, "A", ROLLED_BACK)
        c.done()

        b.provision(a="bad_desc")
        p = parse(b.reset())
        c = Case("TC-HW-BL-006b", "factory Slot A has a bad descriptor: falls back to Slot B")
        expect_boot(c, p, inn=0, out=ROLLED_BACK, bank="B", run=SLOT_B, ev=EV_INIT | EV_INVALID, rb=1)
        expect_app(c, p, "B", ROLLED_BACK)
        c.done()

    # --- BL-007: nothing to boot ---------------------------------------------
    if want("BL-007"):
        b.provision(a="bad_body", b="none")
        t = b.reset()
        p = parse(t)
        c = Case("TC-HW-BL-007", "no bootable slot: halts with a message and starts nothing")
        c.check(p["halt"] == "no image", f"halt {p['halt']!r}")
        c.check("app" not in p and p["run"] is None, "something ran")
        c.note("LD2 (PB14) should blink: check by eye")
        c.done()

    # --- BL-009: verification time -------------------------------------------
    if want("BL-009"):
        b.provision(full_a=True)
        p = parse(b.reset())
        c = Case("TC-HW-BL-009", "verification time of a maximum-size (480 KB) slot")
        bl = p.get("bl")
        if c.check(bl is not None, "no bootloader report"):
            c.check(bl["len"] == SLOT_SIZE, f"len {bl['len']}")
            c.check(bl["ms"] <= 500, f"{bl['ms']} ms > 500 ms (10% of the 5 s restart budget)")
            c.note(f"descriptor + body CRC + vector check of {bl['len']} bytes: {bl['ms']} ms "
                   f"(budget: REQ-NF-203 5 s for the whole restart)")
        expect_app(c, p, "A", CONFIRMED)
        c.done()

    # --- BL-008: torn record ---------------------------------------------------
    if want("BL-008"):
        b.provision()
        b.reset()                                   # factory record, Slot A
        b.command("p", wait=False)
        b.settle()                                  # Slot B trial
        b.command("c")                              # confirmed: B running from bank 2
        c = Case("TC-HW-BL-008", "reset in the middle of a record write: the bootloader always boots a valid record")
        torn = 0
        ecc_total = 0
        rows = []
        for d in range(10):
            b.command(f"t{d}", wait=False)          # record write interrupted d * 10 us in
            p = parse(b.settle())
            bl = p.get("bl")
            if not c.check(bl is not None, f"delay {d * 10} us: no bootloader report"):
                continue
            c.check(p["run"] == SLOT_B, f"delay {d * 10} us: jump target {p['run']!r}")
            expect_app(c, p, "B", CONFIRMED)
            if bl["ev"] & EV_COPY:
                torn += 1
            ecc_total += bl["ecc"]
            rows.append(f"{d * 10:3d} us: gen={bl['gen']} ev={bl['ev']:#04x} ecc={bl['ecc']}"
                        f"{'  <- torn copy rejected' if bl['ev'] & EV_COPY else ''}")
            # keep the sequence going from a clean state
            b.command("c")
        for r in rows:
            c.note(r)
        c.check(torn > 0, "no interruption left a torn record: the case did not exercise its path")
        c.note(f"{torn} of 10 interruptions left a torn copy; bootloader ECC NMIs absorbed: {ecc_total}")
        c.done()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default=os.path.join(REPO, "firmware", "gateway", "Debug"))
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--no-flash", action="store_true", help="images are already on the board")
    ap.add_argument("--only", nargs="*", default=[], help="BL-001 BL-003 BL-006 BL-007 BL-008 BL-009")
    args = ap.parse_args()

    bench = Bench(args.build_dir, args.port)
    if not args.no_flash:
        print("flashing bootloader and slot images ...")
        bench.flash_images()
    run_all(bench, set(args.only))

    failed = [r for r in Case.results if not r[2]]
    print(f"\n{len(Case.results) - len(failed)} passed, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
