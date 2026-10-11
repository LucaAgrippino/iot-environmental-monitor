/**
 * @file test_time_provider.c
 * @brief Unit tests for TimeProvider middleware, Gateway build (host, Unity).
 *
 * Test IDs follow docs/lld/middleware/time-provider.md §7 (TC-TP-001..010).
 * TC-TP-011 onward cover guards and conversions the plan does not name.
 *
 * Mocking strategy:
 *   - RtcDriver     -> s_mock_rtc vtable injected through the rtc_driver pointer
 *   - IHealthReport -> s_mock_health vtable passed to time_provider_init(); the
 *                      type comes from the placeholder in time_provider.h
 *   - FreeRTOS      -> freertos_mock.h (auto-links freertos_mock.c)
 *
 * Runs under tests/project_gateway.yml because the Gateway and Field Device
 * TimeProvider share a basename (see that file's header). Build defines:
 * STM32L475xx, TEST (:test_time_provider:).
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "rtc_driver_stub.h"

#include "FreeRTOS.h"
#include "freertos_mock.h" /* causes Ceedling to auto-link freertos_mock.c */
#include "semphr.h"
#include "task.h"

#include "time_provider.h"
#include "time_provider_config.h"

/* ====================================================================== */
/* rtc_driver singleton — defined here, injected in setUp                 */
/* ====================================================================== */

const irtc_t *rtc_driver;

/* ====================================================================== */
/* RTC mock                                                               */
/* ====================================================================== */

static rtc_datetime_t g_rtc_now;
static rtc_datetime_t g_rtc_last_set;
static rtc_err_t g_rtc_get_ret;
static rtc_err_t g_rtc_set_ret;
static uint32_t g_rtc_get_calls;
static uint32_t g_rtc_set_calls;

static uint8_t g_bkup_last_idx;
static uint32_t g_bkup_last_val;
static uint32_t g_bkup_write_calls;
static uint32_t g_bkup_read_val;
static rtc_err_t g_bkup_read_ret;
static rtc_err_t g_bkup_write_ret;

static rtc_err_t stub_rtc_init(void)
{
    return RTC_OK;
}

static rtc_err_t stub_rtc_get_time(rtc_datetime_t *dt)
{
    g_rtc_get_calls++;

    if ((dt != NULL) && (g_rtc_get_ret == RTC_OK))
    {
        *dt = g_rtc_now;
    }

    return g_rtc_get_ret;
}

static rtc_err_t stub_rtc_set_time(const rtc_datetime_t *dt)
{
    g_rtc_set_calls++;

    if (dt != NULL)
    {
        g_rtc_last_set = *dt;
    }

    return g_rtc_set_ret;
}

static bool stub_rtc_is_backup_valid(void)
{
    return true;
}

static rtc_err_t stub_rtc_read_backup(uint8_t idx, uint32_t *out)
{
    (void) idx;

    if (out != NULL)
    {
        *out = g_bkup_read_val;
    }

    return g_bkup_read_ret;
}

static rtc_err_t stub_rtc_write_backup(uint8_t idx, uint32_t value)
{
    g_bkup_last_idx = idx;
    g_bkup_last_val = value;
    g_bkup_write_calls++;

    return g_bkup_write_ret;
}

static irtc_t s_mock_rtc;

static void rtc_mock_reset(void)
{
    (void) memset(&g_rtc_now, 0, sizeof(g_rtc_now));
    (void) memset(&g_rtc_last_set, 0, sizeof(g_rtc_last_set));
    g_rtc_get_ret = RTC_OK;
    g_rtc_set_ret = RTC_OK;
    g_rtc_get_calls = 0U;
    g_rtc_set_calls = 0U;
    g_bkup_last_idx = 0xFFU;
    g_bkup_last_val = 0U;
    g_bkup_write_calls = 0U;
    g_bkup_read_val = 0U;
    g_bkup_read_ret = RTC_OK;
    g_bkup_write_ret = RTC_OK;
}

/* ====================================================================== */
/* IHealthReport mock                                                     */
/* ====================================================================== */

static uint32_t g_push_calls;
static health_event_t g_push_last;

static health_monitor_err_t stub_health_init(void)
{
    return HEALTH_MONITOR_ERR_OK;
}

static health_monitor_err_t stub_health_push_event(health_event_t event, uint32_t param)
{
    (void) param;
    g_push_calls++;
    g_push_last = event;

    return HEALTH_MONITOR_ERR_OK;
}

