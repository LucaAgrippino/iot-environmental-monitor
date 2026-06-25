/**
 * @file sensor_service.c
 * @brief SensorService — acquisition pipeline, IIR filter, and subscriber dispatch.
 *
 * @see docs/lld/application/sensor-alarm-service.md
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#ifdef TEST
/* Prevent Ceedling from auto-linking real driver and application .c files. */
#include "barometer_driver_stub.h"
#include "humidity_temp_driver_stub.h"
#include "health_monitor_stub.h"
#define LOG_ERROR(m, f, ...) ((void) 0)
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#define LOG_DEBUG(m, f, ...) ((void) 0)
#else
#include "barometer_driver/barometer_driver.h"
#include "humidity_temp_driver/humidity_temp_driver.h"
#include "health_monitor/health_monitor.h"
#include "logger/logger.h"
#endif /* TEST */

#include "sensor_service/sensor_service.h"

/* ======================================================================= */
/* Module tag                                                               */
/* ======================================================================= */

#define MODULE_TAG "SS"

/* ======================================================================= */
/* Constants                                                                */
/* ======================================================================= */

/* Default IIR alpha as fraction: alpha = ALPHA_NUM / ALPHA_DEN = 0.1 */
#define SENSOR_ALPHA_NUM_DEFAULT (1U)
#define SENSOR_ALPHA_DEN_DEFAULT (10U)

/* Poll timer period.
 * DEVIATION from companion §9: period is 200 ms instead of 100 ms.
 * See bug-log.md for details. */
#define SENSOR_POLL_INTERVAL_MS (100U)

/* Default range limits in native driver fixed-point units (REQ-SA-050).
 * TEMPERATURE / HUMIDITY: x100 (0.01 °C / 0.01 %RH)
 * PRESSURE: x10 (0.1 hPa)
 * GW-only sensors use reasonable physical bounds; they are pre-failed on FD
 * so these values are never evaluated. */
static const int32_t s_range_min[SENSOR_ID_COUNT] = {
    [SENSOR_ID_TEMPERATURE] = -4000, /* -40.00 °C  */
    [SENSOR_ID_HUMIDITY] = 0,        /*   0.00 %RH */
    [SENSOR_ID_PRESSURE] = 3000,     /* 300.0 hPa  */
    [SENSOR_ID_ACCEL_X] = -10000,    [SENSOR_ID_ACCEL_Y] = -10000, [SENSOR_ID_ACCEL_Z] = -10000,
    [SENSOR_ID_GYRO_X] = -25000,     [SENSOR_ID_GYRO_Y] = -25000,  [SENSOR_ID_GYRO_Z] = -25000,
    [SENSOR_ID_MAG_X] = -40000,      [SENSOR_ID_MAG_Y] = -40000,   [SENSOR_ID_MAG_Z] = -40000,
};

static const int32_t s_range_max[SENSOR_ID_COUNT] = {
    [SENSOR_ID_TEMPERATURE] = 8500, /*  85.00 °C  */
    [SENSOR_ID_HUMIDITY] = 10000,   /* 100.00 %RH */
    [SENSOR_ID_PRESSURE] = 11000,   /* 1100.0 hPa */
    [SENSOR_ID_ACCEL_X] = 10000,    [SENSOR_ID_ACCEL_Y] = 10000, [SENSOR_ID_ACCEL_Z] = 10000,
    [SENSOR_ID_GYRO_X] = 25000,     [SENSOR_ID_GYRO_Y] = 25000,  [SENSOR_ID_GYRO_Z] = 25000,
    [SENSOR_ID_MAG_X] = 40000,      [SENSOR_ID_MAG_Y] = 40000,   [SENSOR_ID_MAG_Z] = 40000,
};

/* ======================================================================= */
/* Internal state                                                           */
/* ======================================================================= */

