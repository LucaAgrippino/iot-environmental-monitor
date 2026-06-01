/**
 * @file alarm_service.c
 * @brief AlarmService — hysteresis-based threshold evaluation.
 *
 * @see docs/lld/application/sensor-alarm-service.md
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef TEST
#define LOG_ERROR(m, f, ...) ((void) 0)
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#define LOG_DEBUG(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

#include "alarm_service/alarm_service.h"

/* ======================================================================= */
/* Module tag                                                               */
/* ======================================================================= */

#define MODULE_TAG "AS"

/* ======================================================================= */
/* Default alarm thresholds (loaded from IConfigProvider when available)   */
/* SS-O1: IConfigProvider not yet implemented; compile-time defaults used. */
/* ======================================================================= */

/* Units match sensor_reading_t.value per sensor ID:
 *   TEMPERATURE / HUMIDITY: x100 (0.01 °C / 0.01 %RH)
 *   PRESSURE: x10 (0.1 hPa) */
#define ALARM_TEMP_HIGH_DEFAULT (3500) /*  35.00 °C  */
#define ALARM_TEMP_LOW_DEFAULT (0)     /*   0.00 °C  */
#define ALARM_TEMP_HYST_DEFAULT (200)  /*   2.00 °C  */

#define ALARM_HUMIDITY_HIGH_DEFAULT (8000) /* 80.00 %RH */
#define ALARM_HUMIDITY_LOW_DEFAULT (2000)  /* 20.00 %RH */
#define ALARM_HUMIDITY_HYST_DEFAULT (500)  /*  5.00 %RH */

#define ALARM_PRESSURE_HIGH_DEFAULT (10500) /* 1050.0 hPa */
#define ALARM_PRESSURE_LOW_DEFAULT (9500)   /*  950.0 hPa */
#define ALARM_PRESSURE_HYST_DEFAULT (50)    /*    5.0 hPa */

/* ======================================================================= */
/* Internal types                                                           */
/* ======================================================================= */

typedef struct
{
    int32_t high;
    int32_t low;
    int32_t hysteresis;
} alarm_threshold_t;

typedef struct
{
    bool initialised;
    alarm_state_t alarm_state[SENSOR_ID_COUNT];
    alarm_threshold_t thresholds[SENSOR_ID_COUNT];
    void (*subscribers[ALARM_MAX_SUBSCRIBERS])(sensor_id_t, alarm_event_t,
                                               const sensor_reading_t *);
    uint8_t subscriber_count;
} AlarmServiceState;

static AlarmServiceState s_as;

/* ======================================================================= */
/* Forward declarations                                                     */
/* ======================================================================= */

static void set_alarm(int id, alarm_state_t new_state, alarm_event_t event,
                      const sensor_reading_t *reading);

/* ======================================================================= */
/* Singleton vtable                                                         */
/* ======================================================================= */

static const ialarm_service_t s_alarm_service_vtable = {
    .init = alarm_service_init,
    .get_state = alarm_service_get_state,
    .get_all_states = alarm_service_get_all_states,
    .subscribe = alarm_service_subscribe,
    .ack_all = alarm_service_ack_all,
};

const ialarm_service_t *const alarm_service = &s_alarm_service_vtable;

/* ======================================================================= */
/* Public API implementation                                                */
/* ======================================================================= */

alarm_service_err_t alarm_service_init(void)
{
    (void) memset(&s_as, 0, sizeof(s_as));

    /* Load defaults (IConfigProvider absent — SS-O1). */
    s_as.thresholds[SENSOR_ID_TEMPERATURE].high = ALARM_TEMP_HIGH_DEFAULT;
    s_as.thresholds[SENSOR_ID_TEMPERATURE].low = ALARM_TEMP_LOW_DEFAULT;
    s_as.thresholds[SENSOR_ID_TEMPERATURE].hysteresis = ALARM_TEMP_HYST_DEFAULT;

    s_as.thresholds[SENSOR_ID_HUMIDITY].high = ALARM_HUMIDITY_HIGH_DEFAULT;
    s_as.thresholds[SENSOR_ID_HUMIDITY].low = ALARM_HUMIDITY_LOW_DEFAULT;
    s_as.thresholds[SENSOR_ID_HUMIDITY].hysteresis = ALARM_HUMIDITY_HYST_DEFAULT;

    s_as.thresholds[SENSOR_ID_PRESSURE].high = ALARM_PRESSURE_HIGH_DEFAULT;
    s_as.thresholds[SENSOR_ID_PRESSURE].low = ALARM_PRESSURE_LOW_DEFAULT;
    s_as.thresholds[SENSOR_ID_PRESSURE].hysteresis = ALARM_PRESSURE_HYST_DEFAULT;
    /* GW-only sensors (3-11): thresholds stay at 0; readings are always
     * invalid on FD so alarm_service_evaluate skips them (SS-O3). */

    /* Register the evaluation callback with SensorService. */
    sensor_service_err_t ss_err = sensor_service_subscribe(alarm_service_evaluate);
    if (ss_err != SENSOR_SERVICE_ERR_OK)
    {
        LOG_ERROR(MODULE_TAG, "sensor_service_subscribe failed (%d)", (int) ss_err);
        return ALARM_SERVICE_ERR_NOT_INIT;
    }

    s_as.initialised = true;
    LOG_INFO(MODULE_TAG, "AlarmService initialised");
    return ALARM_SERVICE_ERR_OK;
}

