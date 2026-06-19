# Session Report — LcdDriver

**Date:** 2026-06-19
**Branch:** `feature/phase-4-lcd-driver`
**Companion:** `docs/lld/drivers/lcd-driver.md` (v0.5, Status: Pass H)

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| `firmware/field-device/drivers/lcd_driver/lcd_driver.h` | 134 | New — public API, test-only block |
| `firmware/field-device/drivers/lcd_driver/lcd_driver.c` | 194 | New — two-phase init, ISR, test reset |
| `firmware/field-device/drivers/lcd_driver/lcd_driver_internal.h` | 52 | New — TEST vs production BSP include split |
| `firmware/field-device/drivers/lcd_driver/lcd_hal_tick.c` | 47 | New — DWT-backed HAL tick overrides |
| `firmware/field-device/drivers/lcd_driver/bsp_shims/stm32469i_discovery_sdram.h` | 37 | New — SDRAM BSP shadow header |
| `firmware/field-device/drivers/lcd_driver/bsp_shims/stm32469i_discovery_sdram.c` | 43 | New — SDRAM BSP shim implementation |
| `firmware/field-device/drivers/lcd_driver/bsp_shims/README.md` | 22 | New — include-path ordering note |
| `vendor/stm32/f469-disco/Drivers/STM32F4xx_HAL_Driver/Inc/stm32f4xx_hal_conf.h` | 48 | New — project-authored; enables DSI, LTDC, RCC, CORTEX, GPIO modules only |
| `tests/field-device/drivers/lcd_driver/test_lcd_driver.c` | 267 | New — TC-LCD-001..010 |
| `firmware/field-device/integration-tests/lcd_driver/test_lcd_driver_main.c` | 298 | New — TC-LCD-011..015 |
| `scripts/check-hal-encapsulation.sh` | 28 | New — CI script; fails if non-lcd_driver firmware includes HAL/BSP |

### Reused / extended infrastructure

| File | Change |
|------|--------|
| `tests/mocks/stm32f469xx.h` | Extended: `LTDC_TypeDef`, `LTDC_IRQn`/`LTDC_ER_IRQn`, `RCC_AHB1ENR_DMA2DEN`, `RCC_APB2ENR_LTDCEN`, `RCC_APB2ENR_DSIEN` |
| `tests/mocks/stm32_cmsis_mock.c` | Extended: `g_mock_ltdc` storage and LTDC reset block in `stm32_cmsis_mock_reset()` |
| `tests/project.yml` | Extended: `:test_lcd_driver:` with `STM32F469xx` and `TEST` defines |
| `firmware/field-device/drivers/sdram_driver/sdram_driver.h` | Extended: `SDRAM_BASE_ADDR` macro and `sdram_is_ready()` declaration made public |
| `firmware/field-device/drivers/sdram_driver/sdram_driver.c` | Extended: `sdram_is_ready()` implementation added |
| `cppcheck-suppressions.txt` | Extended: `redundantAssignment:lcd_driver.c` for the volatile stage-sentinel pattern |

---

## Unit test results

| Test ID | Description | Result |
|---------|-------------|--------|
| TC-LCD-001 | `lcd_init` happy path: LTDC, DSI, DMA2D clock bits set in RCC | PASS |
| TC-LCD-002 | `lcd_init` calls `BSP_LCD_Init` with `LCD_ORIENTATION_LANDSCAPE` | PASS |
| TC-LCD-003 | `lcd_init` BSP failure → returns `LCD_ERR_INIT` | PASS |
| TC-LCD-004 | `lcd_init` BSP failure → `s_lcd_init_stage == LCD_STAGE_FAIL_BSP` | PASS |
| TC-LCD-005 | `lcd_init` success → `s_lcd_init_stage == LCD_STAGE_SUCCESS` | PASS |
| TC-LCD-006 | `lcd_get_framebuffer` returns `NULL` before `lcd_init`; returns `SDRAM_BASE_ADDR` after | PASS |
| TC-LCD-007 | `lcd_attach_frame_done` sets `LTDC->IER |= LTDC_IER_LIE`; NVIC priority 6 for `LTDC_IRQn` | PASS |
| TC-LCD-008 | `LTDC_IRQHandler` dispatches callback and clears `LTDC_ICR_CLIF` | PASS |
| TC-LCD-009 | `lcd_flush` returns `LCD_ERR_STATE` before init; calls `BSP_LCD_Reload` after | PASS |
| TC-LCD-010 | `lcd_attach_frame_done` returns `LCD_ERR_NULL` for null `cb`; `LCD_ERR_STATE` before init | PASS |

