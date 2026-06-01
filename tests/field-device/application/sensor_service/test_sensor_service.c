/**
 * @file test_sensor_service.c
 * @brief Unit tests for SensorService — TC-SS-001..007.
 *
 * Covers docs/lld/application/sensor-alarm-service.md §7.
 *
 * Mocking strategy:
 *   - BarometerDriver   → stub_baro_read/init via UNIT_TEST macros in sensor_service.h
 *   - HumidityTempDriver→ stub_ht_read/init via UNIT_TEST macros in sensor_service.h
 *   - TimeProvider      → stub_time_get via UNIT_TEST macro in sensor_service.h
 *   - IHealthReport     → s_stub_health vtable defined in this TU
 *   - FreeRTOS          → freertos_mock.h (auto-links freertos_mock.c)
 *   - RtcDriver         → rtc_driver = NULL (time_provider.c linked but dormant)
 *
 * Build defines: STM32F469xx, TEST, UNIT_TEST (project.yml :test_sensor_service:).
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* health_monitor_stub.h must come before sensor_service.h so that
 * struct ihealth_report_s is complete when time_provider.h typedef resolves. */
#include "health_monitor_stub.h"
#include "rtc_driver_stub.h"

#include "FreeRTOS.h"
#include "task.h"
#include "freertos_mock.h"

#include "sensor_service/sensor_service.h"

/* ======================================================================= */
/* rtc_driver singleton — time_provider.c requires this symbol at link time */
/* ======================================================================= */

const irtc_t *rtc_driver = NULL;

/* ======================================================================= */
/* Health report mock                                                       */
/* ======================================================================= */

static uint32_t       g_health_push_calls;
static health_event_t g_health_last_event;

static health_monitor_err_t stub_push_event(health_event_t event, uint32_t param)
{
    (void)param;
    g_health_push_calls++;
    g_health_last_event = event;
    return HEALTH_MONITOR_ERR_OK;
}

static const ihealth_report_t s_stub_health = {
    .init        = NULL,
    .push_event  = stub_push_event,
};

const ihealth_report_t *const health_report = &s_stub_health;

static void health_mock_reset(void)
{
    g_health_push_calls = 0U;
    g_health_last_event = (health_event_t)0xFFU;
}

/* ======================================================================= */
/* Barometer driver stub                                                    */
/* ======================================================================= */

static baro_err_t g_baro_read_ret;
static int32_t    g_baro_pressure_x10;

baro_err_t stub_baro_init(void) { return BARO_ERR_OK; }

baro_err_t stub_baro_read(baro_reading_t *r)
{
    if (r != NULL) { r->pressure_x10 = g_baro_pressure_x10; }
    return g_baro_read_ret;
}

static void baro_stub_reset(void)
{
    g_baro_read_ret      = BARO_ERR_OK;
    g_baro_pressure_x10  = 10132; /* 1013.2 hPa — standard atmosphere */
}

/* ======================================================================= */
/* HumidityTemp driver stub                                                 */
/* ======================================================================= */

static ht_err_t  g_ht_read_ret;
static int32_t   g_ht_temp_x100;
static uint32_t  g_ht_hum_x100;

ht_err_t stub_ht_init(void) { return HT_ERR_OK; }

ht_err_t stub_ht_read(ht_reading_t *r)
{
    if (r != NULL)
    {
        r->temperature_x100 = g_ht_temp_x100;
        r->humidity_x100    = g_ht_hum_x100;
    }
    return g_ht_read_ret;
}

static void ht_stub_reset(void)
{
    g_ht_read_ret  = HT_ERR_OK;
    g_ht_temp_x100 = 2200;  /* 22.00 °C */
    g_ht_hum_x100  = 5000U; /* 50.00 %RH */
}

/* ======================================================================= */
/* TimeProvider stub                                                        */
/* ======================================================================= */

static time_provider_ts_t g_ts;

time_provider_err_t stub_time_get(time_provider_ts_t *ts)
{
    if (ts != NULL) { *ts = g_ts; }
    return TIME_PROVIDER_ERR_OK;
}

static void ts_stub_reset(void)
{
    (void)memset(&g_ts, 0, sizeof(g_ts));
    g_ts.epoch      = 1000UL;
    g_ts.sync_state = TIME_SYNC_SYNCHRONISED;
}

/* ======================================================================= */
/* Subscriber callback spy                                                  */
/* ======================================================================= */

static bool              g_cb_called;
static sensor_snapshot_t g_cb_snapshot;

