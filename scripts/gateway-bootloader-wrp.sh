#!/usr/bin/env bash
# Write protection of the Gateway bootloader pages (BL-O4, bootloader.md BL-D12).
#
# usage: scripts/gateway-bootloader-wrp.sh status|set|clear|rdp1
#   status  print the protection register (WRP1AR) and the read-out protection level
#   set     protect flash pages 0 to 7 (0x0800_0000..0x0800_3FFF, the 16 KB bootloader): erase and
#           program there, and a mass erase, are refused until `clear`
#   clear   remove the protection (needed before flashing the bootloader or an unslotted image)
#   rdp1    production lock (CST-O3): read-out protection level 1. A probe can then no longer read the
#           internal flash, the RAM or the chip's unique ID, which is what the CertStore key is derived
#           from, so a dump of the external QSPI flash cannot be decrypted. Needs CONFIRM_RDP1=yes.
#           Going back to level 0 (`stm32l4x unlock`) erases the whole internal flash: the bootloader and
#           the application must be flashed again. The CertStore lives in the external QSPI flash and
#           survives. Never level 2: it is permanent.
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
    local out
    out=$(ocd 'echo WRP1AR=[read_memory 0x4002202C 32 1]; echo OPTR=[read_memory 0x40022020 32 1]' |
        grep -E '^(WRP1AR|OPTR)=' || true)
    echo "$out"
    local optr
    optr=$(echo "$out" | sed -n 's/^OPTR=0x[0-9a-fA-F]*\([0-9a-fA-F][0-9a-fA-F]\)$/\1/p')
    case "${optr,,}" in
    aa) echo "read-out protection: level 0 (a probe can read everything)" ;;
    cc) echo "read-out protection: level 2 (permanent)" ;;
    "") echo "read-out protection: not readable (the chip may already be at level 1)" ;;
    *) echo "read-out protection: level 1" ;;
    esac
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
rdp1)
    if [[ "${CONFIRM_RDP1:-}" != "yes" ]]; then
        echo "refusing: this locks the debug read-out. Read the header of this script, then run" >&2
        echo "  CONFIRM_RDP1=yes $0 rdp1" >&2
        exit 3
    fi
    ocd 'stm32l4x lock 0' > /dev/null
    echo "read-out protection level 1 requested; power-cycle the board, then run status"
    ;;
*)
    echo "usage: $0 status|set|clear|rdp1" >&2
    exit 2
    ;;
esac
