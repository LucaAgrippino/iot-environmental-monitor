/**
 * @file test_time_provider.c
 * @brief Unit tests for TimeProvider middleware (host build, Unity).
 *
 * Covers TC-TP-001..TC-TP-013 per docs/lld/middleware/time-provider.md §7.
 *
 * Mocking strategy:
 *   - RtcDriver   → s_mock_rtc vtable injected via *(const irtc_t **)&rtc_driver
 *   - IHealthReport → s_mock_health vtable passed to time_provider_init()
 *   - FreeRTOS    → freertos_mock.h (auto-links freertos_mock.c)
 *
 * Build defines required: STM32F469xx, TEST (project.yml :test_time_provider:).
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Include health_monitor_stub.h BEFORE time_provider.h.
 * It provides the struct ihealth_report_s body; time_provider.h (in TEST
 * mode) forward-declares struct ihealth_report_s and typedefs ihealth_report_t.
 * Including the body first ensures the type is complete immediately after
 * time_provider.h is parsed. */
#include "health_monitor_stub.h"
#include "rtc_driver_stub.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "freertos_mock.h"   /* causes Ceedling to auto-link freertos_mock.c */

#include "time_provider.h"
#include "time_provider_config.h"

/* ========================================================================= */
/* rtc_driver singleton — defined here, injected in setUp                   */
/* ========================================================================= */

const irtc_t *rtc_driver;

/* ========================================================================= */
/* RTC mock state                                                            */
/* ========================================================================= */

static rtc_datetime_t g_stub_rtc_datetime;
static rtc_err_t      g_stub_rtc_get_time_ret;
static rtc_err_t      g_stub_rtc_set_time_ret;
static uint32_t       g_stub_rtc_get_time_calls;
static uint32_t       g_stub_rtc_set_time_calls;

static uint8_t        g_stub_bkup_last_idx_write;
static uint32_t       g_stub_bkup_last_val_write;
static uint32_t       g_stub_bkup_write_calls;
static uint32_t       g_stub_bkup_read_ret_val;
static rtc_err_t      g_stub_bkup_read_ret;

static rtc_err_t stub_rtc_init(void) { return RTC_OK; }

static rtc_err_t stub_rtc_get_time(rtc_datetime_t *dt)
{
    g_stub_rtc_get_time_calls++;
    if ((dt != NULL) && (g_stub_rtc_get_time_ret == RTC_OK))
    {
        *dt = g_stub_rtc_datetime;
    }
    return g_stub_rtc_get_time_ret;
}

static rtc_err_t stub_rtc_set_time(const rtc_datetime_t *dt)
{
    (void)dt;
    g_stub_rtc_set_time_calls++;
    return g_stub_rtc_set_time_ret;
}

static bool stub_rtc_is_backup_valid(void) { return true; }

static rtc_err_t stub_rtc_read_backup(uint8_t idx, uint32_t *out)
{
    (void)idx;
    if (out != NULL)
    {
        *out = g_stub_bkup_read_ret_val;
    }
    return g_stub_bkup_read_ret;
}

static rtc_err_t stub_rtc_write_backup(uint8_t idx, uint32_t value)
{
    g_stub_bkup_last_idx_write = idx;
    g_stub_bkup_last_val_write = value;
    g_stub_bkup_write_calls++;
    return RTC_OK;
}

static irtc_t s_mock_rtc;

static void rtc_mock_reset(void)
{
    (void)memset(&g_stub_rtc_datetime, 0, sizeof(g_stub_rtc_datetime));
    g_stub_rtc_get_time_ret    = RTC_OK;
    g_stub_rtc_set_time_ret    = RTC_OK;
    g_stub_rtc_get_time_calls  = 0U;
    g_stub_rtc_set_time_calls  = 0U;
    g_stub_bkup_last_idx_write = 0U;
    g_stub_bkup_last_val_write = 0U;
    g_stub_bkup_write_calls    = 0U;
    g_stub_bkup_read_ret_val   = 0U;
    g_stub_bkup_read_ret       = RTC_OK;
}

/* ========================================================================= */
/* Health mock state                                                         */
/* ========================================================================= */

