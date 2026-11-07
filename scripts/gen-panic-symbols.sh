#!/usr/bin/env bash
# Two-pass build of the panic symbol table (cpu-driver.md section 3.4).
#
# The table is generated from the linker map of a finished build and compiled into the next one, so
# the image that carries it differs from the one the map describes by the table itself. The
# functions move only if the table's size changes the layout; cpu.c therefore checks an anchor (the
# address of cpu_panic) and refuses a table that does not match the running image. If the check
# fails after the second build, run this script once more: the table size is now stable.
#
# usage: scripts/gen-panic-symbols.sh gateway|field-device [--flash]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
board="${1:?usage: gen-panic-symbols.sh gateway|field-device [--flash]}"
shift || true
case "$board" in
  gateway)      build=build-gateway-app.sh; map="$root/firmware/gateway/Debug/app/gateway_app.map" ;;
  field-device) build=build-fd-app.sh;      map="$root/firmware/field-device/Debug/app/field_device_app.map" ;;
  *) echo "board must be gateway or field-device" >&2; exit 1 ;;
esac
data="$root/firmware/$board/drivers/cpu/panic_symbols_data.c"
rm -f "$data"
bash "$root/scripts/$build"                       # pass 1: no table, map only
python3 "$root/scripts/gen-panic-symbols.py" "$map" "$data"
bash "$root/scripts/$build"                       # pass 2: table compiled in
python3 "$root/scripts/gen-panic-symbols.py" "$map" "$data"
bash "$root/scripts/$build" "$@"                  # pass 3: table sized for its own layout
