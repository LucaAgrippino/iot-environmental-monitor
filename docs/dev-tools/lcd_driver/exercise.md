# Exercise — LcdDriver

**Topic:** HAL isolation, two-phase init, ISR-to-task signalling, volatile debug sentinels
**Level:** Intermediate embedded C
**Time:** 45–60 minutes

---

## Background

The STM32F469I-DISCO display subsystem is unusual: the MIPI-DSI host and the OTM8009A
panel controller cannot be configured at the CMSIS register level without re-implementing
several thousand lines of ST driver code. The project therefore grants a one-off exception
allowing `lcd_driver.c` to use `stm32f4xx_hal.h` and `stm32469i_discovery_lcd.h` — the
only module in the firmware permitted to do so.

Your task is to implement the driver so it:
1. Provides a HAL-free public API (`lcd_driver.h`)
2. Passes the 10 unit tests in `test_lcd_driver.c`
3. Compiles cleanly under cppcheck and clang-format

---

## Given files

You are given (read-only):

| File | Purpose |
|------|---------|
| `lcd_driver.h` | Complete public API, `LCD_TEST_VISIBLE` macro, `lcd_init_stage_t` enum |
| `lcd_driver_internal.h` | BSP split: forward declarations in `#ifdef TEST`, real includes in `#else` |
| `tests/mocks/stm32f469xx.h` | CMSIS register mock (RCC, NVIC, LTDC) |
| `tests/mocks/stm32_cmsis_mock.c` | Mock storage and `stm32_cmsis_mock_reset()` |
| `tests/field-device/drivers/lcd_driver/test_lcd_driver.c` | 10 unit tests (scaffold, some assertions removed) |

You must implement:

| File | Task |
|------|------|
| `lcd_driver.c` | Full implementation of all 4 public API functions + ISR + `lcd_driver_reset()` |
| `lcd_hal_tick.c` | `HAL_InitTick`, `HAL_GetTick` (DWT-based), `HAL_Delay` |
| `bsp_shims/stm32469i_discovery_sdram.h` | SDRAM BSP shadow header |
| `bsp_shims/stm32469i_discovery_sdram.c` | SDRAM BSP shim using `memcpy` against the memory-mapped region |

---

## Questions

Answer these before writing any code.

**Q1.** `LTDC_IRQHandler` is declared in `lcd_driver.h` (public header), not just in `lcd_driver.c`.
Why does the unit test require this? What would happen if the declaration were only in the `.c` file?

**Q2.** The `LCD_TEST_VISIBLE` macro expands to nothing in test builds and to `static` in production.
Explain what problem this solves. What would break if you simply made `s_lcd` and `s_lcd_init_stage`
`static` unconditionally?

**Q3.** `lcd_hal_tick.c` has no corresponding `lcd_hal_tick.h`. Ceedling discovers which `.c` files
to compile by looking for headers included in the test TU. What is the consequence of the missing
header for the test build, and is this intentional?

**Q4.** `lcd_init()` enables LTDC, DSI, and DMA2D peripheral clocks before calling `BSP_LCD_Init`.
What prevents `BSP_LCD_Init` from enabling these clocks itself (potentially double-toggling the bits)?

**Q5.** The integration test uses `vTaskNotifyGiveFromISR` + `ulTaskNotifyTake` to synchronise the
LCD flush with `LcdTestTask`. Why is a FreeRTOS notification preferred over a plain global flag
for this purpose?

---

## Implementation hints

- `lcd_init()` must write `s_lcd_init_stage` at each stage transition — the tests assert the
  exact value after a BSP failure and after a successful init.
- `lcd_attach_frame_done()` must guard against two error conditions: not initialised
  (`LCD_ERR_STATE`) and null callback (`LCD_ERR_NULL`). Order matters — check init state first.
- In `LTDC_IRQHandler`, check `LTDC->ISR & LTDC_ISR_LIF` before dispatching the callback.
  Clear the interrupt flag by writing to `LTDC->ICR` (write-to-clear register).
- The BSP shim `BSP_SDRAM_Init` is never called by the LCD path — it exists for completeness.
  Return `SDRAM_OK` immediately.

---

## Model solution sketch

