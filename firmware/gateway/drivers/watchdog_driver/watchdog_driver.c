/**
 * @file watchdog_driver.c
 * @brief WatchdogDriver implementation (Gateway, STM32L475 IWDG).
 *
 * @see docs/lld/drivers/watchdog-driver.md
 */

#include "watchdog_driver.h"

#include "stm32l475xx.h"

#define IWDG_KEY_UNLOCK (0x5555U)
#define IWDG_KEY_START (0xCCCCU)
#define IWDG_KEY_REFRESH (0xAAAAU)
#define IWDG_PRESCALER_256 (6U)
#define IWDG_TICK_HZ (125U) /**< 32 kHz LSI / 256 */
#define IWDG_RELOAD_MAX (4095U)
#define IWDG_SR_WAIT_LOOPS (200000U)

static bool s_started;

watchdog_err_t watchdog_start(uint32_t timeout_ms)
{
    if ((timeout_ms < WATCHDOG_TIMEOUT_MIN_MS) || (timeout_ms > WATCHDOG_TIMEOUT_MAX_MS))
    {
        return WATCHDOG_ERR_RANGE;
    }
    if (s_started)
    {
        return WATCHDOG_OK;
    }

    uint32_t reload = (timeout_ms * IWDG_TICK_HZ) / 1000U;

    if (reload < 1U)
    {
        reload = 1U;
    }
    if (reload > IWDG_RELOAD_MAX)
    {
        reload = IWDG_RELOAD_MAX;
    }

    /* A halted debugger must not reset the board (bench work). */
    DBGMCU->APB1FZR1 |= DBGMCU_APB1FZR1_DBG_IWDG_STOP;

    IWDG->KR = IWDG_KEY_START; /* starts the LSI and the counter */
    IWDG->KR = IWDG_KEY_UNLOCK;
    IWDG->PR = IWDG_PRESCALER_256;
    IWDG->RLR = reload;

    for (uint32_t i = 0U; (i < IWDG_SR_WAIT_LOOPS) && ((IWDG->SR & 0x3U) != 0U); i++)
    {
        /* wait for PVU and RVU to clear (the new values are latched) */
    }

    IWDG->KR = IWDG_KEY_REFRESH;
    s_started = true;
    return WATCHDOG_OK;
}

void watchdog_kick(void)
{
    if (s_started)
    {
        IWDG->KR = IWDG_KEY_REFRESH;
    }
}

bool watchdog_is_running(void)
{
    return s_started;
}

#ifdef TEST
void watchdog_reset_for_test(void)
{
    s_started = false;
}
#endif
