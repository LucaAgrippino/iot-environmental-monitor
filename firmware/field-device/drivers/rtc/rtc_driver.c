/**
 * @file rtc_driver.c
 * @brief STM32F469 implementation of RtcDriver.
 *
 * Backup-domain RTC clocked from LSE 32.768 kHz. Implements IRtc.
 * See docs/lld/drivers/rtc-driver.md for the full design.
 *
 * Layout follows the per-driver convention established by DebugUartDriver:
 *   §1 Includes and configuration
 *   §2 Private state
 *   §3 Forward declarations
 *   §4 BCD helpers (test-visible)
 *   §5 Default tick source
 *   §6 Internal helpers (DBP unlock, clock select, WPR unlock/lock)
 *   §7 Public API
 *   §8 Singleton vtable
 *   §9 Test-only hooks
 */

#include "rtc_driver.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "stm32f469xx.h"

/* ===================================================================== */
/* §1. Configuration                                                     */
/* ===================================================================== */

#if defined(STM32F469xx)
#define RTC_BACKUP_MAX_IDX RTC_BACKUP_MAX_IDX_F469
#elif defined(STM32L475xx)
#define RTC_BACKUP_MAX_IDX RTC_BACKUP_MAX_IDX_L475
#else
#error "Define STM32F469xx or STM32L475xx for RtcDriver."
#endif

/** RTC default DR value — 2000-01-01, weekday = Monday (RM0386 reset value). */
#define RTC_DEFAULT_DR (0x00002101UL)

/** RTC default TR value — 00:00:00. */
#define RTC_DEFAULT_TR (0x00000000UL)

/** Prescaler — PREDIV_A=127, PREDIV_S=255 → 1 Hz tick at 32.768 kHz LSE. */
#define RTC_PRESCALER_VALUE ((127UL << 16) | 255UL)

/** Timeout budget for INITF / RSF polls (rtc-driver.md §4.2). */
#define RTC_TIMEOUT_MS (2U)

/**
 * @brief Tick-to-ms scale (RTCD-O1).
 *
 * The default tick source returns 0, so this scale is effectively unused at
 * runtime — deadlines never fire and polls are unbounded. Tests inject their
 * own tick sources with arbitrary units. Pinned to a meaningful value once
 * clock-config.md lands and SysTick frequency is known.
 */
#define RTC_MS_TO_TICKS(ms) ((uint32_t) (ms))

/* Test-visibility shim: BCD helpers are exposed (non-static) under TEST so
 * the host test TU can call them directly without a separate header. */
#ifdef TEST
#define RTC_TEST_VISIBLE
#else
#define RTC_TEST_VISIBLE static
#endif

/* ===================================================================== */
/* §2. Private state                                                     */
/* ===================================================================== */

typedef struct
{
    bool initialised;
    bool backup_valid;
    rtc_tick_source_fn tick;
} rtc_driver_t;

static rtc_driver_t s_rtc;

/* ===================================================================== */
/* §3. Forward declarations                                              */
/* ===================================================================== */

static uint32_t default_tick_source(void);
static void backup_domain_unlock(void);
static rtc_err_t rtc_clock_select_and_enable(void);
static void wpr_unlock(void);
static void wpr_lock(void);

/* ===================================================================== */
/* §4. BCD helpers (test-visible)                                        */
/* ===================================================================== */

RTC_TEST_VISIBLE uint8_t rtc_bcd_to_bin(uint8_t bcd)
{
    return (uint8_t) (((bcd >> 4) * 10U) + (bcd & 0x0FU));
}

RTC_TEST_VISIBLE uint8_t rtc_bin_to_bcd(uint8_t bin)
{
    return (uint8_t) (((bin / 10U) << 4) | (bin % 10U));
}

/* ===================================================================== */
/* §5. Default tick source                                               */
/* ===================================================================== */

/*
 * Returns 0 unconditionally. With this default, deadline = (0 + n) and
 * subsequent calls also return 0, so the deadline test (current >= deadline)
 * never trips and poll loops are unbounded — matching DebugUartDriver's
 * "no-timeout default". Production code calls rtc_set_tick_source() during
 * startup with a real SysTick-based ms reader. See RTCD-O1.
 */
static uint32_t default_tick_source(void)
{
    return 0U;
}

/* ===================================================================== */
/* §6. Internal helpers                                                  */
/* ===================================================================== */

static void backup_domain_unlock(void)
{
#if defined(STM32F469xx)
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void) RCC->APB1ENR; /* read-back ensures the write took effect */
    PWR->CR |= PWR_CR_DBP;