typedef struct
{
    bool initialised;
    sensor_snapshot_t snapshot;             /* guarded by taskENTER_CRITICAL for reads */
    int32_t prev_filtered[SENSOR_ID_COUNT]; /* IIR filter state (same units as reading.value) */
    uint8_t alpha_num;                      /* IIR coefficient numerator   */
    uint8_t alpha_den;                      /* IIR coefficient denominator */
    bool driver_failed[SENSOR_ID_COUNT];    /* permanent fail flag */
    void (*subscribers[SENSOR_MAX_SUBSCRIBERS])(const sensor_snapshot_t *);
    uint8_t subscriber_count;
    TimerHandle_t poll_timer;
    StaticTimer_t timer_buf;
    TaskHandle_t sensor_task_handle;
} SensorServiceState;

static SensorServiceState s_ss;

/* ======================================================================= */
/* Forward declarations                                                     */
/* ======================================================================= */

static void poll_timer_cb(TimerHandle_t xTimer);
static void process_sensor_reading(int id, int32_t raw_value, bool driver_ok,
                                   bool *first_fail_this_cycle, sensor_snapshot_t *working_snap);
static sensor_service_err_t run_cycle_impl(void);

/* ======================================================================= */
/* Singleton vtable                                                         */
/* ======================================================================= */

static const isensor_service_t s_sensor_service_vtable = {
    .init = sensor_service_init,
    .run_cycle = sensor_service_run_cycle,
    .get_snapshot = sensor_service_get_snapshot,
    .subscribe = sensor_service_subscribe,
    .read_on_demand = sensor_service_read_on_demand,
    .is_ready = sensor_service_is_ready,
    .reconfigure = sensor_service_reconfigure,
};

const isensor_service_t *const sensor_service = &s_sensor_service_vtable;

/* ======================================================================= */
/* Public API implementation                                                */
/* ======================================================================= */

sensor_service_err_t sensor_service_init(void)
{
    (void) memset(&s_ss, 0, sizeof(s_ss));
    s_ss.alpha_num = SENSOR_ALPHA_NUM_DEFAULT;
    s_ss.alpha_den = SENSOR_ALPHA_DEN_DEFAULT;

    /* Pre-fail GW-only sensors on FD builds (no driver available). */
#if !defined(BOARD_GATEWAY)
    for (int i = (int) SENSOR_ID_ACCEL_X; i < (int) SENSOR_ID_COUNT; i++)
    {
        s_ss.driver_failed[i] = true;
    }
#endif

    /* Initialise active drivers; permanently fail any that error. */
    if (humidity_temp_init() != HT_ERR_OK)
    {
        s_ss.driver_failed[SENSOR_ID_TEMPERATURE] = true;
        s_ss.driver_failed[SENSOR_ID_HUMIDITY] = true;
        LOG_ERROR(MODULE_TAG, "humidity_temp_init failed — sensors marked failed");
    }

    if (barometer_init() != BARO_ERR_OK)
    {
        s_ss.driver_failed[SENSOR_ID_PRESSURE] = true;
        LOG_ERROR(MODULE_TAG, "barometer_init failed — pressure sensor marked failed");
    }

    /* Create the periodic poll timer (fires every SENSOR_POLL_INTERVAL_MS ms). */
    s_ss.sensor_task_handle = xTaskGetCurrentTaskHandle();
    s_ss.poll_timer = xTimerCreateStatic("SensorPoll", pdMS_TO_TICKS(SENSOR_POLL_INTERVAL_MS),
                                         pdTRUE, NULL, poll_timer_cb, &s_ss.timer_buf);

    if (s_ss.poll_timer == NULL)
    {
        LOG_ERROR(MODULE_TAG, "xTimerCreateStatic failed");
        return SENSOR_SERVICE_ERR_NOT_INIT;
    }

    (void) xTimerStart(s_ss.poll_timer, pdMS_TO_TICKS(0));

    s_ss.initialised = true;
    LOG_INFO(MODULE_TAG, "SensorService initialised");
    return SENSOR_SERVICE_ERR_OK;
}

