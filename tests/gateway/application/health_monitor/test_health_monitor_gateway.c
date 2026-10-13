/**
 * @file test_health_monitor_gateway.c
 * @brief Unit tests for HealthMonitor, Gateway build (host, Unity).
 *
 * Test IDs follow docs/lld/application/health-monitor-lld.md §12.2 (HM-T01..T35);
 * HM-T36 onward cover guards the plan does not name.
 *
 * Mocking strategy:
 *   - LedDriver -> led_driver_stub.h; led_set() is a spy defined below
 *   - FreeRTOS  -> freertos_mock.h (auto-links freertos_mock.c); the mutex,
 *                  tick count and per-task stack watermarks are controlled
 *                  through the mock's globals
 *   - Logger    -> compiled out by the SUT's TEST shim
 *
 * Runs under tests/project_gateway.yml because the Gateway and Field Device
 * HealthMonitor share a basename. Build defines: STM32L475xx, BOARD_GATEWAY,
 * TEST (:test_health_monitor_gateway:).
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "led_driver_stub.h"

#include "FreeRTOS.h"
#include "freertos_mock.h" /* causes Ceedling to auto-link freertos_mock.c */
#include "task.h"

#include "health_monitor.h"

/* ====================================================================== */
/* LedDriver spy                                                          */
/* ====================================================================== */

#define LED_SPY_MAX (16U)

static uint32_t g_led_calls;
static led_id_t g_led_ids[LED_SPY_MAX];
static led_pattern_t g_led_patterns[LED_SPY_MAX];
static led_err_t g_led_ret;

led_err_t led_set(led_id_t id, led_pattern_t pattern)
{
    if (g_led_calls < LED_SPY_MAX)
    {
        g_led_ids[g_led_calls] = id;
        g_led_patterns[g_led_calls] = pattern;
    }

    g_led_calls++;

    return g_led_ret;
}

led_err_t led_on(led_id_t id)
{
    (void) id;
    return LED_ERR_OK;
}

led_err_t led_off(led_id_t id)
{
    (void) id;
    return LED_ERR_OK;
}

led_err_t led_toggle(led_id_t id)
{
    (void) id;
    return LED_ERR_OK;
}

static void led_spy_reset(void)
{
    g_led_calls = 0U;
    g_led_ret = LED_ERR_OK;
    (void) memset(g_led_ids, 0, sizeof(g_led_ids));
    (void) memset(g_led_patterns, 0, sizeof(g_led_patterns));
}

static led_pattern_t last_led_pattern(void)
{
    TEST_ASSERT_TRUE(g_led_calls > 0U);

    return g_led_patterns[g_led_calls - 1U];
}

/* ====================================================================== */
/* Fixture                                                                */
/* ====================================================================== */

static health_monitor_handle_t s_h;
static health_monitor_config_t s_cfg;

void setUp(void)
{
    mock_freertos_reset();
    led_spy_reset();
    health_monitor_reset_for_test();

    s_h = NULL;
    s_cfg.boot_reset_cause = LIFECYCLE_RESET_WATCHDOG;
}

void tearDown(void)
{
}

static void create_ok(void)
{
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, health_monitor_create(&s_cfg, &s_h));
    TEST_ASSERT_NOT_NULL(s_h);
}

static health_snapshot_t snap(void)
{
    health_snapshot_t s;

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, health_monitor_get_snapshot(s_h, &s));

    return s;
}

/* ====================================================================== */
/* Group 1 — create / lifecycle                                           */
/* ====================================================================== */

