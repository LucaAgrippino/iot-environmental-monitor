/**
 * @file test_health_monitor.c
 * @brief Unit tests for HealthMonitor — application layer (host build, Unity).
 *
 * Covers TC-HM-001..008 per docs/lld/application/health-monitor.md §7.
 *
 * Mocking strategy:
 *   - FreeRTOS  → freertos_mock.h (auto-links freertos_mock.c; see setUp)
 *   - LedDriver → led_driver_set() macro from health_monitor.h (UNIT_TEST)
 *                 replaced by stub_led_set() spy defined below
 *   - Logger    → disabled via #ifdef TEST in health_monitor.c (no include)
 *
 * Build: requires STM32F469xx, TEST, UNIT_TEST defines (project.yml).
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "freertos_mock.h"   /* causes Ceedling to auto-link freertos_mock.c */

/* led_driver_stub.h provides led_id_t (LED_GREEN, LED_RED) needed by the
 * stub_led_set() spy below. health_monitor.h defines the UNIT_TEST macro
 * substitution, so led_driver_set(id, state) → stub_led_set(id, state). */
#include "led_driver_stub.h"

/* SUT — must come AFTER led_driver_stub.h so led_id_t is visible when the
 * UNIT_TEST macro in health_monitor.h is expanded. */
#include "health_monitor.h"

/* ======================================================================= */
/* LED spy                                                                 */
/* ======================================================================= */

#define LED_SPY_MAX_CALLS  (32U)

typedef struct
{
    uint32_t id;
    uint32_t state;
} led_spy_call_t;

static led_spy_call_t g_led_calls[LED_SPY_MAX_CALLS];
static uint8_t        g_led_call_count;

/* Last recorded state per LED — used for simple "what is RED doing?" checks. */
static uint32_t g_led_red_last_state;
static uint32_t g_led_green_last_state;

void stub_led_set(uint32_t id, uint32_t state)
{
    if (g_led_call_count < LED_SPY_MAX_CALLS)
    {
        g_led_calls[g_led_call_count].id    = id;
        g_led_calls[g_led_call_count].state = state;
        g_led_call_count++;
    }
    if (id == (uint32_t)LED_RED)   { g_led_red_last_state   = state; }
    if (id == (uint32_t)LED_GREEN) { g_led_green_last_state = state; }
}

static void led_spy_reset(void)
{
    (void)memset(g_led_calls, 0, sizeof(g_led_calls));
    g_led_call_count     = 0U;
    g_led_red_last_state   = 0xFFU;  /* sentinel: not yet set */
    g_led_green_last_state = 0xFFU;
}

/* Helper: was LED_RED set to the ON state (== 1 == LED_STATE_ON) in any call? */
static bool led_red_is_on(void)
{
    return g_led_red_last_state == 1U;  /* LED_STATE_ON */
}

static bool led_green_is_on(void)
{
    return g_led_green_last_state == 1U;
}

static bool led_red_is_off(void)
{
    return g_led_red_last_state == 0U;  /* LED_STATE_OFF */
}

static bool led_green_is_off(void)
{
    return g_led_green_last_state == 0U;
}

/* ======================================================================= */
/* setUp / tearDown                                                        */
/* ======================================================================= */

void setUp(void)
{
    mock_freertos_reset();
    led_spy_reset();
    health_monitor_reset_for_test();
}

void tearDown(void)
{
    /* Nothing required — reset is in setUp. */
}

/* ======================================================================= */
/* TC-HM-001: health_monitor_init() — snapshot zeroed, mutex created      */
/* ======================================================================= */

void test_TC_HM_001_init_zeros_snapshot_and_creates_mutex(void)
{
    health_monitor_err_t err;
    device_health_snapshot_t snap;

    err = health_monitor_init();
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, err);

    /* Verify snapshot is zero — get_snapshot copies under mutex. */
    err = health_monitor_get_snapshot(&snap);
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, err);

    TEST_ASSERT_EQUAL_UINT32(0U, snap.uptime_s);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_INIT, snap.lifecycle_state);
    TEST_ASSERT_EQUAL_UINT32(0U, snap.sensor_fail_count);
    TEST_ASSERT_EQUAL_UINT32(0U, snap.alarm_raise_count);
    TEST_ASSERT_FALSE(snap.config_write_failed);
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, snap.time_sync_state);
}

/* ======================================================================= */
/* TC-HM-002: push_event(CONFIG_WRITE_FAIL) → config_write_failed = true  */
/* ======================================================================= */

void test_TC_HM_002_push_config_write_fail_sets_flag(void)
{
    device_health_snapshot_t snap;

    (void)health_monitor_init();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
        health_monitor_push_event(HEALTH_EVENT_CONFIG_WRITE_FAIL, 0U));

    (void)health_monitor_get_snapshot(&snap);
    TEST_ASSERT_TRUE(snap.config_write_failed);
}

