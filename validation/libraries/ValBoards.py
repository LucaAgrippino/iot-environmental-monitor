"""Robot library: the Field Device console and SWD, reached over ssh to the Raspberry Pi where the boards are plugged in."""
import os
import subprocess

PI = os.environ.get("VAL_PI", "luca@192.168.0.209")
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))


def _ssh(cmd, timeout=120):
    r = subprocess.run(["ssh", "-o", "LogLevel=ERROR", "-o", "SetEnv=LC_ALL=C", PI, cmd], capture_output=True, text=True,
                       timeout=timeout)
    if r.returncode != 0:
        raise AssertionError("ssh %s failed (%d): %s" % (PI, r.returncode, r.stderr.strip()))
    return r.stdout


class ValBoards:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def install_helpers(self):
        """Copy the two helper scripts to the Pi (idempotent)."""
        for f in ("fdcmd.py", "ocd.sh"):
            subprocess.run(["scp", "-q", os.path.join(ROOT, f), "%s:/tmp/%s" % (PI, f)], check=True)

    def fd_console(self, *lines, wait=1.5):
        """Send console lines to the Field Device, return its output (ANSI stripped)."""
        quoted = " ".join("'%s'" % l.replace("'", "'\\''") for l in lines)
        return _ssh("python3 /tmp/fdcmd.py --wait %s %s" % (wait, quoted))

    def fd_set_config(self, key, value):
        """config set + commit + y, like the Field Technician at the console; returns the output."""
        return self.fd_console("config set %s %s" % (key, value), "config commit", "y", "@2", wait=1.5)

    def fd_swd(self, *cmds):
        quoted = " ".join("'%s'" % c for c in cmds)
        return _ssh("bash /tmp/ocd.sh %s" % quoted)

    def fd_inject_pressure_fault(self, on=True):
        """Set (or clear) the barometer driver's fault flag over SWD. The address is the one in the ELF (arm-none-eabi-nm -l, barometer_driver.c); it changes with every build, the default is for v0.0.0+09d5588f."""
        addr = os.environ.get("VAL_FAULT_ADDR", "0x20009a21")
        return self.fd_swd("init", "mwb %s %d" % (addr, 1 if on else 0), "exit")

    def fd_reset(self):
        return self.fd_swd("init", "reset run", "exit")