alarm_service_err_t alarm_service_get_state(sensor_id_t sensor, alarm_state_t *state_out)
{
    if (!s_as.initialised)
    {
        return ALARM_SERVICE_ERR_NOT_INIT;
    }

    if (state_out == NULL)
    {
        return ALARM_SERVICE_ERR_NULL_ARG;
    }

    *state_out = s_as.alarm_state[(int) sensor];
    return ALARM_SERVICE_ERR_OK;
}

alarm_service_err_t alarm_service_get_all_states(alarm_state_t states[SENSOR_ID_COUNT])
{
    if (!s_as.initialised)
    {
        return ALARM_SERVICE_ERR_NOT_INIT;
    }

    if (states == NULL)
    {
        return ALARM_SERVICE_ERR_NULL_ARG;
    }

    for (int i = 0; i < (int) SENSOR_ID_COUNT; i++)
    {
        states[i] = s_as.alarm_state[i];
    }

    return ALARM_SERVICE_ERR_OK;
}

alarm_service_err_t alarm_service_subscribe(void (*cb)(sensor_id_t sensor, alarm_event_t event,
                                                       const sensor_reading_t *reading))
{
    if (!s_as.initialised)
    {
        return ALARM_SERVICE_ERR_NOT_INIT;
    }

    if (cb == NULL)
    {
        return ALARM_SERVICE_ERR_NULL_ARG;
    }

    if (s_as.subscriber_count >= ALARM_MAX_SUBSCRIBERS)
    {
        return ALARM_SERVICE_ERR_NO_SUB;
    }

    s_as.subscribers[s_as.subscriber_count] = cb;
    s_as.subscriber_count++;
    return ALARM_SERVICE_ERR_OK;
}

alarm_service_err_t alarm_service_ack_all(void)
{
    if (!s_as.initialised)
    {
        return ALARM_SERVICE_ERR_NOT_INIT;
    }

    for (int i = 0; i < (int) SENSOR_ID_COUNT; i++)
    {
        s_as.alarm_state[i] = ALARM_STATE_CLEAR;
    }

    LOG_INFO(MODULE_TAG, "All alarms acknowledged");
    return ALARM_SERVICE_ERR_OK;
}

/* ======================================================================= */
/* Alarm evaluation (SensorService subscriber callback)                    */
/* ======================================================================= */

ALARM_SERVICE_TEST_VISIBLE void alarm_service_evaluate(const sensor_snapshot_t *snap)
{
    for (int i = 0; i < (int) SENSOR_ID_COUNT; i++)
    {
        const sensor_reading_t *r = &snap->readings[i];

        if (!r->valid)
        {
            continue;
        } /* skip invalid readings */

        int32_t thr_high = s_as.thresholds[i].high;
        int32_t thr_low = s_as.thresholds[i].low;
        int32_t hyst = s_as.thresholds[i].hysteresis;

        switch (s_as.alarm_state[i])
        {
        case ALARM_STATE_CLEAR:
            if (r->value > thr_high)
            {
                set_alarm(i, ALARM_STATE_ACTIVE_HIGH, ALARM_EVENT_RAISED_HIGH, r);
            }
            else if (r->value < thr_low)
            {
                set_alarm(i, ALARM_STATE_ACTIVE_LOW, ALARM_EVENT_RAISED_LOW, r);
            }
            break;

        case ALARM_STATE_ACTIVE_HIGH:
            if (r->value < (thr_high - hyst))
            {
                set_alarm(i, ALARM_STATE_CLEAR, ALARM_EVENT_CLEARED, r);
            }
            break;

        case ALARM_STATE_ACTIVE_LOW:
            if (r->value > (thr_low + hyst))
            {
                set_alarm(i, ALARM_STATE_CLEAR, ALARM_EVENT_CLEARED, r);
            }
            break;

        default:
            break;
        }
    }
}

/* ======================================================================= */
/* Private helpers                                                          */
/* ======================================================================= */

static void set_alarm(int id, alarm_state_t new_state, alarm_event_t event,
                      const sensor_reading_t *reading)
{
    s_as.alarm_state[id] = new_state;

    for (uint8_t i = 0U; i < s_as.subscriber_count; i++)
    {
        if (s_as.subscribers[i] != NULL)
        {
            s_as.subscribers[i]((sensor_id_t) id, event, reading);
        }
    }
}

/* ======================================================================= */
/* Test-only hooks                                                          */
/* ======================================================================= */

#ifdef TEST
void alarm_service_reset_for_test(void)
{
    (void) memset(&s_as, 0, sizeof(s_as));
}
#endif /* TEST */