void test_HM_T01_create_initialises_the_snapshot_and_the_init_led(void)
{
    create_ok();

    health_snapshot_t s = snap();

    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_INIT, s.lifecycle_state);
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_WATCHDOG, s.last_reset_cause);
    TEST_ASSERT_EQUAL_UINT32(0U, s.sensor_fail_count);
    TEST_ASSERT_EQUAL_UINT32(0U, s.alarm_raise_count);

    for (uint8_t i = 0U; i < (uint8_t) SENSOR_ID_COUNT; i++)
    {
        TEST_ASSERT_TRUE(s.sensor_valid[i]);
        TEST_ASSERT_EQUAL(ALARM_STATE_CLEAR, s.alarm_state[i]);
    }

    TEST_ASSERT_EQUAL_UINT32(1U, g_led_calls);
    TEST_ASSERT_EQUAL(LED_GREEN, g_led_ids[0]);
    TEST_ASSERT_EQUAL(LED_PATTERN_BLINK_MEDIUM, g_led_patterns[0]);
}

void test_HM_T02_create_null_config_is_rejected(void)
{
    health_monitor_handle_t untouched = (health_monitor_handle_t) 0x1234;

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR, health_monitor_create(NULL, &untouched));
    TEST_ASSERT_EQUAL_PTR(0x1234, untouched);
}

void test_HM_T03_create_null_handle_is_rejected(void)
{
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR, health_monitor_create(&s_cfg, NULL));
}

void test_HM_T04_second_create_exhausts_the_pool(void)
{
    health_monitor_handle_t second = NULL;

    create_ok();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NO_RESOURCE, health_monitor_create(&s_cfg, &second));
    TEST_ASSERT_NULL(second);
}

void test_HM_T05_push_event_on_a_stale_handle_is_rejected(void)
{
    create_ok();
    health_monitor_handle_t stale = s_h;

    health_monitor_reset_for_test();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NOT_INIT,
                      health_monitor_push_event(stale, HEALTH_EVENT_CONFIG_WRITE_FAIL, 0U));
}

/* ====================================================================== */
/* Group 2 — register task                                                */
/* ====================================================================== */

void test_HM_T06_register_task_stores_one_task(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_register_task(s_h, "SensorTask", (TaskHandle_t) 1));
}

void test_HM_T07_eight_tasks_can_be_registered(void)
{
    create_ok();

    for (uint8_t i = 0U; i < HEALTH_MONITOR_MAX_TASKS; i++)
    {
        TEST_ASSERT_EQUAL(
            HEALTH_MONITOR_ERR_OK,
            health_monitor_register_task(s_h, "Task", (TaskHandle_t) (uintptr_t) (i + 1U)));
    }
}

void test_HM_T08_ninth_task_is_rejected(void)
{
    create_ok();

    for (uint8_t i = 0U; i < HEALTH_MONITOR_MAX_TASKS; i++)
    {
        TEST_ASSERT_EQUAL(
            HEALTH_MONITOR_ERR_OK,
            health_monitor_register_task(s_h, "Task", (TaskHandle_t) (uintptr_t) (i + 1U)));
    }

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NO_RESOURCE,
                      health_monitor_register_task(s_h, "NinthTask", (TaskHandle_t) 9));
}

/* ====================================================================== */
/* Group 3 — push event, happy path                                       */
/* ====================================================================== */

void test_HM_T09_config_write_fail_sets_the_sticky_flag(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_push_event(s_h, HEALTH_EVENT_CONFIG_WRITE_FAIL, 0U));

    TEST_ASSERT_TRUE(snap().config_write_failed);
}

void test_HM_T10_sensor_fail_clears_the_flag_and_counts(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_push_event(s_h, HEALTH_EVENT_SENSOR_FAIL, 0U));

    health_snapshot_t s = snap();

    TEST_ASSERT_FALSE(s.sensor_valid[0]);
    TEST_ASSERT_TRUE(s.sensor_valid[1]);
    TEST_ASSERT_EQUAL_UINT32(1U, s.sensor_fail_count);
}

void test_HM_T11_sensor_recovered_restores_the_flag_but_keeps_the_count(void)
{
    create_ok();
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_SENSOR_FAIL, 0U);

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_push_event(s_h, HEALTH_EVENT_SENSOR_RECOVERED, 0U));

    health_snapshot_t s = snap();

    TEST_ASSERT_TRUE(s.sensor_valid[0]);
    TEST_ASSERT_EQUAL_UINT32(1U, s.sensor_fail_count);
}

