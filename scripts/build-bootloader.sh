#!/usr/bin/env bash
# scripts/build-bootloader.sh - build the Gateway bootloader (16 KB at 0x0800_0000).
#
# The bootloader is its own image, not part of the CubeIDE project (that project lists its source
# folders explicitly and bootloader/ is not one of them). It is built from:
#   firmware/gateway/bootloader/*.c                      decision logic, hardware seam, startup
#   firmware/gateway/drivers/internal_flash/internal_flash.c   the hardware-validated flash driver
#   firmware/gateway/middleware/config_store/config_store_crc.c  the CRC32 shared with ConfigStore
#
# Usage:   scripts/build-bootloader.sh [outdir]      (default: firmware/gateway/Debug)
# Output:  <outdir>/bootloader.elf, .bin, .map
# Flash:   openocd ... -c "program bootloader.elf verify reset exit"
#          Then program the slot images (scripts/build-slot-a.sh) at 0x0800_6000 / 0x0808_0000.
#
# Override the rollback cap with BOOT_MAX_ROLLBACKS=<n>.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
GW="${REPO}/firmware/gateway"
BL="${GW}/bootloader"
OUT="${1:-${GW}/Debug}"
CC=arm-none-eabi-gcc
CMSIS="${REPO}/vendor/stm32/iot-node/Drivers/CMSIS"

mkdir -p "${OUT}"

SOURCES=(
    "${BL}/boot_main.c"
    "${BL}/boot_core.c"
    "${BL}/boot_hw_l4.c"
    "${GW}/drivers/internal_flash/internal_flash.c"
    "${GW}/middleware/config_store/config_store_crc.c"
)

DEFINES=(-DSTM32L475xx)
if [[ -n "${BOOT_MAX_ROLLBACKS:-}" ]]; then
    DEFINES+=("-DBOOT_MAX_ROLLBACKS=${BOOT_MAX_ROLLBACKS}U")
fi

# Soft float: the bootloader never touches the FPU, and the application enables it itself.
"${CC}" -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -std=gnu11 -Os -g3 \
    -Wall -Wextra -Wshadow -Wconversion -Werror \
    -ffunction-sections -fdata-sections -fno-common \
    "${DEFINES[@]}" \
    -I"${BL}" -I"${GW}/drivers" -I"${GW}/middleware" -I"${GW}/drivers/internal_flash" \
    -I"${GW}/middleware/config_store" \
    -isystem "${CMSIS}/Core/Include" -isystem "${CMSIS}/Device/ST/STM32L4xx/Include" \
    -nostartfiles --specs=nano.specs --specs=nosys.specs \
    -T"${BL}/bootloader.ld" -Wl,-Map="${OUT}/bootloader.map" -Wl,--gc-sections \
    "${SOURCES[@]}" -o "${OUT}/bootloader.elf"

arm-none-eabi-objcopy -O binary "${OUT}/bootloader.elf" "${OUT}/bootloader.bin"
arm-none-eabi-size "${OUT}/bootloader.elf"
echo "bootloader: $(stat -c %s "${OUT}/bootloader.bin") of 16384 bytes"
