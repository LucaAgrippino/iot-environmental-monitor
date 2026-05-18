#!/usr/bin/env python3
"""
fix_open_items_format.py — Expand 2-column §8 open-items tables to 4 columns.

The gate-review check requires each open-item row to have ≥ 4 non-empty
pipe-separated cells (ID | Item | Resolution path | Status).  Files written
before this convention had only 2 columns.  This script expands those tables
in-place while leaving 4-column tables untouched.
"""
import re
from pathlib import Path

REPO_ROOT = Path(__file__).parent.parent
LLD_ROOT  = REPO_ROOT / "docs" / "lld"

# Fine-grained resolution paths keyed by (filename_stem, id_prefix).
# Any row whose ID starts with the given prefix maps to the given tuple.
RESOLUTIONS: dict[tuple[str, str], tuple[str, str]] = {
    # CircularFlashLog
    ("circular-flash-log", "CFL-O1"): ("Confirm at UpdateService LLD — chunk size must fit CFL_MAX_RECORD_BYTES", "Open"),
    ("circular-flash-log", "CFL-O2"): ("Measure boot-scan latency at integration; add persisted tail pointer if > 100 ms", "Open"),
    ("circular-flash-log", "CFL-O3"): ("Design decision confirmed (pack records); revisit only if CFL-O1 forces redesign", "Open"),
    # ConfigStore (middleware)
    ("config-store", "CS-O1"):  ("Verify at ConfigService LLD — struct must fit CONFIG_STORE_MAX_DATA_BYTES", "Open"),
    ("config-store", "CS-O2"):  ("Confirm at QspiFlashDriver LLD — erase/write granularity check", "Open"),
    ("config-store", "CS-O3"):  ("Document overflow bound in code comments; no implementation change needed", "Open"),
    # FirmwareStore
    ("firmware-store", "FS-O1"): ("Measure at integration; add watchdog kick or suspend if duration > WDG period", "Open"),
    ("firmware-store", "FS-O2"): ("Confirm with provisioning-tool owner; define CertStore sub-offset in cert_store_config.h", "Open"),
    ("firmware-store", "FS-O3"): ("Confirm at QspiFlashDriver LLD — check 4 KB read efficiency", "Open"),
    ("firmware-store", "FS-O4"): ("Verify against UpdateServiceTask stack budget at integration (static-stack analysis)", "Open"),
    # GraphicsLibrary
    ("graphics-library", "GL-O1"): ("Evaluate visual quality at integration; switch to Option B if tearing observed", "Open"),
    ("graphics-library", "GL-O2"): ("Confirm at LcdDriver LLD companion — DMA async vs blocking flush", "Open"),
    ("graphics-library", "GL-O3"): ("Benchmark lv_task_handler() at integration with/without DMA2D enabled", "Open"),
    ("graphics-library", "GL-O4"): ("Check SDL2 availability on development machine; use BMP-export fallback if absent", "Open"),
    # ModbusMaster/Poller
    ("modbus-master-poller", "MBM-O1"): ("Measure peak queue occupancy at integration under worst-case callers", "Open"),
    ("modbus-master-poller", "MBM-O2"): ("Address when second Modbus slave is added to the system", "Open"),
    ("modbus-master-poller", "MBM-O3"): ("Confirm timing tolerance requirement at integration; software timer assumed sufficient", "Open"),
    ("modbus-master-poller", "MBM-O4"): ("Confirm FC03 support at implementation — API is FC-agnostic by design", "Open"),
    # ModbusSlave
    ("modbus-slave", "MBS-O1"): ("Validate boundary behaviour against modbus-register-map.md §4 at implementation", "Open"),
    ("modbus-slave", "MBS-O2"): ("Resolve at ModbusUartDriver LLD companion — DMA vs polling TC-flag strategy", "Open"),
    # MqttClient
    ("mqtt-client", "MQTT-O1"): ("Verify mbedTLS SRAM footprint against REQ-NF-400 budget at integration", "Open"),
    ("mqtt-client", "MQTT-O2"): ("Validate TLS handshake timing at integration; adjust timeout if needed", "Open"),
    ("mqtt-client", "MQTT-O3"): ("Confirm max OTA chunk size at UpdateService LLD — must fit MQTT_PKT_BUF_SIZE", "Open"),
    ("mqtt-client", "MQTT-O4"): ("Validate PUBACK timeout at integration against observed AWS IoT Core RTT", "Open"),
    ("mqtt-client", "MQTT-O5"): ("Confirm cert partition address/format at QspiFlashDriver LLD", "Open"),
    # NtpClient
    ("ntp-client", "NTP-O1"): ("Validate query timeout at integration against worst-case WiFi + internet RTT", "Open"),
    ("ntp-client", "NTP-O2"): ("Confirm wifi_driver_dns_lookup() existence at WifiDriver LLD companion", "Open"),
    ("ntp-client", "NTP-O3"): ("Document IPv4-only constraint; no action required", "Open"),
    # TimeProvider
    ("time-provider", "TP-O1"): ("Determine via RTC drift measurement at integration; set in time_provider_config.h", "Open"),
    ("time-provider", "TP-O2"): ("Allocate from project-wide backup-register map when that map is produced", "Open"),
    ("time-provider", "TP-O3"): ("Confirm threshold value at TimeService LLD companion (GW)", "Open"),
    # ConfigService (application)
    ("config-service", "CS-O1"): ("Design migration path when first firmware upgrade requiring it is planned", "Open"),
    ("config-service", "CS-O2"): ("Document mutex requirement per string field in code; add assertion", "Open"),
    ("config-service", "CS-O3"): ("Add static_assert for struct size at implementation; verify against MAX_DATA_BYTES", "Open"),
    # HealthMonitor
    ("health-monitor", "HM-O1"): ("Decide health-poll caller at FD integration — LifecycleTask or ModbusRegisterMap", "Open"),
    ("health-monitor", "HM-O2"): ("Decide GW LED blink-code mapping at integration; document in HealthMonitor code", "Open"),
    ("health-monitor", "HM-O3"): ("Add static_assert for snapshot size at implementation", "Open"),
    # LifecycleController
    ("lifecycle-controller", "LC-O1"): ("Verify BSS budget at integration; consider dedicated BSS section if too large", "Open"),
    ("lifecycle-controller", "LC-O2"): ("Implement timer at coding time once REQ-NF-213 TBD value is resolved in SRS", "Open"),
    ("lifecycle-controller", "LC-O3"): ("Allocate bit from project-wide event-group map when that map is produced", "Open"),
    ("lifecycle-controller", "LC-O4"): ("Implement FreeRTOS software timer at coding time once SRS TBD value resolved", "Open"),
    # SensorService/AlarmService
    ("sensor-alarm-service", "SS-O1"): ("Confirm IIR alpha with sensor characterisation data or customer spec before coding", "Open"),
    ("sensor-alarm-service", "SS-O2"): ("Implement N=1 (latest reading only); revisit if historical access is required", "Open"),
    ("sensor-alarm-service", "SS-O3"): ("Design per-axis/magnitude thresholds at coding time once customer spec confirmed", "Open"),
    ("sensor-alarm-service", "SS-O4"): ("Time sensor_service_run_cycle() on target under debugger; record in task-breakdown.md §8", "Open"),
}

