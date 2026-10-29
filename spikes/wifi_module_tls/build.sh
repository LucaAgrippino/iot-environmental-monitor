#!/usr/bin/env bash
# Build the wifi_module_tls spike into spikes/wifi_module_tls/build/spike.elf.
# No CubeIDE and no .cproject change: the source list and flags are the ones the generated Debug
# makefiles use for the wifi_driver bring-up image (logger, drivers, FreeRTOS, startup, linker).
# Needs firmware/gateway/certs/spike_config.h (run gen_spike_config.sh first).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
gw="$root/firmware/gateway"
v="$root/vendor"
out="$here/build"
[[ -f "$gw/certs/spike_config.h" ]] || { echo "run gen_spike_config.sh first" >&2; exit 1; }
mkdir -p "$out"

cflags=(-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -std=gnu11 -Os -g3 -Wall -Wextra
  -ffunction-sections -fdata-sections --specs=nano.specs
  -DDEBUG -DSTM32L4 -DSTM32 -DB_L475E_IOT01A2 -DSTM32L475VGTx -DSTM32L475xx -DWIFI_SPIKE_RAW_AT
  "-DWIFI_FIRMWARE_VERSION=\"${SPIKE_MODULE_FW:-C3.5.2.3}\""
  -I"$v/freertos-kernel/portable/GCC/ARM_CM4F" -I"$v/freertos-kernel/include"
  -I"$v/stm32/iot-node/Drivers/CMSIS/Core/Include"
  -I"$v/stm32/iot-node/Drivers/CMSIS/Device/ST/STM32L4xx/Include"
  -I"$gw/../shared" -I"$gw/drivers" -I"$gw/Inc" -I"$gw/middleware" -I"$gw/application" -I"$gw/certs")

srcs=(
  "$here/main_spike_wifi_tls.c" "$here/stubs.c"
  "$gw/drivers/cpu/cpu.c" "$gw/drivers/cpu/cpu_fault.c" "$gw/drivers/cpu/panic_symbols.c"
  "$gw/drivers/cpu/panic_symbols_data.c" "$gw/drivers/debug_uart/debug_uart.c"
  "$gw/drivers/exti/exti_driver.c" "$gw/drivers/gpio/gpio_driver.c" "$gw/drivers/rtc/rtc.c"
  "$gw/drivers/spi/spi.c" "$gw/drivers/wifi_driver/wifi_driver.c" "$gw/middleware/logger/logger.c"
  "$gw/Src/freertos_hooks.c" "$gw/Src/stm32l4xx_it.c" "$gw/Src/syscalls.c" "$gw/Src/sysmem.c"
  "$v/stm32/iot-node/Drivers/CMSIS/Device/ST/STM32L4xx/Source/Templates/system_stm32l4xx.c"
  "$v/freertos-kernel/croutine.c" "$v/freertos-kernel/event_groups.c" "$v/freertos-kernel/list.c"
  "$v/freertos-kernel/queue.c" "$v/freertos-kernel/stream_buffer.c" "$v/freertos-kernel/tasks.c"
  "$v/freertos-kernel/timers.c" "$v/freertos-kernel/portable/GCC/ARM_CM4F/port.c")

objs=()
for s in "${srcs[@]}"; do
  o="$out/$(echo "${s#"$root"/}" | tr '/' '_').o"
  arm-none-eabi-gcc "${cflags[@]}" -c "$s" -o "$o"
  objs+=("$o")
done
arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -x assembler-with-cpp \
  -c "$gw/Startup/startup_stm32l475vgtx.s" -o "$out/startup.o"
arm-none-eabi-gcc -o "$out/spike.elf" "${objs[@]}" "$out/startup.o" -mcpu=cortex-m4 \
  -T"$gw/STM32L475VGTX_FLASH.ld" --specs=nosys.specs --specs=nano.specs -mfpu=fpv4-sp-d16 \
  -mfloat-abi=hard -mthumb -Wl,-Map="$out/spike.map" -Wl,--gc-sections -static \
  -Wl,--start-group -lc -lm -Wl,--end-group
arm-none-eabi-size "$out/spike.elf"