static ihealth_report_t s_mock_health;

static void health_mock_reset(void)
{
    g_push_calls = 0U;
    g_push_last = (health_event_t) 0xFFU;
}

/* ====================================================================== */
/* Reference values                                                       */
/* ====================================================================== */

#define EPOCH_Y2K (946684800UL)           /* 2000-01-01 00:00:00 */
#define EPOCH_2000_LEAP_DAY (951782400UL) /* 2000-02-29 00:00:00 */
#define EPOCH_2024_LEAP (1709210096UL)    /* 2024-02-29 12:34:56 */
#define EPOCH_2038 (2147483647UL)         /* 2038-01-19 03:14:07 */
#define EPOCH_2099_END (4102444799UL)     /* 2099-12-31 23:59:59 */

static void set_rtc_now(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                        uint8_t second)
{
    g_rtc_now.year = year;
    g_rtc_now.month = month;
    g_rtc_now.day = day;
    g_rtc_now.hour = hour;
    g_rtc_now.minute = minute;
    g_rtc_now.second = second;
}

static void set_rtc_now_y2k(void)
{
    set_rtc_now(2000U, 1U, 1U, 0U, 0U, 0U);
}

/** @brief Init unsynchronised, then set_time() so the provider is SYNCHRONISED. */
static void transition_to_synchronised(void)
{
    g_bkup_read_val = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(&s_mock_health));
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_set_time(EPOCH_Y2K));
    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, time_provider_get_sync_state());

    g_bkup_write_calls = 0U;
    g_push_calls = 0U;
}

/* ====================================================================== */
/* Fixture                                                                */
/* ====================================================================== */

void setUp(void)
{
    mock_freertos_reset();
    rtc_mock_reset();
    health_mock_reset();

    s_mock_rtc.init = stub_rtc_init;
    s_mock_rtc.get_time = stub_rtc_get_time;
    s_mock_rtc.set_time = stub_rtc_set_time;
    s_mock_rtc.is_backup_valid = stub_rtc_is_backup_valid;
    s_mock_rtc.read_backup = stub_rtc_read_backup;
    s_mock_rtc.write_backup = stub_rtc_write_backup;
    rtc_driver = &s_mock_rtc;

    s_mock_health.init = stub_health_init;
    s_mock_health.push_event = stub_health_push_event;

    time_provider_reset_for_test();
}

void tearDown(void)
{
}

/* ====================================================================== */
/* LLD §7 — TC-TP-001..010                                                */
/* ====================================================================== */

void test_TC_TP_001_get_before_init_returns_not_init(void)
{
    time_provider_ts_t ts;

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_NOT_INIT, time_provider_get(&ts));
}

void test_TC_TP_002_get_after_init_unsynchronised_returns_uptime(void)
{
    time_provider_ts_t ts;

    g_bkup_read_val = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(&s_mock_health));

    g_mock_tick_count = 5000U; /* 5 000 ticks / 1 000 Hz = 5 s */

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_get(&ts));
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, ts.sync_state);
    TEST_ASSERT_EQUAL_UINT32(5U, ts.epoch);
    TEST_ASSERT_EQUAL_UINT32(0U, g_rtc_get_calls);
}

void test_TC_TP_003_get_null_returns_null_arg(void)
{
    g_bkup_read_val = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(&s_mock_health));

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_NULL_ARG, time_provider_get(NULL));
}

void test_TC_TP_004_first_set_time_synchronises_and_pushes_event_once(void)
{
    g_bkup_read_val = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(&s_mock_health));

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_set_time(EPOCH_Y2K));

    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, time_provider_get_sync_state());
    TEST_ASSERT_EQUAL_UINT32(1U, g_push_calls);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_TIME_SYNC_ACQUIRED, g_push_last);
    TEST_ASSERT_EQUAL_UINT32(TIME_PROVIDER_BKUP_REG, g_bkup_last_idx);
    TEST_ASSERT_EQUAL_UINT32(TIME_PROVIDER_SYNC_MAGIC, g_bkup_last_val);
}

void test_TC_TP_005_repeated_set_time_does_not_push_again(void)
{
    transition_to_synchronised();
    set_rtc_now_y2k();

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_set_time(EPOCH_Y2K + 60UL));

    TEST_ASSERT_EQUAL_UINT32(0U, g_push_calls);
    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, time_provider_get_sync_state());
}