ID_RE = re.compile(r"^\|\s*([A-Z]+-O?\d+)\s*\|")

TWO_COL_HEADER_RE = re.compile(r"^\|\s*ID\s*\|\s*Item\s*\|\s*$")
TWO_COL_SEP_RE    = re.compile(r"^\|[-| ]+\|[-| ]+\|\s*$")


def count_cells(line: str) -> int:
    stripped = line.strip("|").split("|")
    return sum(1 for c in stripped if c.strip())


def fix_open_items(path: Path) -> bool:
    stem = path.stem
    text = path.read_text(encoding="utf-8")

    if "## 8. Open items" not in text:
        return False

    lines = text.splitlines(keepends=True)
    changed = False
    in_section = False

    for i, line in enumerate(lines):
        stripped = line.rstrip("\n\r")

        # Track entry/exit of §8
        if stripped.strip() == "## 8. Open items":
            in_section = True
            continue
        if in_section and stripped.startswith("## ") and stripped.strip() != "## 8. Open items":
            in_section = False

        if not in_section:
            continue

        # Fix 2-column table header
        if TWO_COL_HEADER_RE.match(stripped):
            lines[i] = "| ID | Item | Resolution path | Status |\n"
            changed = True
            continue

        # Fix 2-column separator
        if TWO_COL_SEP_RE.match(stripped) and count_cells(stripped) == 2:
            lines[i] = "|--------|------|-----------------|--------|\n"
            changed = True
            continue

        # Fix 2-column data rows
        m = ID_RE.match(stripped)
        if m and count_cells(stripped) == 2:
            item_id = m.group(1).strip()
            resolution, status = RESOLUTIONS.get(
                (stem, item_id),
                ("Confirm at integration", "Open")
            )
            new_line = stripped.rstrip("|").rstrip() + f" | {resolution} | {status} |\n"
            lines[i] = new_line
            changed = True

    if changed:
        path.write_text("".join(lines), encoding="utf-8")
        print(f"  Fixed {path.relative_to(REPO_ROOT)}")
    return changed


def discover_companions(lld_root: Path) -> list[Path]:
    result = []
    for layer in ("drivers", "middleware", "application"):
        layer_dir = lld_root / layer
        if layer_dir.exists():
            result.extend(sorted(layer_dir.glob("*.md")))
    return result


def main() -> None:
    companions = discover_companions(LLD_ROOT)
    fixed = sum(fix_open_items(p) for p in companions)
    print(f"\nExpanded open-items tables in {fixed} companions.")


if __name__ == "__main__":
    main()