void test_HM_T12_time_sync_events_set_the_state(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, snap().time_sync_state);

    (void) health_monitor_push_event(s_h, HEALTH_EVENT_TIME_SYNC_ACQUIRED, 0U);
    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, snap().time_sync_state);

    (void) health_monitor_push_event(s_h, HEALTH_EVENT_TIME_SYNC_LOST, 0U);
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, snap().time_sync_state);
}

void test_HM_T13_ntp_sync_ok_records_the_epoch(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_push_event(s_h, HEALTH_EVENT_NTP_SYNC_OK, 1700000000UL));

    TEST_ASSERT_EQUAL_UINT32(1700000000UL, snap().last_ntp_sync_epoch);
}

void test_HM_T14_cloud_connected_follows_the_events(void)
{
    create_ok();

    TEST_ASSERT_FALSE(snap().cloud_connected);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_CLOUD_CONNECTED, 0U);
    TEST_ASSERT_TRUE(snap().cloud_connected);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_CLOUD_DISCONNECTED, 0U);
    TEST_ASSERT_FALSE(snap().cloud_connected);
}

void test_HM_T15_alarm_raised_records_the_alarm_and_slows_the_led(void)
{
    create_ok();
    led_spy_reset();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_push_event(s_h, HEALTH_EVENT_ALARM_RAISED, 0U));

    health_snapshot_t s = snap();

    TEST_ASSERT_NOT_EQUAL(ALARM_STATE_CLEAR, s.alarm_state[0]);
    TEST_ASSERT_EQUAL_UINT32(1U, s.alarm_raise_count);
    TEST_ASSERT_EQUAL(LED_GREEN, g_led_ids[0]);
    TEST_ASSERT_EQUAL(LED_PATTERN_BLINK_SLOW, last_led_pattern());
}

void test_HM_T16_out_of_range_event_changes_nothing(void)
{
    create_ok();
    led_spy_reset();
    health_snapshot_t before = snap();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_push_event(s_h, HEALTH_EVENT_COUNT, 0U));

    health_snapshot_t after = snap();

    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));
    TEST_ASSERT_EQUAL_UINT32(0U, g_led_calls);
}

/* ====================================================================== */
/* Group 4 — stats update                                                 */
/* ====================================================================== */

void test_HM_T17_modbus_master_stats_are_copied(void)
{
    modbus_master_stats_t stats = {.transactions_ok = 100U, .timeouts = 5U};

    create_ok();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_update_modbus_master_stats(s_h, &stats));

    health_snapshot_t s = snap();

    TEST_ASSERT_EQUAL_UINT32(100U, s.modbus_transactions_ok);
    TEST_ASSERT_EQUAL_UINT32(5U, s.modbus_timeouts);
    /* The link state is event-driven; the stats never touch it. */
    TEST_ASSERT_FALSE(s.modbus_link_online);
}

void test_HM_T18_mqtt_stats_are_copied_with_the_real_field_names(void)
{
    mqtt_stats_t stats;

    (void) memset(&stats, 0, sizeof(stats));
    stats.publishes_sent = 50U;
    stats.publish_failures = 2U;
    stats.reconnect_count = 3U;
    stats.connect_ok = 99U; /* not in the snapshot */

    create_ok();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, health_monitor_update_mqtt_stats(s_h, &stats));

    health_snapshot_t s = snap();

    TEST_ASSERT_EQUAL_UINT32(50U, s.mqtt_publishes_sent);
    TEST_ASSERT_EQUAL_UINT32(2U, s.mqtt_publishes_failed);
    TEST_ASSERT_EQUAL_UINT32(3U, s.mqtt_reconnect_count);
}