void test_TC_TP_006_mark_unsynchronised_transitions_and_pushes_lost(void)
{
    transition_to_synchronised();

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_mark_unsynchronised());

    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, time_provider_get_sync_state());
    TEST_ASSERT_EQUAL_UINT32(1U, g_push_calls);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_TIME_SYNC_LOST, g_push_last);
    TEST_ASSERT_EQUAL_UINT32(TIME_PROVIDER_BKUP_REG, g_bkup_last_idx);
    TEST_ASSERT_EQUAL_UINT32(TIME_PROVIDER_SYNC_CLEARED, g_bkup_last_val);
}

void test_TC_TP_007_mark_unsynchronised_when_already_unsynchronised_pushes_nothing(void)
{
    g_bkup_read_val = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(&s_mock_health));

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_mark_unsynchronised());

    TEST_ASSERT_EQUAL_UINT32(0U, g_push_calls);
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, time_provider_get_sync_state());
}

void test_TC_TP_008_sanity_delta_exceeded_is_rejected_and_state_unchanged(void)
{
    transition_to_synchronised();
    set_rtc_now_y2k();
    g_rtc_set_calls = 0U;

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_RTC_FAIL,
                      time_provider_set_time(EPOCH_Y2K + TIME_PROVIDER_SANITY_DELTA_S + 1UL));

    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, time_provider_get_sync_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_rtc_set_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, g_bkup_write_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, g_push_calls);
}

void test_TC_TP_009_init_with_magic_starts_synchronised(void)
{
    g_bkup_read_val = TIME_PROVIDER_SYNC_MAGIC;

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(&s_mock_health));
    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, time_provider_get_sync_state());
}

void test_TC_TP_010_init_without_magic_starts_unsynchronised(void)
{
    g_bkup_read_val = 0x12345678UL;

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(&s_mock_health));
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, time_provider_get_sync_state());
}

/* ====================================================================== */
/* Added during implementation                                            */
/* ====================================================================== */

void test_TC_TP_011_sanity_delta_equal_to_threshold_is_accepted(void)
{
    transition_to_synchronised();
    set_rtc_now_y2k();

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK,
                      time_provider_set_time(EPOCH_Y2K + TIME_PROVIDER_SANITY_DELTA_S));
}

void test_TC_TP_012_sanity_delta_applies_to_a_backward_step(void)
{
    transition_to_synchronised();
    set_rtc_now(2000U, 3U, 1U, 0U, 0U, 0U);

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_RTC_FAIL, time_provider_set_time(EPOCH_Y2K));
}

void test_TC_TP_013_first_sync_skips_the_sanity_check(void)
{
    g_bkup_read_val = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(&s_mock_health));
    set_rtc_now_y2k();

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_set_time(EPOCH_2038));
    TEST_ASSERT_EQUAL_UINT32(0U, g_rtc_get_calls);
}

void test_TC_TP_014_get_synchronised_returns_rtc_epoch(void)
{
    time_provider_ts_t ts;

    transition_to_synchronised();
    set_rtc_now_y2k();

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_get(&ts));
    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, ts.sync_state);
    TEST_ASSERT_EQUAL_UINT32(EPOCH_Y2K, ts.epoch);
}

void test_TC_TP_015_get_converts_leap_day_and_century_dates(void)
{
    time_provider_ts_t ts;

    transition_to_synchronised();

    set_rtc_now(2000U, 2U, 29U, 0U, 0U, 0U);
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_get(&ts));
    TEST_ASSERT_EQUAL_UINT32(EPOCH_2000_LEAP_DAY, ts.epoch);

    set_rtc_now(2024U, 2U, 29U, 12U, 34U, 56U);
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_get(&ts));
    TEST_ASSERT_EQUAL_UINT32(EPOCH_2024_LEAP, ts.epoch);

    set_rtc_now(2038U, 1U, 19U, 3U, 14U, 7U);
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_get(&ts));
    TEST_ASSERT_EQUAL_UINT32(EPOCH_2038, ts.epoch);

    set_rtc_now(2099U, 12U, 31U, 23U, 59U, 59U);
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_get(&ts));
    TEST_ASSERT_EQUAL_UINT32(EPOCH_2099_END, ts.epoch);
}

