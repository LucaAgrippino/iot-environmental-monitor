/**
 * @file time_provider.c
 * @brief TimeProvider middleware implementation.
 *
 * @see docs/lld/middleware/time-provider.md
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#ifdef TEST
/* Prevent Ceedling auto-linking real driver and application .c files.
 * Stub headers declare only the symbols the SUT actually calls. */
#include "rtc_driver_stub.h"
#include "health_monitor_stub.h"
#define LOG_ERROR(m, f, ...) ((void) 0)
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#define LOG_DEBUG(m, f, ...) ((void) 0)
#else
#include "rtc/rtc_driver.h"
#include "health_monitor/health_monitor.h"
#include "logger/logger.h"
#endif /* TEST */

#include "time_provider/time_provider.h"
#include "time_provider/time_provider_config.h"

/* ========================================================================= */
/* Module tag                                                                */
/* ========================================================================= */

#define MODULE_TAG "TP"

/* ========================================================================= */
/* Internal state                                                            */
/* ========================================================================= */

typedef struct
{
    bool initialised;
    time_sync_state_t sync_state;
    const ihealth_report_t *health;
    StaticSemaphore_t mutex_buf;
    SemaphoreHandle_t mutex;
} time_provider_state_t;

static time_provider_state_t s_tp;

/* ========================================================================= */
/* Internal helpers — epoch ↔ calendar conversion                           */
/* ========================================================================= */

/** Return true if year is a Gregorian leap year. */
static bool is_leap_year(uint16_t year)
{
    return ((year % 4U == 0U) && (year % 100U != 0U)) || (year % 400U == 0U);
}

/** Return the number of days in month (1..12) for the given year. */
static uint8_t days_in_month(uint8_t month, uint16_t year)
{
    static const uint8_t k_month_days[12U] = {31U, 28U, 31U, 30U, 31U, 30U,
                                              31U, 31U, 30U, 31U, 30U, 31U};
    uint8_t d = k_month_days[month - 1U];
    if ((month == 2U) && is_leap_year(year))
    {
        d = 29U;
    }
    return d;
}

/**
 * @brief Convert rtc_datetime_t to Unix epoch seconds (UTC, since 1970-01-01).
 *
 * Caller must ensure dt fields are valid calendar values.
 */
static uint32_t datetime_to_epoch(const rtc_datetime_t *dt)
{
    uint32_t days = 0U;
    uint16_t y;
    uint8_t m;

    for (y = 1970U; y < dt->year; y++)
    {
        days += is_leap_year(y) ? 366U : 365U;
    }
    for (m = 1U; m < dt->month; m++)
    {
        days += days_in_month(m, dt->year);
    }
    days += (uint32_t) (dt->day - 1U);

    return (days * 86400UL) + ((uint32_t) dt->hour * 3600UL) + ((uint32_t) dt->minute * 60UL) +
           (uint32_t) dt->second;
}

/**
 * @brief Convert Unix epoch seconds to rtc_datetime_t (UTC).
 *
 * Handles years 1970..2105 (full uint32_t epoch range).
 */
static void epoch_to_datetime(uint32_t epoch, rtc_datetime_t *dt)
{
    uint32_t remaining = epoch;
    uint32_t days;
    uint8_t month;
    /* cppcheck-suppress variableScope - BARR-C: all declarations at block start */
    uint8_t mdays;

    dt->second = (uint8_t) (remaining % 60U);
    remaining /= 60U;
    dt->minute = (uint8_t) (remaining % 60U);
    remaining /= 60U;
    dt->hour = (uint8_t) (remaining % 24U);
    days = remaining / 24U;

    dt->year = 1970U;
    while (dt->year < 2106U)
    {
        uint32_t yd = is_leap_year(dt->year) ? 366U : 365U;
        if (days < yd)
        {
            break;
        }
        days -= yd;
        dt->year++;
    }

    for (month = 1U; month <= 12U; month++)
    {
        mdays = days_in_month(month, dt->year);
        if (days < (uint32_t) mdays)
        {
            break;
        }
        days -= (uint32_t) mdays;
    }
    dt->month = month;
    dt->day = (uint8_t) (days + 1U);
}