void test_HM_T19_buffer_occupancy_is_stored(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, health_monitor_update_buffer_occupancy(s_h, 42U));
    TEST_ASSERT_EQUAL_UINT32(42U, snap().buffer_entry_count);
}

void test_HM_T20_wifi_rssi_is_stored(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, health_monitor_update_wifi_rssi(s_h, -65));
    TEST_ASSERT_EQUAL_INT32(-65, snap().wifi_rssi_dbm);
}

void test_HM_T21_operational_lifecycle_makes_the_led_solid(void)
{
    create_ok();
    led_spy_reset();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_update_lifecycle(s_h, LIFECYCLE_STATE_OPERATIONAL));

    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, snap().lifecycle_state);
    TEST_ASSERT_EQUAL(LED_PATTERN_ON, last_led_pattern());
}

void test_HM_T22_poll_collects_a_watermark_for_each_registered_task(void)
{
    create_ok();
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_register_task(s_h, "A", (TaskHandle_t) 1));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_register_task(s_h, "B", (TaskHandle_t) 2));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_register_task(s_h, "C", (TaskHandle_t) 3));

    g_mock_uxTaskGetStackHighWaterMark_seq[0] = 100U;
    g_mock_uxTaskGetStackHighWaterMark_seq[1] = 200U;
    g_mock_uxTaskGetStackHighWaterMark_seq[2] = 150U;
    g_mock_uxTaskGetStackHighWaterMark_seq_len = 3U;

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, health_monitor_poll(s_h));

    health_snapshot_t s = snap();

    TEST_ASSERT_EQUAL_UINT16(100U, s.stack_watermark_words[0]);
    TEST_ASSERT_EQUAL_UINT16(200U, s.stack_watermark_words[1]);
    TEST_ASSERT_EQUAL_UINT16(150U, s.stack_watermark_words[2]);
    TEST_ASSERT_EQUAL_UINT16(0U, s.stack_watermark_words[3]);
    TEST_ASSERT_EQUAL_UINT32(3U, g_mock_uxTaskGetStackHighWaterMark_call_count);
}

/* ====================================================================== */
/* Group 5 — snapshot                                                     */
/* ====================================================================== */

void test_HM_T23_snapshot_is_a_copy(void)
{
    create_ok();
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_NTP_SYNC_FAILED, 0U);

    health_snapshot_t first = snap();

    (void) health_monitor_push_event(s_h, HEALTH_EVENT_NTP_SYNC_FAILED, 0U);

    TEST_ASSERT_EQUAL_UINT32(1U, first.ntp_sync_fail_count);
    TEST_ASSERT_EQUAL_UINT32(2U, snap().ntp_sync_fail_count);
}

void test_HM_T24_uptime_is_computed_from_the_tick_count(void)
{
    create_ok();
    g_mock_tick_count = 80000U; /* 80 s at 1 kHz */

    TEST_ASSERT_EQUAL_UINT32(80U, snap().uptime_s);

    g_mock_tick_count = 125000U;
    TEST_ASSERT_EQUAL_UINT32(125U, snap().uptime_s);
}

void test_HM_T25_get_snapshot_null_output_is_rejected(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR, health_monitor_get_snapshot(s_h, NULL));
}

void test_HM_T26_concurrent_access_deferred(void)
{
    TEST_IGNORE_MESSAGE("deferred: needs real tasks; covered on hardware by TC-HW-HM-009 "
                        "(two writers and a reader checking multi-field consistency).");
}

/* ====================================================================== */
/* Group 6 — LED state machine                                            */
/* ====================================================================== */

void test_HM_T27_init_state_uses_the_medium_blink(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(LED_PATTERN_BLINK_MEDIUM, last_led_pattern());
}

void test_HM_T28_operational_uses_a_solid_led(void)
{
    create_ok();
    (void) health_monitor_update_lifecycle(s_h, LIFECYCLE_STATE_OPERATIONAL);

    TEST_ASSERT_EQUAL(LED_PATTERN_ON, last_led_pattern());
}

