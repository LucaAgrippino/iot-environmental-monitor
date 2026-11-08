"""Robot library for wave 0: host tests, build size analysis, symbol checks and tool runs. No hardware."""
import csv
import glob
import os
import re
import subprocess

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _run(cmd, cwd=ROOT, env=None, timeout=1800):
    full = dict(os.environ)
    full.update(env or {})
    return subprocess.run(cmd, cwd=cwd, env=full, capture_output=True, text=True, timeout=timeout)


class Baseline:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def unit_level_requirements(self):
        """IDs of the requirements the method table verifies at unit level (host tests credited as evidence)."""
        with open(os.path.join(ROOT, "docs/test/verification-methods.csv"), newline="") as fh:
            return [r["id"] for r in csv.DictReader(fh) if r["level"] == "U"]

    def run_host_tests(self, project="default"):
        """Run `ceedling test:all` of the default or the Gateway project; returns (tested, passed, failed)."""
        env = {"CEEDLING_MAIN_PROJECT_FILE": "project_gateway.yml"} if project == "gateway" else {}
        r = _run(["ceedling", "test:all"], cwd=os.path.join(ROOT, "tests"), env=env)
        out = r.stdout + r.stderr
        nums = {k: int(v) for k, v in re.findall(r"^(TESTED|PASSED|FAILED):\s+(\d+)", out, re.M)}
        if "TESTED" not in nums:
            raise AssertionError("ceedling gave no summary:\n" + out[-600:])
        return nums["TESTED"], nums.get("PASSED", 0), nums.get("FAILED", 0)

    def build_usage(self, board):
        """Build the product image and return {'ram': bytes, 'ram_limit': bytes, 'flash': bytes, 'flash_limit': bytes, 'sram2': ...}."""
        if board.upper() == "GW":
            r = _run(["bash", "scripts/build-gateway-app.sh"])
        else:
            r = _run(["bash", "scripts/build-fd-app.sh"], env={"GUI": "1"})
        out = r.stdout + r.stderr
        use = {}
        for name, used, unit_limit in re.findall(r"^\s*(RAM|FLASH|SRAM2):\s+(\d+) B\s+(\d+ [KM]B)", out, re.M):
            n, u = unit_limit.split()
            use[name.lower()] = int(used)
            use[name.lower() + "_limit"] = int(n) * (1024 if u == "KB" else 1024 * 1024)
        if "ram" not in use or "flash" not in use:
            raise AssertionError("no memory summary in the build output:\n" + out[-600:])
        return use

    def partition_sizes(self, board):
        """Sizes in bytes of the QSPI partitions of `board` ({name: bytes}) from docs/hld/flash-partition-layout.md.

        The Field Device table is the one with the "LCD assets" row, the Gateway table the one with "CertStore"."""
        text = open(os.path.join(ROOT, "docs/hld/flash-partition-layout.md")).read()
        key = "CertStore" if board.upper() == "GW" else "LCD assets"
        blocks, cur = [], []
        for line in text.split("\n"):
            if line.startswith("|"):
                cur.append(line)
            elif cur:
                blocks.append(cur)
                cur = []
        block = next(b for b in blocks if any(key in l for l in b))
        rows = {}
        for line in block:
            m = re.match(r"^\|\s*[`*(]*([A-Za-z][A-Za-z ]*?)[`*)]*\s*\|\s*`0x9[^|]*\|\s*`0x9[^|]*\|\s*~?([\d.]+) (KB|MB)", line)
            if m:
                rows[m.group(1).strip()] = int(float(m.group(2)) * (1024 if m.group(3) == "KB" else 1024 * 1024))
        return rows

    def objects_calling_malloc(self, board):
        """Project object files of the Debug/app build that reference malloc, calloc, realloc or free."""
        folder = "firmware/field-device/Debug/app" if board.upper() == "FD" else "firmware/gateway/Debug/app"
        bad = []
        for obj in glob.glob(os.path.join(ROOT, folder, "firmware_*.o")):
            out = _run(["arm-none-eabi-nm", "-u", obj]).stdout
            hits = [s for s in out.split() if s in ("malloc", "calloc", "realloc", "free", "_malloc_r", "_free_r")]
            if hits:
                bad.append("%s: %s" % (os.path.basename(obj), ",".join(sorted(set(hits)))))
        return bad

    def run_tool(self, command):
        """Run a repository command (a script or tool) from the root; returns (exit code, output tail)."""
        r = _run(["bash", "-c", command])
        return r.returncode, (r.stdout + r.stderr)[-500:]