**Total:** 10 pass, 0 failed, 0 ignored.

---

## Integration test — expected behaviour

Flash `test_lcd_driver_main.c` to the STM32F469I-DISCO. The test calls `lcd_init()` before
`vTaskStartScheduler()` and `lcd_attach_frame_done()` from within `LcdTestTask`.

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | Blue solid fill covers entire 800×480 panel | `lcd_get_framebuffer()`, `lcd_flush()`, frame-done callback via LTDC ISR |
| 2 | 8-colour test pattern (RGB horizontal stripes) | Framebuffer DMA write path, LTDC layer config |
| 3 | Log: `[LCD] flush_cycle=100 ok` after 100 frame completions | LTDC line-interrupt latency; vTask notification round-trip ~16 ms per frame |
| 4 | Log: `[LCD] touch x=NNN y=NNN` on panel press | Touchscreen FT6x06 I2C integration not broken by LCD init |
| 5 | Log: `[LCD] forced BSP fail: err=1` | `LCD_ERR_INIT` returned when `BSP_LCD_Init` is pre-programmed to fail |

Requires: SDRAM driver initialised before `lcd_init()`; system clock and DWT running.

---

## Deviations from standard build rules

| # | Deviation | Justification |
|---|-----------|---------------|
| D1 | `lcd_driver` uses `stm32f4xx_hal.h` and `stm32469i_discovery_lcd.h` | Documented HAL/BSP exception in `hal-usage-patch.md`; no CMSIS register path exists for DSI/MIPI |
| D2 | `HAL_Init()` not called | SysTick owned by FreeRTOS; `lcd_hal_tick.c` provides DWT-backed tick overrides |
| D3 | `HAL_DeInit()` not called | Would reset all peripherals and disable SysTick |
| D4 | Actual ST HAL/BSP `.c` source not committed | Must be sourced from STM32CubeF4 SDK per companion §3.1–§3.4 and placed under `vendor/stm32/f469-disco/`; not required for Ceedling CI |

---

## Errors fixed during iteration

| # | Error | Root cause | Fix |
|---|-------|------------|-----|
| E1 | `implicit declaration of 'LTDC_IRQHandler'` (Ceedling link) | TC-LCD-008 called ISR directly; no prior declaration visible | Added `void LTDC_IRQHandler(void);` to `lcd_driver.h` |
| E2 | `constParameterPointer` (cppcheck) on `BSP_SDRAM_WriteData.pData` | `pData` passed as source to `memcpy`; not modified | Changed to `const uint32_t *pData` in header and implementation |
| E3 | `redundantAssignment` (cppcheck) on `s_lcd_init_stage` | Volatile sentinel written sequentially without reads between stages | Added `redundantAssignment:lcd_driver.c` to `cppcheck-suppressions.txt` |
| E4 | clang-format violations (enum alignment, assignment alignment) | Column-aligned `=` signs in enums and struct fields exceed project style | Resolved by `-Fix` pass; `.clang-format` has `AlignConsecutiveAssignments: false` |

---

## PR

**Title:** `feat(lcd_driver): add LcdDriver for STM32F469I-DISCO display subsystem`

**Description:**

Implements the LcdDriver module per companion `docs/lld/drivers/lcd-driver.md` v0.5 (Pass H).

### What changed
- `lcd_driver.c/h`: two-phase init (pre/post-scheduler), LTDC ISR, `lcd_flush()` with BSP_LCD_Reload
- `lcd_driver_internal.h`: splits BSP includes for test vs production builds
- `lcd_hal_tick.c`: DWT-backed `HAL_GetTick`/`HAL_Delay` overrides; not compiled in test
- `bsp_shims/`: SDRAM BSP shadow; shadows ST's header to prevent FMC re-init
- `vendor/stm32/`: project-authored `stm32f4xx_hal_conf.h` enabling only required HAL modules
- `sdram_driver.h`: `SDRAM_BASE_ADDR` and `sdram_is_ready()` made public (single source of truth)
- `stm32f469xx.h` mock: LTDC_TypeDef, LTDC_IRQn/LTDC_ER_IRQn, DMA2D/LTDC/DSI RCC bits
- 10 unit tests (TC-LCD-001..010), 5 integration tests (TC-LCD-011..015)
- `scripts/check-hal-encapsulation.sh`: CI enforcement of HAL isolation exception

### HAL exception
This is the only module permitted to include STM32 HAL/BSP headers.
Documented in `hal-usage-patch.md`. `HAL_Init()` and `HAL_DeInit()` are never called.

### Intentional bug (for training)
See `docs/dev-tools/lcd_driver/bug-log.md`.