sensor_service_err_t sensor_service_run_cycle(void)
{
    if (!s_ss.initialised)
    {
        return SENSOR_SERVICE_ERR_NOT_INIT;
    }

    return run_cycle_impl();
}

sensor_service_err_t sensor_service_get_snapshot(sensor_snapshot_t *snap)
{
    if (!s_ss.initialised)
    {
        return SENSOR_SERVICE_ERR_NOT_INIT;
    }

    if (snap == NULL)
    {
        return SENSOR_SERVICE_ERR_NULL_ARG;
    }

    taskENTER_CRITICAL();
    (void) memcpy(snap, &s_ss.snapshot, sizeof(sensor_snapshot_t));
    taskEXIT_CRITICAL();

    return SENSOR_SERVICE_ERR_OK;
}

sensor_service_err_t sensor_service_subscribe(void (*cb)(const sensor_snapshot_t *snap))
{
    if (!s_ss.initialised)
    {
        return SENSOR_SERVICE_ERR_NOT_INIT;
    }

    if (cb == NULL)
    {
        return SENSOR_SERVICE_ERR_NULL_ARG;
    }

    if (s_ss.subscriber_count >= SENSOR_MAX_SUBSCRIBERS)
    {
        return SENSOR_SERVICE_ERR_NO_SUB;
    }

    s_ss.subscribers[s_ss.subscriber_count] = cb;
    s_ss.subscriber_count++;
    return SENSOR_SERVICE_ERR_OK;
}

sensor_service_err_t sensor_service_read_on_demand(void)
{
    if (!s_ss.initialised)
    {
        return SENSOR_SERVICE_ERR_NOT_INIT;
    }

    return run_cycle_impl();
}

bool sensor_service_is_ready(void)
{
    if (!s_ss.initialised)
    {
        return false;
    }

    for (int i = 0; i < (int) SENSOR_ID_COUNT; i++)
    {
        if (!s_ss.driver_failed[i] && !s_ss.snapshot.readings[i].valid)
        {
            return false;
        }
    }

    return true;
}

sensor_service_err_t sensor_service_reconfigure(void)
{
    if (!s_ss.initialised)
    {
        return SENSOR_SERVICE_ERR_NOT_INIT;
    }
    return SENSOR_SERVICE_ERR_OK;
}

/* ======================================================================= */
/* Private helpers                                                          */
/* ======================================================================= */

static sensor_service_err_t run_cycle_impl(void)
{
    sensor_snapshot_t working;

    taskENTER_CRITICAL();
    (void) memcpy(&working, &s_ss.snapshot, sizeof(sensor_snapshot_t));
    taskEXIT_CRITICAL();

#if !defined(BOARD_GATEWAY)
    bool first_fail = true;
    /* FD: temperature + humidity come from the same driver call. */
    if (!s_ss.driver_failed[SENSOR_ID_TEMPERATURE] || !s_ss.driver_failed[SENSOR_ID_HUMIDITY])
    {
        ht_reading_t ht = {0};
        bool ht_ok = (humidity_temp_read(&ht) == HT_ERR_OK);

        if (!s_ss.driver_failed[SENSOR_ID_TEMPERATURE])
        {
            /* temperature_x100 is int32_t in 0.01 °C units — use directly. */
            process_sensor_reading((int) SENSOR_ID_TEMPERATURE, ht_ok ? ht.temperature_x100 : 0,
                                   ht_ok, &first_fail, &working);
        }

        if (!s_ss.driver_failed[SENSOR_ID_HUMIDITY])
        {
            /* humidity_x100 is uint32_t; cast to int32_t (0..10000 fits safely). */
            process_sensor_reading((int) SENSOR_ID_HUMIDITY, ht_ok ? (int32_t) ht.humidity_x100 : 0,
                                   ht_ok, &first_fail, &working);
        }
    }

    if (!s_ss.driver_failed[SENSOR_ID_PRESSURE])
    {
        baro_reading_t baro = {0};
        bool baro_ok = (barometer_read(&baro) == BARO_ERR_OK);
        /* pressure_x10 is int32_t in 0.1 hPa units — use directly. */
        process_sensor_reading((int) SENSOR_ID_PRESSURE, baro_ok ? baro.pressure_x10 : 0, baro_ok,
                               &first_fail, &working);
    }
#endif /* !BOARD_GATEWAY */

    /* Atomically commit the processed snapshot and advance cycle counter. */
    taskENTER_CRITICAL();
    (void) memcpy(&s_ss.snapshot, &working, sizeof(sensor_snapshot_t));
    s_ss.snapshot.cycle_count++;
    taskEXIT_CRITICAL();

    /* Fire subscriber callbacks outside the critical section. */
    for (uint8_t i = 0U; i < s_ss.subscriber_count; i++)
    {
        if (s_ss.subscribers[i] != NULL)
        {
            s_ss.subscribers[i](&s_ss.snapshot);
        }
    }

    return SENSOR_SERVICE_ERR_OK;
}