/* ========================================================================= */
/* Public API                                                                */
/* ========================================================================= */

time_provider_err_t time_provider_init(const ihealth_report_t *health)
{
    uint32_t bkup_val = 0U;
    rtc_err_t rtc_err;

    (void) memset(&s_tp, 0, sizeof(s_tp));

    s_tp.mutex = xSemaphoreCreateMutexStatic(&s_tp.mutex_buf);
    if (s_tp.mutex == NULL)
    {
        LOG_ERROR(MODULE_TAG, "Mutex creation failed");
        return TIME_PROVIDER_ERR_RTC_FAIL;
    }

    s_tp.health = health;

    rtc_err = rtc_driver->read_backup(TIME_PROVIDER_BKUP_REG, &bkup_val);
    if (rtc_err != RTC_OK)
    {
        LOG_ERROR(MODULE_TAG, "Backup register read failed: %d", (int) rtc_err);
        return TIME_PROVIDER_ERR_RTC_FAIL;
    }

    s_tp.sync_state =
        (bkup_val == TIME_PROVIDER_SYNC_MAGIC) ? TIME_SYNC_SYNCHRONISED : TIME_SYNC_UNSYNCHRONISED;

    s_tp.initialised = true;

    LOG_INFO(MODULE_TAG, "Initialised sync_state=%d", (int) s_tp.sync_state);
    return TIME_PROVIDER_ERR_OK;
}

time_provider_err_t time_provider_get(time_provider_ts_t *ts_out)
{
    rtc_datetime_t dt;
    /* cppcheck-suppress variableScope - BARR-C: all declarations at block start */
    rtc_err_t rtc_err;

    if (!s_tp.initialised)
    {
        return TIME_PROVIDER_ERR_NOT_INIT;
    }
    if (ts_out == NULL)
    {
        return TIME_PROVIDER_ERR_NULL_ARG;
    }

    if (xSemaphoreTake(s_tp.mutex, portMAX_DELAY) != pdTRUE)
    {
        return TIME_PROVIDER_ERR_NOT_INIT;
    }

    if (s_tp.sync_state == TIME_SYNC_SYNCHRONISED)
    {
        rtc_err = rtc_driver->get_time(&dt);
        if (rtc_err != RTC_OK)
        {
            (void) xSemaphoreGive(s_tp.mutex);
            LOG_WARN(MODULE_TAG, "RTC read failed in get: %d", (int) rtc_err);
            return TIME_PROVIDER_ERR_RTC_FAIL;
        }
        ts_out->epoch = datetime_to_epoch(&dt);
    }
    else
    {
        ts_out->epoch = (uint32_t) (xTaskGetTickCount() / configTICK_RATE_HZ);
    }

    ts_out->sync_state = s_tp.sync_state;
    (void) xSemaphoreGive(s_tp.mutex);
    return TIME_PROVIDER_ERR_OK;
}

