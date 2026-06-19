# bsp_shims — Include Path Order and Rationale

## Purpose

`stm32469i_discovery_lcd.c` (from the ST BSP) includes
`stm32469i_discovery_sdram.h` and may call `BSP_SDRAM_Init` during LCD
framebuffer setup.  The project already owns SDRAM initialisation in
`SdramDriver`; allowing the BSP to repeat it would corrupt the running
FMC controller.

This folder provides a **shadow implementation** that satisfies the BSP's
SDRAM contract by delegating to `SdramDriver`.

## Include path order in CubeIDE (load-bearing)

```
1. firmware/field-device/drivers/lcd_driver/bsp_shims/      ← MUST be first
2. vendor/stm32/f469-disco/Drivers/BSP/STM32469I-Discovery/
3. vendor/stm32/f469-disco/Drivers/BSP/Components/otm8009a/
4. vendor/stm32/f469-disco/Drivers/BSP/Components/Common/
5. vendor/stm32/f469-disco/Drivers/STM32F4xx_HAL_Driver/Inc/
6. vendor/stm32/f469-disco/Drivers/CMSIS/Device/ST/STM32F4xx/Include/
7. vendor/stm32/f469-disco/Drivers/CMSIS/Core/Include/
```

**Do not reorder.** Because `bsp_shims/` appears first, the preprocessor
resolves `#include "stm32469i_discovery_sdram.h"` to this shim before
reaching `vendor/stm32/f469-disco/Drivers/BSP/STM32469I-Discovery/`.
ST's `stm32469i_discovery_sdram.{c,h}` are intentionally absent from the
vendor tree (companion §3.4).  Reordering this list would expose ST's
file if it were ever accidentally vendored and silently change build
behaviour.

## Files

| File | Role |
|------|------|
| `stm32469i_discovery_sdram.h` | Shadow header; re-exports `SDRAM_BASE_ADDR` as `LCD_FB_START_ADDRESS` |
| `stm32469i_discovery_sdram.c` | Shadow implementation; `BSP_SDRAM_Init` calls `sdram_is_ready()` |

## Rule: everything under `vendor/` is verbatim ST source

This folder lives **outside** `vendor/` so the audit invariant is
preserved.  Any file differing from the SDK must be in a `bsp_shims/`
folder, never directly under `vendor/`.
