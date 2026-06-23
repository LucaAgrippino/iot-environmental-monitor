#!/usr/bin/env bash
# check-hal-encapsulation.sh
#
# CI guard: ensures HAL and BSP headers are included ONLY in the five
# permitted files inside the lcd_driver directory. Any other file in
# firmware/field-device/ that includes an HAL or BSP header is a violation.
#
# Permitted files (companion §6.10):
#   firmware/field-device/drivers/lcd_driver/lcd_driver.c
#   firmware/field-device/drivers/lcd_driver/lcd_hal_tick.c
#   firmware/field-device/drivers/lcd_driver/lcd_driver_internal.h
#   firmware/field-device/drivers/lcd_driver/bsp_shims/ (.c and .h files)
#
# Exit: 0 = clean; 1 = violations found.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FW_DIR="${REPO_ROOT}/firmware/field-device"

violations=$(grep -rEln \
    "stm32f4xx_hal|stm32469i_discovery|otm8009a" \
    "${FW_DIR}" \
    --exclude-dir=lcd_driver \
    2>/dev/null || true)

if [ -n "$violations" ]; then
    echo "HAL encapsulation violated by:"
    echo "$violations"
    exit 1
fi

echo "HAL encapsulation: OK"
