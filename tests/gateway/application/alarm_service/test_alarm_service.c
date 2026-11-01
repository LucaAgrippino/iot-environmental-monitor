/**
 * @file test_alarm_service.c
 * @brief Unity tests for AlarmService (Gateway).
 *
 * SensorService is replaced by a spy for sensor_service_subscribe(); the test never
 * includes sensor_service.h directly, so Ceedling does not link sensor_service.c.
 */

#include <string.h>

#include "alarm_service.h"
#include "freertos_mock.h"
#include "unity.h"

#define TEMP_HIGH (4000)
#define TEMP_LOW (0)
#define TEMP_HYST (100)

static sensor_service_err_t s_subscribe_result;
static uint32_t s_subscribe_calls;
static void (*s_registered)(const sensor_snapshot_t *);

static uint32_t s_event_count;
static sensor_id_t s_event_sensor[8];
static alarm_event_t s_event_type[8];
static int32_t s_event_value[8];

sensor_service_err_t sensor_service_subscribe(void (*cb)(const sensor_snapshot_t *snap))
{
    s_subscribe_calls++;
    s_registered = cb;
    return s_subscribe_result;
}

static void on_alarm(sensor_id_t sensor, alarm_event_t event, const sensor_reading_t *reading)
{
    if (s_event_count < 8U)
    {
        s_event_sensor[s_event_count] = sensor;
        s_event_type[s_event_count] = event;
        s_event_value[s_event_count] = reading->value;
    }
    s_event_count++;
}

void setUp(void)
{
    alarm_service_reset_for_test();
    s_subscribe_result = SENSOR_SERVICE_ERR_OK;
    s_subscribe_calls = 0U;
    s_registered = NULL;
    s_event_count = 0U;
}

void tearDown(void)
{
}

static void init_with_subscriber(void)
{
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_OK, alarm_service_init());
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_OK, alarm_service_subscribe(on_alarm));
}

static sensor_snapshot_t snapshot_with(sensor_id_t id, int32_t value, bool valid)
{
    sensor_snapshot_t snap;
    (void) memset(&snap, 0, sizeof(snap));
    snap.readings[id].value = value;
    snap.readings[id].valid = valid;
    return snap;
}

static alarm_state_t state_of(sensor_id_t id)
{
    alarm_state_t s = ALARM_STATE_CLEAR;
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_OK, alarm_service_get_state(id, &s));
    return s;
}

/* TC-AS-001 */
void test_init_registers_evaluate_with_sensor_service(void)
{
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_OK, alarm_service_init());
    TEST_ASSERT_EQUAL_UINT32(1U, s_subscribe_calls);
    TEST_ASSERT_TRUE(s_registered == alarm_service_evaluate);
}

/* TC-AS-100 */
void test_init_fails_when_sensor_service_refuses_the_subscription(void)
{
    s_subscribe_result = SENSOR_SERVICE_ERR_NO_SUB;
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_NOT_INIT, alarm_service_init());
}

/* TC-AS-015 */
void test_calls_before_init_return_not_init(void)
{
    alarm_state_t s;
    alarm_state_t all[SENSOR_ID_COUNT];

    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_NOT_INIT, alarm_service_get_state(SENSOR_ID_TEMPERATURE, &s));
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_NOT_INIT, alarm_service_get_all_states(all));
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_NOT_INIT, alarm_service_subscribe(on_alarm));
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_NOT_INIT, alarm_service_ack_all());
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_NOT_INIT,
                      alarm_service_get_limit(SENSOR_ID_TEMPERATURE, ALARM_EVENT_RAISED_HIGH, &(int32_t){0}));
}

/* TC-AS-014 */
void test_argument_checks(void)
{
    alarm_state_t s;
    (void) alarm_service_init();

    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_NULL_ARG, alarm_service_get_state(SENSOR_ID_TEMPERATURE, NULL));
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_NULL_ARG, alarm_service_get_all_states(NULL));
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_NULL_ARG, alarm_service_subscribe(NULL));
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_INVALID_ARG,
                      alarm_service_get_state(SENSOR_ID_COUNT, &s));
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_INVALID_ARG,
                      alarm_service_get_state((sensor_id_t) -1, &s));
}

/* TC-AS-016 */
void test_get_limit_returns_the_crossed_threshold(void)
{
    int32_t limit = -1;
    (void) alarm_service_init();

    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_OK,
                      alarm_service_get_limit(SENSOR_ID_TEMPERATURE, ALARM_EVENT_RAISED_HIGH, &limit));
    TEST_ASSERT_EQUAL_INT32(TEMP_HIGH, limit);
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_OK,
                      alarm_service_get_limit(SENSOR_ID_PRESSURE, ALARM_EVENT_RAISED_LOW, &limit));
    TEST_ASSERT_EQUAL_INT32(9500, limit);
}

