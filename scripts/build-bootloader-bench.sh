#!/usr/bin/env bash
# scripts/build-bootloader-bench.sh - build the two slot images of the bootloader bench harness.
#
# integration-tests/bootloader/main_test_bootloader.c is linked twice, once for Slot A
# (0x0800_6000) and once for Slot B (0x0808_0000), so the bootloader has two distinct images to
# choose between. Independent of the CubeIDE project: no IDE build is needed.
#
# Usage:   scripts/build-bootloader-bench.sh [outdir]      (default: firmware/gateway/Debug)
# Output:  <outdir>/slot_app_a.{elf,bin}  <outdir>/slot_app_b.{elf,bin}
# Then:    scripts/bootloader_bench.py (provisions descriptors, flashes, runs TC-HW-BL-nnn)

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
GW="${REPO}/firmware/gateway"
OUT="${1:-${GW}/Debug}"
CC=arm-none-eabi-gcc
CMSIS="${REPO}/vendor/stm32/iot-node/Drivers/CMSIS"

mkdir -p "${OUT}"

# Slot B's script must be the normal one with exactly two changes: a comment header and the
# FLASH region line (the same rule build-slot-a.sh applies to Slot A).
NORMAL="${GW}/STM32L475VGTX_FLASH.ld"
SLOT_B="${GW}/STM32L475VGTX_SLOT_B.ld"
HEADER_LINES=$(( $(wc -l < "${SLOT_B}") - $(wc -l < "${NORMAL}") ))
if ! diff -q \
        <(sed 's/ORIGIN = 0x8000000,   LENGTH = 1024K/ORIGIN = 0x8080000,   LENGTH = 480K/' "${NORMAL}") \
        <(tail -n +$(( HEADER_LINES + 1 )) "${SLOT_B}") >/dev/null; then
    echo "STM32L475VGTX_SLOT_B.ld has drifted from STM32L475VGTX_FLASH.ld (more than the FLASH line)." >&2
    exit 1
fi

SOURCES=(
    "${GW}/Startup/startup_stm32l475vgtx.s"
    "${GW}/integration-tests/bootloader/main_test_bootloader.c"
    "${GW}/drivers/cpu/cpu.c"
    "${GW}/drivers/cpu/cpu_fault.c"
    "${GW}/drivers/cpu/panic_symbols.c"
    "${GW}/drivers/cpu/panic_symbols_data.c"
    "${GW}/drivers/internal_flash/internal_flash.c"
    "${GW}/middleware/config_store/config_store_crc.c"
)

build() {
    local name="$1" ld="$2" version="$3"
    "${CC}" -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -std=gnu11 -Os -g3 \
        -Wall -Wextra \
        -ffunction-sections -fdata-sections \
        -DSTM32L475xx -DSLOT_APP_NAME="\"${name^^}\"" -DSLOT_APP_VERSION="${version}UL" \
        -I"${GW}/drivers" -I"${GW}/middleware" -I"${GW}/drivers/cpu" -I"${GW}/Inc" \
        -I"${REPO}/firmware/shared" \
        -isystem "${CMSIS}/Core/Include" -isystem "${CMSIS}/Device/ST/STM32L4xx/Include" \
        --specs=nano.specs --specs=nosys.specs \
        -T"${ld}" -Wl,-Map="${OUT}/slot_app_${name}.map" -Wl,--gc-sections \
        "${SOURCES[@]}" -o "${OUT}/slot_app_${name}.elf"
    arm-none-eabi-objcopy -O binary "${OUT}/slot_app_${name}.elf" "${OUT}/slot_app_${name}.bin"
    arm-none-eabi-size "${OUT}/slot_app_${name}.elf"
}

build a "${GW}/STM32L475VGTX_SLOT_A.ld" 0x01000000
build b "${GW}/STM32L475VGTX_SLOT_B.ld" 0x02000000
