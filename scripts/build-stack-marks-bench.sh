#!/usr/bin/env bash
# Build the Gateway stack high-water bench (bench/stack_marks/main_bench_stack_marks.c): the real task
# set running together, for the WP4 D-1 gate. No CubeIDE and no .cproject change.
#
# Links every Gateway application, driver and middleware source (never the integration tests, the
# bootloader or the boot stub) with the flags and optimization levels of the generated Debug makefiles
# (-O0 project code, -O2 mbedTLS). Output: firmware/gateway/Debug/bench/stack_marks/ (gitignored).
#
# usage: [WITH_CONSOLE=0 WITH_CLOUD=1] [ARENA_BYTES=48128] scripts/build-stack-marks-bench.sh [--flash]
#   The whole task set needs about 147 KB of static RAM and the chip has 96 KB (0x20000000) plus 32 KB
#   of SRAM2 (0x10000000 only: 0x20018000 is NOT an alias on the STM32L475, verified on the board).
#   mode A (default): everything but the console. SRAM2 is free, so the bench linker script places the
#            task stacks (about 27 KB) there and the main bank keeps the arena, pools and state.
#   mode B: WITH_CONSOLE=1 WITH_CLOUD=0 (console and sensors only; the console owns SRAM2).
#   ARENA_BYTES        mbedTLS arena (default: tls_arena.h)
#   RAM_KB / SRAM2_KB  override the region sizes (hardware: 96 and 32); bigger values only LINK an image
#                      that does not fit, to read its section sizes
#   --flash            program the board with openocd and reset it
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
gw="$root/firmware/gateway"
v="$root/vendor"
out="$gw/Debug/bench/stack_marks"
ram_kb="${RAM_KB:-96}"
sram2_kb="${SRAM2_KB:-32}"

for f in bringup_certs.h bringup_secrets.h; do
    [[ -f "$gw/certs/$f" ]] || { echo "missing $gw/certs/$f" >&2; exit 1; }
done
command -v arm-none-eabi-gcc >/dev/null || { echo "arm-none-eabi-gcc not on PATH" >&2; exit 1; }
rm -rf "$out"
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
  -I"$v/mbedtls/include" -I"$gw/application" -I"$gw/certs"
  ${ARENA_BYTES:+-DTLS_ARENA_SIZE_BYTES=${ARENA_BYTES}U}
  -DBENCH_WITH_CONSOLE=${WITH_CONSOLE:-0} -DBENCH_WITH_CLOUD=${WITH_CLOUD:-1})

# Every product source of the three layers; the integration tests, bootloader and boot stub are not.
mapfile -t layer_srcs < <(find "$gw/application" "$gw/drivers" "$gw/middleware" -name '*.c' | sort)
project_srcs=(
  "$root/bench/stack_marks/main_bench_stack_marks.c"
  "${layer_srcs[@]}"
  "$gw/Src/freertos_hooks.c" "$gw/Src/stm32l4xx_it.c" "$gw/Src/syscalls.c" "$gw/Src/sysmem.c"
  "$v/stm32/iot-node/Drivers/CMSIS/Device/ST/STM32L4xx/Source/Templates/system_stm32l4xx.c"
  "$v/coreMQTT/source/core_mqtt.c" "$v/coreMQTT/source/core_mqtt_serializer.c"
  "$v/coreMQTT/source/core_mqtt_state.c"
  "$v/freertos-kernel/croutine.c" "$v/freertos-kernel/event_groups.c" "$v/freertos-kernel/list.c"
  "$v/freertos-kernel/queue.c" "$v/freertos-kernel/stream_buffer.c" "$v/freertos-kernel/tasks.c"
  "$v/freertos-kernel/timers.c" "$v/freertos-kernel/portable/GCC/ARM_CM4F/port.c")
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

# Bench linker script: the product one with the two RAM region sizes replaced.
sed -E "s/(RAM +\(xrw\) +: ORIGIN = 0x20000000, +LENGTH = )96K/\1${ram_kb}K/; \
        s/(SRAM2 +\(xrw\) +: ORIGIN = 0x10000000, +LENGTH = )32K/\1${sram2_kb}K/" \
    "$gw/STM32L475VGTX_FLASH.ld" > "$out/bench.ld"

if [[ "${WITH_CONSOLE:-0}" == "0" ]]; then
    # Task stacks into SRAM2 (cleared by the startup code with .bss.sram2). Object file names are the
    # source paths with '/' replaced by '_'.
    stacks=(
      "*bench_stack_marks_main_bench_stack_marks.c.o(.bss.s_boot_stack)"
      "*bench_stack_marks_main_bench_stack_marks.c.o(.bss.s_poller_stack)"
      "*bench_stack_marks_main_bench_stack_marks.c.o(.bss.s_stats_stack)"
      "*application_update_service_update_service.c.o(.bss.s_stack)"
      "*application_time_service_time_service.c.o(.bss.s_task_stack)"
      "*application_cloud_publisher_cloud_publisher.c.o(.bss.s_task_stack)"
      "*application_sensor_service_sensor_task.c.o(.bss.s_task_stack)"
      "*middleware_wifi_task_wifi_task.c.o(.bss.s_wifitask_stack)"
      "*middleware_logger_logger.c.o(.bss.s_drain_stack)"
      "*Src_freertos_hooks.c.o(.bss.s_idle_stack)"
      "*Src_freertos_hooks.c.o(.bss.uxTimerTaskStack)"
      # two small pools as well, so the main bank can hold a larger arena (option A in miniature)
      "*middleware_logger_logger.c.o(.bss.s_queue_storage)"
      "*middleware_wifi_task_wifi_task.c.o(.bss.g_pool)")
    ins=""
    for st in "${stacks[@]}"; do ins+="    ${st}\n"; done
    sed -i "s|    _ssram2_bss = .;|    _ssram2_bss = .;\n${ins}|" "$out/bench.ld"
fi

arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -x assembler-with-cpp \
  -c "$gw/Startup/startup_stm32l475vgtx.s" -o "$out/startup.o"
arm-none-eabi-gcc -o "$out/stack_marks.elf" "$out"/*.c.o "$out/startup.o" -mcpu=cortex-m4 \
  -T"$out/bench.ld" --specs=nosys.specs --specs=nano.specs -mfpu=fpv4-sp-d16 \
  -mfloat-abi=hard -mthumb -Wl,-Map="$out/stack_marks.map" -Wl,--gc-sections -static \
  -Wl,--start-group -lc -lm -Wl,--end-group
arm-none-eabi-size -A "$out/stack_marks.elf" | grep -E "^\.(text|data|bss|sram2|sram2_bss|user_heap_stack)|^Total"

if [[ "${1:-}" == "--flash" ]]; then
    p=/opt/st/stm32cubeide_2.2.0/plugins
    "$p"/com.st.stm32cube.ide.mcu.externaltools.openocd.linux64_*/tools/bin/openocd \
        -s "$(echo "$p"/com.st.stm32cube.ide.mcu.debug.openocd_*/resources/openocd/st_scripts)" \
        -f "$gw/gateway.cfg" -c "program $out/stack_marks.elf verify reset exit"
fi