static void process_sensor_reading(int id, int32_t raw_value, bool driver_ok,
                                   bool *first_fail_this_cycle, sensor_snapshot_t *working_snap)
{
    sensor_reading_t *r = &working_snap->readings[id];

    if (!driver_ok)
    {
        r->valid = false;
        LOG_WARN(MODULE_TAG, "Sensor %d driver error", id);
        if (*first_fail_this_cycle)
        {
            *first_fail_this_cycle = false;
            (void) health_report->push_event(HEALTH_EVENT_SENSOR_FAIL, (uint32_t) id);
        }
        return; /* skip steps 2–5 for this sensor */
    }

    /* Step 2: stamp (REQ-SA-100). */
    (void) time_provider_get(&r->timestamp);

    /* Step 3: range validate (REQ-SA-120). */
    if (raw_value < s_range_min[id] || raw_value > s_range_max[id])
    {
        r->valid = false;
    }
    else
    {
        r->valid = true;
    }

    /* Step 4: clamp (REQ-SA-130). */
    int32_t clamped = raw_value;
    if (clamped < s_range_min[id])
    {
        clamped = s_range_min[id];
    }
    if (clamped > s_range_max[id])
    {
        clamped = s_range_max[id];
    }

    /* Step 5: IIR low-pass filter (REQ-SA-140). Applied to clamped value.
     * filtered = (alpha_num * clamped + (alpha_den - alpha_num) * prev + alpha_den/2)
     *            / alpha_den
     * The half-denominator term provides round-to-nearest rather than truncation. */
    int32_t filtered = ((int32_t) s_ss.alpha_num * clamped +
                        (int32_t) (s_ss.alpha_den - s_ss.alpha_num) * s_ss.prev_filtered[id] +
                        (int32_t) (s_ss.alpha_den / 2U)) /
                       (int32_t) s_ss.alpha_den;
    s_ss.prev_filtered[id] = filtered;
    r->value = filtered;
}

static void poll_timer_cb(TimerHandle_t xTimer)
{
    (void) xTimer;
    xTaskNotifyGive(s_ss.sensor_task_handle);
}

/* ======================================================================= */
/* Test-only hooks                                                          */
/* ======================================================================= */

#ifdef TEST
void sensor_service_reset_for_test(void)
{
    (void) memset(&s_ss, 0, sizeof(s_ss));
}

void sensor_service_set_alpha_for_test(uint8_t num, uint8_t den)
{
    s_ss.alpha_num = num;
    s_ss.alpha_den = den;
}

void sensor_service_set_prev_filtered_for_test(int id, int32_t value)
{
    s_ss.prev_filtered[id] = value;
}

void sensor_service_get_prev_filtered_for_test(int32_t out[SENSOR_ID_COUNT])
{
    (void) memcpy(out, s_ss.prev_filtered, sizeof(int32_t) * (size_t) SENSOR_ID_COUNT);
}
#endif /* TEST */
