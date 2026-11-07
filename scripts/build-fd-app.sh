#!/usr/bin/env bash
# Build the Field Device application image (firmware/field-device/Src/main.c) without CubeIDE and
# without touching .cproject (standing rule: Claude never edits it; Src/main.c is excluded there).
#
# Links every Field Device application, driver and middleware source (never the integration tests)
# with the flags of the generated Debug makefiles (-O0, -pedantic-errors) and the product linker
# script. The link fails if the image does not fit the chip.
# Output: firmware/field-device/Debug/app/ (gitignored).
#
# usage: scripts/build-fd-app.sh [--flash]
#   --flash   program the STM32F469I-DISCO with openocd (ST-LINK on the board) and reset it
#   MAIN      source file to use instead of Src/main.c, e.g. an integration-tests/<module>/*main.c bring-up
#             harness (it is built with all product sources; output stays in Debug/app)
#   GUI       1 = also build the display stack: LcdUi, LcdDriver, SdramDriver, TouchscreenDriver and
#             GraphicsLibrary, with LVGL (vendor/lvgl, a submodule), ST's BSP/HAL and the fonts. The
#             product Src/main.c does not use them yet, so the default image is built without them,
#             and CubeIDE's Debug configuration excludes them too. Pair it with MAIN=
#             firmware/field-device/integration-tests/<module>/test_<module>_main.c.
#   ST_SERIAL ST-LINK serial number to use when several boards are attached (ls /dev/serial/by-id)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fd="$root/firmware/field-device"
v="$root/vendor"
out="$fd/Debug/app"

command -v arm-none-eabi-gcc >/dev/null || { echo "arm-none-eabi-gcc not on PATH" >&2; exit 1; }
[[ -f "$root/firmware/shared/firmware_version.h" ]] || \
    python3 "$root/scripts/gen-version.py" "$root/firmware/shared/firmware_version.h.in" \
        "$root/firmware/shared/firmware_version.h"
rm -rf "$out"
mkdir -p "$out"

cmsis="$v/stm32/f469-disco/Drivers/CMSIS"
common=(-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -std=gnu11 -g3 -Wall -pedantic
  -pedantic-errors -ffunction-sections -fdata-sections --specs=nano.specs
  -DDEBUG -DLV_CONF_INCLUDE_SIMPLE -DSTM32 -DSTM32F469NIHx -DSTM32F4 -DSTM32F469I_DISCO -DSTM32F469xx
  -DBOARD_FIELD_DEVICE
  -I"$fd/Inc" -I"$fd/application" -I"$fd/drivers" -I"$fd/middleware"
  -I"$v/freertos-kernel/include" -I"$v/freertos-kernel/portable/GCC/ARM_CM4F"
  -I"$cmsis/Device/ST/STM32F4xx/Include" -I"$cmsis/Core/Include" -I"$fd/../shared"
  ${EXTRA_CFLAGS:-})

gui_excl=(-not -path '*/lcd_ui/*' -not -path '*/lcd_driver/*' -not -path '*/sdram_driver/*'
  -not -path '*/touchscreen_driver/*' -not -path '*/graphics_library/*')
if [[ "${GUI:-0}" == 1 ]]; then
    [[ -f "$v/lvgl/lvgl.h" ]] || { echo "vendor/lvgl is empty: git submodule update --init vendor/lvgl" >&2; exit 1; }
    mapfile -t layer_srcs < <(find "$fd/application" "$fd/drivers" "$fd/middleware" -name '*.c' | sort)
else
    mapfile -t layer_srcs < <(find "$fd/application" "$fd/drivers" "$fd/middleware" -name '*.c' "${gui_excl[@]}" | sort)
fi
project_srcs=(
  "${MAIN:-$fd/Src/main.c}" "${layer_srcs[@]}"
  "$fd/Src/freertos_hooks.c" "$fd/Src/syscalls.c" "$fd/Src/sysmem.c" "$fd/Src/system_clock.c"
  "$cmsis/Device/ST/STM32F4xx/Source/Templates/system_stm32f4xx.c"
  "$v/freertos-kernel/croutine.c" "$v/freertos-kernel/event_groups.c" "$v/freertos-kernel/list.c"
  "$v/freertos-kernel/queue.c" "$v/freertos-kernel/stream_buffer.c" "$v/freertos-kernel/tasks.c"
  "$v/freertos-kernel/timers.c" "$v/freertos-kernel/portable/GCC/ARM_CM4F/port.c")

