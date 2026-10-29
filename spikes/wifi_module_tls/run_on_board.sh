#!/usr/bin/env bash
# Build, flash and capture one spike run. usage: run_on_board.sh <label>   (spike_config.h already generated)
# Output: build/run-<label>.log (raw UART capture; it never contains the WiFi password: the spike does not print it).
# env: SPIKE_MODULE_FW  module firmware the driver should accept (default C3.5.2.3)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; root="$(cd "$here/../.." && pwd)"
P=/opt/st/stm32cubeide_2.2.0/plugins
log="$here/build/run-$1.log"
"$here/build.sh" >/dev/null 2>&1 || { echo "BUILD FAILED" >&2; exit 1; }
stty -F /dev/ttyACM0 115200 raw -echo
: > "$log"
timeout 150 cat /dev/ttyACM0 >> "$log" &
rd=$!
sleep 1
"$P"/com.st.stm32cube.ide.mcu.externaltools.openocd.linux64_*/tools/bin/openocd \
  -s "$(echo "$P"/com.st.stm32cube.ide.mcu.debug.openocd_*/resources/openocd/st_scripts)" \
  -f "$root/firmware/gateway/gateway.cfg" \
  -c "program $here/build/spike.elf verify reset exit" >/dev/null 2>&1 || { echo "FLASH FAILED" >&2; kill $rd; exit 1; }
until grep -qE "DONE\.|halt|FATAL|SPIKE  " "$log"; do sleep 2; done
sleep 1; kill $rd 2>/dev/null
tr -d '\r' < "$log" | sed 's/\x1b\[[0-9;]*m//g' | grep -E "Wifi.*(FATAL|fail)|Spike" | cut -c1-200