void test_HM_T29_alarm_while_operational_uses_the_slow_blink(void)
{
    create_ok();
    (void) health_monitor_update_lifecycle(s_h, LIFECYCLE_STATE_OPERATIONAL);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_ALARM_RAISED, 0U);

    TEST_ASSERT_EQUAL(LED_PATTERN_BLINK_SLOW, last_led_pattern());
}

void test_HM_T30_clearing_the_only_alarm_returns_to_solid(void)
{
    create_ok();
    (void) health_monitor_update_lifecycle(s_h, LIFECYCLE_STATE_OPERATIONAL);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_ALARM_RAISED, 0U);

    (void) health_monitor_push_event(s_h, HEALTH_EVENT_ALARM_CLEARED, 0U);

    TEST_ASSERT_EQUAL(LED_PATTERN_ON, last_led_pattern());
}

void test_HM_T31_fault_is_latched_and_overrides_later_alarms(void)
{
    create_ok();
    (void) health_monitor_update_lifecycle(s_h, LIFECYCLE_STATE_OPERATIONAL);

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, health_monitor_set_led_fault(s_h));
    TEST_ASSERT_EQUAL(LED_PATTERN_BLINK_FAST, last_led_pattern());

    (void) health_monitor_push_event(s_h, HEALTH_EVENT_ALARM_RAISED, 1U);
    TEST_ASSERT_EQUAL(LED_PATTERN_BLINK_FAST, last_led_pattern());

    (void) health_monitor_push_event(s_h, HEALTH_EVENT_ALARM_CLEARED, 1U);
    (void) health_monitor_update_lifecycle(s_h, LIFECYCLE_STATE_OPERATIONAL);
    TEST_ASSERT_EQUAL(LED_PATTERN_BLINK_FAST, last_led_pattern());
}

/* ====================================================================== */
/* Group 7 — admin                                                        */
/* ====================================================================== */

void test_HM_T32_reset_metrics_zeroes_only_the_counters(void)
{
    modbus_master_stats_t mm = {.transactions_ok = 9U, .timeouts = 4U};
    mqtt_stats_t mq;

    (void) memset(&mq, 0, sizeof(mq));
    mq.publishes_sent = 7U;
    mq.publish_failures = 3U;
    mq.reconnect_count = 2U;

    create_ok();
    (void) health_monitor_update_lifecycle(s_h, LIFECYCLE_STATE_OPERATIONAL);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_SENSOR_FAIL, 2U);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_ALARM_RAISED, 1U);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_NTP_SYNC_FAILED, 0U);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_NTP_SYNC_OK, 1700000000UL);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_BUFFER_OVERFLOW, 0U);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_CONFIG_WRITE_FAIL, 0U);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_CLOUD_CONNECTED, 0U);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_MODBUS_LINK_UP, 0U);
    (void) health_monitor_update_modbus_master_stats(s_h, &mm);
    (void) health_monitor_update_mqtt_stats(s_h, &mq);
    (void) health_monitor_update_buffer_occupancy(s_h, 11U);
    (void) health_monitor_update_wifi_rssi(s_h, -70);

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, health_monitor_reset_metrics(s_h));

    health_snapshot_t s = snap();

    /* Zeroed. */
    TEST_ASSERT_EQUAL_UINT32(0U, s.sensor_fail_count);
    TEST_ASSERT_EQUAL_UINT32(0U, s.alarm_raise_count);
    TEST_ASSERT_EQUAL_UINT32(0U, s.modbus_transactions_ok);
    TEST_ASSERT_EQUAL_UINT32(0U, s.modbus_timeouts);
    TEST_ASSERT_EQUAL_UINT32(0U, s.mqtt_publishes_sent);
    TEST_ASSERT_EQUAL_UINT32(0U, s.mqtt_publishes_failed);
    TEST_ASSERT_EQUAL_UINT32(0U, s.mqtt_reconnect_count);
    TEST_ASSERT_EQUAL_UINT32(0U, s.ntp_sync_fail_count);
    TEST_ASSERT_EQUAL_UINT32(0U, s.buffer_overflow_count);

    /* Preserved. */
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, s.lifecycle_state);
    TEST_ASSERT_FALSE(s.sensor_valid[2]);
    TEST_ASSERT_NOT_EQUAL(ALARM_STATE_CLEAR, s.alarm_state[1]);
    TEST_ASSERT_TRUE(s.config_write_failed);
    TEST_ASSERT_TRUE(s.modbus_link_online);
    TEST_ASSERT_TRUE(s.cloud_connected);
    TEST_ASSERT_EQUAL_UINT32(11U, s.buffer_entry_count);
    TEST_ASSERT_EQUAL_UINT32(1700000000UL, s.last_ntp_sync_epoch);
    TEST_ASSERT_EQUAL_INT32(-70, s.wifi_rssi_dbm);
}