static void snapshot_cb(const sensor_snapshot_t *snap)
{
    g_cb_called   = true;
    g_cb_snapshot = *snap;
}

/* ======================================================================= */
/* setUp / tearDown                                                         */
/* ======================================================================= */

void setUp(void)
{
    mock_freertos_reset();
    health_mock_reset();
    baro_stub_reset();
    ht_stub_reset();
    ts_stub_reset();
    g_cb_called = false;
    (void)memset(&g_cb_snapshot, 0, sizeof(g_cb_snapshot));
    sensor_service_reset_for_test();
}

void tearDown(void) {}

/* ======================================================================= */
/* Helper: initialise then set alpha to default directly                    */
/* ======================================================================= */

static void init_with_default_alpha(void)
{
    sensor_service_err_t err = sensor_service_init();
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, err);
    /* alpha initialised to SENSOR_ALPHA_DEFAULT (0.1) inside init */
}

/* ======================================================================= */
/* TC-SS-001: All drivers succeed — snapshot valid, cycle_count increments */
/* ======================================================================= */

void test_TC_SS_001_all_drivers_succeed_snapshot_valid(void)
{
    init_with_default_alpha();

    sensor_service_err_t err = sensor_service_run_cycle();
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, err);

    sensor_snapshot_t snap;
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, sensor_service_get_snapshot(&snap));

    TEST_ASSERT_TRUE(snap.readings[SENSOR_ID_TEMPERATURE].valid);
    TEST_ASSERT_TRUE(snap.readings[SENSOR_ID_HUMIDITY].valid);
    TEST_ASSERT_TRUE(snap.readings[SENSOR_ID_PRESSURE].valid);
    TEST_ASSERT_EQUAL_UINT32(1U, snap.cycle_count);

    /* Timestamps were stamped */
    TEST_ASSERT_EQUAL_UINT32(g_ts.epoch, snap.readings[SENSOR_ID_TEMPERATURE].timestamp.epoch);
    TEST_ASSERT_EQUAL_UINT32(g_ts.epoch, snap.readings[SENSOR_ID_PRESSURE].timestamp.epoch);
}

/* ======================================================================= */
/* TC-SS-002: HT driver error — temp+hum invalid, cycle continues, no abort */
/* ======================================================================= */

void test_TC_SS_002_ht_driver_error_readings_invalid_cycle_continues(void)
{
    init_with_default_alpha();
    g_ht_read_ret = HT_ERR_FAULT;

    sensor_service_err_t err = sensor_service_run_cycle();
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, err);

    sensor_snapshot_t snap;
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, sensor_service_get_snapshot(&snap));

    /* Temperature and humidity (same driver) must be invalid */
    TEST_ASSERT_FALSE(snap.readings[SENSOR_ID_TEMPERATURE].valid);
    TEST_ASSERT_FALSE(snap.readings[SENSOR_ID_HUMIDITY].valid);

    /* Pressure (different driver, no fault) must still be valid */
    TEST_ASSERT_TRUE(snap.readings[SENSOR_ID_PRESSURE].valid);

    /* Health event was pushed for the first failure */
    TEST_ASSERT_EQUAL_UINT32(1U, g_health_push_calls);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_SENSOR_FAIL, g_health_last_event);
}

/* ======================================================================= */
/* TC-SS-003: Range validation — out-of-range value marks invalid, clamped  */
/*            value updates filter state                                     */
/* ======================================================================= */

void test_TC_SS_003_range_violation_valid_false_clamped_feeds_filter(void)
{
    init_with_default_alpha();

    /* Set temperature above range_max (85°C → 8500 in x100 units).
     * Use 9000 (90.00°C) — above the 85°C range limit. */
    g_ht_temp_x100 = 9000; /* 90.00°C — out of range */
    g_ht_hum_x100  = 5000U;

    sensor_service_err_t err = sensor_service_run_cycle();
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, err);

    sensor_snapshot_t snap;
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, sensor_service_get_snapshot(&snap));

    /* Reading marked invalid due to range check */
    TEST_ASSERT_FALSE(snap.readings[SENSOR_ID_TEMPERATURE].valid);

    /* Filter should have been updated with the CLAMPED value (85.0°C):
     * filtered = 0.1 * 85.0 + 0.9 * 0.0 = 8.5
     * Verify by running a second cycle with an in-range value; the filter
     * output must reflect the 8.5 contribution from the previous cycle. */
    g_ht_temp_x100 = 2200; /* 22.00°C — in range */
    err = sensor_service_run_cycle();
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, err);

    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, sensor_service_get_snapshot(&snap));
    TEST_ASSERT_TRUE(snap.readings[SENSOR_ID_TEMPERATURE].valid);

    /* cycle 1 filter: 0.1*85 + 0.9*0 = 8.5
     * cycle 2 filter: 0.1*22 + 0.9*8.5 = 2.2 + 7.65 = 9.85 */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 9.85f, snap.readings[SENSOR_ID_TEMPERATURE].value);
}

