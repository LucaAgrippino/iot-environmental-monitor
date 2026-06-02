/**
 * @file test_alarm_service.c
 * @brief Unit tests for AlarmService — TC-AS-001..007.
 *
 * Covers docs/lld/application/sensor-alarm-service.md §7.
 *
 * Mocking strategy:
 *   - SensorService → sensor_service_stub.h (prevents auto-link of
 *                     sensor_service.c and its driver chain)
 *   - sensor_service_subscribe → stub defined in this TU
 *
 * alarm_service_evaluate() is called directly via the TEST-exposed declaration
 * in alarm_service.h, injecting sensor_snapshot_t values without going through
 * the SensorService subscriber mechanism.
 *
 * Build defines: STM32F469xx, TEST (project.yml :test_alarm_service:).
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "alarm_service/alarm_service.h"

/* ======================================================================= */
/* sensor_service_subscribe stub (alarm_service_init calls this)           */
/* ======================================================================= */

static uint32_t g_ss_subscribe_calls;

sensor_service_err_t sensor_service_subscribe(void (*cb)(const sensor_snapshot_t *snap))
{
    (void) cb;
    g_ss_subscribe_calls++;
    return SENSOR_SERVICE_ERR_OK;
}

/* ======================================================================= */
/* Alarm subscriber spy                                                     */
/* ======================================================================= */

#define SPY_MAX_EVENTS (16U)

typedef struct
{
    sensor_id_t sensor;
    alarm_event_t event;
    sensor_reading_t reading;
} alarm_spy_entry_t;

static alarm_spy_entry_t g_spy_events[SPY_MAX_EVENTS];
static uint8_t g_spy_count;

static void alarm_spy_cb(sensor_id_t sensor, alarm_event_t event, const sensor_reading_t *reading)
{
    if (g_spy_count < SPY_MAX_EVENTS)
    {
        g_spy_events[g_spy_count].sensor = sensor;
        g_spy_events[g_spy_count].event = event;
        g_spy_events[g_spy_count].reading = *reading;
        g_spy_count++;
    }
}

static void spy_reset(void)
{
    (void) memset(g_spy_events, 0, sizeof(g_spy_events));
    g_spy_count = 0U;
}

/* ======================================================================= */
/* Helpers                                                                  */
/* ======================================================================= */

/* Default thresholds matching alarm_service.c defaults (fixed-point x100):
 * Temperature: high=3500 (35.00°C), low=0 (0.00°C), hysteresis=200 (2.00°C) */
#define TEST_TEMP_HIGH ((int32_t) 3500)
#define TEST_TEMP_LOW ((int32_t) 0)
#define TEST_TEMP_HYST ((int32_t) 200)

static sensor_snapshot_t make_snap_with_temp(int32_t temp, bool valid)
{
    sensor_snapshot_t snap;
    (void) memset(&snap, 0, sizeof(snap));
    snap.readings[SENSOR_ID_TEMPERATURE].value = temp;
    snap.readings[SENSOR_ID_TEMPERATURE].valid = valid;
    return snap;
}

/* ======================================================================= */
/* setUp / tearDown                                                         */
/* ======================================================================= */

void setUp(void)
{
    alarm_service_reset_for_test();
    spy_reset();
    g_ss_subscribe_calls = 0U;
    alarm_service_init();
    alarm_service_subscribe(alarm_spy_cb);
}

void tearDown(void)
{
}

/* ======================================================================= */
/* TC-AS-001: Reading within range — state stays CLEAR, no event            */
/* ======================================================================= */

void test_TC_AS_001_reading_within_range_state_stays_clear(void)
{
    sensor_snapshot_t snap = make_snap_with_temp(2000, true); /* 20.00°C */
    alarm_service_evaluate(&snap);

    alarm_state_t state;
    alarm_service_get_state(SENSOR_ID_TEMPERATURE, &state);

    TEST_ASSERT_EQUAL(ALARM_STATE_CLEAR, state);
    TEST_ASSERT_EQUAL_UINT8(0U, g_spy_count);
}

/* ======================================================================= */
/* TC-AS-002: Reading above threshold_high — ACTIVE_HIGH + event raised    */
/* ======================================================================= */

void test_TC_AS_002_reading_above_threshold_high_raises_alarm(void)
{
    sensor_snapshot_t snap = make_snap_with_temp(TEST_TEMP_HIGH + 100, true); /* 36.00°C */
    alarm_service_evaluate(&snap);

    alarm_state_t state;
    alarm_service_get_state(SENSOR_ID_TEMPERATURE, &state);

    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_HIGH, state);
    TEST_ASSERT_EQUAL_UINT8(1U, g_spy_count);
    TEST_ASSERT_EQUAL(SENSOR_ID_TEMPERATURE, g_spy_events[0].sensor);
    TEST_ASSERT_EQUAL(ALARM_EVENT_RAISED_HIGH, g_spy_events[0].event);
    TEST_ASSERT_EQUAL_INT32(TEST_TEMP_HIGH + 100, g_spy_events[0].reading.value);
}

/* ======================================================================= */
/* TC-AS-003: Hysteresis — stays ACTIVE_HIGH above clear threshold         */
/* ======================================================================= */

void test_TC_AS_003_hysteresis_stays_active_high_above_clear_threshold(void)
{
    /* First, raise the alarm */
    sensor_snapshot_t snap = make_snap_with_temp(TEST_TEMP_HIGH + 100, true); /* 36.00°C */
    alarm_service_evaluate(&snap);

    spy_reset();

    /* Value within hysteresis band: (high - hyst + 10) = 3500 - 200 + 10 = 3310 (33.10°C)
     * Must remain ACTIVE_HIGH (not yet cleared). */
    int32_t within_hyst = TEST_TEMP_HIGH - TEST_TEMP_HYST + 10;
    snap = make_snap_with_temp(within_hyst, true);
    alarm_service_evaluate(&snap);

    alarm_state_t state;
    alarm_service_get_state(SENSOR_ID_TEMPERATURE, &state);

    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_HIGH, state);
    TEST_ASSERT_EQUAL_UINT8(0U, g_spy_count); /* no new event */
}