#elif defined(STM32L475xx)
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    (void) RCC->APB1ENR1;
    PWR->CR1 |= PWR_CR1_DBP;
#endif
}

static rtc_err_t rtc_clock_select_and_enable(void)
{
    /* Pre-condition: LSE has been started by system clock configuration. */
    if ((RCC->BDCR & RCC_BDCR_LSERDY) == 0U)
    {
        return RTC_ERR_LSE_NOT_READY;
    }

    /* RTCSEL is write-once after a backup-domain reset. If a prior boot
     * locked it to HSE or LSI, the only way to change is BDRST — which
     * wipes every backup register, including TimeProvider's sync flag.
     * Treat the wrong-RTCSEL case as a clock-config fault (RTCD-D14). */
    uint32_t bdcr = RCC->BDCR;
    uint32_t rtcsel = bdcr & RCC_BDCR_RTCSEL;

    if (rtcsel == 0U)
    {
        bdcr |= RCC_BDCR_RTCSEL_0; /* first boot — select LSE (0b01) */
    }
    else if (rtcsel != RCC_BDCR_RTCSEL_0)
    {
        return RTC_ERR_LSE_NOT_READY; /* already locked to non-LSE source */
    }

    bdcr |= RCC_BDCR_RTCEN;
    RCC->BDCR = bdcr;
    return RTC_OK;
}

static void wpr_unlock(void)
{
    RTC->WPR = 0xCAU;
    RTC->WPR = 0x53U;
}

static void wpr_lock(void)
{
    RTC->WPR = 0xFFU; /* any invalid key re-enables protection */
}

/* ===================================================================== */
/* §7. Public API                                                        */
/* ===================================================================== */

rtc_err_t rtc_init(void)
{
    if (s_rtc.initialised)
    {
        return RTC_OK; /* idempotent — RTCD-D10 */
    }

    if (s_rtc.tick == NULL)
    {
        s_rtc.tick = &default_tick_source;
    }

    backup_domain_unlock();

    rtc_err_t err = rtc_clock_select_and_enable();
    if (err != RTC_OK)
    {
        return err; /* no rollback — RTCD-D15 */
    }

    s_rtc.backup_valid = ((RTC->ISR & RTC_ISR_INITS) != 0U);

    if (!s_rtc.backup_valid)
    {
        wpr_unlock();

        RTC->ISR |= RTC_ISR_INIT;
        uint32_t deadline = s_rtc.tick() + RTC_MS_TO_TICKS(RTC_TIMEOUT_MS);
        while ((RTC->ISR & RTC_ISR_INITF) == 0U)
        {
            if (s_rtc.tick() >= deadline)
            {
                wpr_lock();
                return RTC_ERR_INIT_TIMEOUT;
            }
        }

        RTC->CR &= ~RTC_CR_FMT; /* 24-hour format */
        RTC->PRER = RTC_PRESCALER_VALUE;
        RTC->TR = RTC_DEFAULT_TR;
        RTC->DR = RTC_DEFAULT_DR;

        RTC->ISR &= ~RTC_ISR_INIT;
        deadline = s_rtc.tick() + RTC_MS_TO_TICKS(RTC_TIMEOUT_MS);
        while ((RTC->ISR & RTC_ISR_RSF) == 0U)
        {
            if (s_rtc.tick() >= deadline)
            {
                wpr_lock();
                return RTC_ERR_SYNC_TIMEOUT;
            }
        }

        wpr_lock();
    }

    s_rtc.initialised = true;
    return RTC_OK;
}

rtc_err_t rtc_get_time(rtc_datetime_t *dt)
{
    assert(s_rtc.initialised);

    if (dt == NULL)
    {
        return RTC_ERR_NULL_ARG;
    }

    (void) memset(dt, 0, sizeof(*dt)); /* zero on error-return path */

    uint32_t deadline = s_rtc.tick() + RTC_MS_TO_TICKS(RTC_TIMEOUT_MS);
    while ((RTC->ISR & RTC_ISR_RSF) == 0U)
    {
        if (s_rtc.tick() >= deadline)
        {
            return RTC_ERR_SYNC_TIMEOUT;
        }
    }

    /* RM0386 §27.3.6 / RM0351 §38.4.4: read TR first, then DR. The TR read
     * locks the calendar shadow until DR is read — coherent snapshot. */
    uint32_t tr = RTC->TR;
    uint32_t dr = RTC->DR;

    dt->second = rtc_bcd_to_bin((uint8_t) (tr & 0x7FU));
    dt->minute = rtc_bcd_to_bin((uint8_t) ((tr >> 8) & 0x7FU));
    dt->hour = rtc_bcd_to_bin((uint8_t) ((tr >> 16) & 0x3FU));
    dt->day = rtc_bcd_to_bin((uint8_t) (dr & 0x3FU));
    dt->month = rtc_bcd_to_bin((uint8_t) ((dr >> 8) & 0x1FU));
    dt->year = (uint16_t) (2000U + rtc_bcd_to_bin((uint8_t) ((dr >> 16) & 0xFFU)));

    return RTC_OK;
}

