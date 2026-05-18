#!/usr/bin/env python3
"""
fix_companion_headers.py — Add missing companion header fields.

Adds **Version:**, **Date:**, **Status:**, **HLD anchor:** to the first 30
lines of each companion that is missing one or more of these fields.
"""
import re
from pathlib import Path

REPO_ROOT = Path(__file__).parent.parent
LLD_ROOT = REPO_ROOT / "docs" / "lld"

REQUIRED_FIELDS = ["**Version:**", "**Date:**", "**Status:**", "**HLD anchor:**"]

# HLD anchor text per companion (component name → anchor description)
HLD_ANCHORS = {
    # Drivers
    "exti-driver.md":                    "ExtiDriver in `components.md` (FD + GW driver layer)",
    "humidity-temp-barometer-drivers.md":"HumidityTempDriver + BarometerDriver in `components.md` (GW driver layer)",
    "magnetometer-imu-drivers.md":       "MagnetometerDriver + ImuDriver in `components.md` (GW driver layer)",
    "wifi-driver.md":                    "WifiDriver in `components.md` (GW driver layer)",
    "i2c-driver.md":                     "I2cDriver in `components.md` (FD + GW driver layer)",
    "lcd-driver.md":                     "LcdDriver in `components.md` (FD driver layer)",
    "led-driver.md":                     "LedDriver in `components.md` (FD + GW driver layer)",
    "modbus-uart-driver.md":             "ModbusUartDriver in `components.md` (FD + GW driver layer)",
    "qspi-flash-driver.md":              "QspiFlashDriver in `components.md` (FD + GW driver layer)",
    "reset-driver.md":                   "ResetDriver in `components.md` (GW driver layer)",
    "rtc-driver.md":                     "RtcDriver in `components.md` (FD + GW driver layer)",
    "sdram-driver.md":                   "SdramDriver in `components.md` (FD driver layer)",
    "simulated-sensor-drivers.md":       "BarometerDriver + HumidityTempDriver (simulated) in `components.md` (FD driver layer)",
    "spi-driver.md":                     "SpiDriver in `components.md` (GW driver layer)",
    "touchscreen-driver.md":             "TouchscreenDriver in `components.md` (FD driver layer)",
    # Middleware
    "circular-flash-log.md":             "CircularFlashLog in `components.md` (GW middleware layer)",
    "config-store.md":                   "ConfigStore in `components.md` (FD + GW middleware layer)",
    "firmware-store.md":                 "FirmwareStore in `components.md` (GW middleware layer)",
    "graphics-library.md":               "GraphicsLibrary in `components.md` (FD middleware layer)",
    "logger.md":                         "Logger in `components.md` (FD + GW middleware layer)",
    "modbus-master-poller.md":           "ModbusMaster + ModbusPoller in `components.md` (GW middleware layer)",
    "modbus-slave.md":                   "ModbusSlave in `components.md` (FD middleware layer)",
    "mqtt-client.md":                    "MqttClient in `components.md` (GW middleware layer)",
    "ntp-client.md":                     "NtpClient in `components.md` (GW middleware layer)",
    "time-provider.md":                  "TimeProvider in `components.md` (FD + GW middleware layer)",
    # Application
    "cloud-publisher-lld.md":            "CloudPublisher in `components.md` (GW application layer)",
    "config-service.md":                 "ConfigService in `components.md` (FD + GW application layer)",
    "console-service-lld.md":            "ConsoleService in `components.md` (FD + GW application layer)",
    "device-profile-registry-lld.md":    "DeviceProfileRegistry in `components.md` (GW application layer)",
    "health-monitor.md":                 "HealthMonitor in `components.md` (FD + GW application layer)",
    "lcd-ui-lld.md":                     "LcdUi in `components.md` (FD application layer)",
    "lifecycle-controller.md":           "LifecycleController in `components.md` (FD + GW application layer)",
    "modbus-register-map-lld.md":        "ModbusRegisterMap in `components.md` (FD application layer)",
    "sensor-alarm-service.md":           "SensorService + AlarmService in `components.md` (FD + GW application layer)",
    "store-and-forward-lld.md":          "StoreAndForward in `components.md` (GW application layer)",
    "time-service-lld.md":               "TimeService in `components.md` (GW application layer)",
    "update-service-lld.md":             "UpdateService in `components.md` (GW application layer)",
}

SKIP_FILES = {"gpio-driver.md", "debug-uart-driver.md"}


def discover_companions(lld_root: Path) -> list[Path]:
    result = []
    for layer_dir in (lld_root / "drivers", lld_root / "middleware", lld_root / "application"):
        if layer_dir.exists():
            result.extend(sorted(layer_dir.glob("*.md")))
    return result


def fix_header(path: Path) -> bool:
    if path.name in SKIP_FILES:
        return False

    text = path.read_text(encoding="utf-8")
    head_lines = text.splitlines()[:30]
    head = "\n".join(head_lines)

    missing = [f for f in REQUIRED_FIELDS if f not in head]
    if not missing:
        return False

    anchor = HLD_ANCHORS.get(path.name, f"see `components.md` — {path.stem}")
    defaults = {
        "**Version:**": "**Version:** 0.1",
        "**Date:**":    "**Date:** May 2026",
        "**Status:**":  "**Status:** Draft",
        "**HLD anchor:**": f"**HLD anchor:** {anchor}",
    }

    lines = text.splitlines(keepends=True)

    # Find insertion point: after the first heading (# Title line)
    # Insert missing fields right after the title + optional blank line
    insert_after = 0
    for i, line in enumerate(lines[:10]):
        if line.startswith("# "):
            insert_after = i + 1
            # Skip a blank line after the title if present
            if insert_after < len(lines) and lines[insert_after].strip() == "":
                insert_after += 1
            break

    # Build the block of missing fields to insert
    # We want them grouped together before the first --- separator
    # Find where existing header fields end (the --- line or first ##)
    separator_line = None
    for i, line in enumerate(lines):
        if i < 3:
            continue
        if line.strip() == "---" or line.startswith("## "):
            separator_line = i
            break

    # Insert missing fields just before the separator
    if separator_line is not None:
        insert_pos = separator_line
        # Find where existing **Field:** entries end
        # Walk backwards from separator to find last header field
        last_field_line = insert_pos
        for i in range(insert_pos - 1, max(0, insert_after - 1), -1):
            stripped = lines[i].strip()
            if stripped and not stripped.startswith("**") and stripped != "":
                break
            if stripped.startswith("**"):
                last_field_line = i + 1
                break

        # Build insert block
        to_insert = []
        for field in REQUIRED_FIELDS:
            if field in missing:
                to_insert.append(defaults[field] + "\n")

        # Insert before separator, with a blank line before HLD anchor if needed
        if "**HLD anchor:**" in missing and to_insert:
            # Add blank line before HLD anchor if it's not the first item
            anchor_line = defaults["**HLD anchor:**"] + "\n"
            other_lines = [l for l in to_insert if not l.startswith("**HLD anchor:**")]
            if other_lines:
                to_insert = other_lines + ["\n", anchor_line]
            else:
                to_insert = [anchor_line]

        # Insert at last_field_line
        new_lines = lines[:last_field_line] + to_insert + lines[last_field_line:]
        path.write_text("".join(new_lines), encoding="utf-8")
        print(f"  Fixed {path.name}: added {[f.split(':')[0] for f in missing]}")
        return True

    return False


def main() -> None:
    companions = discover_companions(LLD_ROOT)
    fixed = 0
    for companion in companions:
        if fix_header(companion):
            fixed += 1
    print(f"\nFixed header fields in {fixed} companions.")


if __name__ == "__main__":
    main()
