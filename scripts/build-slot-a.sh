#!/usr/bin/env bash
# scripts/build-slot-a.sh - link the Gateway image for Slot A and build the boot stub.
#
# The Slot-A image runs execute-in-place from 0x0800_6000, so it has to be linked for that
# address (docs/hld/flash-partition-layout.md 5.1). This re-links the objects of the last
# CubeIDE "Debug" build with STM32L475VGTX_SLOT_A.ld, so no second build configuration has to be
# kept in .cproject, and builds the 16 KB boot stub that jumps to it.
#
# Usage:   scripts/build-slot-a.sh            (after a headless or IDE build of gateway/Debug)
# Output:  firmware/gateway/Debug/gateway_slot_a.elf   linked at 0x0800_6000
#          firmware/gateway/Debug/boot_stub.elf        linked at 0x0800_0000
# Flash:   openocd ... -c "program boot_stub.elf verify" -c "program gateway_slot_a.elf verify reset exit"

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
GW="${REPO}/firmware/gateway"
DEBUG="${GW}/Debug"
CC=arm-none-eabi-gcc

[[ -f "${DEBUG}/objects.list" ]] || { echo "No ${DEBUG}/objects.list: build gateway/Debug first." >&2; exit 1; }

# The Slot-A script must be the normal one with exactly two changes: a comment header, and the
# FLASH region line. Anything else means the two have drifted apart.
NORMAL="${GW}/STM32L475VGTX_FLASH.ld"
SLOT_A="${GW}/STM32L475VGTX_SLOT_A.ld"
HEADER_LINES=$(( $(wc -l < "${SLOT_A}") - $(wc -l < "${NORMAL}") ))
if ! diff -q \
        <(sed 's/ORIGIN = 0x8000000,   LENGTH = 1024K/ORIGIN = 0x8006000,   LENGTH = 480K/' "${NORMAL}") \
        <(tail -n +$(( HEADER_LINES + 1 )) "${SLOT_A}") >/dev/null; then
    echo "STM32L475VGTX_SLOT_A.ld has drifted from STM32L475VGTX_FLASH.ld (more than the FLASH line)." >&2
    exit 1
fi

echo "[1/2] Linking the image for Slot A (0x08006000)"
(
    cd "${DEBUG}"
    "${CC}" -o gateway_slot_a.elf @objects.list \
        -mcpu=cortex-m4 -T"${GW}/STM32L475VGTX_SLOT_A.ld" --specs=nosys.specs \
        -Wl,-Map=gateway_slot_a.map -Wl,--gc-sections -static --specs=nano.specs \
        -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb \
        -Wl,--start-group -lc -lm -Wl,--end-group
)

echo "[2/2] Building the boot stub (0x08000000)"
"${CC}" -mcpu=cortex-m4 -mthumb -Os -Wall -Wextra -ffreestanding -nostdlib \
    -T"${GW}/boot-stub/boot_stub.ld" -Wl,-Map="${DEBUG}/boot_stub.map" \
    "${GW}/boot-stub/boot_stub.c" -o "${DEBUG}/boot_stub.elf"

arm-none-eabi-size "${DEBUG}/gateway_slot_a.elf" "${DEBUG}/boot_stub.elf"
echo "vector table of the Slot-A image:"
arm-none-eabi-objdump -s -j .isr_vector "${DEBUG}/gateway_slot_a.elf" | sed -n '5,6p'