static uint32_t       g_stub_push_event_calls;
static health_event_t g_stub_push_event_last;

static health_monitor_err_t stub_health_init(void) { return HEALTH_MONITOR_ERR_OK; }

static health_monitor_err_t stub_health_push_event(health_event_t event, uint32_t param)
{
    (void)param;
    g_stub_push_event_calls++;
    g_stub_push_event_last = event;
    return HEALTH_MONITOR_ERR_OK;
}

static ihealth_report_t s_mock_health;

static void health_mock_reset(void)
{
    g_stub_push_event_calls = 0U;
    g_stub_push_event_last  = (health_event_t)0xFFU;
}

/* ========================================================================= */
/* Reference epoch: 2000-01-01 00:00:00 UTC = 946684800                     */
/* ========================================================================= */

#define TEST_EPOCH_Y2K       (946684800UL)
#define TEST_DT_Y2K_YEAR     (2000U)
#define TEST_DT_Y2K_MONTH    (1U)
#define TEST_DT_Y2K_DAY      (1U)
#define TEST_DT_Y2K_HOUR     (0U)
#define TEST_DT_Y2K_MINUTE   (0U)
#define TEST_DT_Y2K_SECOND   (0U)

/** Set the stub RTC current time to 2000-01-01 00:00:00 (Y2K reference). */
static void set_stub_rtc_to_y2k(void)
{
    g_stub_rtc_datetime.year   = TEST_DT_Y2K_YEAR;
    g_stub_rtc_datetime.month  = TEST_DT_Y2K_MONTH;
    g_stub_rtc_datetime.day    = TEST_DT_Y2K_DAY;
    g_stub_rtc_datetime.hour   = TEST_DT_Y2K_HOUR;
    g_stub_rtc_datetime.minute = TEST_DT_Y2K_MINUTE;
    g_stub_rtc_datetime.second = TEST_DT_Y2K_SECOND;
}

/**
 * @brief Helper: init with no backup magic → UNSYNCHRONISED, then call
 *        set_time(TEST_EPOCH_Y2K) to transition to SYNCHRONISED.
 */
static void transition_to_synchronised(void)
{
    time_provider_err_t err;
    g_stub_bkup_read_ret_val = 0U;   /* no magic */
    err = time_provider_init(&s_mock_health);
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, err);

    /* set_time when UNSYNCHRONISED skips sanity check → always succeeds */
    g_stub_bkup_write_calls  = 0U;
    g_stub_push_event_calls  = 0U;
    err = time_provider_set_time(TEST_EPOCH_Y2K);
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, err);
    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, time_provider_get_sync_state());
}

/* ========================================================================= */
/* setUp / tearDown                                                          */
/* ========================================================================= */

void setUp(void)
{
    mock_freertos_reset();
    rtc_mock_reset();
    health_mock_reset();

    s_mock_rtc.init            = stub_rtc_init;
    s_mock_rtc.get_time        = stub_rtc_get_time;
    s_mock_rtc.set_time        = stub_rtc_set_time;
    s_mock_rtc.is_backup_valid = stub_rtc_is_backup_valid;
    s_mock_rtc.read_backup     = stub_rtc_read_backup;
    s_mock_rtc.write_backup    = stub_rtc_write_backup;
    rtc_driver = &s_mock_rtc;

    s_mock_health.init        = stub_health_init;
    s_mock_health.push_event  = stub_health_push_event;

    time_provider_reset_for_test();
}

void tearDown(void) { /* reset in setUp */ }

/* ========================================================================= */
/* TC-TP-001: Init without backup magic → UNSYNCHRONISED                    */
/* ========================================================================= */

void test_TC_TP_001_init_no_magic_starts_unsynchronised(void)
{
    g_stub_bkup_read_ret_val = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK,
                      time_provider_init(&s_mock_health));
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED,
                      time_provider_get_sync_state());
}

/* ========================================================================= */
/* TC-TP-002: Init with backup magic present → SYNCHRONISED                 */
/* ========================================================================= */

void test_TC_TP_002_init_with_magic_starts_synchronised(void)
{
    g_stub_bkup_read_ret_val = TIME_PROVIDER_SYNC_MAGIC;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK,
                      time_provider_init(&s_mock_health));
    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED,
                      time_provider_get_sync_state());
}

