#!/usr/bin/env bash
# Build the MqttClient bring-up harness (firmware/gateway/integration-tests/mqtt_client) into an ELF
# without CubeIDE and without touching .cproject (standing rule: never edit it). Used for the
# TC-HW-MQTT-014 bench run: mbedTLS arena peak with a TLS session open (MQTT-O1, FS-O4).
#
# Same flags, include paths and optimization levels as the generated Debug makefiles
# (-O0 for project code, -O2 for the mbedTLS library). Output: firmware/gateway/Debug/bench/mqtt_client/
# (Debug/ is gitignored). Needs firmware/gateway/certs/{bringup_certs.h,bringup_secrets.h}.
#
# usage: scripts/build-mqtt-client-bench.sh [--flash]
#   --flash   also program the board with openocd (CubeIDE's copy) and reset it
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
gw="$root/firmware/gateway"
v="$root/vendor"
out="$gw/Debug/bench/mqtt_client"

for f in bringup_certs.h bringup_secrets.h; do
    [[ -f "$gw/certs/$f" ]] || { echo "missing $gw/certs/$f (see scripts/bringup_secrets.h.example)" >&2; exit 1; }
done
command -v arm-none-eabi-gcc >/dev/null || { echo "arm-none-eabi-gcc not on PATH" >&2; exit 1; }
mkdir -p "$out"

common=(-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -std=gnu11 -g3 -Wall
  -ffunction-sections -fdata-sections --specs=nano.specs
  -DDEBUG -DSTM32L4 -DSTM32 -DB_L475E_IOT01A2 -DSTM32L475VGTx -DSTM32L475xx
  -DMQTT_DO_NOT_USE_CUSTOM_CONFIG '-DMBEDTLS_CONFIG_FILE="mbedtls_config_gateway.h"'
  -I"$v/freertos-kernel/portable/GCC/ARM_CM4F" -I"$v/freertos-kernel/include"
  -I"$v/stm32/iot-node/Drivers/CMSIS/Core/Include"
  -I"$v/stm32/iot-node/Drivers/CMSIS/Device/ST/STM32L4xx/Include"
  -I"$gw/../shared" -I"$gw/drivers" -I"$gw/Inc" -I"$gw/middleware" -I"$gw/middleware/mqtt_client"
  -I"$gw/middleware/wifi_task" -I"$v/coreMQTT/source/include" -I"$v/coreMQTT/source/interface"
  -I"$v/mbedtls/include" -I"$gw/application" -I"$gw/certs")

# Project code, -O0.
project_srcs=(
  "$gw/integration-tests/mqtt_client/main_test_mqtt_client.c"
  "$gw/middleware/mqtt_client/mqtt_client.c" "$gw/middleware/mqtt_client/tls_arena.c"
  "$gw/middleware/wifi_task/wifi_task.c" "$gw/middleware/logger/logger.c"
  "$gw/drivers/wifi_driver/wifi_driver.c" "$gw/drivers/spi/spi.c" "$gw/drivers/exti/exti_driver.c"
  "$gw/drivers/gpio/gpio_driver.c" "$gw/drivers/rtc/rtc.c" "$gw/drivers/debug_uart/debug_uart.c"
  "$gw/drivers/cpu/cpu.c" "$gw/drivers/cpu/cpu_fault.c" "$gw/drivers/cpu/panic_symbols.c"
  "$gw/drivers/cpu/panic_symbols_data.c"
  "$root/scripts/bench-irq-stubs.c"
  "$gw/Src/freertos_hooks.c" "$gw/Src/stm32l4xx_it.c" "$gw/Src/syscalls.c" "$gw/Src/sysmem.c"
  "$v/stm32/iot-node/Drivers/CMSIS/Device/ST/STM32L4xx/Source/Templates/system_stm32l4xx.c"
  "$v/coreMQTT/source/core_mqtt.c" "$v/coreMQTT/source/core_mqtt_serializer.c"
  "$v/coreMQTT/source/core_mqtt_state.c"
  "$v/freertos-kernel/croutine.c" "$v/freertos-kernel/event_groups.c" "$v/freertos-kernel/list.c"
  "$v/freertos-kernel/queue.c" "$v/freertos-kernel/stream_buffer.c" "$v/freertos-kernel/tasks.c"
  "$v/freertos-kernel/timers.c" "$v/freertos-kernel/portable/GCC/ARM_CM4F/port.c")

# The whole mbedTLS library, -O2 (mbedtls_config_gateway.h selects what is actually built).
mapfile -t mbedtls_srcs < <(ls "$v"/mbedtls/library/*.c)

compile_one() {
    local opt="$1" src="$2" obj
    obj="$out/$(echo "${src#"$root"/}" | tr '/' '_').o"
    arm-none-eabi-gcc "${common[@]}" "$opt" -c "$src" -o "$obj" 2> "$obj.log" \
        || { cat "$obj.log" >&2; echo "FAILED: $src" >&2; return 1; }
}
# Jobs are forked from this shell, so they inherit the flag array; a few run at a time.
jobs_max="$(nproc)"
pids=()
fail=0
run() {
    compile_one "$@" &
    pids+=($!)
    if (( ${#pids[@]} >= jobs_max )); then
        wait "${pids[0]}" || fail=1
        pids=("${pids[@]:1}")
    fi
}
for s in "${project_srcs[@]}"; do run -O0 "$s"; done
for s in "${mbedtls_srcs[@]}"; do run -O2 "$s"; done
for p in "${pids[@]}"; do wait "$p" || fail=1; done
(( fail == 0 )) || { echo "compile errors above" >&2; exit 1; }

arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -x assembler-with-cpp \
  -c "$gw/Startup/startup_stm32l475vgtx.s" -o "$out/startup.o"
arm-none-eabi-gcc -o "$out/mqtt_client.elf" "$out"/*.c.o "$out/startup.o" -mcpu=cortex-m4 \
  -T"$gw/STM32L475VGTX_FLASH.ld" --specs=nosys.specs --specs=nano.specs -mfpu=fpv4-sp-d16 \
  -mfloat-abi=hard -mthumb -Wl,-Map="$out/mqtt_client.map" -Wl,--gc-sections -static \
  -Wl,--start-group -lc -lm -Wl,--end-group
arm-none-eabi-size "$out/mqtt_client.elf"

if [[ "${1:-}" == "--flash" ]]; then
    p=/opt/st/stm32cubeide_2.2.0/plugins
    "$p"/com.st.stm32cube.ide.mcu.externaltools.openocd.linux64_*/tools/bin/openocd \
        -s "$(echo "$p"/com.st.stm32cube.ide.mcu.debug.openocd_*/resources/openocd/st_scripts)" \
        -f "$gw/gateway.cfg" -c "program $out/mqtt_client.elf verify reset exit"
fi
