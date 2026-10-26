/**
 * @file sensor_service.c
 * @brief SensorService implementation (Gateway).
 *
 * @see docs/lld/application/sensor-alarm-service.md
 */

#include "sensor_service.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "sensor_service_internal.h"
#include "sensor_task.h"

#ifdef TEST
#define LOG_ERROR(m, f, ...) ((void) 0)
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

#define MODULE_TAG "SS"

/* ======================================================================= */
/* Constants                                                                */
/* ======================================================================= */

/* Default IIR coefficient alpha = NUM / DEN = 0.1 (SS-O1). */
#define SENSOR_ALPHA_NUM_DEFAULT (1U)
#define SENSOR_ALPHA_DEN_DEFAULT (10U)

/* ConfigService carries alpha in per-mille (config_params_t.filter_alpha, range 1..999). */
#define CONFIG_ALPHA_DEN (1000U)
#define CONFIG_ALPHA_MIN (1U)
#define CONFIG_ALPHA_MAX (999U)

/* Raw LSB -> engineering units. Sensitivities come from the sensor drivers' headers:
 * LSM6DSL +/-2 g = 16384 LSB/g, +/-250 dps = 8.75 mdps/LSB; LIS3MDL +/-4 gauss =
 * 6842 LSB/gauss. A raw int16 times the numerator stays inside int32_t. */
#define ACCEL_MG_NUM (1000)
#define ACCEL_MG_DEN (16384)
#define GYRO_MDPS_NUM (875)
#define GYRO_MDPS_DEN (100)
#define MAG_MGAUSS_NUM (1000)
#define MAG_MGAUSS_DEN (6842)

/* HTS221 and LPS22HB run at 1 Hz (GPA-D2). Reading them every 100 ms makes nine reads in ten
 * hit the driver's 2 ms not-ready retry sleep, so they are read every 10th cycle (SS-D19). */
#define GROUP_A_PERIOD_CYCLES_DEFAULT (10U)

/* Extra fractional bits kept in the IIR state: 16 steps per output unit (SS-D18). */
#define FILTER_FRAC_BITS (4)
#define FILTER_SCALE (1 << FILTER_FRAC_BITS)

/* Plausibility range per channel, in the units of sensor_reading_t.value. The three
 * scalar ranges are the sensor operating limits (REQ-SA-120); the IMU and
 * magnetometer ranges are the configured full scales. */
static const int32_t k_range_min_default[SENSOR_ID_COUNT] = {
    [SENSOR_ID_TEMPERATURE] = -4000, /* -40.00 degC  */
    [SENSOR_ID_HUMIDITY] = 0,        /*   0.00 %RH   */
    [SENSOR_ID_PRESSURE] = 2600,     /* 260.0 hPa    */
    [SENSOR_ID_ACCEL_X] = -2000,     [SENSOR_ID_ACCEL_Y] = -2000,  [SENSOR_ID_ACCEL_Z] = -2000,
    [SENSOR_ID_GYRO_X] = -250000,    [SENSOR_ID_GYRO_Y] = -250000, [SENSOR_ID_GYRO_Z] = -250000,
    [SENSOR_ID_MAG_X] = -4000,       [SENSOR_ID_MAG_Y] = -4000,    [SENSOR_ID_MAG_Z] = -4000,
};

static const int32_t k_range_max_default[SENSOR_ID_COUNT] = {
    [SENSOR_ID_TEMPERATURE] = 12000, /* 120.00 degC  */
    [SENSOR_ID_HUMIDITY] = 10000,    /* 100.00 %RH   */
    [SENSOR_ID_PRESSURE] = 12600,    /* 1260.0 hPa   */
    [SENSOR_ID_ACCEL_X] = 2000,      [SENSOR_ID_ACCEL_Y] = 2000,  [SENSOR_ID_ACCEL_Z] = 2000,
    [SENSOR_ID_GYRO_X] = 250000,     [SENSOR_ID_GYRO_Y] = 250000, [SENSOR_ID_GYRO_Z] = 250000,
    [SENSOR_ID_MAG_X] = 4000,        [SENSOR_ID_MAG_Y] = 4000,    [SENSOR_ID_MAG_Z] = 4000,
};

/* Working copies: the three scalar channels' limits can be narrowed by reconfigure() (SS-D20). */
static int32_t s_range_min[SENSOR_ID_COUNT];
static int32_t s_range_max[SENSOR_ID_COUNT];

/* ======================================================================= */
/* Module state                                                             */
/* ======================================================================= */

/* Outside s_ss so init does not reset it; tests set it to 1 to read Group A every cycle. */
static uint32_t s_group_a_period = GROUP_A_PERIOD_CYCLES_DEFAULT;

