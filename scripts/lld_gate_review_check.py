#!/usr/bin/env python3
"""
LLD gate review — Layer 1 mechanical checks.

Runs automated conformance checks against the LLD tree under docs/lld/.
Findings are classified by severity:

  BLOCKER  — must be fixed before baseline.
  FIX_NOW  — should be fixed during the gate review; degrades quality.
  DEFER    — known limitation, tracked elsewhere; informational.
  COSMETIC — style or polish issue.

Exit code: 0 if no BLOCKER findings, 1 otherwise.

Usage: python scripts/lld_gate_review_check.py [--repo-root PATH]
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Optional


# ----------------------------------------------------------------------
# Finding model
# ----------------------------------------------------------------------

class Severity(Enum):
    BLOCKER = "BLOCKER"
    FIX_NOW = "FIX_NOW"
    DEFER = "DEFER"
    COSMETIC = "COSMETIC"


@dataclass
class Finding:
    severity: Severity
    category: str
    message: str
    location: str
    line: Optional[int] = None

    def __str__(self) -> str:
        loc = self.location + (f":{self.line}" if self.line is not None else "")
        return f"  [{self.severity.value}] {self.category}: {self.message} ({loc})"


# ----------------------------------------------------------------------
# Path setup
# ----------------------------------------------------------------------

def get_paths(repo_root: Path) -> dict:
    docs = repo_root / "docs"
    lld = docs / "lld"
    return {
        "root":          repo_root,
        "srs":           docs / "SRS.md",
        "components":    docs / "hld" / "components.md",
        "lld_master":    lld / "lld.md",
        "methodology":   lld / "lld-methodology.md",
        "lld_root":      lld,
        "drivers":       lld / "drivers",
        "middleware":    lld / "middleware",
        "application":   lld / "application",
        "cross_cutting": lld / "cross-cutting",
    }


def discover_companions(paths: dict) -> list[Path]:
    """All companion .md files under docs/lld/<layer>/."""
    result = []
    for layer_dir in (paths["drivers"], paths["middleware"],
                      paths["application"], paths["cross_cutting"]):
        if layer_dir.exists():
            result.extend(sorted(layer_dir.glob("*.md")))
    return result


# ----------------------------------------------------------------------
# Parsers
# ----------------------------------------------------------------------

# Match catalogue rows like:
#   | 1 | `drivers/gpio-driver.md` | Drivers | Both | Reviewed |
# Skip placeholder rows like `<one per sensor>` or `middleware/<…>.md`.
CATALOGUE_ROW_RE = re.compile(
    r"^\|\s*[^|]+?\s*\|\s*`([^`]+?\.md)`\s*\|\s*([^|]+?)\s*\|\s*[^|]+?\s*\|\s*([^|]+?)\s*\|"
)

TRACEABILITY_ROW_RE = re.compile(
    r"^\|\s*`([^`]+?\.md)`\s*\|"
)

REQ_RE = re.compile(r"\bREQ-[A-Z]+-\d+\b")


def parse_section(text: str, header_pattern: str, next_header_pattern: str) -> str:
    """Extract the body of a numbered markdown section."""
    m = re.search(rf"({header_pattern})(.+?)(?=\n{next_header_pattern}|\Z)",
                  text, re.DOTALL)
    return m.group(2) if m else ""


def parse_lld_catalogue(text: str) -> list[tuple[str, str, str]]:
    """Return [(path, layer, status), …] from §4 of lld.md."""
    section = parse_section(text, r"## 4\. Companion catalogue", r"## 5\.")
    out = []
    for line in section.splitlines():
        m = CATALOGUE_ROW_RE.match(line)
        if m:
            path = m.group(1).strip()
            layer = m.group(2).strip()
            status = m.group(3).strip()
            if "<" not in path:    # skip placeholder rows
                out.append((path, layer, status))
    return out


def parse_lld_traceability(text: str) -> set[str]:
    """Return {path, …} from §7 of lld.md."""
    section = parse_section(text, r"## 7\. Traceability", r"## 8\.|\n##|\Z")
    out = set()
    for line in section.splitlines():
        m = TRACEABILITY_ROW_RE.match(line)
        if m:
            out.add(m.group(1).strip())
    return out


def collect_srs_reqs(srs_path: Path) -> set[str]:
    """Return {REQ-XX-NNN, …} defined in SRS.md."""
    if not srs_path.exists():
        return set()
    text = srs_path.read_text(encoding="utf-8")
    # Either `- [REQ-XX-NNN] ...` definition lines or in-text mentions.
    # We collect all distinct IDs encountered; SRS is the authoritative list.
    return set(REQ_RE.findall(text))


# ----------------------------------------------------------------------
# Checks
# ----------------------------------------------------------------------

def check_catalogue_paths(paths: dict, findings: list) -> list[Path]:
    """Every catalogued companion exists on disk."""
    if not paths["lld_master"].exists():
        findings.append(Finding(
            Severity.BLOCKER, "Catalogue",
            "lld.md not found", str(paths["lld_master"])))
        return []
    text = paths["lld_master"].read_text(encoding="utf-8")
    rows = parse_lld_catalogue(text)
    catalogued = []
    for path, _layer, status in rows:
        full = paths["lld_root"] / path
        catalogued.append(full)
        if not full.exists():
            findings.append(Finding(
                Severity.BLOCKER, "Catalogue",
                f"Catalogued companion '{path}' missing on disk (status: {status})",
                str(paths["lld_master"].relative_to(paths["root"]))))
    return catalogued


def check_orphan_companions(paths: dict, catalogued: list[Path], findings: list):
    """Every file under docs/lld/<layer>/ appears in §4."""
    catalogued_set = {p.resolve() for p in catalogued}
    for f in discover_companions(paths):
        if f.resolve() not in catalogued_set:
            findings.append(Finding(
                Severity.FIX_NOW, "Catalogue",
                f"Companion '{f.name}' is not listed in lld.md §4",
                str(f.relative_to(paths["root"]))))


def check_status_consistency(paths: dict, findings: list):
    """Catalogue row says Planned/In progress but file exists."""
    if not paths["lld_master"].exists():
        return
    text = paths["lld_master"].read_text(encoding="utf-8")
    for path, _layer, status in parse_lld_catalogue(text):
        full = paths["lld_root"] / path
        if full.exists() and status.lower() in ("planned", "in progress"):
            findings.append(Finding(
                Severity.FIX_NOW, "Catalogue",
                f"Companion '{path}' exists but catalogue status is '{status}'",
                str(paths["lld_master"].relative_to(paths["root"]))))


def check_traceability(paths: dict, findings: list):
    """Every Reviewed/Baselined companion has a §7 traceability row."""
    if not paths["lld_master"].exists():
        return
    text = paths["lld_master"].read_text(encoding="utf-8")
    traced = parse_lld_traceability(text)
    for path, _layer, status in parse_lld_catalogue(text):
        if status.lower() in ("reviewed", "baselined") and path not in traced:
            findings.append(Finding(
                Severity.BLOCKER, "Traceability",
                f"Companion '{path}' is '{status}' but absent from §7 traceability matrix",
                str(paths["lld_master"].relative_to(paths["root"]))))


def check_srs_citations(paths: dict, findings: list):
    """Every REQ-XXX cited in any companion is defined in SRS.md."""
    defined = collect_srs_reqs(paths["srs"])
    if not defined:
        findings.append(Finding(
            Severity.FIX_NOW, "SRS Reference",
            "Could not extract any REQ-XXX from SRS.md (check parser or file path)",
            str(paths["srs"].relative_to(paths["root"]) if paths["srs"].exists()
                else paths["srs"])))
        return
    for companion in discover_companions(paths):
        text = companion.read_text(encoding="utf-8")
        for m in REQ_RE.finditer(text):
            req = m.group(0)
            if req not in defined:
                line_no = text[:m.start()].count("\n") + 1
                findings.append(Finding(
                    Severity.BLOCKER, "SRS Reference",
                    f"Citation '{req}' not defined in SRS.md",
                    str(companion.relative_to(paths["root"])), line_no))


def check_error_naming(paths: dict, findings: list):
    """Every error enum follows <module>_err_t (v1.1 rule)."""
    status_t_re = re.compile(r"}\s*(\w+_status_t)\s*;")
    for companion in discover_companions(paths):
        text = companion.read_text(encoding="utf-8")
        for m in status_t_re.finditer(text):
            line_no = text[:m.start()].count("\n") + 1
            findings.append(Finding(
                Severity.BLOCKER, "Naming (v1.1)",
                f"Type '{m.group(1)}' uses '_status_t'; lld.md §3.2 mandates '_err_t'",
                str(companion.relative_to(paths["root"])), line_no))


REQUIRED_SECTIONS = [
    ("## 1. Sources",                  "all"),
    ("## 2. Public API",               "all"),
    ("## 3. Internal design",          "all"),
    ("## 4. Hardware contract",        "drivers_only"),
    ("## 5. Sequence integration",     "all"),
    ("## 6. Error and fault behaviour", "all"),
    ("## 7. Unit-test plan",           "all"),
    ("## 8. Open items",               "all"),
]


def check_section_structure(paths: dict, findings: list):
    """Each companion has the eight numbered sections from the methodology."""
    for companion in discover_companions(paths):
        text = companion.read_text(encoding="utf-8")
        is_driver = companion.parent.name == "drivers"
        for heading, scope in REQUIRED_SECTIONS:
            present = heading in text
            if scope == "all" and not present:
                findings.append(Finding(
                    Severity.BLOCKER, "Structure",
                    f"Missing required section: '{heading}'",
                    str(companion.relative_to(paths["root"]))))
            elif scope == "drivers_only" and is_driver and not present:
                findings.append(Finding(
                    Severity.BLOCKER, "Structure",
                    f"Driver companion missing required section: '{heading}'",
                    str(companion.relative_to(paths["root"]))))


FORBIDDEN_DRIVER_INCLUDES = [
    "FreeRTOS.h", "task.h", "semphr.h", "queue.h",
    "event_groups.h", "timers.h", "croutine.h", "stream_buffer.h",
    "message_buffer.h",
]


def check_driver_dependencies(paths: dict, findings: list):
    """Driver companions must not include FreeRTOS headers (v1.1 dep-conformance)."""
    if not paths["drivers"].exists():
        return
    for companion in sorted(paths["drivers"].glob("*.md")):
        text = companion.read_text(encoding="utf-8")
        for block in re.finditer(r"```c\n(.*?)```", text, re.DOTALL):
            block_text = block.group(1)
            block_start = block.start()
            for inc in FORBIDDEN_DRIVER_INCLUDES:
                pat = re.compile(rf'#include\s+["<]\s*{re.escape(inc)}\s*[">]')
                for m in pat.finditer(block_text):
                    abs_pos = block_start + m.start()
                    line_no = text[:abs_pos].count("\n") + 1
                    findings.append(Finding(
                        Severity.BLOCKER, "Dependency (v1.1)",
                        f"Driver header includes forbidden RTOS dep: '{inc}'",
                        str(companion.relative_to(paths["root"])), line_no))


def check_companion_header(paths: dict, findings: list):
    """Each companion has a Version / Date / Status / HLD anchor header."""
    required = ["**Version:**", "**Date:**", "**Status:**", "**HLD anchor:**"]
    for companion in discover_companions(paths):
        head = "\n".join(companion.read_text(encoding="utf-8").splitlines()[:30])
        for field in required:
            if field not in head:
                findings.append(Finding(
                    Severity.FIX_NOW, "Structure",
                    f"Companion header missing field: '{field}'",
                    str(companion.relative_to(paths["root"]))))


def check_open_items_format(paths: dict, findings: list):
    """Each §8 row has more than just an ID — needs item + resolution + status."""
    for companion in discover_companions(paths):
        text = companion.read_text(encoding="utf-8")
        section = parse_section(text, r"## 8\. Open items", r"\n##|\Z")
        if not section.strip():
            continue
        for line in section.splitlines():
            # Heuristic: a real data row has 4+ pipe-separated cells, ID-pattern in col 1
            if re.match(r"^\|\s*[A-Z]+-O?\d+", line):
                cells = [c.strip() for c in line.strip("|").split("|")]
                # ID | Item | Resolution path | Status — 4 cells, all non-empty
                if len(cells) < 4 or any(not c for c in cells):
                    findings.append(Finding(
                        Severity.FIX_NOW, "Open items",
                        f"Open-item row appears incomplete: {line.strip()[:80]}",
                        str(companion.relative_to(paths["root"]))))


def check_principles_applied(paths: dict, findings: list):
    """Every companion §3 Internal design has a 'Principles applied' subsection."""
    for companion in discover_companions(paths):
        text = companion.read_text(encoding="utf-8")
        section = parse_section(text, r"## 3\. Internal design", r"\n## [4-9]\.|\n## \d\d\.|\Z")
        if "Principles applied" not in section:
            findings.append(Finding(
                Severity.BLOCKER, "Principles (Pass B)",
                "§3 Internal design missing 'Principles applied' subsection "
                "(lld-methodology.md §3 Step 3 gate criterion)",
                str(companion.relative_to(paths["root"]))))


# Threading annotation keywords accepted in a Doxygen block (lld.md §3.4).
_THREADING_KWS = [
    "task-context only", "isr-safe", "isr-only", "thread-safe",
    "non-blocking", "threading:", "@note threading",
    "may be called from any task", "may be called from an isr",
    "not isr-safe", "pre-scheduler", "init only",
    "mutex", "semaphore",  # explicit sync-primitive mention implies threading contract
]

# Regex for a C function declaration (return_type name(params);)
# Skips typedefs, externs, struct/enum heads, macro lines, comments, and
# vtable function-pointer fields.
_FUNC_DECL_RE = re.compile(
    r"^[ \t]*"
    r"(?!typedef|extern|struct|enum|#|/|\*)"  # not these starters
    r"(\w[\w\s*]*?)\s+"                        # return type
    r"(\w+)\s*\("                              # function name + opening paren
    r"([^;{}]*)\)\s*;",                        # params + closing ; (no body)
    re.MULTILINE,
)


def _sec2_body(text: str) -> str:
    """Return the body of the last '## 2. Public API' section."""
    matches = list(re.finditer(r"## 2\. Public API", text))
    if not matches:
        return ""
    sec2_start = matches[-1].start()
    after = text[sec2_start:]
    end_m = re.search(r"\n## [3-9]\.|\n## \d\d\.", after)
    return after[: end_m.start()] if end_m else after


def _sec4_body(text: str) -> str | None:
    """Return §4 Hardware contract section body, or None if absent."""
    m = re.search(r"## 4\. Hardware contract", text)
    if not m:
        return None
    after = text[m.start():]
    end_m = re.search(r"\n## [5-9]\.|\n## \d\d\.", after)
    return after[: end_m.start()] if end_m else after


# Pure-software drivers excluded from the hardware-contract check.
# LedDriver wraps GpioDriver (hardware contract lives there).
# SimulatedSensorDrivers are pure-software; no physical peripheral.
_PURE_SOFTWARE_DRIVERS = frozenset({
    "led-driver",
    "simulated-sensor-drivers",
})

# Regex patterns that indicate each required subsection is present.
# Accepts a ### heading containing the keyword or an explicit "N/A" declaration.
# All patterns compiled with re.IGNORECASE in check_pass_e.
_SEC4_SUBSECTIONS: dict[str, str] = {
    "Registers": r"###[^\n]*(register|peripheral)|N/A[^\n]*(register|CMSIS)",
    "Pins":      r"###[^\n]*\bpins?\b|N/A[^\n]*\bpins?\b|no GPIO",
    "Clocks":    r"###[^\n]*(clock|RCC)|N/A[^\n]*(clock|RCC)",
    "NVIC":      r"###[^\n]*\bNVIC\b|N/A[^\n]*\bNVIC\b|no interrupt|no ISR",
}


def _sec3_body(text: str) -> str:
    """Return the body of '## 3. Internal design' section."""
    m = re.search(r"## 3\. Internal design", text)
    if not m:
        return ""
    after = text[m.start():]
    end_m = re.search(r"\n## [4-9]\.|\n## \d\d\.", after)
    return after[: end_m.start()] if end_m else after


def _extract_func_names_from_sec2(text: str) -> list[str]:
    """Extract function names declared in §2 Public API code blocks."""
    body = _sec2_body(text)
    names: list[str] = []
    for code_m in re.finditer(r"```c\n(.*?)```", body, re.DOTALL):
        for line in code_m.group(1).splitlines():
            m = _FUNC_DECL_RE.match(line)
            if m and "(*" not in line:
                names.append(m.group(2))
    return names


def check_pass_d(paths: dict, findings: list):
    """Pass D: §3 must have private struct, sync declaration, per-function flow headings."""
    for companion in discover_companions(paths):
        text = companion.read_text(encoding="utf-8")
        rel = str(companion.relative_to(paths["root"]))

        sec3 = _sec3_body(text)
        if not sec3:
            findings.append(Finding(
                Severity.BLOCKER, "Internal design (Pass D)",
                "§3 Internal design section missing or not parseable",
                rel))
            continue

        # 1. Private struct: requires a ```c block with typedef struct / struct <name> {
        struct_present = bool(re.search(
            r"```c\b[^`]*?(typedef\s+struct|struct\s+\w+\s*\{)",
            sec3, re.DOTALL))
        if not struct_present:
            findings.append(Finding(
                Severity.BLOCKER, "Internal design (Pass D)",
                "§3 missing a fenced ```c block containing 'typedef struct' or 'struct { ' "
                "(private/module-state struct required — lld-methodology.md §3 Step 3)",
                rel))

        # 2. Synchronisation: requires a subsection mentioning "synchroni" or literal
        #    "caller serialises"
        sync_present = (
            bool(re.search(r"(?i)synchroni[sz]", sec3))
            or "caller serialises" in sec3
        )
        if not sync_present:
            findings.append(Finding(
                Severity.BLOCKER, "Internal design (Pass D)",
                "§3 missing a Synchronisation subsection or the phrase "
                "'caller serialises' — lld-methodology.md §3 Step 3 gate criterion",
                rel))

        # 3. Per-function flow: every function in §2 needs a ### <func_name> heading in §3
        func_names = _extract_func_names_from_sec2(text)
        for func_name in func_names:
            pattern = re.compile(
                rf"###\s+[`*]*\s*{re.escape(func_name)}", re.IGNORECASE)
            if not pattern.search(sec3):
                findings.append(Finding(
                    Severity.BLOCKER, "Internal design (Pass D)",
                    f"§3 missing '### {func_name}' flow-description heading "
                    "(one ### per public function required — lld-methodology.md §3 Step 3)",
                    rel))


def check_api_doxygen(paths: dict, findings: list):
    """Every C function declaration in §2 has @brief, @return (non-void), and threading."""
    for companion in discover_companions(paths):
        text = companion.read_text(encoding="utf-8")
        rel = str(companion.relative_to(paths["root"]))
        body = _sec2_body(text)
        if not body:
            continue

        for code_m in re.finditer(r"```c\n(.*?)```", body, re.DOTALL):
            block = code_m.group(1)
            lines = block.splitlines()

            for i, line in enumerate(lines):
                m = _FUNC_DECL_RE.match(line)
                if not m:
                    continue
                ret_type = m.group(1).strip()
                func_name = m.group(2)
                # Skip vtable function-pointer fields
                if "(*" in line:
                    continue

                # Collect the Doxygen comment that immediately precedes this line
                # (look back up to 30 lines within the same block)
                look_back = "\n".join(lines[max(0, i - 30): i])
                dox_m = re.search(r"/\*\*(.*?)\*/\s*$", look_back, re.DOTALL)
                dox = dox_m.group(1) if dox_m else ""
                dox_lower = dox.lower()

                if not dox or "@brief" not in dox:
                    findings.append(Finding(
                        Severity.BLOCKER, "API (Pass C)",
                        f"Function '{func_name}' missing @brief in §2",
                        rel))
                    continue   # no point checking sub-items if no doxygen at all

                is_void = ret_type == "void"
                if not is_void and "@return" not in dox:
                    findings.append(Finding(
                        Severity.FIX_NOW, "API (Pass C)",
                        f"Non-void function '{func_name}' missing @return in §2",
                        rel))

                if not any(kw in dox_lower for kw in _THREADING_KWS):
                    findings.append(Finding(
                        Severity.FIX_NOW, "API (Pass C)",
                        f"Function '{func_name}' missing threading annotation in §2",
                        rel))


def check_pass_e(paths: dict, findings: list):
    """Pass E: driver §4 must have Registers/Pins/Clocks/NVIC subsections; no HAL refs."""
    if not paths["drivers"].exists():
        return

    lld_text = (paths["lld_master"].read_text(encoding="utf-8")
                if paths["lld_master"].exists() else "")
    nvic_table_present = "NVIC priority allocation" in lld_text
    any_nvic_driver = False

    for companion in sorted(paths["drivers"].glob("*.md")):
        if companion.stem in _PURE_SOFTWARE_DRIVERS:
            continue

        text = companion.read_text(encoding="utf-8")
        rel = str(companion.relative_to(paths["root"]))

        sec4 = _sec4_body(text)
        if sec4 is None:
            continue  # already caught by check_section_structure

        # HAL reference check — BLOCKER
        hal_re = re.compile(r"HAL_\w+|#include\s+[<\"]stm32\w*_hal\b", re.IGNORECASE)
        for m in hal_re.finditer(sec4):
            # Compute line number relative to the full file
            sec4_start = text.find(sec4[:50])
            line_no = text[: sec4_start + m.start()].count("\n") + 1
            findings.append(Finding(
                Severity.BLOCKER, "Hardware contract (Pass E)",
                f"HAL reference '{m.group(0)}' in §4 — CMSIS-only above the driver layer "
                "(lld.md §3.1 / lld-methodology.md §4 gate criterion)",
                rel, line_no))

        # Four-subsection check — BLOCKER per missing
        for sub_name, pattern in _SEC4_SUBSECTIONS.items():
            if not re.search(pattern, sec4, re.IGNORECASE):
                findings.append(Finding(
                    Severity.BLOCKER, "Hardware contract (Pass E)",
                    f"§4 missing '{sub_name}' subsection "
                    "(or 'N/A — <reason>' — lld-methodology.md §4 gate criterion)",
                    rel))

        # Track whether any companion has real NVIC content
        if re.search(r"\b(IRQn|_IRQHandler|IRQ_Handler)\b", sec4):
            any_nvic_driver = True

    # NVIC priority allocation table must exist in lld.md — FIX_NOW
    if any_nvic_driver and not nvic_table_present:
        findings.append(Finding(
            Severity.FIX_NOW, "Hardware contract (Pass E)",
            "At least one driver companion has NVIC content but lld.md §6 lacks an "
            "'NVIC priority allocation' subsection — add per lld-methodology.md §4 guidance",
            str(paths["lld_master"].relative_to(paths["root"]))))


# ----------------------------------------------------------------------
# Pass F — Sequence integration (§5)
# ----------------------------------------------------------------------

# Map companion stem → component names it documents (as they appear in sequence-diagrams.md).
# Combined companions (e.g. SensorService·AlarmService) list all components.
_COMPANION_COMPONENTS: dict[str, list[str]] = {
    "cloud-publisher-lld":          ["CloudPublisher"],
    "config-service":               ["ConfigService"],
    "console-service-lld":          ["ConsoleService"],
    "device-profile-registry-lld":  ["DeviceProfileRegistry"],
    "health-monitor":               ["HealthMonitor"],
    "lcd-ui-lld":                   ["LcdUi"],
    "lifecycle-controller":         ["LifecycleController"],
    "modbus-register-map-lld":      ["ModbusRegisterMap", "IModbusRegisterMap"],
    "sensor-alarm-service":         ["SensorService", "AlarmService"],
    "store-and-forward-lld":        ["StoreAndForward"],
    "time-service-lld":             ["TimeService"],
    "update-service-lld":           ["UpdateService"],
    "circular-flash-log":           ["CircularFlashLog"],
    "config-store":                 ["ConfigStore"],
    "firmware-store":               ["FirmwareStore"],
    "graphics-library":             ["GraphicsLibrary"],
    "logger":                       ["Logger"],
    "modbus-master-poller":         ["ModbusMaster", "ModbusPoller"],
    "modbus-slave":                 ["ModbusSlave"],
    "mqtt-client":                  ["MqttClient"],
    "ntp-client":                   ["NtpClient"],
    "time-provider":                ["TimeProvider"],
    "debug-uart-driver":            ["DebugUartDriver"],
    "exti-driver":                  ["ExtiDriver"],
    "gpio-driver":                  ["GpioDriver"],
    "humidity-temp-barometer-drivers": ["HumidityTempDriver", "BarometerDriver"],
    "i2c-driver":                   ["I2cDriver"],
    "lcd-driver":                   ["LcdDriver"],
    "led-driver":                   ["LedDriver"],
    "magnetometer-imu-drivers":     ["MagnetometerDriver", "ImuDriver"],
    "modbus-uart-driver":           ["ModbusUartDriver"],
    "qspi-flash-driver":            ["QspiFlashDriver"],
    "reset-driver":                 ["ResetDriver"],
    "rtc-driver":                   ["RtcDriver"],
    "sdram-driver":                 ["SdramDriver"],
    "simulated-sensor-drivers":     ["BarometerDriver", "HumidityTempDriver"],
    "spi-driver":                   ["SpiDriver"],
    "touchscreen-driver":           ["TouchscreenDriver"],
    "wifi-driver":                  ["WifiDriver"],
}

# Logger is cross-cutting (P4) — elided from SD diagrams by convention; skip SD check.
_SD_SKIP_COMPONENTS: frozenset[str] = frozenset({"Logger"})

# Sub-diagram families: child SD-IDs (e.g. SD-00a) roll up to a parent (SD-00).
_SD_PARENT_RE = re.compile(r"^(SD-\d+)[a-z]$")


def _sd_parent(sd_id: str) -> str:
    """Return parent SD-ID if this is a sub-diagram, else the ID itself."""
    m = _SD_PARENT_RE.match(sd_id)
    return m.group(1) if m else sd_id


def _parse_sd_targets(sd_path: Path) -> dict[str, set[str]]:
    """
    Parse sequence-diagrams.md and return {component_name: {sd_id, ...}}.
    Only numeric-numbered message rows are considered (fragment rows are skipped).
    Board qualifiers like ' (GW)' and ' (FD)' are stripped from component names.
    """
    if not sd_path.exists():
        return {}
    text = sd_path.read_text(encoding="utf-8")
    targets: dict[str, set[str]] = {}
    current_sd = "unknown"
    # Col indices for message tables: # | From | To | Type | ...
    msg_row_re = re.compile(
        r"^\|\s*(\d[\d'\"]*)\s*\|[^|]+\|([^|]+)\|"
    )
    for line in text.splitlines():
        # Detect SD heading (## N. SD-XX or ### N.N SD-XX)
        sd_m = re.search(r"\b(SD-\d+[a-z]?)\b", line)
        if sd_m and re.match(r"#{2,4}\s", line):
            current_sd = sd_m.group(1)
        # Parse message rows
        if line.startswith("|"):
            m = msg_row_re.match(line)
            if m:
                to_cell = m.group(2)
                # Extract PascalCase component names from backtick-quoted tokens
                for name in re.findall(r"`([A-Z][A-Za-z]+(?:\s+\([^)]+\))?)`", to_cell):
                    base = re.sub(r"\s+\([^)]+\)$", "", name).strip()
                    if base not in targets:
                        targets[base] = set()
                    targets[base].add(current_sd)
    return targets



def check_pass_f(paths: dict, findings: list):
    """
    Pass F — Sequence integration (§5).

    For each companion:
    1. Determine which SD-IDs target its component(s) (where the component appears as 'To').
    2. Verify companion §5 body mentions each required SD-ID (or its parent).
       Missing SD-ID reference -> BLOCKER.
    USES edge cross-check is done by inspection in the gate-review report (too many
    false positives from return/self messages and interface-name aliasing to automate).
    """
    sd_path = paths["root"] / "docs" / "hld" / "sequence-diagrams.md"

    sd_targets = _parse_sd_targets(sd_path)

    # Per-companion SD-reference check
    for companion in discover_companions(paths):
        stem = companion.stem
        comp_names = _COMPANION_COMPONENTS.get(stem, [])
        if not comp_names:
            continue

        text = companion.read_text(encoding="utf-8")
        rel = str(companion.relative_to(paths["root"]))
        sec5 = _sec5_body(text)

        # Collect all SD-IDs mentioned in §5 (both exact and parents)
        mentioned = set(re.findall(r"SD-\d+[a-z]?", sec5))
        mentioned_parents = {_sd_parent(s) for s in mentioned} | mentioned

        # Compute required SD-IDs for this companion (deduplicated by parent)
        required_parents: set[str] = set()
        for comp in comp_names:
            if comp in _SD_SKIP_COMPONENTS:
                continue
            for sd_id in sd_targets.get(comp, set()):
                required_parents.add(_sd_parent(sd_id))

        for sd_parent_id in sorted(required_parents):
            # Satisfied if §5 mentions the parent or any child with that parent
            satisfied = sd_parent_id in mentioned_parents or any(
                _sd_parent(s) == sd_parent_id for s in mentioned
            )
            if not satisfied:
                comps_needing = [
                    c for c in comp_names
                    if sd_parent_id in {_sd_parent(s) for s in sd_targets.get(c, set())}
                ]
                findings.append(Finding(
                    Severity.BLOCKER, "Sequence integration (Pass F)",
                    f"§5 does not reference {sd_parent_id} but {comps_needing} "
                    f"appear as 'To' in that sequence - add SD trace row per "
                    f"lld-methodology.md Step 5",
                    rel))


def _sec5_body(text: str) -> str:
    # Companions with rich design content reuse section numbers (## 5. Sector layout, etc.)
    # before the gate-review standard ## 5. Sequence integration is appended.
    # Prefer the heading that explicitly names Sequence integration.
    m = re.search(r"## 5\.[^\n]*[Ss]equence", text)
    if not m:
        m = re.search(r"## 5\.", text)
    if not m:
        return ""
    after = text[m.start():]
    end_m = re.search(r"\n## [6-9]\.|\n## \d\d\.", after)
    return after[: end_m.start()] if end_m else after


# ----------------------------------------------------------------------
# Pass G — Error & fault behaviour (§6)
# ----------------------------------------------------------------------

def _sec6_body(text: str) -> str:
    """Return the body of '## 6. Error and fault behaviour' section."""
    m = re.search(r"## 6\.[^\n]*[Ee]rror", text)
    if not m:
        m = re.search(r"## 6\.", text)
    if not m:
        return ""
    after = text[m.start():]
    end_m = re.search(r"\n## [7-9]\.|\n## \d\d\.", after)
    return after[: end_m.start()] if end_m else after


_ERR_ENUM_RE = re.compile(
    r"typedef\s+enum\s*\{([^}]+)\}\s*(\w+_err_t)\s*;",
    re.DOTALL,
)

_ERR_VALUE_LINE_RE = re.compile(r"^\s*([A-Z_][A-Z0-9_]+)\s*[=,\n]", re.MULTILINE)

_RETRY_KWS = [
    "retry", "retries", "retried", "no retry", "surfaced to caller",
    "propagated", "propagates", "caller retries", "bounded attempt",
    "max attempt", "attempt", "re-attempt",
]

_OBSERVABILITY_KWS = [
    "log", "logger", "ihealth", "ihealthreport", "health_report",
    "logged", "observ", "severity", "warn", "error level", "debug",
    "not observed",
]


def check_pass_g(paths: dict, findings: list):
    """Pass G: §6 must enumerate every non-OK _err_t value with retry + observability."""
    for companion in discover_companions(paths):
        text = companion.read_text(encoding="utf-8")
        rel = str(companion.relative_to(paths["root"]))

        # Extract all non-OK values from every _err_t enum in the file
        non_ok_values: list[str] = []
        for m in _ERR_ENUM_RE.finditer(text):
            body = m.group(1)
            for val_m in _ERR_VALUE_LINE_RE.finditer(body):
                val = val_m.group(1)
                if not (val.endswith("_OK") or val == "OK"):
                    non_ok_values.append(val)

        if not non_ok_values:
            # Component has no _err_t enum (e.g. reset-driver with void returns)
            continue

        sec6 = _sec6_body(text)
        sec6_lower = sec6.lower()

        # 1. Each non-OK symbol must appear in §6 by name
        for val in non_ok_values:
            if val not in sec6:
                findings.append(Finding(
                    Severity.BLOCKER, "Error behaviour (Pass G)",
                    f"§6 does not mention error value '{val}' — each non-OK enum value "
                    "must be documented with cause, local behaviour, and caller-visible "
                    "result (lld-methodology.md §6 gate criterion)",
                    rel))

        # 2. Retry policy — at least one mention per companion (section-level)
        if not any(kw in sec6_lower for kw in _RETRY_KWS):
            findings.append(Finding(
                Severity.BLOCKER, "Error behaviour (Pass G)",
                "§6 does not state a retry policy — declare 'no retry — surfaced to caller' "
                "or a bounded retry count per failure mode "
                "(lld-methodology.md §6 gate criterion)",
                rel))

        # 3. Observability — at least one mention per companion
        if not any(kw in sec6_lower for kw in _OBSERVABILITY_KWS):
            findings.append(Finding(
                Severity.FIX_NOW, "Error behaviour (Pass G)",
                "§6 does not mention any observability sink (Logger severity or IHealthReport) "
                "for failure modes (lld-methodology.md §6 gate criterion)",
                rel))


# ----------------------------------------------------------------------
# Runner
# ----------------------------------------------------------------------

def run_all_checks(repo_root: Path) -> tuple[list[Finding], dict]:
    paths = get_paths(repo_root)
    findings: list[Finding] = []

    catalogued = check_catalogue_paths(paths, findings)
    check_orphan_companions(paths, catalogued, findings)
    check_status_consistency(paths, findings)
    check_traceability(paths, findings)
    check_srs_citations(paths, findings)
    check_error_naming(paths, findings)
    check_section_structure(paths, findings)
    check_driver_dependencies(paths, findings)
    check_companion_header(paths, findings)
    check_open_items_format(paths, findings)
    check_principles_applied(paths, findings)
    check_api_doxygen(paths, findings)
    check_pass_d(paths, findings)
    check_pass_e(paths, findings)
    check_pass_f(paths, findings)
    check_pass_g(paths, findings)

    return findings, paths


def print_report(findings: list[Finding], paths: dict):
    print(f"LLD gate review — Layer 1 + Layer 2 (Passes B, C, D, E, F, G)")
    print(f"Root: {paths['root']}\n")

    by_severity = defaultdict(list)
    for f in findings:
        by_severity[f.severity].append(f)

    for sev in (Severity.BLOCKER, Severity.FIX_NOW,
                Severity.DEFER, Severity.COSMETIC):
        items = by_severity[sev]
        if not items:
            continue
        print(f"=== {sev.value} ({len(items)}) ===")
        # Group by category for readability
        by_cat = defaultdict(list)
        for f in items:
            by_cat[f.category].append(f)
        for cat in sorted(by_cat):
            print(f"-- {cat} --")
            for f in by_cat[cat]:
                print(f)
        print()

    n_blockers = len(by_severity[Severity.BLOCKER])
    n_total = len(findings)
    print(f"Summary: {n_total} findings ({n_blockers} blockers)")
    if n_blockers == 0:
        print("Layer 1 PASSES — no blockers found.")
    else:
        print("Layer 1 FAILS — blockers must be resolved before baseline.")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd(),
                        help="Repository root (default: current working directory)")
    args = parser.parse_args()
    findings, paths = run_all_checks(args.repo_root)
    print_report(findings, paths)
    return 0 if not any(f.severity == Severity.BLOCKER for f in findings) else 1


if __name__ == "__main__":
    sys.exit(main())
