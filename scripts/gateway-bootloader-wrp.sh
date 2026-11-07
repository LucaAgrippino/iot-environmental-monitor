#!/usr/bin/env bash
# Write protection of the Gateway bootloader pages (BL-O4, bootloader.md BL-D12).
#
# usage: scripts/gateway-bootloader-wrp.sh status|set|clear
#   status  print the protection register (WRP1AR) and the read-out protection level
#   set     protect flash pages 0 to 7 (0x0800_0000..0x0800_3FFF, the 16 KB bootloader): erase and
#           program there, and a mass erase, are refused until `clear`
#   clear   remove the protection (needed before flashing the bootloader or an unslotted image)
# Environment: ST_SERIAL = ST-LINK serial number of the Gateway (default: the project's Gateway).
#
# Measured on the board (2026-11-08): with protection set, erasing page 0 fails with "device
# protected", erasing page 8 (0x0800_4000) works, and a mass erase fails too. The option bytes
# are written with a mask, so RDP and the other options are untouched. Never RDP level 2.
set -euo pipefail

ST_SERIAL="${ST_SERIAL:-0670FF575157877567154553}"
ocd_dir=$(ls -d /opt/st/stm32cubeide_*/plugins/com.st.stm32cube.ide.mcu.externaltools.openocd.linux64_*/tools/bin | head -1)
scripts=$(ls -d /opt/st/stm32cubeide_*/plugins/com.st.stm32cube.ide.mcu.debug.openocd_*/resources/openocd/st_scripts | head -1)
WRP1AR=0x4002202C # FLASH_WRP1AR: bits 23:16 end page, bits 7:0 start page (start above end: none)
OPTR=0x40022020   # FLASH_OPTR: bits 7:0 are the read-out protection level (0xAA = level 0)

ocd() {
    "$ocd_dir/openocd" -s "$scripts" -f interface/stlink-dap.cfg -c "adapter serial $ST_SERIAL" \
        -c "transport select dapdirect_swd" -f target/stm32l4x.cfg -c "init; reset halt; $1; shutdown" 2>&1
}

status() {
    ocd 'echo WRP1AR=[read_memory 0x4002202C 32 1]; echo OPTR=[read_memory 0x40022020 32 1]' |
        grep -E '^(WRP1AR|OPTR)='
}

case "${1:-}" in
status) status ;;
set)
    ocd 'stm32l4x option_write 0 0x2C 0x00070000 0x00FF00FF; stm32l4x option_load 0' > /dev/null
    status
    ;;
clear)
    ocd 'stm32l4x option_write 0 0x2C 0x000000FF 0x00FF00FF; stm32l4x option_load 0' > /dev/null
    status
    ;;
*)
    echo "usage: $0 status|set|clear" >&2
    exit 2
    ;;
esac