/* TC-AS-017 */
void test_get_limit_rejects_bad_arguments(void)
{
    int32_t limit = 0;
    (void) alarm_service_init();

    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_NULL_ARG,
                      alarm_service_get_limit(SENSOR_ID_TEMPERATURE, ALARM_EVENT_RAISED_HIGH, NULL));
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_INVALID_ARG,
                      alarm_service_get_limit(SENSOR_ID_COUNT, ALARM_EVENT_RAISED_HIGH, &limit));
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_INVALID_ARG,
                      alarm_service_get_limit(SENSOR_ID_ACCEL_X, ALARM_EVENT_RAISED_HIGH, &limit));
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_INVALID_ARG,
                      alarm_service_get_limit(SENSOR_ID_TEMPERATURE, ALARM_EVENT_CLEARED, &limit));
}

/* TC-AS-013 */
void test_subscribe_rejects_a_full_table(void)
{
    (void) alarm_service_init();
    for (uint8_t i = 0U; i < ALARM_MAX_SUBSCRIBERS; i++)
    {
        TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_OK, alarm_service_subscribe(on_alarm));
    }
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_NO_SUB, alarm_service_subscribe(on_alarm));
}

/* TC-AS-101 */
void test_vtable_exposes_the_public_functions(void)
{
    TEST_ASSERT_NOT_NULL(alarm_service);
    TEST_ASSERT_TRUE(alarm_service->init == alarm_service_init);
    TEST_ASSERT_TRUE(alarm_service->get_state == alarm_service_get_state);
    TEST_ASSERT_TRUE(alarm_service->get_all_states == alarm_service_get_all_states);
    TEST_ASSERT_TRUE(alarm_service->subscribe == alarm_service_subscribe);
    TEST_ASSERT_TRUE(alarm_service->get_limit == alarm_service_get_limit);
    TEST_ASSERT_TRUE(alarm_service->ack_all == alarm_service_ack_all);
}

/* TC-AS-003 */
void test_reading_inside_the_band_leaves_the_alarm_clear(void)
{
    init_with_subscriber();
    sensor_snapshot_t snap = snapshot_with(SENSOR_ID_TEMPERATURE, 2300, true);

    alarm_service_evaluate(&snap);

    TEST_ASSERT_EQUAL(ALARM_STATE_CLEAR, state_of(SENSOR_ID_TEMPERATURE));
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
}

/* TC-AS-004/010 */
void test_reading_above_high_threshold_raises_a_high_alarm(void)
{
    init_with_subscriber();
    sensor_snapshot_t snap = snapshot_with(SENSOR_ID_TEMPERATURE, TEMP_HIGH + 1, true);

    alarm_service_evaluate(&snap);

    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_HIGH, state_of(SENSOR_ID_TEMPERATURE));
    TEST_ASSERT_EQUAL_UINT32(1U, s_event_count);
    TEST_ASSERT_EQUAL(SENSOR_ID_TEMPERATURE, s_event_sensor[0]);
    TEST_ASSERT_EQUAL(ALARM_EVENT_RAISED_HIGH, s_event_type[0]);
    TEST_ASSERT_EQUAL_INT32(TEMP_HIGH + 1, s_event_value[0]);
}

/* TC-AS-005 */
void test_reading_below_low_threshold_raises_a_low_alarm(void)
{
    init_with_subscriber();
    sensor_snapshot_t snap = snapshot_with(SENSOR_ID_TEMPERATURE, TEMP_LOW - 1, true);

    alarm_service_evaluate(&snap);

    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_LOW, state_of(SENSOR_ID_TEMPERATURE));
    TEST_ASSERT_EQUAL(ALARM_EVENT_RAISED_LOW, s_event_type[0]);
}

/* TC-AS-102 */
void test_a_reading_exactly_on_a_threshold_does_not_alarm(void)
{
    init_with_subscriber();
    sensor_snapshot_t hi = snapshot_with(SENSOR_ID_TEMPERATURE, TEMP_HIGH, true);
    sensor_snapshot_t lo = snapshot_with(SENSOR_ID_TEMPERATURE, TEMP_LOW, true);

    alarm_service_evaluate(&hi);
    alarm_service_evaluate(&lo);

    TEST_ASSERT_EQUAL(ALARM_STATE_CLEAR, state_of(SENSOR_ID_TEMPERATURE));
}