void test_TC_TP_016_set_time_writes_the_matching_calendar_fields(void)
{
    g_bkup_read_val = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(&s_mock_health));

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_set_time(EPOCH_2024_LEAP));

    TEST_ASSERT_EQUAL_UINT16(2024U, g_rtc_last_set.year);
    TEST_ASSERT_EQUAL_UINT8(2U, g_rtc_last_set.month);
    TEST_ASSERT_EQUAL_UINT8(29U, g_rtc_last_set.day);
    TEST_ASSERT_EQUAL_UINT8(12U, g_rtc_last_set.hour);
    TEST_ASSERT_EQUAL_UINT8(34U, g_rtc_last_set.minute);
    TEST_ASSERT_EQUAL_UINT8(56U, g_rtc_last_set.second);
}

void test_TC_TP_017_epoch_range_limits_are_accepted(void)
{
    g_bkup_read_val = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(&s_mock_health));

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_set_time(TIME_PROVIDER_EPOCH_MIN));
    TEST_ASSERT_EQUAL_UINT16(2000U, g_rtc_last_set.year);
    TEST_ASSERT_EQUAL_UINT8(1U, g_rtc_last_set.month);
    TEST_ASSERT_EQUAL_UINT8(1U, g_rtc_last_set.day);

    /* Force the sanity check out of the way by marking unsynchronised first. */
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_mark_unsynchronised());

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_set_time(TIME_PROVIDER_EPOCH_MAX));
    TEST_ASSERT_EQUAL_UINT16(2099U, g_rtc_last_set.year);
    TEST_ASSERT_EQUAL_UINT8(12U, g_rtc_last_set.month);
    TEST_ASSERT_EQUAL_UINT8(31U, g_rtc_last_set.day);
    TEST_ASSERT_EQUAL_UINT8(23U, g_rtc_last_set.hour);
    TEST_ASSERT_EQUAL_UINT8(59U, g_rtc_last_set.minute);
    TEST_ASSERT_EQUAL_UINT8(59U, g_rtc_last_set.second);
}

void test_TC_TP_018_epoch_outside_the_rtc_range_is_rejected_before_the_driver(void)
{
    g_bkup_read_val = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(&s_mock_health));

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_RTC_FAIL, time_provider_set_time(0UL));
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_RTC_FAIL,
                      time_provider_set_time(TIME_PROVIDER_EPOCH_MIN - 1UL));
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_RTC_FAIL,
                      time_provider_set_time(TIME_PROVIDER_EPOCH_MAX + 1UL));

    TEST_ASSERT_EQUAL_UINT32(0U, g_rtc_set_calls);
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, time_provider_get_sync_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_push_calls);
}

void test_TC_TP_019_rtc_set_failure_leaves_state_and_backup_untouched(void)
{
    g_bkup_read_val = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(&s_mock_health));
    g_rtc_set_ret = RTC_ERR_INIT_TIMEOUT;

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_RTC_FAIL, time_provider_set_time(EPOCH_Y2K));

    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, time_provider_get_sync_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_bkup_write_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, g_push_calls);
}

void test_TC_TP_020_rtc_read_failure_in_get_is_reported(void)
{
    time_provider_ts_t ts;

    transition_to_synchronised();
    g_rtc_get_ret = RTC_ERR_SYNC_TIMEOUT;

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_RTC_FAIL, time_provider_get(&ts));
}

void test_TC_TP_021_rtc_read_failure_in_the_sanity_check_is_reported(void)
{
    transition_to_synchronised();
    g_rtc_get_ret = RTC_ERR_SYNC_TIMEOUT;
    g_rtc_set_calls = 0U;

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_RTC_FAIL, time_provider_set_time(EPOCH_Y2K + 60UL));
    TEST_ASSERT_EQUAL_UINT32(0U, g_rtc_set_calls);
}

void test_TC_TP_022_init_backup_read_failure_leaves_the_module_uninitialised(void)
{
    time_provider_ts_t ts;

    g_bkup_read_ret = RTC_ERR_BACKUP_BOUNDS;

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_RTC_FAIL, time_provider_init(&s_mock_health));
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_NOT_INIT, time_provider_get(&ts));
}

void test_TC_TP_023_init_mutex_creation_failure_is_reported(void)
{
    g_mock_xSemaphoreCreateMutexStatic_return = NULL;

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_RTC_FAIL, time_provider_init(&s_mock_health));
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, time_provider_get_sync_state());
}

