/**
 * @file time_provider.c
 * @brief TimeProvider middleware implementation (Gateway).
 *
 * @see docs/lld/middleware/time-provider.md
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#ifdef TEST
/* Keep the real RtcDriver and Logger out of the host link unit: Ceedling
 * auto-links any .c whose header a translation unit includes. The stub declares
 * only what TimeProvider calls through the irtc_t vtable. */
#include "rtc_driver_stub.h"
#define LOG_ERROR(m, f, ...) ((void) 0)
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#define LOG_DEBUG(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#include "rtc/rtc.h"
#endif /* TEST */

#include "time_provider.h"
#include "time_provider_config.h"

#define MODULE_TAG "TP"

#define SECONDS_PER_MINUTE (60UL)
#define SECONDS_PER_HOUR (3600UL)
#define SECONDS_PER_DAY (86400UL)
#define HOURS_PER_DAY (24UL)
#define MINUTES_PER_HOUR (60UL)
#define MONTHS_PER_YEAR (12U)
#define EPOCH_YEAR (1970U)
#define EPOCH_YEAR_LIMIT (2106U)
#define DAYS_PER_YEAR (365U)
#define DAYS_PER_LEAP_YEAR (366U)
#define FEBRUARY (2U)
#define FEBRUARY_LEAP_DAYS (29U)
#define LEAP_CYCLE_YEARS (4U)
#define CENTURY_YEARS (100U)
#define LEAP_400_YEARS (400U)

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
/* Epoch <-> calendar conversion                                             */
/* ========================================================================= */

/** @brief True if @p year is a Gregorian leap year. */
static bool is_leap_year(uint16_t year)
{
    return (((year % LEAP_CYCLE_YEARS) == 0U) && ((year % CENTURY_YEARS) != 0U)) ||
           ((year % LEAP_400_YEARS) == 0U);
}

/** @brief Number of days in @p month (1..12) of @p year. */
static uint8_t days_in_month(uint8_t month, uint16_t year)
{
    static const uint8_t k_month_days[MONTHS_PER_YEAR] = {31U, 28U, 31U, 30U, 31U, 30U,
                                                          31U, 31U, 30U, 31U, 30U, 31U};
    uint8_t days = k_month_days[month - 1U];

    if ((month == FEBRUARY) && is_leap_year(year))
    {
        days = FEBRUARY_LEAP_DAYS;
    }

    return days;
}

/**
 * @brief Convert an RTC date/time to Unix epoch seconds (UTC).
 *
 * The caller guarantees valid calendar fields; RtcDriver returns them that way.
 */
static uint32_t datetime_to_epoch(const rtc_datetime_t *dt)
{
    uint32_t days = 0U;

    for (uint16_t y = EPOCH_YEAR; y < dt->year; y++)
    {
        days += is_leap_year(y) ? DAYS_PER_LEAP_YEAR : DAYS_PER_YEAR;
    }

    for (uint8_t m = 1U; m < dt->month; m++)
    {
        days += days_in_month(m, dt->year);
    }

    days += (uint32_t) (dt->day - 1U);

    return (days * SECONDS_PER_DAY) + ((uint32_t) dt->hour * SECONDS_PER_HOUR) +
           ((uint32_t) dt->minute * SECONDS_PER_MINUTE) + (uint32_t) dt->second;
}