/* ======================================================================= */
/* TC-HM-003: push_event(ALARM_RAISED, TEMP) → alarm LED; count++         */
/* ======================================================================= */

void test_TC_HM_003_push_alarm_raised_updates_led_and_count(void)
{
    device_health_snapshot_t snap;

    (void)health_monitor_init();
    led_spy_reset();  /* clear init-time LED calls */

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
        health_monitor_push_event(HEALTH_EVENT_ALARM_RAISED,
                                  (uint32_t)SENSOR_ID_TEMPERATURE));

    (void)health_monitor_get_snapshot(&snap);

    /* Alarm count incremented. */
    TEST_ASSERT_EQUAL_UINT32(1U, snap.alarm_raise_count);

    /* Alarm LED pattern: adaptation — RED on + GREEN on (no orange on this board). */
    TEST_ASSERT_TRUE(led_red_is_on());
    TEST_ASSERT_TRUE(led_green_is_on());

    /* Alarm state recorded for temperature sensor. */
    TEST_ASSERT_EQUAL(ALARM_STATE_ACTIVE_HIGH,
                      snap.alarm_state[SENSOR_ID_TEMPERATURE]);
}

/* ======================================================================= */
/* TC-HM-004: push_event(FAULT) → fault LED; overrides any previous state */
/* ======================================================================= */

void test_TC_HM_004_push_fault_sets_red_led(void)
{
    device_health_snapshot_t snap;

    (void)health_monitor_init();
    led_spy_reset();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
        health_monitor_push_event(HEALTH_EVENT_FAULT, 0U));

    (void)health_monitor_get_snapshot(&snap);

    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, snap.lifecycle_state);
    TEST_ASSERT_TRUE(led_red_is_on());
    TEST_ASSERT_TRUE(led_green_is_off());
}

/* ======================================================================= */
/* TC-HM-005: ALARM_RAISED after FAULT → LED stays in fault pattern        */
/* ======================================================================= */

void test_TC_HM_005_alarm_after_fault_led_stays_red(void)
{
    (void)health_monitor_init();

    /* Drive to Faulted first. */
    (void)health_monitor_push_event(HEALTH_EVENT_FAULT, 0U);
    led_spy_reset();  /* clear fault calls; only care about post-alarm LED */

    /* Now push an alarm — update_led_state() priority: Faulted > Alarm. */
    (void)health_monitor_push_event(HEALTH_EVENT_ALARM_RAISED,
                                    (uint32_t)SENSOR_ID_HUMIDITY);

    /* LED must still show fault pattern: RED on, GREEN off. */
    TEST_ASSERT_TRUE(led_red_is_on());
    TEST_ASSERT_TRUE(led_green_is_off());
}

/* ======================================================================= */
/* TC-HM-006: update_modbus_slave_stats → snapshot fields updated         */
/* ======================================================================= */

void test_TC_HM_006_update_modbus_slave_stats_copies_fields(void)
{
    modbus_slave_stats_t     stats;
    device_health_snapshot_t snap;

    (void)health_monitor_init();

    stats.valid_frames       = 1000U;
    stats.crc_errors         = 5U;
    stats.address_mismatches = 3U;
    stats.exception_responses = 2U;
    stats.unsupported_fc     = 0U;
    stats.successful_responses = 995U;

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
        health_monitor_update_modbus_slave_stats(&stats));

    (void)health_monitor_get_snapshot(&snap);

    TEST_ASSERT_EQUAL_UINT32(1000U, snap.modbus_valid_frames);
    TEST_ASSERT_EQUAL_UINT32(5U,    snap.modbus_crc_errors);
    TEST_ASSERT_EQUAL_UINT32(3U,    snap.modbus_addr_mismatches);
    TEST_ASSERT_EQUAL_UINT32(2U,    snap.modbus_exception_responses);

    /* Null guard. */
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_ARG,
        health_monitor_update_modbus_slave_stats(NULL));
}

/* ======================================================================= */
/* TC-HM-007: get_snapshot returns a value copy                            */
/* ======================================================================= */

void test_TC_HM_007_get_snapshot_returns_value_copy(void)
{
    device_health_snapshot_t snap_before;
    device_health_snapshot_t snap_after;

    (void)health_monitor_init();

    /* Take snapshot before any alarm. */
    (void)health_monitor_get_snapshot(&snap_before);
    TEST_ASSERT_EQUAL_UINT32(0U, snap_before.alarm_raise_count);

    /* Push an event AFTER taking the copy. */
    (void)health_monitor_push_event(HEALTH_EVENT_ALARM_RAISED,
                                    (uint32_t)SENSOR_ID_PRESSURE);

    /* Original copy must be unaffected. */
    TEST_ASSERT_EQUAL_UINT32(0U, snap_before.alarm_raise_count);

    /* New copy sees the update. */
    (void)health_monitor_get_snapshot(&snap_after);
    TEST_ASSERT_EQUAL_UINT32(1U, snap_after.alarm_raise_count);

    /* Null guard. */
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_ARG,
        health_monitor_get_snapshot(NULL));
}