static struct
{
    bool initialised;
    health_monitor_handle_t health;
    sensor_snapshot_t snapshot;             /* guarded by a critical section */
    int32_t prev_filtered[SENSOR_ID_COUNT]; /* filter state, FILTER_SCALE times the value */
    bool filter_seeded[SENSOR_ID_COUNT];    /* first accepted sample seeds the filter */
    bool driver_failed[SENSOR_ID_COUNT];    /* permanent: init failed */
    bool fail_reported[SENSOR_ID_COUNT];    /* HEALTH_EVENT_SENSOR_FAIL sent, not yet recovered */
    uint16_t alpha_num;
    uint16_t alpha_den;
    void (*subscribers[SENSOR_MAX_SUBSCRIBERS])(const sensor_snapshot_t *);
    uint8_t subscriber_count;
} s_ss;

/* ======================================================================= */
/* Forward declarations                                                     */
/* ======================================================================= */

static int32_t prv_convert(sensor_id_t id, int16_t raw);
static int32_t prv_div_round(int32_t numerator, int32_t divisor);
static int64_t prv_div_round64(int64_t numerator, int64_t divisor);
static void prv_accept(sensor_snapshot_t *snap, sensor_id_t id, int32_t value);
static void prv_fail(sensor_snapshot_t *snap, sensor_id_t id);
static void prv_fail_range(sensor_snapshot_t *snap, sensor_id_t first, uint8_t count);
static void prv_read_humidity_temp(sensor_snapshot_t *snap);
static void prv_read_barometer(sensor_snapshot_t *snap);
static void prv_read_imu(sensor_snapshot_t *snap);
static void prv_read_magnetometer(sensor_snapshot_t *snap);
static void prv_init_driver(bool ok, sensor_id_t first, uint8_t count, const char *name);
static void prv_load_defaults(void);

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
/* Public API                                                               */
/* ======================================================================= */

sensor_service_err_t sensor_service_init(health_monitor_handle_t health)
{
    if (health == NULL)
    {
        return SENSOR_SERVICE_ERR_NULL_ARG;
    }

    (void) memset(&s_ss, 0, sizeof(s_ss));
    s_ss.health = health;
    prv_load_defaults();

    prv_init_driver(humidity_temp_driver->init() == HUMIDITY_TEMP_ERR_OK, SENSOR_ID_TEMPERATURE, 2U,
                    "humidity_temp");
    prv_init_driver(barometer_driver->init() == BAROMETER_ERR_OK, SENSOR_ID_PRESSURE, 1U,
                    "barometer");
    prv_init_driver(imu_driver->init() == IMU_ERR_OK, SENSOR_ID_ACCEL_X, 6U, "imu");
    prv_init_driver(magnetometer_driver->init() == MAGNETOMETER_ERR_OK, SENSOR_ID_MAG_X, 3U,
                    "magnetometer");

    s_ss.initialised = true;
    LOG_INFO(MODULE_TAG, "SensorService initialised");
    return SENSOR_SERVICE_ERR_OK;
}

sensor_service_err_t sensor_service_reconfigure(const config_params_t *params)
{
    if (!s_ss.initialised)
    {
        return SENSOR_SERVICE_ERR_NOT_INIT;
    }
    if (params == NULL)
    {
        return SENSOR_SERVICE_ERR_NULL_ARG;
    }

    /* Validate everything first so a bad configuration changes nothing (SS-D20). */
    if ((params->filter_alpha < CONFIG_ALPHA_MIN) || (params->filter_alpha > CONFIG_ALPHA_MAX) ||
        (params->polling_interval_ms < SENSOR_TASK_PERIOD_MS) ||
        (params->temp_range_min >= params->temp_range_max) ||
        (params->humidity_range_min >= params->humidity_range_max) ||
        (params->pressure_range_min >= params->pressure_range_max))
    {
        LOG_WARN(MODULE_TAG, "reconfigure: configuration rejected");
        return SENSOR_SERVICE_ERR_INVALID_ARG;
    }

    /* Group A (temperature, humidity, pressure) is the 1 Hz group: its read period is the
     * configured polling interval in acquisition cycles. */
    const uint32_t group_a_cycles = params->polling_interval_ms / SENSOR_TASK_PERIOD_MS;

    taskENTER_CRITICAL();
    s_ss.alpha_num = params->filter_alpha;
    s_ss.alpha_den = (uint16_t) CONFIG_ALPHA_DEN;
    s_range_min[SENSOR_ID_TEMPERATURE] = (int32_t) params->temp_range_min;
    s_range_max[SENSOR_ID_TEMPERATURE] = (int32_t) params->temp_range_max;
    s_range_min[SENSOR_ID_HUMIDITY] = (int32_t) params->humidity_range_min;
    s_range_max[SENSOR_ID_HUMIDITY] = (int32_t) params->humidity_range_max;
    s_range_min[SENSOR_ID_PRESSURE] = (int32_t) params->pressure_range_min;
    s_range_max[SENSOR_ID_PRESSURE] = (int32_t) params->pressure_range_max;
    s_group_a_period = group_a_cycles;
    taskEXIT_CRITICAL();

    LOG_INFO(MODULE_TAG, "reconfigured: alpha=%u/1000, group A every %lu cycles",
             (unsigned) params->filter_alpha, (unsigned long) group_a_cycles);
    return SENSOR_SERVICE_ERR_OK;
}