/* ========================================================================= */
/* TC-TP-003: get() UNSYNCHRONISED returns uptime epoch                     */
/* ========================================================================= */

void test_TC_TP_003_get_unsynchronised_returns_uptime(void)
{
    time_provider_ts_t ts;
    const TickType_t   ticks = 5000U; /* 5 000 ticks / 1 000 Hz = 5 s */

    g_stub_bkup_read_ret_val = 0U;
    (void)time_provider_init(&s_mock_health);

    g_mock_tick_count = ticks;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_get(&ts));
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, ts.sync_state);
    TEST_ASSERT_EQUAL_UINT32(5U, ts.epoch);
}

/* ========================================================================= */
/* TC-TP-004: set_time() UNSYNCHRONISED → SYNCHRONISED, event pushed once   */
/* ========================================================================= */

void test_TC_TP_004_set_time_unsynchronised_transitions_and_pushes_event(void)
{
    g_stub_bkup_read_ret_val = 0U;
    (void)time_provider_init(&s_mock_health);
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, time_provider_get_sync_state());

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK,
                      time_provider_set_time(TEST_EPOCH_Y2K));

    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, time_provider_get_sync_state());
    TEST_ASSERT_EQUAL_UINT32(1U, g_stub_push_event_calls);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_TIME_SYNC_ACQUIRED, g_stub_push_event_last);
    /* Backup magic must have been written */
    TEST_ASSERT_EQUAL_UINT32(TIME_PROVIDER_BKUP_REG, g_stub_bkup_last_idx_write);
    TEST_ASSERT_EQUAL_UINT32(TIME_PROVIDER_SYNC_MAGIC, g_stub_bkup_last_val_write);
}

/* ========================================================================= */
/* TC-TP-005: Repeated set_time() when SYNCHRONISED — no duplicate event    */
/* ========================================================================= */

void test_TC_TP_005_repeated_set_time_no_duplicate_event(void)
{
    /* Transition to SYNCHRONISED (event fired once). */
    transition_to_synchronised();
    TEST_ASSERT_EQUAL_UINT32(1U, g_stub_push_event_calls);

    /* Second set_time — within sanity delta; must NOT push another event. */
    set_stub_rtc_to_y2k();   /* get_time returns Y2K → epoch 946684800 */
    g_stub_push_event_calls = 0U;

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK,
                      time_provider_set_time(TEST_EPOCH_Y2K + 60UL));

    TEST_ASSERT_EQUAL_UINT32(0U, g_stub_push_event_calls);
    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, time_provider_get_sync_state());
}

/* ========================================================================= */
/* TC-TP-006: mark_unsynchronised() → UNSYNCHRONISED, TIME_SYNC_LOST pushed */
/* ========================================================================= */

void test_TC_TP_006_mark_unsynchronised_transitions_and_pushes_event(void)
{
    transition_to_synchronised();

    g_stub_push_event_calls = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK,
                      time_provider_mark_unsynchronised());

    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, time_provider_get_sync_state());
    TEST_ASSERT_EQUAL_UINT32(1U, g_stub_push_event_calls);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_TIME_SYNC_LOST, g_stub_push_event_last);
    /* Backup register must be cleared */
    TEST_ASSERT_EQUAL_UINT32(0x00000000UL, g_stub_bkup_last_val_write);
}

/* ========================================================================= */
/* TC-TP-007: Sanity-check rejection — delta > threshold, state unchanged   */
/* ========================================================================= */

void test_TC_TP_007_sanity_check_rejects_large_delta(void)
{
    transition_to_synchronised();

    set_stub_rtc_to_y2k();  /* current epoch = 946684800 */
    g_stub_push_event_calls = 0U;

    /* Delta = 86401 > TIME_PROVIDER_SANITY_DELTA_S (86400) → reject */
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_RTC_FAIL,
                      time_provider_set_time(TEST_EPOCH_Y2K + 86401UL));

    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, time_provider_get_sync_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_stub_push_event_calls);
}