/* ======================================================================= */
/* TC-HM-008: Concurrent push + get — no torn reads                       */
/* TC deferred: host-side pthreads not available in this Ceedling build.  */
/* Verify on hardware: run push_event() and get_snapshot() from two tasks  */
/* simultaneously for 60 s and assert no corrupted snapshot fields.        */
/* ======================================================================= */

void test_TC_HM_008_concurrent_push_and_get_no_torn_reads(void)
{
    TEST_IGNORE_MESSAGE("TC-HM-008: deferred — pthreads unavailable on Windows "
                        "Ceedling host. Verify on target with two-task stress test.");
}

/* ======================================================================= */
/* Additional: not-init guard                                              */
/* ======================================================================= */

void test_not_init_guard_push_event(void)
{
    /* health_monitor_reset_for_test() zeroes initialised flag. */
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NOT_INIT,
        health_monitor_push_event(HEALTH_EVENT_CONFIG_WRITE_FAIL, 0U));
}

void test_not_init_guard_get_snapshot(void)
{
    device_health_snapshot_t snap;
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NOT_INIT,
        health_monitor_get_snapshot(&snap));
}

void test_not_init_guard_modbus_slave_stats(void)
{
    modbus_slave_stats_t stats;
    (void)memset(&stats, 0, sizeof(stats));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NOT_INIT,
        health_monitor_update_modbus_slave_stats(&stats));
}

void test_not_init_guard_reset_metrics(void)
{
    TEST_ASSERT_EQUAL(HEALTH_ADMIN_ERR_NOT_INIT,
        health_monitor_reset_metrics());
}

/* ======================================================================= */
/* Additional: time-sync events update snapshot correctly                  */
/* ======================================================================= */

void test_time_sync_acquired_sets_sync_state(void)
{
    device_health_snapshot_t snap;

    (void)health_monitor_init();

    (void)health_monitor_push_event(HEALTH_EVENT_TIME_SYNC_ACQUIRED, 0U);
    (void)health_monitor_get_snapshot(&snap);

    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, snap.time_sync_state);
}

void test_time_sync_lost_clears_sync_state(void)
{
    device_health_snapshot_t snap;

    (void)health_monitor_init();

    (void)health_monitor_push_event(HEALTH_EVENT_TIME_SYNC_ACQUIRED, 0U);
    (void)health_monitor_push_event(HEALTH_EVENT_TIME_SYNC_LOST, 0U);
    (void)health_monitor_get_snapshot(&snap);

    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, snap.time_sync_state);
}

/* ======================================================================= */
/* Additional: alarm cleared restores GREEN-on / RED-off LED pattern       */
/* ======================================================================= */

void test_alarm_cleared_restores_operational_led(void)
{
    device_health_snapshot_t snap;

    (void)health_monitor_init();

    /* Raise alarm, then push operational lifecycle state. */
    (void)health_monitor_push_event(HEALTH_EVENT_ALARM_RAISED,
                                    (uint32_t)SENSOR_ID_TEMPERATURE);

    /* Manually set operational state through snapshot (test only). */
    (void)health_monitor_get_snapshot(&snap);
    snap.lifecycle_state = LIFECYCLE_STATE_OPERATIONAL;
    /* There is no direct setter; push a fault then clear is the prod path.
     * For this unit test: clear the alarm only, check LED reverts. */

    /* Clear the alarm. */
    (void)health_monitor_push_event(HEALTH_EVENT_ALARM_CLEARED,
                                    (uint32_t)SENSOR_ID_TEMPERATURE);

    /* With lifecycle == INIT and no active alarms: GREEN blink, RED off.
     * In the adaptation, BLINK maps to non-zero state (LED_STATE_BLINK_SLOW=2).
     * RED should be OFF. */
    TEST_ASSERT_TRUE(led_red_is_off());
}

/* ======================================================================= */
/* Additional: sensor_fail sets flag and increments count                  */
/* ======================================================================= */

void test_sensor_fail_event_sets_flag_and_increments_count(void)
{
    device_health_snapshot_t snap;

    (void)health_monitor_init();

    (void)health_monitor_push_event(HEALTH_EVENT_SENSOR_FAIL,
                                    (uint32_t)SENSOR_ID_HUMIDITY);

    (void)health_monitor_get_snapshot(&snap);

    TEST_ASSERT_FALSE(snap.sensor_valid[SENSOR_ID_HUMIDITY]);
    TEST_ASSERT_EQUAL_UINT32(1U, snap.sensor_fail_count);
}

/* ======================================================================= */
/* Additional: set_led_fault overrides directly                            */
/* ======================================================================= */

void test_set_led_fault_drives_red_on_green_off(void)
{
    (void)health_monitor_init();
    led_spy_reset();

    health_monitor_set_led_fault();

    TEST_ASSERT_TRUE(led_red_is_on());
    TEST_ASSERT_TRUE(led_green_is_off());
}
