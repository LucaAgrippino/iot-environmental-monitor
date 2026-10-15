/**
 * @file alarm_service.c
 * @brief AlarmService implementation (Gateway) — hysteresis threshold evaluation.
 *
 * @see docs/lld/application/sensor-alarm-service.md
 */

#include "alarm_service.h"

#include <stddef.h>
#include <string.h>

#ifdef TEST
#define LOG_ERROR(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

#define MODULE_TAG "AS"

/* Default thresholds, in the units of sensor_reading_t.value (SS-O6: replaced by
 * IConfigProvider values once the Gateway has one). */
#define ALARM_TEMP_HIGH_DEFAULT (3500)      /*   35.00 degC */
#define ALARM_TEMP_LOW_DEFAULT (0)          /*    0.00 degC */
#define ALARM_TEMP_HYST_DEFAULT (200)       /*    2.00 degC */
#define ALARM_HUMIDITY_HIGH_DEFAULT (8000)  /*   80.00 %RH  */
#define ALARM_HUMIDITY_LOW_DEFAULT (2000)   /*   20.00 %RH  */
#define ALARM_HUMIDITY_HYST_DEFAULT (500)   /*    5.00 %RH  */
#define ALARM_PRESSURE_HIGH_DEFAULT (10500) /* 1050.0 hPa   */
#define ALARM_PRESSURE_LOW_DEFAULT (9500)   /*  950.0 hPa   */
#define ALARM_PRESSURE_HYST_DEFAULT (50)    /*    5.0 hPa   */

typedef struct
{
    bool enabled; /* false: never evaluated (IMU / magnetometer, SS-O3) */
    int32_t high;
    int32_t low;
    int32_t hysteresis;
} alarm_threshold_t;

static struct
{
    bool initialised;
    alarm_state_t alarm_state[SENSOR_ID_COUNT];
    alarm_threshold_t thresholds[SENSOR_ID_COUNT];
    void (*subscribers[ALARM_MAX_SUBSCRIBERS])(sensor_id_t, alarm_event_t,
                                               const sensor_reading_t *);
    uint8_t subscriber_count;
} s_as;

#ifdef TEST
#define ALARM_SERVICE_TEST_VISIBLE
#else
#define ALARM_SERVICE_TEST_VISIBLE static
#endif

static void prv_set_threshold(sensor_id_t id, int32_t high, int32_t low, int32_t hysteresis);
static void prv_set_alarm(sensor_id_t id, alarm_state_t new_state, alarm_event_t event,
                          const sensor_reading_t *reading);
ALARM_SERVICE_TEST_VISIBLE void alarm_service_evaluate(const sensor_snapshot_t *snap);

static const ialarm_service_t s_alarm_service_vtable = {
    .init = alarm_service_init,
    .get_state = alarm_service_get_state,
    .get_all_states = alarm_service_get_all_states,
    .subscribe = alarm_service_subscribe,
    .ack_all = alarm_service_ack_all,
};

const ialarm_service_t *const alarm_service = &s_alarm_service_vtable;

/* ======================================================================= */
/* Public API                                                               */
/* ======================================================================= */

alarm_service_err_t alarm_service_init(void)
{
    (void) memset(&s_as, 0, sizeof(s_as));

    prv_set_threshold(SENSOR_ID_TEMPERATURE, ALARM_TEMP_HIGH_DEFAULT, ALARM_TEMP_LOW_DEFAULT,
                      ALARM_TEMP_HYST_DEFAULT);
    prv_set_threshold(SENSOR_ID_HUMIDITY, ALARM_HUMIDITY_HIGH_DEFAULT, ALARM_HUMIDITY_LOW_DEFAULT,
                      ALARM_HUMIDITY_HYST_DEFAULT);
    prv_set_threshold(SENSOR_ID_PRESSURE, ALARM_PRESSURE_HIGH_DEFAULT, ALARM_PRESSURE_LOW_DEFAULT,
                      ALARM_PRESSURE_HYST_DEFAULT);

    if (sensor_service_subscribe(alarm_service_evaluate) != SENSOR_SERVICE_ERR_OK)
    {
        LOG_ERROR(MODULE_TAG, "sensor_service_subscribe failed");
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
    if (((int32_t) sensor < 0) || ((int32_t) sensor >= (int32_t) SENSOR_ID_COUNT))
    {
        return ALARM_SERVICE_ERR_INVALID_ARG;
    }

    *state_out = s_as.alarm_state[sensor];
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

    for (uint8_t i = 0U; i < (uint8_t) SENSOR_ID_COUNT; i++)
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

    for (uint8_t i = 0U; i < (uint8_t) SENSOR_ID_COUNT; i++)
    {
        s_as.alarm_state[i] = ALARM_STATE_CLEAR;
    }
    LOG_INFO(MODULE_TAG, "all alarms acknowledged");
    return ALARM_SERVICE_ERR_OK;
}

/* ======================================================================= */
/* Evaluation (SensorService subscriber)                                    */
/* ======================================================================= */

ALARM_SERVICE_TEST_VISIBLE void alarm_service_evaluate(const sensor_snapshot_t *snap)
{
    for (uint8_t i = 0U; i < (uint8_t) SENSOR_ID_COUNT; i++)
    {
        const sensor_reading_t *r = &snap->readings[i];
        const alarm_threshold_t *t = &s_as.thresholds[i];

        if (!t->enabled || !r->valid)
        {
            continue;
        }

        switch (s_as.alarm_state[i])
        {
        case ALARM_STATE_CLEAR:
            if (r->value > t->high)
            {
                prv_set_alarm((sensor_id_t) i, ALARM_STATE_ACTIVE_HIGH, ALARM_EVENT_RAISED_HIGH, r);
            }
            else if (r->value < t->low)
            {
                prv_set_alarm((sensor_id_t) i, ALARM_STATE_ACTIVE_LOW, ALARM_EVENT_RAISED_LOW, r);
            }
            else
            {
                /* Inside the band: nothing to do. */
            }
            break;

        case ALARM_STATE_ACTIVE_HIGH:
            if (r->value < (t->high - t->hysteresis))
            {
                prv_set_alarm((sensor_id_t) i, ALARM_STATE_CLEAR, ALARM_EVENT_CLEARED, r);
            }
            break;

        case ALARM_STATE_ACTIVE_LOW:
            if (r->value > (t->low + t->hysteresis))
            {
                prv_set_alarm((sensor_id_t) i, ALARM_STATE_CLEAR, ALARM_EVENT_CLEARED, r);
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

static void prv_set_threshold(sensor_id_t id, int32_t high, int32_t low, int32_t hysteresis)
{
    s_as.thresholds[id].enabled = true;
    s_as.thresholds[id].high = high;
    s_as.thresholds[id].low = low;
    s_as.thresholds[id].hysteresis = hysteresis;
}

static void prv_set_alarm(sensor_id_t id, alarm_state_t new_state, alarm_event_t event,
                          const sensor_reading_t *reading)
{
    s_as.alarm_state[id] = new_state;

    for (uint8_t i = 0U; i < s_as.subscriber_count; i++)
    {
        s_as.subscribers[i](id, event, reading);
    }
}

#ifdef TEST
void alarm_service_reset_for_test(void)
{
    (void) memset(&s_as, 0, sizeof(s_as));
}
#endif /* TEST */