void test_HM_T33_set_led_fault_latches_and_drives_the_fast_blink(void)
{
    create_ok();
    led_spy_reset();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, health_monitor_set_led_fault(s_h));

    TEST_ASSERT_EQUAL(LED_GREEN, g_led_ids[0]);
    TEST_ASSERT_EQUAL(LED_PATTERN_BLINK_FAST, g_led_patterns[0]);
}

void test_HM_T34_reset_metrics_on_a_stale_handle_is_rejected(void)
{
    create_ok();
    health_monitor_handle_t stale = s_h;

    health_monitor_reset_for_test();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NOT_INIT, health_monitor_reset_metrics(stale));
}

void test_HM_T35_set_led_fault_on_a_stale_handle_is_rejected(void)
{
    create_ok();
    health_monitor_handle_t stale = s_h;

    health_monitor_reset_for_test();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NOT_INIT, health_monitor_set_led_fault(stale));
}

/* ====================================================================== */
/* Added during implementation                                            */
/* ====================================================================== */

void test_HM_T36_every_function_rejects_a_null_handle(void)
{
    health_snapshot_t s;
    modbus_master_stats_t mm = {0};
    mqtt_stats_t mq;
    uint16_t wm[HEALTH_MONITOR_MAX_TASKS] = {0U};

    (void) memset(&mq, 0, sizeof(mq));

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR, health_monitor_register_task(NULL, "x", NULL));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR, health_monitor_poll(NULL));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR,
                      health_monitor_push_event(NULL, HEALTH_EVENT_FAULT, 0U));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR,
                      health_monitor_update_modbus_master_stats(NULL, &mm));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR, health_monitor_update_mqtt_stats(NULL, &mq));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR,
                      health_monitor_update_buffer_occupancy(NULL, 1U));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR,
                      health_monitor_update_stack_watermarks(NULL, wm));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR,
                      health_monitor_update_lifecycle(NULL, LIFECYCLE_STATE_INIT));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR, health_monitor_update_wifi_rssi(NULL, 0));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR, health_monitor_get_snapshot(NULL, &s));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR, health_monitor_set_led_fault(NULL));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR, health_monitor_reset_metrics(NULL));
}

void test_HM_T37_every_function_rejects_a_stale_handle(void)
{
    health_snapshot_t s;
    modbus_master_stats_t mm = {0};
    mqtt_stats_t mq;
    uint16_t wm[HEALTH_MONITOR_MAX_TASKS] = {0U};

    (void) memset(&mq, 0, sizeof(mq));
    create_ok();
    health_monitor_handle_t stale = s_h;

    health_monitor_reset_for_test();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NOT_INIT, health_monitor_register_task(stale, "x", NULL));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NOT_INIT, health_monitor_poll(stale));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NOT_INIT,
                      health_monitor_update_modbus_master_stats(stale, &mm));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NOT_INIT, health_monitor_update_mqtt_stats(stale, &mq));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NOT_INIT,
                      health_monitor_update_buffer_occupancy(stale, 1U));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NOT_INIT,
                      health_monitor_update_stack_watermarks(stale, wm));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NOT_INIT,
                      health_monitor_update_lifecycle(stale, LIFECYCLE_STATE_INIT));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NOT_INIT, health_monitor_update_wifi_rssi(stale, 0));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NOT_INIT, health_monitor_get_snapshot(stale, &s));
}

