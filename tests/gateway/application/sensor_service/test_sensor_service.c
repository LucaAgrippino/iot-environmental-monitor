/**
 * @file test_sensor_service.c
 * @brief Unity tests for SensorService (Gateway).
 *
 * The four sensor drivers, TimeProvider and HealthMonitor are replaced by spies
 * defined in this file; the test never includes their headers directly, so Ceedling
 * links none of their .c files. FreeRTOS comes from freertos_mock.
 */

#include <string.h>

#include "freertos_mock.h"
#include "sensor_service.h"
#include "sensor_service_internal.h"
#include "unity.h"

#define HEALTH_HANDLE ((health_monitor_handle_t) 0x1234)
#define ALPHA_OFF_NUM (10U) /* alpha = 1: filter passes the input through */
#define ALPHA_OFF_DEN (10U)

/* ===================================================================== */
/* Spies                                                                 */
/* ===================================================================== */

static humidity_temp_err_t s_ht_init_result;
static humidity_temp_err_t s_ht_read_result;
static humidity_temp_data_t s_ht_data;
static uint32_t s_ht_read_calls;

static barometer_err_t s_baro_init_result;
static barometer_err_t s_baro_read_result;
static barometer_data_t s_baro_data;
static uint32_t s_baro_read_calls;

static imu_err_t s_imu_init_result;
static imu_err_t s_imu_read_result;
static imu_data_t s_imu_data;
static uint32_t s_imu_read_calls;

static magnetometer_err_t s_mag_init_result;
static magnetometer_err_t s_mag_read_result;
static magnetometer_data_t s_mag_data;
static uint32_t s_mag_read_calls;

static health_event_t s_events[16];
static uint32_t s_event_params[16];
static uint32_t s_event_count;

static uint32_t s_cb_calls;
static uint32_t s_cb_last_cycle;

static humidity_temp_err_t spy_ht_init(void)
{
    return s_ht_init_result;
}

static humidity_temp_err_t spy_ht_read(humidity_temp_data_t *out)
{
    s_ht_read_calls++;
    if (s_ht_read_result == HUMIDITY_TEMP_ERR_OK)
    {
        *out = s_ht_data;
    }
    return s_ht_read_result;
}

static barometer_err_t spy_baro_init(void)
{
    return s_baro_init_result;
}

static barometer_err_t spy_baro_read(barometer_data_t *out)
{
    s_baro_read_calls++;
    if (s_baro_read_result == BAROMETER_ERR_OK)
    {
        *out = s_baro_data;
    }
    return s_baro_read_result;
}

static imu_err_t spy_imu_init(void)
{
    return s_imu_init_result;
}

static imu_err_t spy_imu_attach(imu_int1_cb_t cb, void *ctx)
{
    (void) cb;
    (void) ctx;
    return IMU_ERR_OK;
}

static imu_err_t spy_imu_read(imu_data_t *out)
{
    s_imu_read_calls++;
    if (s_imu_read_result == IMU_ERR_OK)
    {
        *out = s_imu_data;
    }
    return s_imu_read_result;
}

static magnetometer_err_t spy_mag_init(void)
{
    return s_mag_init_result;
}

static magnetometer_err_t spy_mag_attach(magnetometer_drdy_cb_t cb, void *ctx)
{
    (void) cb;
    (void) ctx;
    return MAGNETOMETER_ERR_OK;
}

static magnetometer_err_t spy_mag_read(magnetometer_data_t *out)
{
    s_mag_read_calls++;
    if (s_mag_read_result == MAGNETOMETER_ERR_OK)
    {
        *out = s_mag_data;
    }
    return s_mag_read_result;
}

/* The drivers' singleton pointers are defined here instead of by the real driver .c files. */
static const ihumidity_temp_t s_ht_vtable = {.init = spy_ht_init, .read = spy_ht_read};
static const ibarometer_t s_baro_vtable = {.init = spy_baro_init, .read = spy_baro_read};
static const iimu_t s_imu_vtable = {
    .init = spy_imu_init, .attach_int1_callback = spy_imu_attach, .read = spy_imu_read};
static const imagnetometer_t s_mag_vtable = {
    .init = spy_mag_init, .attach_drdy_callback = spy_mag_attach, .read = spy_mag_read};

const ihumidity_temp_t *const humidity_temp_driver = &s_ht_vtable;
const ibarometer_t *const barometer_driver = &s_baro_vtable;
const iimu_t *const imu_driver = &s_imu_vtable;
const imagnetometer_t *const magnetometer_driver = &s_mag_vtable;

