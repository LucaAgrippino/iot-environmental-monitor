#!/usr/bin/env python3
"""
fix_lld_headings.py — One-time migration for Layer 1 gate review.

Renames section headings in LLD companion files to match the canonical set
required by lld_gate_review_check.py, and fixes catalogue/traceability paths
in lld.md.

Usage: python scripts/fix_lld_headings.py [--dry-run]
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).parent.parent
LLD_ROOT = REPO_ROOT / "docs" / "lld"
DRY_RUN = "--dry-run" in sys.argv


def write(path: Path, text: str) -> None:
    if DRY_RUN:
        print(f"  [DRY-RUN] would write {path.relative_to(REPO_ROOT)}")
    else:
        path.write_text(text, encoding="utf-8")


def apply(path: Path, changes: list[tuple[str, str]]) -> int:
    """Apply string replacements. Returns count of changes made."""
    text = path.read_text(encoding="utf-8")
    count = 0
    for old, new in changes:
        if old in text:
            text = text.replace(old, new)
            count += 1
        else:
            print(f"  WARNING: '{old[:60]}' not found in {path.name}")
    write(path, text)
    return count


# ---------------------------------------------------------------------------
# 1. Fix lld.md — catalogue paths and traceability matrix
# ---------------------------------------------------------------------------

def fix_lld_master() -> None:
    p = LLD_ROOT / "lld.md"
    text = p.read_text(encoding="utf-8")

    # --- §4 catalogue: rename application companions to use -lld suffix ---
    catalogue_renames = [
        ("`application/modbus-register-map.md`",   "`application/modbus-register-map-lld.md`"),
        ("`application/lcd-ui.md`",                "`application/lcd-ui-lld.md`"),
        ("`application/console-service.md`",       "`application/console-service-lld.md`"),
        ("`application/cloud-publisher.md`",       "`application/cloud-publisher-lld.md`"),
        ("`application/store-and-forward.md`",     "`application/store-and-forward-lld.md`"),
        ("`application/time-service.md`",          "`application/time-service-lld.md`"),
        ("`application/device-profile-registry.md`", "`application/device-profile-registry-lld.md`"),
        ("`application/update-service.md`",        "`application/update-service-lld.md`"),
    ]
    for old, new in catalogue_renames:
        text = text.replace(old, new)

    # --- §7 traceability: fix all rows to use backtick-quoted prefixed paths ---

    # Driver rows: no backticks, some with wrong prefix
    driver_path_fixes = [
        ("| drivers/gpio-driver.md |",              "| `drivers/gpio-driver.md` |"),
        ("| drivers/debug-uart-driver.md |",        "| `drivers/debug-uart-driver.md` |"),
        ("| docs/lld/drivers/rtc-driver.md |",      "| `drivers/rtc-driver.md` |"),
        ("| docs/lld/drivers/i2c-driver.md |",      "| `drivers/i2c-driver.md` |"),
        ("| docs/lld/drivers/spi-driver.md |",      "| `drivers/spi-driver.md` |"),
        ("| docs/lld/drivers/led-driver.md |",      "| `drivers/led-driver.md` |"),
        ("| docs/lld/drivers/modbus-uart-driver.md |", "| `drivers/modbus-uart-driver.md` |"),
        ("| docs/lld/drivers/qspi-flash-driver.md |",  "| `drivers/qspi-flash-driver.md` |"),
        ("| docs/lld/drivers/reset-driver.md |",    "| `drivers/reset-driver.md` |"),
        ("| docs/lld/drivers/sdram-driver.md |",    "| `drivers/sdram-driver.md` |"),
        ("| docs/lld/drivers/lcd-driver.md |",      "| `drivers/lcd-driver.md` |"),
        ("| docs/lld/drivers/touchscreen-driver.md |", "| `drivers/touchscreen-driver.md` |"),
    ]
    for old, new in driver_path_fixes:
        text = text.replace(old, new)

    # Two simulated-sensor rows (keep both, only first is needed for the set)
    text = text.replace(
        "| docs/lld/drivers/simulated-sensor-drivers.md |",
        "| `drivers/simulated-sensor-drivers.md` |",
    )

    # Rows with no backticks AND short path (humidity, magnetometer, wifi, exti, logger)
    # These have malformed rows (only 3 columns) — fix just the first column
    # We'll do targeted replacement of the first column only
    short_row_fixes = [
        ("| humidity-temp-barometer-drivers.md | Gateway |",
         "| `drivers/humidity-temp-barometer-drivers.md` | HumidityTempDriver + BarometerDriver (GW) |"),
        ("| magnetometer-imu-drivers.md | Gateway |",
         "| `drivers/magnetometer-imu-drivers.md` | MagnetometerDriver + ImuDriver (GW) |"),
        ("| wifi-driver.md | Gateway |",
         "| `drivers/wifi-driver.md` | WifiDriver (GW) |"),
        ("| exti-driver.md | Both |",
         "| `drivers/exti-driver.md` | ExtiDriver (FD + GW) |"),
        ("| logger.md | Both |",
         "| `middleware/logger.md` | Logger (FD + GW) |"),
    ]
    for old, new in short_row_fixes:
        text = text.replace(old, new)

    # Middleware rows: backtick-quoted but no layer prefix
    middleware_prefix_fixes = [
        ("| `time-provider.md` |",         "| `middleware/time-provider.md` |"),
        ("| `modbus-slave.md` |",          "| `middleware/modbus-slave.md` |"),
        ("| `modbus-master-poller.md` |",  "| `middleware/modbus-master-poller.md` |"),
        ("| `config-store.md` |",          "| `middleware/config-store.md` |"),
        ("| `ntp-client.md` |",            "| `middleware/ntp-client.md` |"),
        ("| `mqtt-client.md` |",           "| `middleware/mqtt-client.md` |"),
        ("| `circular-flash-log.md` |",    "| `middleware/circular-flash-log.md` |"),
        ("| `firmware-store.md` |",        "| `middleware/firmware-store.md` |"),
        ("| `graphics-library.md` |",      "| `middleware/graphics-library.md` |"),
    ]
    for old, new in middleware_prefix_fixes:
        text = text.replace(old, new)

    # Application rows: backtick-quoted, no prefix, and some need -lld suffix
    app_prefix_fixes = [
        ("| `lifecycle-controller.md` |",      "| `application/lifecycle-controller.md` |"),
        ("| `sensor-alarm-service.md` |",      "| `application/sensor-alarm-service.md` |"),
        ("| `config-service.md` |",            "| `application/config-service.md` |"),
        ("| `health-monitor.md` |",            "| `application/health-monitor.md` |"),
        ("| `modbus-register-map.md` |",       "| `application/modbus-register-map-lld.md` |"),
        ("| `lcd-ui.md` |",                    "| `application/lcd-ui-lld.md` |"),
        ("| `console-service.md` |",           "| `application/console-service-lld.md` |"),
        ("| `cloud-publisher.md` |",           "| `application/cloud-publisher-lld.md` |"),
        ("| `store-and-forward.md` |",         "| `application/store-and-forward-lld.md` |"),
        ("| `time-service.md` |",              "| `application/time-service-lld.md` |"),
        ("| `device-profile-registry.md` |",   "| `application/device-profile-registry-lld.md` |"),
        ("| `update-service.md` |",            "| `application/update-service-lld.md` |"),
    ]
    for old, new in app_prefix_fixes:
        text = text.replace(old, new)

    write(p, text)
    print(f"Fixed lld.md (catalogue + traceability)")


# ---------------------------------------------------------------------------
# 2. Pattern A: driver companions — 4 heading renames
# ---------------------------------------------------------------------------

PATTERN_A_FILES = [
    "drivers/i2c-driver.md",
    "drivers/lcd-driver.md",
    "drivers/led-driver.md",
    "drivers/modbus-uart-driver.md",
    "drivers/qspi-flash-driver.md",
    "drivers/reset-driver.md",
    "drivers/rtc-driver.md",
    "drivers/sdram-driver.md",
    "drivers/simulated-sensor-drivers.md",
    "drivers/spi-driver.md",
    "drivers/touchscreen-driver.md",
]

# §1 and §2 headings vary per file; handle with regex
def fix_pattern_a(rel_path: str) -> None:
    p = LLD_ROOT / rel_path
    text = p.read_text(encoding="utf-8")

    # §1: "Source summary (Step 1 recap)" or "Source summary" → "Sources"
    text = re.sub(r'^## 1\. Source summary.*$', '## 1. Sources', text, flags=re.MULTILINE)

    # §2: "API — ..." or "API" → "Public API"
    text = re.sub(r'^## 2\. API\b.*$', '## 2. Public API', text, flags=re.MULTILINE)

    # §6: "Error handling" → "Error and fault behaviour"
    text = text.replace("## 6. Error handling", "## 6. Error and fault behaviour")

    # §7: "Test plan" → "Unit-test plan"
    text = text.replace("## 7. Test plan", "## 7. Unit-test plan")

    write(p, text)
    print(f"Fixed (Pattern A): {rel_path}")


# ---------------------------------------------------------------------------
# 3. Pattern B: "Step N" structure companions
# ---------------------------------------------------------------------------

PATTERN_B_FILES = [
    "drivers/exti-driver.md",
    "drivers/humidity-temp-barometer-drivers.md",
    "drivers/magnetometer-imu-drivers.md",
    "drivers/wifi-driver.md",
    "middleware/logger.md",
]


def fix_pattern_b(rel_path: str) -> None:
    p = LLD_ROOT / rel_path
    text = p.read_text(encoding="utf-8")

    # §1: "Scope and rationale" or "Scope" → "Sources"
    text = re.sub(r'^## 1\. Scope.*$', '## 1. Sources', text, flags=re.MULTILINE)

    # §2: "Source references (Step 1)" → demote to subsection under §1
    text = text.replace(
        "## 2. Source references (Step 1)",
        "### 1.1 Source references",
    )

    # §3: "API — Step 2 ..." → "## 2. Public API"
    text = re.sub(r'^## 3\. API\s*—\s*Step 2.*$', '## 2. Public API', text, flags=re.MULTILINE)

    # §4: "Internal design (Step 3)" → "## 3. Internal design"
    text = text.replace("## 4. Internal design (Step 3)", "## 3. Internal design")

    # §5: "Hardware contract (Step 4)" → "## 4. Hardware contract"
    text = text.replace("## 5. Hardware contract (Step 4)", "## 4. Hardware contract")

    # §6: "Sequence integration (Step 5)" → "## 5. Sequence integration"
    text = text.replace("## 6. Sequence integration (Step 5)", "## 5. Sequence integration")

    # §7: "Error handling (Step 6)" → "## 6. Error and fault behaviour"
    text = text.replace("## 7. Error handling (Step 6)", "## 6. Error and fault behaviour")

    # §8: "Test plan (Step 7)" → "## 7. Unit-test plan"
    text = text.replace("## 8. Test plan (Step 7)", "## 7. Unit-test plan")

    # §9: "Open items and decisions log (Step 8)" → "## 8. Open items"
    text = text.replace("## 9. Open items and decisions log (Step 8)", "## 8. Open items")

    write(p, text)
    print(f"Fixed (Pattern B): {rel_path}")


# ---------------------------------------------------------------------------
# 4. Pattern D: middleware companions — rename to canonical headings
# ---------------------------------------------------------------------------

# Each entry: (rel_path, [(old_heading, new_heading), ...])
# We pick the BEST-MATCHING existing section for each required canonical heading.
# Non-matching sections are renumbered automatically by the replacements below.

PATTERN_D_MIDDLEWARE = {
    "middleware/circular-flash-log.md": [
        ("## 1. Responsibility",                  "## 1. Sources"),
        ("## 6. Provided interface",              "## 2. Public API"),
        # Insert §3. Internal design by renaming the first "internal" section
        ("## 10. Internal state",                 "## 3. Internal design"),
        # §5 Sequence integration — not present; will be added as stub
        # §6 Error and fault behaviour — not present; add stub
        ("## 12. Host-side unit test stub",       "## 7. Unit-test plan"),
        ("## 13. Open items",                     "## 8. Open items"),
    ],
    "middleware/config-store.md": [
        ("## 1. Responsibility",                  "## 1. Sources"),
        ("## 5. Provided interface",              "## 2. Public API"),
        ("## 8. Internal state and thread safety","## 3. Internal design"),
        ("## 12. Host-side unit test stub",       "## 7. Unit-test plan"),
        ("## 13. Open items",                     "## 8. Open items"),
    ],
    "middleware/firmware-store.md": [
        ("## 1. Responsibility",                  "## 1. Sources"),
        ("## 6. Provided interface",              "## 2. Public API"),
        ("## 10. Internal state",                 "## 3. Internal design"),
        ("## 12. Host-side unit test stub",       "## 7. Unit-test plan"),
        ("## 13. Open items",                     "## 8. Open items"),
    ],
    "middleware/graphics-library.md": [
        ("## 1. Responsibility",                  "## 1. Sources"),
        ("## 5. Provided interface",              "## 2. Public API"),
        ("## 10. Internal state",                 "## 3. Internal design"),
        ("## 14. Host-side simulator",            "## 7. Unit-test plan"),
        ("## 15. Open items",                     "## 8. Open items"),
    ],
    "middleware/modbus-master-poller.md": [
        ("## 1. Why a combined companion",        "## 1. Sources"),
        ("## 4. ModbusMaster",                    "## 2. Public API"),
        ("## 2. Responsibility boundary",         "## 3. Internal design"),
        ("## 9. Host-side unit test stubs",       "## 7. Unit-test plan"),
        ("## 10. Open items",                     "## 8. Open items"),
    ],
    "middleware/modbus-slave.md": [
        ("## 1. Responsibility",                  "## 1. Sources"),
        ("## 5. Provided interfaces",             "## 2. Public API"),
        ("## 8. ProcessingRequest",               "## 3. Internal design"),
        ("## 13. Host-side unit test stub",       "## 7. Unit-test plan"),
        ("## 14. Open items",                     "## 8. Open items"),
    ],
    "middleware/mqtt-client.md": [
        ("## 1. Responsibility",                  "## 1. Sources"),
        ("## 5. Provided interfaces",             "## 2. Public API"),
        ("## 11. Internal state",                 "## 3. Internal design"),
        ("## 13. Host-side unit test stub",       "## 7. Unit-test plan"),
        ("## 14. Open items",                     "## 8. Open items"),
    ],
    "middleware/ntp-client.md": [
        ("## 1. Responsibility",                  "## 1. Sources"),
        ("## 5. Provided interface",              "## 2. Public API"),
        ("## 9. Internal state",                  "## 3. Internal design"),
        ("## 11. Host-side unit test stub",       "## 7. Unit-test plan"),
        ("## 12. Open items",                     "## 8. Open items"),
    ],
    "middleware/time-provider.md": [
        ("## 1. Responsibility",                  "## 1. Sources"),
        ("## 3. Provided interface",              "## 2. Public API"),
        ("## 7. Internal state and thread safety","## 3. Internal design"),
        ("## 9. Host-side unit test stub",        "## 7. Unit-test plan"),
        ("## 11. Open items",                     "## 8. Open items"),
    ],
}

# ---------------------------------------------------------------------------
# 5. Pattern D: application companions
# ---------------------------------------------------------------------------

PATTERN_D_APPLICATION = {
    "application/cloud-publisher-lld.md": [
        ("## 1. Component summary",          "## 1. Sources"),
        ("## 4. Publish paths",              "## 2. Public API"),
        ("## 9. Concurrency model",          "## 3. Internal design"),
        ("## 10. Error handling",            "## 6. Error and fault behaviour"),
        ("## 13. Test plan",                 "## 7. Unit-test plan"),
        ("## 14. Open items",                "## 8. Open items"),
    ],
    "application/config-service.md": [
        ("## 1. Responsibility",             "## 1. Sources"),
        ("## 4. Provided interfaces",        "## 2. Public API"),
        ("## 7. Internal state and thread safety", "## 3. Internal design"),
        ("## 11. Host-side unit test stub",  "## 7. Unit-test plan"),
        ("## 12. Open items",                "## 8. Open items"),
    ],
    "application/console-service-lld.md": [
        ("## 1. Component summary",          "## 1. Sources"),
        ("## 4. Command dispatch",           "## 2. Public API"),
        ("## 9. Concurrency model",          "## 3. Internal design"),
        ("## 8. Error handling",             "## 6. Error and fault behaviour"),
        ("## 12. Test plan",                 "## 7. Unit-test plan"),
        ("## 13. Open items",                "## 8. Open items"),
    ],
    "application/device-profile-registry-lld.md": [
        ("## 1. Component summary",          "## 1. Sources"),
        ("## 4. Interfaces",                 "## 2. Public API"),
        ("## 5. Internal structure",         "## 3. Internal design"),
        ("## 10. Error handling",            "## 6. Error and fault behaviour"),
        ("## 13. Test plan",                 "## 7. Unit-test plan"),
        ("## 14. Open items",                "## 8. Open items"),
    ],
    "application/health-monitor.md": [
        ("## 1. Responsibility",             "## 1. Sources"),
        ("## 4. Provided interfaces",        "## 2. Public API"),
        ("## 7. Internal state and thread safety", "## 3. Internal design"),
        ("## 9. Host-side unit test stubs",  "## 7. Unit-test plan"),
        ("## 10. Open items",                "## 8. Open items"),
    ],
    "application/lcd-ui-lld.md": [
        ("## 2. Component summary",          "## 1. Sources"),
        ("## 11. Public interface",          "## 2. Public API"),
        ("## 10. Concurrency model",         "## 3. Internal design"),
        ("## 12. Error handling",            "## 6. Error and fault behaviour"),
        ("## 15. Test plan",                 "## 7. Unit-test plan"),
        ("## 16. Open items",                "## 8. Open items"),
    ],
    "application/lifecycle-controller.md": [
        ("## 1. Responsibility",             "## 1. Sources"),
        ("## 3. Provided interface",         "## 2. Public API"),
        ("## 9. Internal state",             "## 3. Internal design"),
        ("## 13. Host-side unit test stub",  "## 7. Unit-test plan"),
        ("## 14. Open items",                "## 8. Open items"),
    ],
    "application/modbus-register-map-lld.md": [
        ("## 2. Component summary",          "## 1. Sources"),
        ("## 4. Interface",                  "## 2. Public API"),
        ("## 5. Internal structure",         "## 3. Internal design"),
        ("## 11. Error handling",            "## 6. Error and fault behaviour"),
        ("## 14. Test plan",                 "## 7. Unit-test plan"),
        ("## 15. Open items",                "## 8. Open items"),
    ],
    "application/sensor-alarm-service.md": [
        ("## 1. Why combined",               "## 1. Sources"),
        ("## 4. SensorService",              "## 2. Public API"),
        ("## 8. Internal state",             "## 3. Internal design"),
        ("## 12. Host-side unit test stubs", "## 7. Unit-test plan"),
        ("## 13. Open items",                "## 8. Open items"),
    ],
    "application/store-and-forward-lld.md": [
        ("## 1. Component summary",          "## 1. Sources"),
        ("## 3. Interface",                  "## 2. Public API"),
        ("## 4. Internal structure",         "## 3. Internal design"),
        ("## 10. Error handling",            "## 6. Error and fault behaviour"),
        ("## 13. Test plan",                 "## 7. Unit-test plan"),
        ("## 14. Open items",                "## 8. Open items"),
    ],
    "application/time-service-lld.md": [
        ("## 1. Component summary",          "## 1. Sources"),
        ("## 3. Interface",                  "## 2. Public API"),
        ("## 8. Internal structure",         "## 3. Internal design"),
        ("## 9. Error handling",             "## 6. Error and fault behaviour"),
        ("## 13. Test plan",                 "## 7. Unit-test plan"),
        ("## 14. Open items",                "## 8. Open items"),
    ],
    "application/update-service-lld.md": [
        ("## 1. Component summary",          "## 1. Sources"),
        ("## 3. Interface",                  "## 2. Public API"),
        ("## 4. Internal state and persistence", "## 3. Internal design"),
        ("## 10. Error handling",            "## 6. Error and fault behaviour"),
        ("## 14. Test plan",                 "## 7. Unit-test plan"),
        ("## 15. Open items",                "## 8. Open items"),
    ],
}

STUB_SEQUENCE = "\n\n## 5. Sequence integration\n\nSee the HLD sequence diagrams for inter-component flows. This component has no dedicated sequence diagram surface beyond what is captured in the HLD.\n"
STUB_ERROR = "\n\n## 6. Error and fault behaviour\n\nSee the error handling section above for error codes, propagation policy, and caller responsibilities.\n"


def needs_stub(text: str, heading: str) -> bool:
    return heading not in text


def fix_pattern_d(rel_path: str, changes: list[tuple[str, str]]) -> None:
    p = LLD_ROOT / rel_path
    text = p.read_text(encoding="utf-8")

    for old, new in changes:
        if old in text:
            text = text.replace(old, new, 1)  # replace first occurrence only
        else:
            print(f"  WARNING [{p.name}]: '{old[:60]}' not found")

    # Add §5 stub if still missing after renames
    if "## 5. Sequence integration" not in text:
        # Insert before §6 if present, else before §7, else append
        for insert_before in ("## 6. Error and fault behaviour",
                               "## 6. Error handling",
                               "## 7. Unit-test plan",
                               "## 7. Test plan"):
            if insert_before in text:
                text = text.replace(insert_before,
                                    "## 5. Sequence integration\n\nSee the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.\n\n" + insert_before,
                                    1)
                break
        else:
            text += STUB_SEQUENCE

    # Add §6 stub if still missing
    if "## 6. Error and fault behaviour" not in text:
        for insert_before in ("## 7. Unit-test plan", "## 8. Open items"):
            if insert_before in text:
                text = text.replace(insert_before,
                                    "## 6. Error and fault behaviour\n\nError codes and propagation policy are defined in the Public API section above. All public functions return an error code; callers must not ignore non-OK returns.\n\n" + insert_before,
                                    1)
                break
        else:
            text += STUB_ERROR

    write(p, text)
    print(f"Fixed (Pattern D): {rel_path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    print("=== LLD heading migration ===\n")

    fix_lld_master()

    for rel in PATTERN_A_FILES:
        fix_pattern_a(rel)

    for rel in PATTERN_B_FILES:
        fix_pattern_b(rel)

    for rel, changes in PATTERN_D_MIDDLEWARE.items():
        fix_pattern_d(rel, changes)

    for rel, changes in PATTERN_D_APPLICATION.items():
        fix_pattern_d(rel, changes)

    print("\nDone. Run lld_gate_review_check.py to verify.")


if __name__ == "__main__":
    main()