/* ======================================================================= */
/* TC-SS-004: IIR filter — single step with known alpha/value/prev          */
/* ======================================================================= */

void test_TC_SS_004_iir_filter_single_step_known_values(void)
{
    init_with_default_alpha();

    /* Force alpha = 0.2 and prev_filtered[TEMP] = 10.0 for a known result. */
    sensor_service_set_alpha_for_test(0.2f);
    sensor_service_set_prev_filtered_for_test((int)SENSOR_ID_TEMPERATURE, 10.0f);

    /* Set stub to return 30.00°C (in-range). */
    g_ht_temp_x100 = 3000;

    sensor_service_run_cycle();

    float prev_out[SENSOR_ID_COUNT];
    sensor_service_get_prev_filtered_for_test(prev_out);

    /* filtered = 0.2 * 30.0 + 0.8 * 10.0 = 6.0 + 8.0 = 14.0 */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 14.0f, prev_out[SENSOR_ID_TEMPERATURE]);

    /* Snapshot value should equal the filtered result */
    sensor_snapshot_t snap;
    sensor_service_get_snapshot(&snap);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 14.0f, snap.readings[SENSOR_ID_TEMPERATURE].value);
}

/* ======================================================================= */
/* TC-SS-005: get_snapshot returns a copy of the latest snapshot           */
/* ======================================================================= */

void test_TC_SS_005_get_snapshot_returns_copy(void)
{
    init_with_default_alpha();
    g_ht_temp_x100 = 2500; /* 25.00°C */

    sensor_service_run_cycle();

    sensor_snapshot_t snap1, snap2;
    sensor_service_get_snapshot(&snap1);

    /* Modify snap1 — should NOT affect internal state */
    snap1.cycle_count = 99U;
    snap1.readings[SENSOR_ID_TEMPERATURE].value = -999.0f;

    sensor_service_get_snapshot(&snap2);

    /* snap2 should still reflect the internal state (cycle_count = 1) */
    TEST_ASSERT_EQUAL_UINT32(1U, snap2.cycle_count);
    TEST_ASSERT_FLOAT_WITHIN(0.01f,
                             0.1f * (2500.0f / 100.0f),
                             snap2.readings[SENSOR_ID_TEMPERATURE].value);
}

/* ======================================================================= */
/* TC-SS-006: subscribe beyond SENSOR_MAX_SUBSCRIBERS returns ERR_NO_SUB  */
/* ======================================================================= */

void test_TC_SS_006_subscribe_table_full_returns_err_no_sub(void)
{
    init_with_default_alpha();

    /* Fill the subscriber table */
    for (uint8_t i = 0U; i < SENSOR_MAX_SUBSCRIBERS; i++)
    {
        sensor_service_err_t err = sensor_service_subscribe(snapshot_cb);
        TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_OK, err);
    }

    /* One more must fail */
    sensor_service_err_t err = sensor_service_subscribe(snapshot_cb);
    TEST_ASSERT_EQUAL(SENSOR_SERVICE_ERR_NO_SUB, err);
}

/* ======================================================================= */
/* TC-SS-007: Callback fired with correct snapshot after run_cycle()        */
/* ======================================================================= */

void test_TC_SS_007_callback_fired_with_correct_snapshot(void)
{
    init_with_default_alpha();
    sensor_service_subscribe(snapshot_cb);

    g_ht_temp_x100 = 2200; /* 22.00°C */
    g_ht_hum_x100  = 5500U; /* 55.00 %RH */

    sensor_service_run_cycle();

    TEST_ASSERT_TRUE(g_cb_called);
    TEST_ASSERT_EQUAL_UINT32(1U, g_cb_snapshot.cycle_count);
    TEST_ASSERT_TRUE(g_cb_snapshot.readings[SENSOR_ID_TEMPERATURE].valid);
    TEST_ASSERT_TRUE(g_cb_snapshot.readings[SENSOR_ID_HUMIDITY].valid);
    TEST_ASSERT_TRUE(g_cb_snapshot.readings[SENSOR_ID_PRESSURE].valid);
}