time_provider_err_t time_provider_get(time_provider_ts_t *ts_out)
{
    ts_out->epoch = 1700000000U;
    ts_out->sync_state = TIME_SYNC_SYNCHRONISED;
    return TIME_PROVIDER_ERR_OK;
}

health_monitor_err_t health_monitor_push_event(health_monitor_handle_t handle, health_event_t event,
                                               uint32_t param)
{
    (void) handle;
    if (s_event_count < (sizeof(s_events) / sizeof(s_events[0])))
    {
        s_events[s_event_count] = event;
        s_event_params[s_event_count] = param;
    }
    s_event_count++;
    return HEALTH_MONITOR_ERR_OK;
}

static void on_snapshot(const sensor_snapshot_t *snap)
{
    s_cb_calls++;
    s_cb_last_cycle = snap->cycle_count;
}

static void on_snapshot_other(const sensor_snapshot_t *snap)
{
    (void) snap;
}

/* ===================================================================== */
/* Fixture                                                               */
/* ===================================================================== */

static void set_good_readings(void)
{
    s_ht_data.temperature_x100 = 2350;
    s_ht_data.humidity_x100 = 5500;
    s_baro_data.pressure_x10 = 10132;
    (void) memset(&s_imu_data, 0, sizeof(s_imu_data));
    (void) memset(&s_mag_data, 0, sizeof(s_mag_data));
}

void setUp(void)
{
    sensor_service_reset_for_test();
    s_ht_init_result = HUMIDITY_TEMP_ERR_OK;
    s_ht_read_result = HUMIDITY_TEMP_ERR_OK;
    s_baro_init_result = BAROMETER_ERR_OK;
    s_baro_read_result = BAROMETER_ERR_OK;
    s_imu_init_result = IMU_ERR_OK;
    s_imu_read_result = IMU_ERR_OK;
    s_mag_init_result = MAGNETOMETER_ERR_OK;
    s_mag_read_result = MAGNETOMETER_ERR_OK;
    s_ht_read_calls = 0U;
    s_baro_read_calls = 0U;
    s_imu_read_calls = 0U;
    s_mag_read_calls = 0U;
    s_event_count = 0U;
    s_cb_calls = 0U;
    s_cb_last_cycle = 0U;
    set_good_readings();
}

void tearDown(void)
{
}

static sensor_snapshot_t snapshot_after_init_and_cycle(void)
{
    sensor_snapshot_t snap;
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, sensor_service_init(HEALTH_HANDLE));
    sensor_service_set_alpha_for_test(ALPHA_OFF_NUM, ALPHA_OFF_DEN);
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, sensor_service_run_cycle());
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, sensor_service_get_snapshot(&snap));
    return snap;
}

/* ===================================================================== */
/* Init and argument checks                                              */
/* ===================================================================== */

/* TC-SS-100 */
void test_init_rejects_null_health_handle(void)
{
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_NULL_ARG, sensor_service_init(NULL));
}

/* TC-SS-016 */
void test_calls_before_init_return_not_init(void)
{
    sensor_snapshot_t snap;

    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_NOT_INIT, sensor_service_run_cycle());
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_NOT_INIT, sensor_service_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_NOT_INIT, sensor_service_subscribe(on_snapshot));
    TEST_ASSERT_FALSE(sensor_service_is_ready());
}

/* TC-SS-015 */
void test_get_snapshot_rejects_null(void)
{
    (void) sensor_service_init(HEALTH_HANDLE);
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_NULL_ARG, sensor_service_get_snapshot(NULL));
}

/* TC-SS-010 */
void test_subscribe_rejects_null_and_full_table(void)
{
    (void) sensor_service_init(HEALTH_HANDLE);

    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_NULL_ARG, sensor_service_subscribe(NULL));
    for (uint8_t i = 0U; i < SENSOR_MAX_SUBSCRIBERS; i++)
    {
        TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, sensor_service_subscribe(on_snapshot_other));
    }
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_NO_SUB, sensor_service_subscribe(on_snapshot_other));
}

/* TC-SS-101 */
void test_vtable_exposes_the_public_functions(void)
{
    TEST_ASSERT_NOT_NULL(sensor_service);
    TEST_ASSERT_TRUE(sensor_service->init == sensor_service_init);
    TEST_ASSERT_TRUE(sensor_service->run_cycle == sensor_service_run_cycle);
    TEST_ASSERT_TRUE(sensor_service->get_snapshot == sensor_service_get_snapshot);
    TEST_ASSERT_TRUE(sensor_service->subscribe == sensor_service_subscribe);
    TEST_ASSERT_TRUE(sensor_service->read_on_demand == sensor_service_read_on_demand);
    TEST_ASSERT_TRUE(sensor_service->is_ready == sensor_service_is_ready);
}

