/**
 * @file lcd_hal_tick.c
 * @brief Non-weak overrides of HAL tick symbols, backed by DWT cycle counter.
 *
 * ST's BSP_LCD module calls HAL_Delay() internally. HAL_Init() is never
 * called in this project (FreeRTOS owns SysTick), so the BSP's only
 * HAL runtime dependency — the tick subsystem — must be satisfied by this
 * override.
 *
 * Pre-conditions:
 *   - DWT->CYCCNT enabled before lcd_init() by system_clock_enable_dwt().
 *   - SystemCoreClock set by SystemInit() and updated by system_clock_init_core().
 *
 * Build verification (link time):
 *   nm build/firmware.elf | grep -E "HAL_GetTick|HAL_Delay|HAL_InitTick"
 *   All three must resolve to lcd_hal_tick.o, not stm32f4xx_hal.o.
 *
 * NOT compiled in host-side Ceedling test builds — this file has no
 * corresponding header, so Ceedling does not auto-link it.
 */

#include "stm32f4xx_hal.h"

#include "core_cm4.h"

extern uint32_t SystemCoreClock;

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
    (void) TickPriority;
    return HAL_OK; /* SysTick owned by FreeRTOS; DWT already running. */
}

uint32_t HAL_GetTick(void)
{
    return DWT->CYCCNT / (SystemCoreClock / 1000U);
}

void HAL_Delay(uint32_t ms)
{
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < ms)
    {
        __NOP();
    }
}