/* ======================================================================= */
/* TC-AS-004: Reading below clear threshold — alarm clears, CLEARED event  */
/* ======================================================================= */

void test_TC_AS_004_reading_below_clear_threshold_clears_alarm(void)
{
    /* Raise alarm */
    sensor_snapshot_t snap = make_snap_with_temp(TEST_TEMP_HIGH + 100, true); /* 36.00°C */
    alarm_service_evaluate(&snap);

    spy_reset();

    /* Value below clear threshold: 3500 - 200 - 10 = 3290 (32.90°C) */
    snap = make_snap_with_temp(TEST_TEMP_HIGH - TEST_TEMP_HYST - 10, true);
    alarm_service_evaluate(&snap);

    alarm_state_t state;
    alarm_service_get_state(SENSOR_ID_TEMPERATURE, &state);

    TEST_ASSERT_EQUAL(ALARM_STATE_CLEAR, state);
    TEST_ASSERT_EQUAL_UINT8(1U, g_spy_count);
    TEST_ASSERT_EQUAL(ALARM_EVENT_CLEARED, g_spy_events[0].event);
}

/* ======================================================================= */
/* TC-AS-005: Low alarm — symmetric to high alarm                          */
/* ======================================================================= */

void test_TC_AS_005_low_alarm_symmetric_to_high_alarm(void)
{
    /* Raise LOW alarm (temperature = -100 x100 = -1.00°C < threshold_low = 0) */
    sensor_snapshot_t snap = make_snap_with_temp(TEST_TEMP_LOW - 100, true);
    alarm_service_evaluate(&snap);

    alarm_state_t state;
    alarm_service_get_state(SENSOR_ID_TEMPERATURE, &state);
    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_LOW, state);
    TEST_ASSERT_EQUAL_UINT8(1U, g_spy_count);
    TEST_ASSERT_EQUAL(ALARM_EVENT_RAISED_LOW, g_spy_events[0].event);

    spy_reset();

    /* Stay in hysteresis band: (low + hyst - 10) = 0 + 200 - 10 = 190 (1.90°C)
     * Must remain ACTIVE_LOW. */
    snap = make_snap_with_temp(TEST_TEMP_LOW + TEST_TEMP_HYST - 10, true);
    alarm_service_evaluate(&snap);
    alarm_service_get_state(SENSOR_ID_TEMPERATURE, &state);
    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_LOW, state);
    TEST_ASSERT_EQUAL_UINT8(0U, g_spy_count);

    /* Clear: exceed (low + hyst): 0 + 200 + 10 = 210 (2.10°C) */
    snap = make_snap_with_temp(TEST_TEMP_LOW + TEST_TEMP_HYST + 10, true);
    alarm_service_evaluate(&snap);
    alarm_service_get_state(SENSOR_ID_TEMPERATURE, &state);
    TEST_ASSERT_EQUAL(ALARM_STATE_CLEAR, state);
    TEST_ASSERT_EQUAL_UINT8(1U, g_spy_count);
    TEST_ASSERT_EQUAL(ALARM_EVENT_CLEARED, g_spy_events[0].event);
}

/* ======================================================================= */
/* TC-AS-006: Invalid reading — alarm state unchanged                      */
/* ======================================================================= */

void test_TC_AS_006_invalid_reading_state_unchanged(void)
{
    /* Start with alarm raised */
    sensor_snapshot_t snap = make_snap_with_temp(TEST_TEMP_HIGH + 100, true); /* 36.00°C */
    alarm_service_evaluate(&snap);

    alarm_state_t state_before;
    alarm_service_get_state(SENSOR_ID_TEMPERATURE, &state_before);
    spy_reset();

    /* Now inject an invalid reading (valid = false) well below threshold */
    snap = make_snap_with_temp(-10000, false); /* -100.00°C, but invalid */
    alarm_service_evaluate(&snap);

    alarm_state_t state_after;
    alarm_service_get_state(SENSOR_ID_TEMPERATURE, &state_after);

    TEST_ASSERT_EQUAL(state_before, state_after);
    TEST_ASSERT_EQUAL_UINT8(0U, g_spy_count); /* no event fired */
}

/* ======================================================================= */
/* TC-AS-007: Subscriber receives correct sensor_id, event, reading        */
/* ======================================================================= */

void test_TC_AS_007_subscriber_receives_correct_fields(void)
{
    /* 3850 = 38.50°C x100 */
    const int32_t trigger_temp = TEST_TEMP_HIGH + 350;
    sensor_snapshot_t snap = make_snap_with_temp(trigger_temp, true);
    snap.readings[SENSOR_ID_TEMPERATURE].timestamp.epoch = 12345UL;

    alarm_service_evaluate(&snap);

    TEST_ASSERT_EQUAL_UINT8(1U, g_spy_count);
    TEST_ASSERT_EQUAL(SENSOR_ID_TEMPERATURE, g_spy_events[0].sensor);
    TEST_ASSERT_EQUAL(ALARM_EVENT_RAISED_HIGH, g_spy_events[0].event);
    TEST_ASSERT_EQUAL_INT32(trigger_temp, g_spy_events[0].reading.value);
    TEST_ASSERT_EQUAL_UINT32(12345UL, g_spy_events[0].reading.timestamp.epoch);
}