time_provider_err_t time_provider_set_time(uint32_t new_epoch)
{
    rtc_datetime_t dt;
    rtc_datetime_t current_dt;
    /* cppcheck-suppress variableScope - BARR-C: all declarations at block start */
    uint32_t current_epoch;
    /* cppcheck-suppress variableScope - BARR-C: all declarations at block start */
    uint32_t delta;
    bool was_unsynchronised;
    rtc_err_t rtc_err;

    if (!s_tp.initialised)
    {
        return TIME_PROVIDER_ERR_NOT_INIT;
    }

    if (xSemaphoreTake(s_tp.mutex, portMAX_DELAY) != pdTRUE)
    {
        return TIME_PROVIDER_ERR_NOT_INIT;
    }

    if (s_tp.sync_state == TIME_SYNC_SYNCHRONISED)
    {
        rtc_err = rtc_driver->get_time(&current_dt);
        if (rtc_err != RTC_OK)
        {
            (void) xSemaphoreGive(s_tp.mutex);
            LOG_WARN(MODULE_TAG, "Sanity-check RTC read failed: %d", (int) rtc_err);
            return TIME_PROVIDER_ERR_RTC_FAIL;
        }

        current_epoch = datetime_to_epoch(&current_dt);
        delta =
            (new_epoch > current_epoch) ? (new_epoch - current_epoch) : (current_epoch - new_epoch);
        if (delta > TIME_PROVIDER_SANITY_DELTA_S)
        {
            LOG_WARN(MODULE_TAG, "Sanity delta exceeded: %lu s", (unsigned long) delta);
            (void) xSemaphoreGive(s_tp.mutex);
            return TIME_PROVIDER_ERR_RTC_FAIL;
        }
    }

    epoch_to_datetime(new_epoch, &dt);
    rtc_err = rtc_driver->set_time(&dt);
    if (rtc_err != RTC_OK)
    {
        LOG_WARN(MODULE_TAG, "RTC set_time failed: %d", (int) rtc_err);
        (void) xSemaphoreGive(s_tp.mutex);
        return TIME_PROVIDER_ERR_RTC_FAIL;
    }

    (void) rtc_driver->write_backup(TIME_PROVIDER_BKUP_REG, TIME_PROVIDER_SYNC_MAGIC);

    was_unsynchronised = (s_tp.sync_state == TIME_SYNC_UNSYNCHRONISED);
    s_tp.sync_state = TIME_SYNC_SYNCHRONISED;

    (void) xSemaphoreGive(s_tp.mutex);

    if (was_unsynchronised && (s_tp.health != NULL))
    {
        (void) s_tp.health->push_event(HEALTH_EVENT_TIME_SYNC_ACQUIRED, 0U);
        LOG_INFO(MODULE_TAG, "Time sync acquired epoch=%lu", (unsigned long) new_epoch);
    }

    return TIME_PROVIDER_ERR_OK;
}

time_provider_err_t time_provider_mark_unsynchronised(void)
{
    bool was_synchronised;

    if (!s_tp.initialised)
    {
        return TIME_PROVIDER_ERR_NOT_INIT;
    }

    if (xSemaphoreTake(s_tp.mutex, portMAX_DELAY) != pdTRUE)
    {
        return TIME_PROVIDER_ERR_NOT_INIT;
    }

    was_synchronised = (s_tp.sync_state == TIME_SYNC_SYNCHRONISED);
    s_tp.sync_state = TIME_SYNC_UNSYNCHRONISED;

    (void) rtc_driver->write_backup(TIME_PROVIDER_BKUP_REG, 0x00000000UL);

    (void) xSemaphoreGive(s_tp.mutex);

    if (was_synchronised && (s_tp.health != NULL))
    {
        (void) s_tp.health->push_event(HEALTH_EVENT_TIME_SYNC_LOST, 0U);
        LOG_INFO(MODULE_TAG, "Time sync lost");
    }

    return TIME_PROVIDER_ERR_OK;
}

time_sync_state_t time_provider_get_sync_state(void)
{
    time_sync_state_t state;

    if (!s_tp.initialised)
    {
        return TIME_SYNC_UNSYNCHRONISED;
    }

    if (xSemaphoreTake(s_tp.mutex, portMAX_DELAY) != pdTRUE)
    {
        return TIME_SYNC_UNSYNCHRONISED;
    }

    state = s_tp.sync_state;
    (void) xSemaphoreGive(s_tp.mutex);
    return state;
}

/* ========================================================================= */
/* Vtable and singleton                                                      */
/* ========================================================================= */

static const itime_provider_t s_time_provider_vtable = {
    .init = time_provider_init,
    .get = time_provider_get,
    .set_time = time_provider_set_time,
    .mark_unsynchronised = time_provider_mark_unsynchronised,
    .get_sync_state = time_provider_get_sync_state,
};

const itime_provider_t *const time_provider = &s_time_provider_vtable;

/* ========================================================================= */
/* Test-only reset                                                           */
/* ========================================================================= */

#ifdef TEST
void time_provider_reset_for_test(void)
{
    (void) memset(&s_tp, 0, sizeof(s_tp));
}
#endif /* TEST */