sensor_service_err_t sensor_service_run_cycle(void)
{
    if (!s_ss.initialised)
    {
        return SENSOR_SERVICE_ERR_NOT_INIT;
    }

    sensor_snapshot_t working;

    taskENTER_CRITICAL();
    (void) memcpy(&working, &s_ss.snapshot, sizeof(working));
    taskEXIT_CRITICAL();

    /* Group A: at its own 1 Hz rate, and every cycle while a channel has no valid reading
     * (before the first sample, or after a fault) so start-up and recovery are not delayed. */
    const bool group_a_due = ((working.cycle_count % s_group_a_period) == 0U);
    if (group_a_due || !working.readings[SENSOR_ID_TEMPERATURE].valid)
    {
        prv_read_humidity_temp(&working);
    }
    if (group_a_due || !working.readings[SENSOR_ID_PRESSURE].valid)
    {
        prv_read_barometer(&working);
    }
    /* GPB-O6: both Group B sensors are read every cycle, whatever happens to the sample,
     * because the sensor holds DRDY / INT1 high until its output registers are read. */
    prv_read_imu(&working);
    prv_read_magnetometer(&working);

    working.cycle_count++;

    taskENTER_CRITICAL();
    (void) memcpy(&s_ss.snapshot, &working, sizeof(working));
    taskEXIT_CRITICAL();

    for (uint8_t i = 0U; i < s_ss.subscriber_count; i++)
    {
        s_ss.subscribers[i](&working);
    }

    return SENSOR_SERVICE_ERR_OK;
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
    (void) memcpy(snap, &s_ss.snapshot, sizeof(*snap));
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

bool sensor_service_is_ready(void)
{
    if (!s_ss.initialised)
    {
        return false;
    }

    sensor_snapshot_t snap;
    taskENTER_CRITICAL();
    (void) memcpy(&snap, &s_ss.snapshot, sizeof(snap));
    taskEXIT_CRITICAL();

    for (uint8_t i = 0U; i < (uint8_t) SENSOR_ID_COUNT; i++)
    {
        if (!s_ss.driver_failed[i] && !snap.readings[i].valid)
        {
            return false;
        }
    }
    return true;
}

sensor_service_err_t sensor_service_read_on_demand(void)
{
    return sensor_service_run_cycle();
}

/* ======================================================================= */
/* Private helpers                                                          */
/* ======================================================================= */

static void prv_init_driver(bool ok, sensor_id_t first, uint8_t count, const char *name)
{
    if (ok)
    {
        return;
    }

    LOG_ERROR(MODULE_TAG, "%s init failed - channels marked failed", name);
    for (uint8_t i = 0U; i < count; i++)
    {
        s_ss.driver_failed[(uint8_t) first + i] = true;
    }
    (void) health_monitor_push_event(s_ss.health, HEALTH_EVENT_SENSOR_FAIL, (uint32_t) first);
    s_ss.fail_reported[first] = true;
}

/* Integer division rounded half away from zero. */
static int32_t prv_div_round(int32_t numerator, int32_t divisor)
{
    const int32_t half = divisor / 2;
    return (numerator + ((numerator >= 0) ? half : -half)) / divisor;
}

static int64_t prv_div_round64(int64_t numerator, int64_t divisor)
{
    const int64_t half = divisor / 2;
    return (numerator + ((numerator >= 0) ? half : -half)) / divisor;
}

static void prv_load_defaults(void)
{
    s_ss.alpha_num = SENSOR_ALPHA_NUM_DEFAULT;
    s_ss.alpha_den = SENSOR_ALPHA_DEN_DEFAULT;
    (void) memcpy(s_range_min, k_range_min_default, sizeof(s_range_min));
    (void) memcpy(s_range_max, k_range_max_default, sizeof(s_range_max));
}

/* Convert a raw IMU / magnetometer LSB count into the channel's engineering unit. */
static int32_t prv_convert(sensor_id_t id, int16_t raw)
{
    int32_t value = (int32_t) raw;

    switch (id)
    {
    case SENSOR_ID_ACCEL_X:
    case SENSOR_ID_ACCEL_Y:
    case SENSOR_ID_ACCEL_Z:
        value = (value * ACCEL_MG_NUM) / ACCEL_MG_DEN;
        break;
    case SENSOR_ID_GYRO_X:
    case SENSOR_ID_GYRO_Y:
    case SENSOR_ID_GYRO_Z:
        value = (value * GYRO_MDPS_NUM) / GYRO_MDPS_DEN;
        break;
    case SENSOR_ID_MAG_X:
    case SENSOR_ID_MAG_Y:
    case SENSOR_ID_MAG_Z:
        value = (value * MAG_MGAUSS_NUM) / MAG_MGAUSS_DEN;
        break;
    default:
        break;
    }
    return value;
}

/* Stamp, range-validate, clamp and filter one good driver sample (SD-01 steps 2-5). */
static void prv_accept(sensor_snapshot_t *snap, sensor_id_t id, int32_t value)
{
    sensor_reading_t *r = &snap->readings[id];

    (void) time_provider_get(&r->timestamp);

    r->valid = (value >= s_range_min[id]) && (value <= s_range_max[id]);

    int32_t clamped = value;
    if (clamped < s_range_min[id])
    {
        clamped = s_range_min[id];
    }
    if (clamped > s_range_max[id])
    {
        clamped = s_range_max[id];
    }

    /* The filter state carries FILTER_FRAC_BITS extra bits (SS-D18). With plain integer
     * state a step smaller than den / (2 * num) rounds to zero and the output sticks: the
     * accelerometer (mg) sat at -166 / -90 / -950 for 30 s on the bench. */
    const int32_t scaled = clamped * FILTER_SCALE;

    /* First accepted sample seeds the filter. Starting from zero would walk a pressure
     * channel up from 0 hPa and raise a false low-pressure alarm on every boot. */
    if (!s_ss.filter_seeded[id])
    {
        s_ss.prev_filtered[id] = scaled;
        s_ss.filter_seeded[id] = true;
    }

    /* 64-bit: a per-mille alpha (den 1000) times a gyro value in mdps scaled by FILTER_SCALE
     * would overflow int32_t (SS-D20). The result is a weighted mean of two int32_t values, so
     * it fits back. */
    const int64_t num = (int64_t) s_ss.alpha_num;
    const int64_t den = (int64_t) s_ss.alpha_den;
    const int64_t sum = (num * (int64_t) scaled) + ((den - num) * (int64_t) s_ss.prev_filtered[id]);
    s_ss.prev_filtered[id] = (int32_t) prv_div_round64(sum, den);

    /* Back to the channel's unit. C division truncates toward zero, so the half-divisor
     * bias must follow the sign (SS-D13); a plain "+ half" rounds negative axes wrongly. */
    r->value = prv_div_round(s_ss.prev_filtered[id], FILTER_SCALE);

    if (s_ss.fail_reported[id] && r->valid)
    {
        s_ss.fail_reported[id] = false;
        (void) health_monitor_push_event(s_ss.health, HEALTH_EVENT_SENSOR_RECOVERED, (uint32_t) id);
    }
}

/* Mark a channel invalid after a driver error; report only the first failure (edge). */
static void prv_fail(sensor_snapshot_t *snap, sensor_id_t id)
{
    snap->readings[id].valid = false;

    if (!s_ss.fail_reported[id])
    {
        s_ss.fail_reported[id] = true;
        LOG_WARN(MODULE_TAG, "sensor %u driver error", (unsigned) id);
        (void) health_monitor_push_event(s_ss.health, HEALTH_EVENT_SENSOR_FAIL, (uint32_t) id);
    }
}

static void prv_fail_range(sensor_snapshot_t *snap, sensor_id_t first, uint8_t count)
{
    for (uint8_t i = 0U; i < count; i++)
    {
        prv_fail(snap, (sensor_id_t) ((uint8_t) first + i));
    }
}

/* Both Group A sensors run at 1 Hz (GPA-D2) against a 100 ms cycle, so most reads return
 * NOT_READY: no new sample, keep the previous reading. Only a bus error is a fault. */
static void prv_read_humidity_temp(sensor_snapshot_t *snap)
{
    if (s_ss.driver_failed[SENSOR_ID_TEMPERATURE])
    {
        return;
    }

    humidity_temp_data_t ht = {0};
    const humidity_temp_err_t err = humidity_temp_driver->read(&ht);
    if (err == HUMIDITY_TEMP_ERR_OK)
    {
        prv_accept(snap, SENSOR_ID_TEMPERATURE, ht.temperature_x100);
        prv_accept(snap, SENSOR_ID_HUMIDITY, ht.humidity_x100);
    }
    else if (err != HUMIDITY_TEMP_ERR_NOT_READY)
    {
        prv_fail_range(snap, SENSOR_ID_TEMPERATURE, 2U);
    }
    else
    {
        /* No new sample: keep the previous reading. */
    }
}

static void prv_read_barometer(sensor_snapshot_t *snap)
{
    if (s_ss.driver_failed[SENSOR_ID_PRESSURE])
    {
        return;
    }

    barometer_data_t baro = {0};
    const barometer_err_t err = barometer_driver->read(&baro);
    if (err == BAROMETER_ERR_OK)
    {
        prv_accept(snap, SENSOR_ID_PRESSURE, baro.pressure_x10);
    }
    else if (err != BAROMETER_ERR_NOT_READY)
    {
        prv_fail(snap, SENSOR_ID_PRESSURE);
    }
    else
    {
        /* No new sample: keep the previous reading. */
    }
}

/* Group B: same rule as Group A - NOT_READY keeps the previous reading (SS-D17). */
static void prv_read_imu(sensor_snapshot_t *snap)
{
    if (s_ss.driver_failed[SENSOR_ID_ACCEL_X])
    {
        return;
    }

    imu_data_t imu = {0};
    const imu_err_t err = imu_driver->read(&imu);
    if (err == IMU_ERR_OK)
    {
        prv_accept(snap, SENSOR_ID_ACCEL_X, prv_convert(SENSOR_ID_ACCEL_X, imu.accel_x));
        prv_accept(snap, SENSOR_ID_ACCEL_Y, prv_convert(SENSOR_ID_ACCEL_Y, imu.accel_y));
        prv_accept(snap, SENSOR_ID_ACCEL_Z, prv_convert(SENSOR_ID_ACCEL_Z, imu.accel_z));
        prv_accept(snap, SENSOR_ID_GYRO_X, prv_convert(SENSOR_ID_GYRO_X, imu.gyro_x));
        prv_accept(snap, SENSOR_ID_GYRO_Y, prv_convert(SENSOR_ID_GYRO_Y, imu.gyro_y));
        prv_accept(snap, SENSOR_ID_GYRO_Z, prv_convert(SENSOR_ID_GYRO_Z, imu.gyro_z));
    }
    else if (err != IMU_ERR_NOT_READY)
    {
        prv_fail_range(snap, SENSOR_ID_ACCEL_X, 6U);
    }
    else
    {
        /* No new sample: keep the previous reading. */
    }
}

static void prv_read_magnetometer(sensor_snapshot_t *snap)
{
    if (s_ss.driver_failed[SENSOR_ID_MAG_X])
    {
        return;
    }

    magnetometer_data_t mag = {0};
    const magnetometer_err_t err = magnetometer_driver->read(&mag);
    if (err == MAGNETOMETER_ERR_OK)
    {
        prv_accept(snap, SENSOR_ID_MAG_X, prv_convert(SENSOR_ID_MAG_X, mag.x));
        prv_accept(snap, SENSOR_ID_MAG_Y, prv_convert(SENSOR_ID_MAG_Y, mag.y));
        prv_accept(snap, SENSOR_ID_MAG_Z, prv_convert(SENSOR_ID_MAG_Z, mag.z));
    }
    else if (err != MAGNETOMETER_ERR_NOT_READY)
    {
        prv_fail_range(snap, SENSOR_ID_MAG_X, 3U);
    }
    else
    {
        /* No new sample: keep the previous reading. */
    }
}

/* ======================================================================= */
/* Test-only hooks                                                          */
/* ======================================================================= */

#ifdef TEST
void sensor_service_reset_for_test(void)
{
    (void) memset(&s_ss, 0, sizeof(s_ss));
    s_group_a_period = GROUP_A_PERIOD_CYCLES_DEFAULT;
    prv_load_defaults();
}

void sensor_service_set_alpha_for_test(uint16_t num, uint16_t den)
{
    s_ss.alpha_num = num;
    s_ss.alpha_den = den;
}

int32_t sensor_service_convert_for_test(sensor_id_t id, int16_t raw)
{
    return prv_convert(id, raw);
}
#endif /* TEST */

void sensor_service_set_group_a_period_for_test(uint32_t cycles)
{
    s_group_a_period = cycles;
}