void test_TC_TP_024_backup_write_failure_still_synchronises(void)
{
    g_bkup_read_val = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(&s_mock_health));
    g_bkup_write_ret = RTC_ERR_BACKUP_BOUNDS;

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_set_time(EPOCH_Y2K));
    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, time_provider_get_sync_state());
    TEST_ASSERT_EQUAL_UINT32(1U, g_push_calls);
}

void test_TC_TP_025_every_operation_rejects_calls_before_init(void)
{
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_NOT_INIT, time_provider_set_time(EPOCH_Y2K));
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_NOT_INIT, time_provider_mark_unsynchronised());
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, time_provider_get_sync_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_rtc_set_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, g_bkup_write_calls);
}

void test_TC_TP_026_null_health_handle_is_allowed(void)
{
    g_bkup_read_val = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(NULL));

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_set_time(EPOCH_Y2K));
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_mark_unsynchronised());
    TEST_ASSERT_EQUAL_UINT32(0U, g_push_calls);
}

void test_TC_TP_027_warm_reset_restores_synchronised_state(void)
{
    transition_to_synchronised();
    TEST_ASSERT_EQUAL_UINT32(0U, g_push_calls);

    /* Warm reset: RAM state is lost, BKP0R still holds the magic. */
    time_provider_reset_for_test();
    g_bkup_read_val = g_bkup_last_val;

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(&s_mock_health));
    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, time_provider_get_sync_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_push_calls);
}

void test_TC_TP_028_sync_lost_then_reset_restarts_unsynchronised(void)
{
    transition_to_synchronised();
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_mark_unsynchronised());

    time_provider_reset_for_test();
    g_bkup_read_val = g_bkup_last_val;

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_init(&s_mock_health));
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, time_provider_get_sync_state());
}

void test_TC_TP_029_singleton_vtable_dispatches_to_the_functions(void)
{
    time_provider_ts_t ts;

    TEST_ASSERT_NOT_NULL(time_provider);

    g_bkup_read_val = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider->init(&s_mock_health));
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider->set_time(EPOCH_Y2K));
    set_rtc_now_y2k();
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider->get(&ts));
    TEST_ASSERT_EQUAL_UINT32(EPOCH_Y2K, ts.epoch);
    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, time_provider->get_sync_state());
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider->mark_unsynchronised());
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, time_provider->get_sync_state());
}

void test_TC_TP_030_mutex_is_released_on_every_path(void)
{
    time_provider_ts_t ts;

    transition_to_synchronised();
    set_rtc_now_y2k();
    g_mock_xSemaphoreTake_call_count = 0U;
    g_mock_xSemaphoreGive_call_count = 0U;

    (void) time_provider_get(&ts);
    (void) time_provider_set_time(EPOCH_Y2K + 60UL);
    (void) time_provider_set_time(EPOCH_Y2K + TIME_PROVIDER_SANITY_DELTA_S + 1UL);
    g_rtc_set_ret = RTC_ERR_INIT_TIMEOUT;
    (void) time_provider_set_time(EPOCH_Y2K + 120UL);
    g_rtc_set_ret = RTC_OK;
    g_rtc_get_ret = RTC_ERR_SYNC_TIMEOUT;
    (void) time_provider_get(&ts);
    (void) time_provider_set_time(EPOCH_Y2K + 60UL);
    (void) time_provider_get_sync_state();
    (void) time_provider_mark_unsynchronised();

    TEST_ASSERT_EQUAL_UINT32(g_mock_xSemaphoreTake_call_count, g_mock_xSemaphoreGive_call_count);
}

void test_TC_TP_031_mutex_take_failure_is_reported_without_touching_the_rtc(void)
{
    time_provider_ts_t ts;

    transition_to_synchronised();
    g_rtc_get_calls = 0U;
    g_rtc_set_calls = 0U;
    g_mock_xSemaphoreTake_return = pdFALSE;

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_NOT_INIT, time_provider_get(&ts));
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_NOT_INIT, time_provider_set_time(EPOCH_Y2K + 60UL));
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_NOT_INIT, time_provider_mark_unsynchronised());
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, time_provider_get_sync_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_rtc_get_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, g_rtc_set_calls);
}

void test_TC_TP_032_concurrent_access_deferred(void)
{
    TEST_IGNORE_MESSAGE("deferred: concurrent get/set needs two real tasks; verify on target "
                        "(integration test) — host has no scheduler.");
}