void test_HM_T38_null_data_pointers_are_rejected(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR, health_monitor_register_task(s_h, NULL, NULL));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR,
                      health_monitor_update_modbus_master_stats(s_h, NULL));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR, health_monitor_update_mqtt_stats(s_h, NULL));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NULL_PTR,
                      health_monitor_update_stack_watermarks(s_h, NULL));
}

void test_HM_T39_out_of_range_sensor_ids_change_nothing(void)
{
    create_ok();
    led_spy_reset();
    health_snapshot_t before = snap();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_push_event(s_h, HEALTH_EVENT_SENSOR_FAIL, SENSOR_ID_COUNT));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_push_event(s_h, HEALTH_EVENT_SENSOR_RECOVERED, 99U));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_push_event(s_h, HEALTH_EVENT_ALARM_RAISED, SENSOR_ID_COUNT));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_push_event(s_h, HEALTH_EVENT_ALARM_CLEARED, 0xFFFFFFFFUL));

    health_snapshot_t after = snap();

    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));
    TEST_ASSERT_EQUAL_UINT32(0U, g_led_calls);
}

void test_HM_T40_the_last_valid_sensor_id_is_accepted(void)
{
    create_ok();

    (void) health_monitor_push_event(s_h, HEALTH_EVENT_SENSOR_FAIL, SENSOR_ID_COUNT - 1U);

    TEST_ASSERT_FALSE(snap().sensor_valid[SENSOR_ID_COUNT - 1U]);
}

void test_HM_T41_one_cleared_alarm_keeps_the_slow_blink_while_another_is_active(void)
{
    create_ok();
    (void) health_monitor_update_lifecycle(s_h, LIFECYCLE_STATE_OPERATIONAL);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_ALARM_RAISED, 0U);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_ALARM_RAISED, 5U);

    (void) health_monitor_push_event(s_h, HEALTH_EVENT_ALARM_CLEARED, 0U);
    TEST_ASSERT_EQUAL(LED_PATTERN_BLINK_SLOW, last_led_pattern());

    (void) health_monitor_push_event(s_h, HEALTH_EVENT_ALARM_CLEARED, 5U);
    TEST_ASSERT_EQUAL(LED_PATTERN_ON, last_led_pattern());
}

void test_HM_T42_modbus_link_and_buffer_overflow_events(void)
{
    create_ok();

    (void) health_monitor_push_event(s_h, HEALTH_EVENT_MODBUS_LINK_UP, 0U);
    TEST_ASSERT_TRUE(snap().modbus_link_online);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_MODBUS_LINK_DOWN, 0U);
    TEST_ASSERT_FALSE(snap().modbus_link_online);

    (void) health_monitor_push_event(s_h, HEALTH_EVENT_BUFFER_OVERFLOW, 0U);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_BUFFER_OVERFLOW, 0U);
    TEST_ASSERT_EQUAL_UINT32(2U, snap().buffer_overflow_count);
}

void test_HM_T43_fault_event_logs_only_and_changes_nothing(void)
{
    create_ok();
    led_spy_reset();
    health_snapshot_t before = snap();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_push_event(s_h, HEALTH_EVENT_FAULT, 0x42U));

    health_snapshot_t after = snap();

    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));
    TEST_ASSERT_EQUAL_UINT32(0U, g_led_calls);
}