```c
/* lcd_driver.c — key excerpts */

lcd_err_t lcd_init(void)
{
    s_lcd_init_stage = LCD_STAGE_RESET;

    RCC->APB2ENR |= RCC_APB2ENR_LTDCEN | RCC_APB2ENR_DSIEN;
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA2DEN;
    s_lcd_init_stage = LCD_STAGE_PERIPH_CLOCKS;

    s_lcd_init_stage = LCD_STAGE_BSP_INIT;
    if (BSP_LCD_Init(LCD_ORIENTATION_LANDSCAPE) != 0U)
    {
        s_lcd_init_stage = LCD_STAGE_FAIL_BSP;
        return LCD_ERR_INIT;
    }
    s_lcd_init_stage = LCD_STAGE_BSP_DONE;

    s_lcd.framebuffer = (uint16_t *)(uintptr_t)sdram_get_base_addr();
    s_lcd_init_stage = LCD_STAGE_FB_CAPTURED;

    s_lcd.initialised = true;
    s_lcd_init_stage = LCD_STAGE_SUCCESS;
    return LCD_ERR_OK;
}

void LTDC_IRQHandler(void)
{
    if (LTDC->ISR & LTDC_ISR_LIF)
    {
        LTDC->ICR = LTDC_ICR_CLIF;
        if (s_lcd.frame_done_cb != NULL)
        {
            s_lcd.frame_done_cb(s_lcd.frame_done_ctx);
        }
    }
}
```

---

## Marking guide

| Criterion | Marks |
|-----------|-------|
| All 10 unit tests pass (`ceedling test:test_lcd_driver`) | 40 |
| cppcheck exits 0 (no findings beyond allowed suppressions) | 20 |
| clang-format exits 0 (no violations) | 10 |
| Q1–Q5 answers correct and concise | 20 |
| `lcd_hal_tick.c` uses DWT not SysTick; no `HAL_Init` / `HAL_DeInit` call | 10 |
| **Total** | **100** |

### Model answers (Q1–Q5)

**A1.** The test TU calls `LTDC_IRQHandler()` directly (TC-LCD-008) to simulate an ISR without a
real interrupt. The linker requires an in-scope declaration at the point of the call. If the
declaration is only in `lcd_driver.c`, it is not visible to `test_lcd_driver.c`, causing an
"implicit declaration" compiler warning (or error with `-Werror`). The public header is the right
place because the pattern is established by `touchscreen_driver.h` (which declares `EXTI9_5_IRQHandler`).

**A2.** Ceedling requires external linkage (no `static`) for symbols asserted in tests: `s_lcd_init_stage`
and `s_lcd` must be visible across translation unit boundaries. Making them `static` unconditionally
would hide them from the test TU even with `extern volatile lcd_init_stage_t s_lcd_init_stage;` in the
header, because `static` limits linkage to the defining TU. `LCD_TEST_VISIBLE` removes `static` in test
builds while keeping it in production to avoid polluting the global symbol namespace.

**A3.** Ceedling auto-links `.c` files whose corresponding `.h` is included by a test TU or the module
under test. Because `lcd_hal_tick.h` does not exist, Ceedling never adds `lcd_hal_tick.c` to the test
build — intentional. `lcd_hal_tick.c` includes `stm32f4xx_hal.h` and `core_cm4.h`, which are not
present in the test environment. Excluding it prevents a cascade of missing-header errors.

**A4.** The companion specifies that `lcd_driver.c` enables the clocks explicitly before delegating to
the BSP, so the BSP's internal `MspInit` callbacks do not need to. `stm32f4xx_hal_conf.h` configures
which HAL modules are compiled in; `HAL_RCC` is enabled so `__HAL_RCC_LTDC_CLK_ENABLE()` macros exist,
but the project convention is to use CMSIS RCC register writes directly (single source of truth) rather
than HAL macros, which are defined as OR-assign and are therefore idempotent anyway.

**A5.** A task notification is a direct kernel signal: `ulTaskNotifyTake(pdTRUE, portMAX_DELAY)` puts
`LcdTestTask` into the blocked state until the ISR delivers exactly one notification per flush, with
bounded latency and no busy-wait. A plain global flag would require either polling (wasting CPU in a
tight loop) or a delay (imprecise and fragile). The notification also integrates with FreeRTOS tracing
and correctly handles the case where the ISR fires before the task reaches the wait point.