/* ===================================================================== */
/* Acquisition                                                           */
/* ===================================================================== */

/* TC-SS-003 */
void test_good_cycle_fills_scalar_channels_and_stamps_them(void)
{
    sensor_snapshot_t snap = snapshot_after_init_and_cycle();

    TEST_ASSERT_TRUE(snap.readings[SENSOR_ID_TEMPERATURE].valid);
    TEST_ASSERT_EQUAL_INT32(2350, snap.readings[SENSOR_ID_TEMPERATURE].value);
    TEST_ASSERT_EQUAL_INT32(5500, snap.readings[SENSOR_ID_HUMIDITY].value);
    TEST_ASSERT_EQUAL_INT32(10132, snap.readings[SENSOR_ID_PRESSURE].value);
    TEST_ASSERT_EQUAL_UINT32(1700000000U, snap.readings[SENSOR_ID_PRESSURE].timestamp.epoch);
    TEST_ASSERT_EQUAL_UINT32(1U, snap.cycle_count);
    TEST_ASSERT_TRUE(sensor_service_is_ready());
}

/* TC-SS-102 */
void test_imu_and_magnetometer_raw_counts_are_converted_to_units(void)
{
    s_imu_data.accel_x = 16384;  /* +1 g            -> 1000 mg   */
    s_imu_data.accel_z = -16384; /* -1 g            -> -1000 mg  */
    s_imu_data.gyro_x = 1000;    /* 8.75 mdps/LSB   -> 8750 mdps */
    s_imu_data.gyro_y = -2000;   /*                 -> -17500    */
    s_mag_data.x = 6842;         /* 1 gauss         -> 1000 mG   */
    s_mag_data.z = -3421;        /* -0.5 gauss      -> -500 mG   */

    sensor_snapshot_t snap = snapshot_after_init_and_cycle();

    TEST_ASSERT_EQUAL_INT32(1000, snap.readings[SENSOR_ID_ACCEL_X].value);
    TEST_ASSERT_EQUAL_INT32(-1000, snap.readings[SENSOR_ID_ACCEL_Z].value);
    TEST_ASSERT_EQUAL_INT32(8750, snap.readings[SENSOR_ID_GYRO_X].value);
    TEST_ASSERT_EQUAL_INT32(-17500, snap.readings[SENSOR_ID_GYRO_Y].value);
    TEST_ASSERT_EQUAL_INT32(1000, snap.readings[SENSOR_ID_MAG_X].value);
    TEST_ASSERT_EQUAL_INT32(-500, snap.readings[SENSOR_ID_MAG_Z].value);
}

/* TC-SS-103 */
void test_conversion_of_extreme_raw_counts_does_not_overflow(void)
{
    TEST_ASSERT_EQUAL_INT32(1999, sensor_service_convert_for_test(SENSOR_ID_ACCEL_Y, 32767));
    TEST_ASSERT_EQUAL_INT32(286711, sensor_service_convert_for_test(SENSOR_ID_GYRO_Z, 32767));
    TEST_ASSERT_EQUAL_INT32(-286720, sensor_service_convert_for_test(SENSOR_ID_GYRO_Z, -32768));
    TEST_ASSERT_EQUAL_INT32(4789, sensor_service_convert_for_test(SENSOR_ID_MAG_Y, 32767));
}

/* TC-SS-011 */
void test_subscriber_runs_after_each_cycle_with_the_new_snapshot(void)
{
    (void) sensor_service_init(HEALTH_HANDLE);
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, sensor_service_subscribe(on_snapshot));

    (void) sensor_service_run_cycle();
    (void) sensor_service_run_cycle();

    TEST_ASSERT_EQUAL_UINT32(2U, s_cb_calls);
    TEST_ASSERT_EQUAL_UINT32(2U, s_cb_last_cycle);
}

/* TC-SS-012 */
void test_read_on_demand_runs_one_extra_cycle_and_notifies_subscribers(void)
{
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_NOT_INIT, sensor_service_read_on_demand());

    (void) sensor_service_init(HEALTH_HANDLE);
    (void) sensor_service_subscribe(on_snapshot);
    (void) sensor_service_run_cycle();
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, sensor_service_read_on_demand());

    TEST_ASSERT_EQUAL_UINT32(2U, s_cb_calls);
    TEST_ASSERT_EQUAL_UINT32(2U, s_cb_last_cycle);
}