/* TC-AS-006/007 */
void test_high_alarm_holds_inside_hysteresis_and_clears_below_it(void)
{
    init_with_subscriber();
    sensor_snapshot_t raise = snapshot_with(SENSOR_ID_TEMPERATURE, TEMP_HIGH + 100, true);
    sensor_snapshot_t hold = snapshot_with(SENSOR_ID_TEMPERATURE, TEMP_HIGH - TEMP_HYST, true);
    sensor_snapshot_t clear = snapshot_with(SENSOR_ID_TEMPERATURE, TEMP_HIGH - TEMP_HYST - 1, true);

    alarm_service_evaluate(&raise);
    alarm_service_evaluate(&hold);
    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_HIGH, state_of(SENSOR_ID_TEMPERATURE));
    TEST_ASSERT_EQUAL_UINT32(1U, s_event_count);

    alarm_service_evaluate(&clear);
    TEST_ASSERT_EQUAL(ALARM_STATE_CLEAR, state_of(SENSOR_ID_TEMPERATURE));
    TEST_ASSERT_EQUAL(ALARM_EVENT_CLEARED, s_event_type[1]);
}

/* TC-AS-008 */
void test_low_alarm_holds_inside_hysteresis_and_clears_above_it(void)
{
    init_with_subscriber();
    sensor_snapshot_t raise = snapshot_with(SENSOR_ID_TEMPERATURE, TEMP_LOW - 100, true);
    sensor_snapshot_t hold = snapshot_with(SENSOR_ID_TEMPERATURE, TEMP_LOW + TEMP_HYST, true);
    sensor_snapshot_t clear = snapshot_with(SENSOR_ID_TEMPERATURE, TEMP_LOW + TEMP_HYST + 1, true);

    alarm_service_evaluate(&raise);
    alarm_service_evaluate(&hold);
    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_LOW, state_of(SENSOR_ID_TEMPERATURE));

    alarm_service_evaluate(&clear);
    TEST_ASSERT_EQUAL(ALARM_STATE_CLEAR, state_of(SENSOR_ID_TEMPERATURE));
}

/* TC-AS-009 */
void test_invalid_reading_leaves_the_alarm_unchanged(void)
{
    init_with_subscriber();
    sensor_snapshot_t raise = snapshot_with(SENSOR_ID_TEMPERATURE, TEMP_HIGH + 100, true);
    sensor_snapshot_t bad = snapshot_with(SENSOR_ID_TEMPERATURE, 0, false);

    alarm_service_evaluate(&raise);
    alarm_service_evaluate(&bad);

    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_HIGH, state_of(SENSOR_ID_TEMPERATURE));
    TEST_ASSERT_EQUAL_UINT32(1U, s_event_count);
}

/* TC-AS-103 — each of the three scalar sensors has its own thresholds */
void test_humidity_and_pressure_use_their_own_thresholds(void)
{
    init_with_subscriber();
    sensor_snapshot_t snap;
    (void) memset(&snap, 0, sizeof(snap));
    snap.readings[SENSOR_ID_HUMIDITY].valid = true;
    snap.readings[SENSOR_ID_HUMIDITY].value = 8100;  /* > 80.00 %RH */
    snap.readings[SENSOR_ID_PRESSURE].valid = true;
    snap.readings[SENSOR_ID_PRESSURE].value = 9400;  /* < 950.0 hPa */

    alarm_service_evaluate(&snap);

    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_HIGH, state_of(SENSOR_ID_HUMIDITY));
    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_LOW, state_of(SENSOR_ID_PRESSURE));
    TEST_ASSERT_EQUAL(ALARM_STATE_CLEAR, state_of(SENSOR_ID_TEMPERATURE));
}

/* TC-AS-104 — SS-O3: valid IMU / magnetometer readings never alarm */
void test_imu_and_magnetometer_channels_are_never_evaluated(void)
{
    init_with_subscriber();
    sensor_snapshot_t snap;
    (void) memset(&snap, 0, sizeof(snap));
    for (uint8_t id = (uint8_t) SENSOR_ID_ACCEL_X; id < (uint8_t) SENSOR_ID_COUNT; id++)
    {
        snap.readings[id].valid = true;
        snap.readings[id].value = 1000000;
    }

    alarm_service_evaluate(&snap);

    for (uint8_t id = (uint8_t) SENSOR_ID_ACCEL_X; id < (uint8_t) SENSOR_ID_COUNT; id++)
    {
        TEST_ASSERT_EQUAL(ALARM_STATE_CLEAR, state_of((sensor_id_t) id));
    }
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
}

/* TC-AS-105 */
void test_get_all_states_reports_every_sensor(void)
{
    init_with_subscriber();
    sensor_snapshot_t snap = snapshot_with(SENSOR_ID_TEMPERATURE, TEMP_HIGH + 100, true);
    alarm_state_t all[SENSOR_ID_COUNT];

    alarm_service_evaluate(&snap);

    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_OK, alarm_service_get_all_states(all));
    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_HIGH, all[SENSOR_ID_TEMPERATURE]);
    TEST_ASSERT_EQUAL(ALARM_STATE_CLEAR, all[SENSOR_ID_HUMIDITY]);
}