void test_HM_T44_led_driver_failure_does_not_fail_the_operation(void)
{
    g_led_ret = LED_ERR_NOT_INIT;

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, health_monitor_create(&s_cfg, &s_h));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_update_lifecycle(s_h, LIFECYCLE_STATE_OPERATIONAL));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, health_monitor_set_led_fault(s_h));
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK,
                      health_monitor_push_event(s_h, HEALTH_EVENT_ALARM_RAISED, 0U));
}

void test_HM_T45_mutex_creation_failure_is_reported_and_keeps_the_pool_free(void)
{
    g_mock_xSemaphoreCreateMutexStatic_return = NULL;

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_NO_RESOURCE, health_monitor_create(&s_cfg, &s_h));
    TEST_ASSERT_NULL(s_h);

    g_mock_xSemaphoreCreateMutexStatic_return = (SemaphoreHandle_t) 1;
    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, health_monitor_create(&s_cfg, &s_h));
}

void test_HM_T46_every_lock_is_released(void)
{
    modbus_master_stats_t mm = {0};
    mqtt_stats_t mq;
    health_snapshot_t s;

    (void) memset(&mq, 0, sizeof(mq));
    create_ok();
    (void) health_monitor_register_task(s_h, "A", (TaskHandle_t) 1);
    g_mock_xSemaphoreTake_call_count = 0U;
    g_mock_xSemaphoreGive_call_count = 0U;

    (void) health_monitor_poll(s_h);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_ALARM_RAISED, 0U);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_SENSOR_FAIL, 99U);
    (void) health_monitor_push_event(s_h, HEALTH_EVENT_FAULT, 1U);
    (void) health_monitor_update_modbus_master_stats(s_h, &mm);
    (void) health_monitor_update_mqtt_stats(s_h, &mq);
    (void) health_monitor_update_buffer_occupancy(s_h, 1U);
    (void) health_monitor_update_lifecycle(s_h, LIFECYCLE_STATE_OPERATIONAL);
    (void) health_monitor_update_wifi_rssi(s_h, -1);
    (void) health_monitor_get_snapshot(s_h, &s);
    (void) health_monitor_set_led_fault(s_h);
    (void) health_monitor_reset_metrics(s_h);
    (void) health_monitor_register_task(s_h, "B", (TaskHandle_t) 2);

    TEST_ASSERT_TRUE(g_mock_xSemaphoreTake_call_count > 0U);
    TEST_ASSERT_EQUAL_UINT32(g_mock_xSemaphoreTake_call_count, g_mock_xSemaphoreGive_call_count);
}

void test_HM_T47_poll_with_no_registered_tasks_succeeds(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, health_monitor_poll(s_h));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_uxTaskGetStackHighWaterMark_call_count);
}

void test_HM_T48_direct_watermark_update_copies_one_value_per_task(void)
{
    uint16_t wm[HEALTH_MONITOR_MAX_TASKS] = {11U, 22U, 33U, 44U, 55U, 66U, 77U, 88U};

    create_ok();
    (void) health_monitor_register_task(s_h, "A", (TaskHandle_t) 1);
    (void) health_monitor_register_task(s_h, "B", (TaskHandle_t) 2);

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, health_monitor_update_stack_watermarks(s_h, wm));

    health_snapshot_t s = snap();

    TEST_ASSERT_EQUAL_UINT16(11U, s.stack_watermark_words[0]);
    TEST_ASSERT_EQUAL_UINT16(22U, s.stack_watermark_words[1]);
    TEST_ASSERT_EQUAL_UINT16(0U, s.stack_watermark_words[2]);
}

void test_HM_T49_long_task_names_are_truncated_not_overrun(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(HEALTH_MONITOR_ERR_OK, health_monitor_register_task(
                                                 s_h, "AVeryLongTaskNameIndeed", (TaskHandle_t) 1));
}

void test_HM_T50_snapshot_fits_its_size_budget(void)
{
    TEST_ASSERT_TRUE(sizeof(health_snapshot_t) <= 256U);
}