/* ===================================================================== */
/* Validation, clamping, filter                                          */
/* ===================================================================== */

/* TC-SS-005/006/008 */
void test_out_of_range_sample_is_invalid_and_clamped(void)
{
    s_ht_data.temperature_x100 = 20000; /* 200 degC, above the 120 degC limit */

    sensor_snapshot_t snap = snapshot_after_init_and_cycle();

    TEST_ASSERT_FALSE(snap.readings[SENSOR_ID_TEMPERATURE].valid);
    TEST_ASSERT_EQUAL_INT32(12000, snap.readings[SENSOR_ID_TEMPERATURE].value);
    TEST_ASSERT_TRUE(snap.readings[SENSOR_ID_HUMIDITY].valid);
}

/* TC-SS-104 */
void test_first_sample_seeds_the_filter_instead_of_ramping_from_zero(void)
{
    (void) sensor_service_init(HEALTH_HANDLE); /* default alpha = 0.1 */
    (void) sensor_service_run_cycle();

    sensor_snapshot_t snap;
    (void) sensor_service_get_snapshot(&snap);
    TEST_ASSERT_EQUAL_INT32(10132, snap.readings[SENSOR_ID_PRESSURE].value);
}

/* TC-SS-007 */
void test_filter_moves_one_tenth_of_the_step_each_cycle(void)
{
    (void) sensor_service_init(HEALTH_HANDLE);
    (void) sensor_service_run_cycle(); /* seeds at 2350 */

    s_ht_data.temperature_x100 = 3350; /* step of +1000 */
    (void) sensor_service_run_cycle();

    sensor_snapshot_t snap;
    (void) sensor_service_get_snapshot(&snap);
    TEST_ASSERT_EQUAL_INT32(2450, snap.readings[SENSOR_ID_TEMPERATURE].value);
}

/* ===================================================================== */
/* Driver faults                                                         */
/* ===================================================================== */

/* TC-SS-004 */
void test_scalar_driver_error_invalidates_channels_and_reports_once(void)
{
    (void) sensor_service_init(HEALTH_HANDLE);
    (void) sensor_service_run_cycle();
    s_ht_read_result = HUMIDITY_TEMP_ERR_I2C;

    (void) sensor_service_run_cycle();
    (void) sensor_service_run_cycle();

    sensor_snapshot_t snap;
    (void) sensor_service_get_snapshot(&snap);
    TEST_ASSERT_FALSE(snap.readings[SENSOR_ID_TEMPERATURE].valid);
    TEST_ASSERT_FALSE(snap.readings[SENSOR_ID_HUMIDITY].valid);
    TEST_ASSERT_TRUE(snap.readings[SENSOR_ID_PRESSURE].valid);
    TEST_ASSERT_FALSE(sensor_service_is_ready());
    /* One FAIL per failed channel, not one per cycle. */
    TEST_ASSERT_EQUAL_UINT32(2U, s_event_count);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_SENSOR_FAIL, s_events[0]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t) SENSOR_ID_TEMPERATURE, s_event_params[0]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t) SENSOR_ID_HUMIDITY, s_event_params[1]);
}

/* TC-SS-106 */
void test_recovery_after_a_fault_reports_recovered(void)
{
    (void) sensor_service_init(HEALTH_HANDLE);
    s_baro_read_result = BAROMETER_ERR_I2C;
    (void) sensor_service_run_cycle();
    s_baro_read_result = BAROMETER_ERR_OK;
    (void) sensor_service_run_cycle();

    TEST_ASSERT_EQUAL_UINT32(2U, s_event_count);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_SENSOR_FAIL, s_events[0]);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_SENSOR_RECOVERED, s_events[1]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t) SENSOR_ID_PRESSURE, s_event_params[1]);
    TEST_ASSERT_TRUE(sensor_service_is_ready());
}