/** @brief Convert Unix epoch seconds to an RTC date/time (UTC). */
static void epoch_to_datetime(uint32_t epoch, rtc_datetime_t *dt)
{
    uint32_t remaining = epoch;
    uint32_t days;
    uint8_t month;

    dt->second = (uint8_t) (remaining % SECONDS_PER_MINUTE);
    remaining /= SECONDS_PER_MINUTE;
    dt->minute = (uint8_t) (remaining % MINUTES_PER_HOUR);
    remaining /= MINUTES_PER_HOUR;
    dt->hour = (uint8_t) (remaining % HOURS_PER_DAY);
    days = remaining / HOURS_PER_DAY;

    dt->year = EPOCH_YEAR;
    while (dt->year < EPOCH_YEAR_LIMIT)
    {
        uint32_t year_days = is_leap_year(dt->year) ? DAYS_PER_LEAP_YEAR : DAYS_PER_YEAR;

        if (days < year_days)
        {
            break;
        }

        days -= year_days;
        dt->year++;
    }

    for (month = 1U; month <= MONTHS_PER_YEAR; month++)
    {
        uint32_t month_days = days_in_month(month, dt->year);

        if (days < month_days)
        {
            break;
        }

        days -= month_days;
    }

    dt->month = month;
    dt->day = (uint8_t) (days + 1U);
}

/** @brief Absolute difference of two epochs, without unsigned wrap. */
static uint32_t epoch_delta(uint32_t a, uint32_t b)
{
    return (a > b) ? (a - b) : (b - a);
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
        rtc_err_t rtc_err = rtc_driver->get_time(&dt);

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
    bool was_unsynchronised;
    rtc_err_t rtc_err;

    if (!s_tp.initialised)
    {
        return TIME_PROVIDER_ERR_NOT_INIT;
    }

    /* RtcDriver stores a two-digit year offset and does not range-check it, so
     * an epoch outside 2000..2099 would be written as a wrapped year. */
    if ((new_epoch < TIME_PROVIDER_EPOCH_MIN) || (new_epoch > TIME_PROVIDER_EPOCH_MAX))
    {
        LOG_WARN(MODULE_TAG, "Epoch %lu outside the RTC range", (unsigned long) new_epoch);
        return TIME_PROVIDER_ERR_RTC_FAIL;
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

        uint32_t delta = epoch_delta(new_epoch, datetime_to_epoch(&current_dt));

        if (delta > TIME_PROVIDER_SANITY_DELTA_S)
        {
            (void) xSemaphoreGive(s_tp.mutex);
            LOG_WARN(MODULE_TAG, "Sanity delta exceeded: %lu s", (unsigned long) delta);
            return TIME_PROVIDER_ERR_RTC_FAIL;
        }
    }

    epoch_to_datetime(new_epoch, &dt);
    rtc_err = rtc_driver->set_time(&dt);
    if (rtc_err != RTC_OK)
    {
        (void) xSemaphoreGive(s_tp.mutex);
        LOG_WARN(MODULE_TAG, "RTC set_time failed: %d", (int) rtc_err);
        return TIME_PROVIDER_ERR_RTC_FAIL;
    }

    rtc_err = rtc_driver->write_backup(TIME_PROVIDER_BKUP_REG, TIME_PROVIDER_SYNC_MAGIC);
    if (rtc_err != RTC_OK)
    {
        /* The clock is set, so the provider is synchronised for this run; only
         * the persistence across a warm reset is lost. */
        LOG_WARN(MODULE_TAG, "Sync flag not persisted: %d", (int) rtc_err);
    }

    was_unsynchronised = (s_tp.sync_state == TIME_SYNC_UNSYNCHRONISED);
    s_tp.sync_state = TIME_SYNC_SYNCHRONISED;

    (void) xSemaphoreGive(s_tp.mutex);

    /* Pushed after the mutex is released to avoid priority inversion against
     * the HealthMonitor mutex. */
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
    rtc_err_t rtc_err;

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

    rtc_err = rtc_driver->write_backup(TIME_PROVIDER_BKUP_REG, TIME_PROVIDER_SYNC_CLEARED);
    if (rtc_err != RTC_OK)
    {
        LOG_WARN(MODULE_TAG, "Sync flag not cleared: %d", (int) rtc_err);
    }

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

#ifdef TEST
void time_provider_reset_for_test(void)
{
    (void) memset(&s_tp, 0, sizeof(s_tp));
}
#endif /* TEST */