/* ========================================================================= */
/* TC-TP-008: Sanity-check border — delta == threshold, accepted            */
/* ========================================================================= */

void test_TC_TP_008_sanity_check_accepts_delta_equal_to_threshold(void)
{
    transition_to_synchronised();

    set_stub_rtc_to_y2k();  /* current epoch = 946684800 */

    /* Delta = 86400 == TIME_PROVIDER_SANITY_DELTA_S → accepted (not >) */
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK,
                      time_provider_set_time(TEST_EPOCH_Y2K + 86400UL));
}

/* ========================================================================= */
/* TC-TP-009: get() SYNCHRONISED returns RTC epoch                          */
/* ========================================================================= */

void test_TC_TP_009_get_synchronised_returns_rtc_epoch(void)
{
    time_provider_ts_t ts;

    transition_to_synchronised();
    set_stub_rtc_to_y2k();  /* RTC returns 2000-01-01 00:00:00 */

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK, time_provider_get(&ts));
    TEST_ASSERT_EQUAL(TIME_SYNC_SYNCHRONISED, ts.sync_state);
    TEST_ASSERT_EQUAL_UINT32(TEST_EPOCH_Y2K, ts.epoch);
}

/* ========================================================================= */
/* TC-TP-010: NULL arg to get() → TIME_PROVIDER_ERR_NULL_ARG               */
/* ========================================================================= */

void test_TC_TP_010_get_null_returns_null_arg_error(void)
{
    g_stub_bkup_read_ret_val = 0U;
    (void)time_provider_init(&s_mock_health);

    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_NULL_ARG, time_provider_get(NULL));
}

/* ========================================================================= */
/* TC-TP-011: mark_unsynchronised() when already UNSYNCHRONISED — no event  */
/* ========================================================================= */

void test_TC_TP_011_mark_unsynchronised_when_already_unsynced_no_event(void)
{
    g_stub_bkup_read_ret_val = 0U;
    (void)time_provider_init(&s_mock_health);
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, time_provider_get_sync_state());

    g_stub_push_event_calls = 0U;
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_OK,
                      time_provider_mark_unsynchronised());

    TEST_ASSERT_EQUAL_UINT32(0U, g_stub_push_event_calls);
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, time_provider_get_sync_state());
}

/* ========================================================================= */
/* TC-TP-012: mark_unsynchronised() clears backup register                  */
/* ========================================================================= */

void test_TC_TP_012_mark_unsynchronised_clears_backup_register(void)
{
    transition_to_synchronised();

    g_stub_bkup_write_calls    = 0U;
    g_stub_bkup_last_val_write = 0xDEADBEEFUL; /* sentinel */

    (void)time_provider_mark_unsynchronised();

    TEST_ASSERT_EQUAL_UINT32(1U, g_stub_bkup_write_calls);
    TEST_ASSERT_EQUAL_UINT32(TIME_PROVIDER_BKUP_REG, g_stub_bkup_last_idx_write);
    TEST_ASSERT_EQUAL_UINT32(0x00000000UL, g_stub_bkup_last_val_write);
}

/* ========================================================================= */
/* TC-TP-013: Not-init guards — all functions return NOT_INIT before init   */
/* ========================================================================= */

void test_TC_TP_013_not_init_guards(void)
{
    time_provider_ts_t ts;
    /* time_provider_reset_for_test() called in setUp — module uninitialized. */
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_NOT_INIT, time_provider_get(&ts));
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_NOT_INIT, time_provider_set_time(0UL));
    TEST_ASSERT_EQUAL(TIME_PROVIDER_ERR_NOT_INIT,
                      time_provider_mark_unsynchronised());
    TEST_ASSERT_EQUAL(TIME_SYNC_UNSYNCHRONISED, time_provider_get_sync_state());
}

/* ========================================================================= */
/* TC-TP-014: Concurrent get/set — deferred (pthreads unavailable on host) */
/* ========================================================================= */

void test_TC_TP_014_concurrent_access_deferred(void)
{
    TEST_IGNORE_MESSAGE("TC-TP-014: deferred — pthreads unavailable on Windows "
                        "Ceedling host. Verify on target with two-task stress test.");
}