/* TC-SS-002 */
void test_failed_driver_init_marks_channels_failed_but_others_continue(void)
{
    s_baro_init_result = BAROMETER_ERR_I2C;
    (void) sensor_service_init(HEALTH_HANDLE);

    (void) sensor_service_run_cycle();

    sensor_snapshot_t snap;
    (void) sensor_service_get_snapshot(&snap);
    TEST_ASSERT_EQUAL_UINT32(0U, s_baro_read_calls); /* not polled after a failed init */
    TEST_ASSERT_TRUE(snap.readings[SENSOR_ID_TEMPERATURE].valid);
    TEST_ASSERT_EQUAL_UINT32(1U, s_event_count);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_SENSOR_FAIL, s_events[0]);
    TEST_ASSERT_TRUE(sensor_service_is_ready()); /* failed sensors do not block readiness */
}

/* TC-SS-107 — GPB-O6 */
void test_group_b_drivers_are_read_every_cycle_even_when_not_ready(void)
{
    s_imu_read_result = IMU_ERR_NOT_READY;
    s_mag_read_result = MAGNETOMETER_ERR_NOT_READY;
    (void) sensor_service_init(HEALTH_HANDLE);

    (void) sensor_service_run_cycle();
    (void) sensor_service_run_cycle();
    (void) sensor_service_run_cycle();

    TEST_ASSERT_EQUAL_UINT32(3U, s_imu_read_calls);
    TEST_ASSERT_EQUAL_UINT32(3U, s_mag_read_calls);
}

/* TC-SS-108 */
void test_not_ready_keeps_the_previous_reading_and_raises_no_fault(void)
{
    s_mag_data.x = 6842;
    (void) sensor_service_init(HEALTH_HANDLE);
    (void) sensor_service_run_cycle();

    s_mag_read_result = MAGNETOMETER_ERR_NOT_READY;
    (void) sensor_service_run_cycle();

    sensor_snapshot_t snap;
    (void) sensor_service_get_snapshot(&snap);
    TEST_ASSERT_TRUE(snap.readings[SENSOR_ID_MAG_X].valid);
    TEST_ASSERT_EQUAL_INT32(1000, snap.readings[SENSOR_ID_MAG_X].value);
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
}

/* TC-SS-109 */
void test_group_b_bus_error_invalidates_all_axes_and_reports(void)
{
    (void) sensor_service_init(HEALTH_HANDLE);
    (void) sensor_service_run_cycle();
    s_imu_read_result = IMU_ERR_I2C;

    (void) sensor_service_run_cycle();

    sensor_snapshot_t snap;
    (void) sensor_service_get_snapshot(&snap);
    for (uint8_t id = (uint8_t) SENSOR_ID_ACCEL_X; id <= (uint8_t) SENSOR_ID_GYRO_Z; id++)
    {
        TEST_ASSERT_FALSE(snap.readings[id].valid);
    }
    TEST_ASSERT_TRUE(snap.readings[SENSOR_ID_MAG_X].valid);
    TEST_ASSERT_EQUAL_UINT32(6U, s_event_count);
}

/* TC-SS-014 */
void test_not_ready_before_first_sample_leaves_readiness_false(void)
{
    s_imu_read_result = IMU_ERR_NOT_READY;
    (void) sensor_service_init(HEALTH_HANDLE);
    (void) sensor_service_run_cycle();

    TEST_ASSERT_FALSE(sensor_service_is_ready());
}

/* TC-SS-105 - a half-way filter result rounds away from zero for both signs */
void test_filter_rounds_halfway_results_symmetrically(void)
{
    sensor_snapshot_t snap;

    (void) sensor_service_init(HEALTH_HANDLE);
    sensor_service_set_alpha_for_test(1U, 2U); /* alpha = 0.5 */

    s_ht_data.temperature_x100 = 100;
    (void) sensor_service_run_cycle(); /* seeds at +100 */
    s_ht_data.temperature_x100 = 101;
    (void) sensor_service_run_cycle(); /* (101 + 100) / 2 = 100.5 -> 101 */
    (void) sensor_service_get_snapshot(&snap);
    TEST_ASSERT_EQUAL_INT32(101, snap.readings[SENSOR_ID_TEMPERATURE].value);

    sensor_service_reset_for_test();
    (void) sensor_service_init(HEALTH_HANDLE);
    sensor_service_set_alpha_for_test(1U, 2U);

    s_ht_data.temperature_x100 = -100;
    (void) sensor_service_run_cycle(); /* seeds at -100 */
    s_ht_data.temperature_x100 = -101;
    (void) sensor_service_run_cycle(); /* (-101 + -100) / 2 = -100.5 -> -101 */
    (void) sensor_service_get_snapshot(&snap);
    TEST_ASSERT_EQUAL_INT32(-101, snap.readings[SENSOR_ID_TEMPERATURE].value);
}