# The display stack: project sources are built with the strict flags above, ST's and LVGL's sources
# (third party, not pedantic-clean) with the same flags minus -pedantic and with their include paths.
vendor_srcs=()
gui_inc=()
if [[ "${GUI:-0}" == 1 ]]; then
    f4="$v/stm32/f469-disco"
    gui_inc=(-DUSE_HAL_DRIVER -DUSE_STM32469I_DISCOVERY
      -I"$fd/drivers/lcd_driver/bsp_shims"            # first: shadows ST's SDRAM BSP
      -I"$f4/Drivers/BSP/STM32469I-Discovery" -I"$f4/Drivers/BSP/Components/Common"
      -I"$f4/Drivers/BSP/Components/otm8009a" -I"$f4/Drivers/BSP/Components/nt35510"
      -I"$f4/Drivers/STM32F4xx_HAL_Driver/Inc" -I"$f4/Utilities/Fonts"
      -I"$fd/middleware/graphics_library" -I"$v/lvgl" -I"$v/lvgl/src")
    mapfile -t vendor_srcs < <(
      find "$v/lvgl/src" -name '*.c' | sort
      find "$f4/Drivers/STM32F4xx_HAL_Driver/Src" -name '*.c' ! -name '*_template.c' | sort
      echo "$f4/Drivers/BSP/STM32469I-Discovery/stm32469i_discovery.c" # OTM8009A_IO_Delay, NT35510_IO_Delay
      echo "$f4/Drivers/BSP/STM32469I-Discovery/stm32469i_discovery_lcd.c"
      echo "$f4/Drivers/BSP/Components/otm8009a/otm8009a.c"
      echo "$f4/Drivers/BSP/Components/nt35510/nt35510.c")
    # The fonts (Utilities/Fonts/font*.c) are #included by stm32469i_discovery_lcd.c itself.
    common+=("${gui_inc[@]}")
fi
vendor_flags=()
for f in "${common[@]}"; do [[ "$f" == -pedantic || "$f" == -pedantic-errors ]] || vendor_flags+=("$f"); done

fail=0
pids=()
for src in "${project_srcs[@]}" "${vendor_srcs[@]}"; do
    obj="$out/$(echo "${src#"$root"/}" | tr '/' '_').o"
    flags=("${common[@]}")
    [[ "$src" == "$v"/* && "$src" != "$v/freertos-kernel/"* && "$src" != "$cmsis"/* ]] && flags=("${vendor_flags[@]}")
    ( arm-none-eabi-gcc "${flags[@]}" -O0 -c "$src" -o "$obj" 2> "$obj.log" \
        || { cat "$obj.log" >&2; echo "FAILED: $src" >&2; exit 1; } ) &
    pids+=($!)
    if (( ${#pids[@]} >= $(nproc) )); then
        wait "${pids[0]}" || fail=1
        pids=("${pids[@]:1}")
    fi
done
for p in "${pids[@]}"; do wait "$p" || fail=1; done
(( fail == 0 )) || { echo "compile errors above" >&2; exit 1; }

arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -x assembler-with-cpp \
  -c "$fd/Startup/startup_stm32f469nihx.s" -o "$out/startup.o"
arm-none-eabi-gcc -o "$out/field_device_app.elf" "$out"/*.c.o "$out/startup.o" -mcpu=cortex-m4 \
  -T"$fd/STM32F469NIHX_FLASH.ld" -Wl,--print-memory-usage --specs=nosys.specs --specs=nano.specs \
  -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -Wl,-Map="$out/field_device_app.map" \
  -Wl,--gc-sections -static -Wl,--start-group -lc -lm -Wl,--end-group
arm-none-eabi-size -A "$out/field_device_app.elf" | grep -E "^\.(text|data|bss)|^Total"

if [[ "${1:-}" == "--flash" ]]; then
    p=/opt/st/stm32cubeide_2.2.0/plugins
    "$p"/com.st.stm32cube.ide.mcu.externaltools.openocd.linux64_*/tools/bin/openocd \
        -s "$(echo "$p"/com.st.stm32cube.ide.mcu.debug.openocd_*/resources/openocd/st_scripts)" \
        -f interface/stlink-dap.cfg ${ST_SERIAL:+-c "adapter serial $ST_SERIAL"} \
        -c "transport select dapdirect_swd" -f target/stm32f4x.cfg -c "program $out/field_device_app.elf verify reset exit"
fi