rtc_err_t rtc_set_time(const rtc_datetime_t *dt)
{
    assert(s_rtc.initialised);

    if (dt == NULL)
    {
        return RTC_ERR_NULL_ARG;
    }

    wpr_unlock();

    RTC->ISR |= RTC_ISR_INIT;
    uint32_t deadline = s_rtc.tick() + RTC_MS_TO_TICKS(RTC_TIMEOUT_MS);
    while ((RTC->ISR & RTC_ISR_INITF) == 0U)
    {
        if (s_rtc.tick() >= deadline)
        {
            wpr_lock();
            return RTC_ERR_INIT_TIMEOUT;
        }
    }

    /* Pack TR (24h): hour:minute:second as packed BCD bytes. */
    uint32_t tr_value = ((uint32_t) rtc_bin_to_bcd(dt->hour) << 16) |
                        ((uint32_t) rtc_bin_to_bcd(dt->minute) << 8) |
                        (uint32_t) rtc_bin_to_bcd(dt->second);

    /* Pack DR: WDU = 1 (Monday) — not exposed via the API, value irrelevant. */
    uint8_t year_offset = (uint8_t) (dt->year - 2000U);
    uint32_t dr_value = ((uint32_t) rtc_bin_to_bcd(year_offset) << 16) | (1UL << 13) |
                        ((uint32_t) rtc_bin_to_bcd(dt->month) << 8) |
                        (uint32_t) rtc_bin_to_bcd(dt->day);

    RTC->TR = tr_value;
    RTC->DR = dr_value;

    RTC->ISR &= ~RTC_ISR_INIT;
    deadline = s_rtc.tick() + RTC_MS_TO_TICKS(RTC_TIMEOUT_MS);
    while ((RTC->ISR & RTC_ISR_RSF) == 0U)
    {
        if (s_rtc.tick() >= deadline)
        {
            wpr_lock();
            return RTC_ERR_SYNC_TIMEOUT;
        }
    }

    wpr_lock();
    return RTC_OK;
}

bool rtc_is_backup_valid(void)
{
    assert(s_rtc.initialised);
    return s_rtc.backup_valid;
}

rtc_err_t rtc_read_backup(uint8_t idx, uint32_t *out)
{
    assert(s_rtc.initialised);

    if (out == NULL)
    {
        return RTC_ERR_NULL_ARG;
    }
    if (idx > RTC_BACKUP_MAX_IDX)
    {
        return RTC_ERR_BACKUP_BOUNDS;
    }

    volatile const uint32_t *bkp = &RTC->BKP0R;
    *out = bkp[idx];
    return RTC_OK;
}

rtc_err_t rtc_write_backup(uint8_t idx, uint32_t value)
{
    assert(s_rtc.initialised);

    if (idx > RTC_BACKUP_MAX_IDX)
    {
        return RTC_ERR_BACKUP_BOUNDS;
    }

    volatile uint32_t *bkp = &RTC->BKP0R;
    bkp[idx] = value;
    return RTC_OK;
}

void rtc_set_tick_source(rtc_tick_source_fn fn)
{
    s_rtc.tick = (fn != NULL) ? fn : &default_tick_source;
}

/* ===================================================================== */
/* §8. Singleton vtable                                                  */
/* ===================================================================== */

static const irtc_t s_rtc_vtable = {
    .init = rtc_init,
    .get_time = rtc_get_time,
    .set_time = rtc_set_time,
    .is_backup_valid = rtc_is_backup_valid,
    .read_backup = rtc_read_backup,
    .write_backup = rtc_write_backup,
};

const irtc_t *const rtc_driver = &s_rtc_vtable;

/* ===================================================================== */
/* §9. Test-only hooks                                                   */
/* ===================================================================== */

#ifdef TEST
void rtc_reset_for_test(void)
{
    s_rtc.initialised = false;
    s_rtc.backup_valid = false;
    s_rtc.tick = NULL;
}
#endif