/* TC-AS-011/012 */
void test_ack_all_clears_alarms_and_a_standing_breach_raises_again(void)
{
    init_with_subscriber();
    sensor_snapshot_t snap = snapshot_with(SENSOR_ID_TEMPERATURE, TEMP_HIGH + 100, true);

    alarm_service_evaluate(&snap);
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_OK, alarm_service_ack_all());
    TEST_ASSERT_EQUAL(ALARM_STATE_CLEAR, state_of(SENSOR_ID_TEMPERATURE));

    alarm_service_evaluate(&snap);
    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_HIGH, state_of(SENSOR_ID_TEMPERATURE));
    TEST_ASSERT_EQUAL_UINT32(2U, s_event_count);
}

/* ===================================================================== */
/* alarm_service_reconfigure (SS-O6, WP12 D-3..D-6)                       */
/* ===================================================================== */

static config_params_t config_with_temp(int16_t high, int16_t low, int16_t hyst)
{
    config_params_t c;

    (void) memset(&c, 0, sizeof(c));
    c.temp_alarm_high = high;
    c.temp_alarm_low = low;
    c.temp_hysteresis = hyst;
    c.humidity_alarm_high = 8000U;
    c.humidity_alarm_low = 2000U;
    c.humidity_hysteresis = 200U;
    c.pressure_alarm_high = 10500U;
    c.pressure_alarm_low = 9500U;
    c.pressure_hysteresis = 50U;
    return c;
}

void test_reconfigure_before_init_and_with_null(void)
{
    const config_params_t c = config_with_temp(3000, 0, 100);

    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_NOT_INIT, alarm_service_reconfigure(&c));
    init_with_subscriber();
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_NULL_ARG, alarm_service_reconfigure(NULL));
}

void test_reconfigure_moves_the_limits(void)
{
    const config_params_t c = config_with_temp(3000, 500, 100);
    int32_t limit = 0;

    init_with_subscriber();
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_OK, alarm_service_reconfigure(&c));

    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_OK,
                      alarm_service_get_limit(SENSOR_ID_TEMPERATURE, ALARM_EVENT_RAISED_HIGH, &limit));
    TEST_ASSERT_EQUAL_INT32(3000, limit);
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_OK,
                      alarm_service_get_limit(SENSOR_ID_TEMPERATURE, ALARM_EVENT_RAISED_LOW, &limit));
    TEST_ASSERT_EQUAL_INT32(500, limit);

    sensor_snapshot_t snap = snapshot_with(SENSOR_ID_TEMPERATURE, 3100, true);
    alarm_service_evaluate(&snap); /* above the NEW high (30.00), below the default (40.00) */
    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_HIGH, state_of(SENSOR_ID_TEMPERATURE));
}

void test_reconfigure_keeps_an_active_alarm_and_applies_from_the_next_sample(void)
{
    init_with_subscriber();
    sensor_snapshot_t snap = snapshot_with(SENSOR_ID_TEMPERATURE, 4100, true);
    alarm_service_evaluate(&snap);
    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_HIGH, state_of(SENSOR_ID_TEMPERATURE));
    TEST_ASSERT_EQUAL_UINT32(1U, s_event_count);

    /* Raise the limit above the reading: the state is kept (D-5) ... */
    const config_params_t c = config_with_temp(6000, 0, 100);
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_OK, alarm_service_reconfigure(&c));
    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_HIGH, state_of(SENSOR_ID_TEMPERATURE));
    TEST_ASSERT_EQUAL_UINT32(1U, s_event_count);

    /* ... and the next sample clears it by the usual rule. */
    alarm_service_evaluate(&snap);
    TEST_ASSERT_EQUAL(ALARM_STATE_CLEAR, state_of(SENSOR_ID_TEMPERATURE));
    TEST_ASSERT_EQUAL_UINT32(2U, s_event_count);
    TEST_ASSERT_EQUAL(ALARM_EVENT_CLEARED, s_event_type[1]);
}

void test_defaults_match_the_config_params_defaults(void)
{
    int32_t limit = 0;

    init_with_subscriber(); /* D-4: 40.00 / 0.00 / 1.00 degC */
    TEST_ASSERT_EQUAL(ALARM_SERVICE_ERR_OK,
                      alarm_service_get_limit(SENSOR_ID_TEMPERATURE, ALARM_EVENT_RAISED_HIGH, &limit));
    TEST_ASSERT_EQUAL_INT32(4000, limit);
}
