/**
 * @file test_time_service.c
 * @brief Unity tests for TimeService (Gateway).
 *
 * NtpClient, TimeProvider and ModbusPoller are replaced by spies defined in this file;
 * the test includes their types only through time_service_internal.h, so Ceedling links
 * none of their .c files. FreeRTOS comes from freertos_mock. Test IDs follow companion
 * §15 (TC-TS-001..019); TC-TS-020.. are additions.
 */

#include <string.h>

#include "freertos_mock.h"
#include "time_service.h"
#include "time_service_internal.h"
#include "unity.h"

#define NTP_HANDLE ((ntp_client_handle_t) 0x1234)
#define BASE_UTC (1760000000UL)
#define NOTIFY_PERIODIC (1UL << 0)
#define NOTIFY_RETRY (1UL << 1)
#define CMD_SET_TIME_HI_REG (0x0210U)

/* ======================================================================= */
/* Spies                                                                    */
/* ======================================================================= */

static ntp_client_err_t s_ntp_result;
static uint32_t s_ntp_epoch;
static uint32_t s_ntp_calls;
static ntp_client_handle_t s_ntp_handle_seen;
static uint8_t s_ntp_count_seen;
static char s_ntp_first_seen[64];

ntp_client_err_t ntp_client_query(ntp_client_handle_t handle, const char *const *server_list,
                                  uint8_t server_count, uint32_t *unix_epoch_out)
{
    s_ntp_calls++;
    s_ntp_handle_seen = handle;
    s_ntp_count_seen = server_count;
    TEST_ASSERT_NOT_NULL(server_list);
    TEST_ASSERT_NOT_NULL(server_list[0]);
    (void) strncpy(s_ntp_first_seen, server_list[0], sizeof(s_ntp_first_seen) - 1U);
    if (s_ntp_result == NTP_CLIENT_ERR_OK)
    {
        *unix_epoch_out = s_ntp_epoch;
    }
    return s_ntp_result;
}

static time_provider_err_t s_tp_get_result;
static time_provider_ts_t s_tp_now;
static time_provider_err_t s_tp_set_result;
static uint32_t s_tp_set_calls;
static uint32_t s_tp_set_epoch;

time_provider_err_t time_provider_get(time_provider_ts_t *ts_out)
{
    *ts_out = s_tp_now;
    return s_tp_get_result;
}

time_provider_err_t time_provider_set_time(uint32_t new_epoch)
{
    s_tp_set_calls++;
    s_tp_set_epoch = new_epoch;
    return s_tp_set_result;
}

static modbus_poller_err_t s_mp_result;
static uint32_t s_mp_calls;
static modbus_command_t s_mp_cmd;

modbus_poller_err_t modbus_poller_send_command(const modbus_command_t *cmd)
{
    s_mp_calls++;
    s_mp_cmd = *cmd;
    return s_mp_result;
}

/* ======================================================================= */
/* Fixture                                                                  */
/* ======================================================================= */

void setUp(void)
{
    mock_freertos_reset();
    time_service_reset_for_test();

    s_ntp_result = NTP_CLIENT_ERR_OK;
    s_ntp_epoch = BASE_UTC;
    s_ntp_calls = 0U;
    s_ntp_handle_seen = NULL;
    s_ntp_count_seen = 0U;

    s_tp_get_result = TIME_PROVIDER_ERR_OK;
    (void) memset(&s_tp_now, 0, sizeof(s_tp_now));
    s_tp_now.epoch = BASE_UTC;
    s_tp_now.sync_state = TIME_SYNC_SYNCHRONISED;
    s_tp_set_result = TIME_PROVIDER_ERR_OK;
    s_tp_set_calls = 0U;
    s_tp_set_epoch = 0U;

    s_mp_result = MODBUS_POLLER_ERR_OK;
    s_mp_calls = 0U;
    (void) memset(&s_mp_cmd, 0, sizeof(s_mp_cmd));

    /* The FD response queue: report one OK reply by default. */
    g_mock_xQueueReceive_return = pdTRUE;
    g_mock_xQueueReceive_available = 0xFFFFFFFFU;
    (void) memset(g_mock_xQueueReceive_next_item, 0, sizeof(g_mock_xQueueReceive_next_item));
    g_mock_xQueueReceive_next_item_size = sizeof(modbus_master_err_t);
}

void tearDown(void)
{
}

static void init_service(void)
{
    TEST_ASSERT_EQUAL(TIME_SERVICE_OK, time_service_init(NTP_HANDLE));
}

static time_service_sync_t state_now(void)
{
    const itime_service_t *ts = time_service_get_interface();
    TEST_ASSERT_NOT_NULL(ts);
    return ts->get_sync_state(ts->ctx);
}

/* Bring the service to SYNC_OK with the RTC at BASE_UTC. */
static void sync_once_ok(void)
{
    init_service();
    TEST_ASSERT_EQUAL(TIME_SERVICE_OK, time_service_sync_for_test());
    TEST_ASSERT_EQUAL(TIME_SERVICE_SYNC_OK, state_now());
}

static void queue_fd_reply(modbus_master_err_t status)
{
    (void) memcpy(g_mock_xQueueReceive_next_item, &status, sizeof(status));
}

/* ======================================================================= */
/* Init and interface                                                       */
/* ======================================================================= */

/* TC-TS-001 */
void test_init_creates_task_timer_and_queue_and_starts_never(void)
{
    init_service();

    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTaskCreateStatic_call_count);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTimerCreateStatic_call_count);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xQueueCreateStatic_call_count);
    TEST_ASSERT_EQUAL(TIME_SERVICE_SYNC_NEVER, state_now());
}

/* TC-TS-002 */
void test_init_is_idempotent(void)
{
    init_service();
    TEST_ASSERT_EQUAL(TIME_SERVICE_OK, time_service_init(NTP_HANDLE));

    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTaskCreateStatic_call_count);
}

/* TC-TS-003 */
void test_get_interface_before_init_is_null(void)
{
    TEST_ASSERT_NULL(time_service_get_interface());
}

/* TC-TS-004 */
void test_get_interface_after_init_exposes_both_operations(void)
{
    init_service();
    const itime_service_t *ts = time_service_get_interface();

    TEST_ASSERT_NOT_NULL(ts);
    TEST_ASSERT_NOT_NULL(ts->get_sync_state);
    TEST_ASSERT_NOT_NULL(ts->trigger_retry);
    TEST_ASSERT_NOT_NULL(ts->ctx);
}

/* TC-TS-020 */
void test_init_rejects_a_null_ntp_handle(void)
{
    TEST_ASSERT_EQUAL(TIME_SERVICE_ERR_NULL_ARG, time_service_init(NULL));
    TEST_ASSERT_NULL(time_service_get_interface());
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xTaskCreateStatic_call_count);
}

/* ======================================================================= */
/* Sync policy                                                              */
/* ======================================================================= */

/* TC-TS-005 */
void test_first_sync_bypasses_delta_check_and_writes_rtc_and_fd(void)
{
    init_service();
    s_ntp_epoch = BASE_UTC;
    s_tp_now.epoch = 5U; /* arbitrary post-reset value, wildly different */

    TEST_ASSERT_EQUAL(TIME_SERVICE_OK, time_service_sync_for_test());

    TEST_ASSERT_EQUAL_UINT32(1U, s_tp_set_calls);
    TEST_ASSERT_EQUAL_UINT32(BASE_UTC, s_tp_set_epoch);
    TEST_ASSERT_EQUAL(TIME_SERVICE_SYNC_OK, state_now());
    TEST_ASSERT_EQUAL_UINT32(1U, s_mp_calls);
    TEST_ASSERT_EQUAL_PTR(NTP_HANDLE, s_ntp_handle_seen);
    TEST_ASSERT_GREATER_THAN_UINT8(0U, s_ntp_count_seen);
}

/* TC-TS-006 */
void test_first_sync_failure_stays_never_and_skips_the_fd_push(void)
{
    init_service();
    s_ntp_result = NTP_CLIENT_ERR_ALL_FAILED;

    TEST_ASSERT_EQUAL(TIME_SERVICE_ERR_NTP_FAILED, time_service_sync_for_test());

    TEST_ASSERT_EQUAL(TIME_SERVICE_SYNC_NEVER, state_now());
    TEST_ASSERT_EQUAL_UINT32(0U, s_tp_set_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_mp_calls);
}

/* TC-TS-007 */
void test_later_sync_within_the_delta_is_accepted(void)
{
    sync_once_ok();
    s_ntp_epoch = BASE_UTC + 1800U;

    TEST_ASSERT_EQUAL(TIME_SERVICE_OK, time_service_sync_for_test());

    TEST_ASSERT_EQUAL_UINT32(BASE_UTC + 1800U, s_tp_set_epoch);
    TEST_ASSERT_EQUAL(TIME_SERVICE_SYNC_OK, state_now());
}

/* TC-TS-008 */
void test_delta_exactly_at_the_threshold_is_accepted(void)
{
    sync_once_ok();
    s_ntp_epoch = BASE_UTC + TIME_SERVICE_NTP_MAX_DELTA_S;

    TEST_ASSERT_EQUAL(TIME_SERVICE_OK, time_service_sync_for_test());
}

/* TC-TS-009 */
void test_delta_above_the_threshold_is_rejected_in_both_directions(void)
{
    sync_once_ok();
    const uint32_t writes_before = s_tp_set_calls;
    const uint32_t pushes_before = s_mp_calls;

    s_ntp_epoch = BASE_UTC + TIME_SERVICE_NTP_MAX_DELTA_S + 1U;
    TEST_ASSERT_EQUAL(TIME_SERVICE_ERR_DELTA_REJECTED, time_service_sync_for_test());
    TEST_ASSERT_EQUAL(TIME_SERVICE_SYNC_STALE, state_now());

    s_ntp_epoch = BASE_UTC - TIME_SERVICE_NTP_MAX_DELTA_S - 1U;
    TEST_ASSERT_EQUAL(TIME_SERVICE_ERR_DELTA_REJECTED, time_service_sync_for_test());

    TEST_ASSERT_EQUAL_UINT32(writes_before, s_tp_set_calls);
    TEST_ASSERT_EQUAL_UINT32(pushes_before, s_mp_calls);
}

/* TC-TS-010 */
void test_rtc_write_failure_still_marks_ok_and_reports_the_error(void)
{
    init_service();
    s_tp_set_result = TIME_PROVIDER_ERR_RTC_FAIL;

    TEST_ASSERT_EQUAL(TIME_SERVICE_ERR_RTC_WRITE, time_service_sync_for_test());

    TEST_ASSERT_EQUAL(TIME_SERVICE_SYNC_OK, state_now());
}

/* TC-TS-016, TC-TS-018 */
void test_state_walks_never_ok_stale_and_recovers(void)
{
    init_service();
    TEST_ASSERT_EQUAL(TIME_SERVICE_SYNC_NEVER, state_now());

    s_ntp_result = NTP_CLIENT_ERR_OK;
    (void) time_service_sync_for_test();
    TEST_ASSERT_EQUAL(TIME_SERVICE_SYNC_OK, state_now());

    s_ntp_result = NTP_CLIENT_ERR_ALL_FAILED;
    (void) time_service_sync_for_test();
    TEST_ASSERT_EQUAL(TIME_SERVICE_SYNC_STALE, state_now());

    s_ntp_result = NTP_CLIENT_ERR_OK;
    (void) time_service_sync_for_test();
    TEST_ASSERT_EQUAL(TIME_SERVICE_SYNC_OK, state_now());
}

/* TC-TS-021 (TS-D12): an unsynchronised RTC is not a reference for the delta check */
void test_delta_check_is_skipped_while_timeprovider_is_unsynchronised(void)
{
    sync_once_ok();
    s_tp_now.sync_state = TIME_SYNC_UNSYNCHRONISED;
    s_tp_now.epoch = 100U; /* uptime seconds, not a wall-clock time */

    TEST_ASSERT_EQUAL(TIME_SERVICE_OK, time_service_sync_for_test());
}

/* TC-TS-022 (TS-D12): an unreadable RTC is not a reference either */
void test_delta_check_is_skipped_when_the_rtc_cannot_be_read(void)
{
    sync_once_ok();
    s_tp_get_result = TIME_PROVIDER_ERR_RTC_FAIL;
    s_ntp_epoch = BASE_UTC + 100000U;

    TEST_ASSERT_EQUAL(TIME_SERVICE_OK, time_service_sync_for_test());
}

/* ======================================================================= */
/* Field Device push                                                        */
/* ======================================================================= */

/* TC-TS-011 */
void test_fd_push_rejected_by_the_poller_is_not_fatal(void)
{
    init_service();
    s_mp_result = MODBUS_POLLER_ERR_QUEUE_FULL;

    TEST_ASSERT_EQUAL(TIME_SERVICE_OK, time_service_sync_for_test());

    TEST_ASSERT_EQUAL(TIME_SERVICE_SYNC_OK, state_now());
}

/* TC-TS-012 */
void test_fd_push_response_timeout_is_not_fatal(void)
{
    init_service();
    g_mock_xQueueReceive_return = pdFALSE;

    TEST_ASSERT_EQUAL(TIME_SERVICE_OK, time_service_sync_for_test());

    TEST_ASSERT_EQUAL_UINT32(1U, s_mp_calls);
    TEST_ASSERT_EQUAL(TIME_SERVICE_SYNC_OK, state_now());
}

/* TC-TS-013 */
void test_fd_push_modbus_error_in_the_response_is_not_fatal(void)
{
    init_service();
    queue_fd_reply(MODBUS_MASTER_ERR_TIMEOUT);

    TEST_ASSERT_EQUAL(TIME_SERVICE_OK, time_service_sync_for_test());

    TEST_ASSERT_EQUAL(TIME_SERVICE_SYNC_OK, state_now());
}

/* TC-TS-014 */
void test_fd_push_encodes_utc_as_high_word_then_low_word(void)
{
    init_service();
    s_ntp_epoch = 0x12345678UL;

    TEST_ASSERT_EQUAL(TIME_SERVICE_OK, time_service_sync_for_test());

    TEST_ASSERT_EQUAL_UINT16(0x1234U, s_mp_cmd.data[0]);
    TEST_ASSERT_EQUAL_UINT16(0x5678U, s_mp_cmd.data[1]);
}

/* TC-TS-023: the command targets the CMD_SET_TIME block with a single FC16 write */
void test_fd_push_command_shape(void)
{
    init_service();

    (void) time_service_sync_for_test();

    TEST_ASSERT_EQUAL_UINT8(0x10U, s_mp_cmd.function_code);
    TEST_ASSERT_EQUAL_UINT16(CMD_SET_TIME_HI_REG, s_mp_cmd.start_reg);
    TEST_ASSERT_EQUAL_UINT16(2U, s_mp_cmd.reg_count);
    TEST_ASSERT_NOT_NULL(s_mp_cmd.response_queue);
    TEST_ASSERT_NOT_EQUAL(0U, s_mp_cmd.slave_addr);
}

/* ======================================================================= */
/* Task behaviour                                                           */
/* ======================================================================= */

/* TC-TS-015 */
void test_trigger_retry_posts_the_retry_bit(void)
{
    init_service();
    const itime_service_t *ts = time_service_get_interface();

    ts->trigger_retry(ts->ctx);

    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTaskNotify_call_count);
    TEST_ASSERT_EQUAL_UINT32(NOTIFY_RETRY, g_mock_xTaskNotify_last_value);
}

/* TC-TS-024 */
void test_boot_syncs_then_arms_and_starts_the_timer(void)
{
    init_service();

    time_service_boot_for_test();

    TEST_ASSERT_EQUAL_UINT32(1U, s_ntp_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTimerChangePeriod_call_count);
    TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(TIME_SERVICE_NTP_INTERVAL_S_DEFAULT * 1000UL),
                             g_mock_xTimerChangePeriod_last_period);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTimerStart_call_count);
}

/* TS-O4 / WP12 D-8: servers and interval come from the configuration */
void test_configured_servers_and_interval_are_used(void)
{
    config_params_t cfg;

    (void) memset(&cfg, 0, sizeof(cfg));
    (void) strcpy(cfg.ntp_servers[0], "time.example.org");
    (void) strcpy(cfg.ntp_servers[1], "10.1.2.3");
    cfg.ntp_server_count = 2U;
    cfg.ntp_interval_min = 15U;

    time_service_reconfigure(&cfg);
    init_service();
    time_service_boot_for_test();

    TEST_ASSERT_EQUAL_UINT8(2U, s_ntp_count_seen);
    TEST_ASSERT_EQUAL_STRING("time.example.org", s_ntp_first_seen);
    TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(15UL * 60UL * 1000UL),
                             g_mock_xTimerChangePeriod_last_period);
}

void test_zero_configured_servers_use_the_builtin_pair(void)
{
    config_params_t cfg;

    (void) memset(&cfg, 0, sizeof(cfg));
    cfg.ntp_interval_min = 60U;
    time_service_reconfigure(&cfg);
    init_service();
    time_service_boot_for_test();

    TEST_ASSERT_EQUAL_UINT8(2U, s_ntp_count_seen);
    TEST_ASSERT_EQUAL_STRING("162.159.200.1", s_ntp_first_seen);
}

/* TC-TS-025 */
void test_periodic_and_retry_notifications_each_trigger_a_sync(void)
{
    init_service();

    time_service_notify_for_test(NOTIFY_PERIODIC);
    time_service_notify_for_test(NOTIFY_RETRY);
    time_service_notify_for_test(0U);

    TEST_ASSERT_EQUAL_UINT32(2U, s_ntp_calls);
}

/* TC-TS-017 */
void test_periodic_tick_rereads_the_interval_but_a_retry_does_not_touch_the_timer(void)
{
    init_service();
    time_service_set_interval_for_test(7200U);

    time_service_notify_for_test(NOTIFY_RETRY);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xTimerChangePeriod_call_count);

    time_service_notify_for_test(NOTIFY_PERIODIC);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTimerChangePeriod_call_count);
    TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(7200UL * 1000UL), g_mock_xTimerChangePeriod_last_period);
}

/* TC-TS-026 */
void test_timer_callback_notifies_the_task_with_the_periodic_bit(void)
{
    init_service();
    TEST_ASSERT_NOT_NULL(g_mock_xTimerCreateStatic_last_cb);

    g_mock_xTimerCreateStatic_last_cb(NULL);

    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTaskNotify_call_count);
    TEST_ASSERT_EQUAL_UINT32(NOTIFY_PERIODIC, g_mock_xTaskNotify_last_value);
}

/* TC-TS-019 */
void test_reset_returns_the_module_to_its_uninitialised_state(void)
{
    sync_once_ok();

    time_service_reset_for_test();

    TEST_ASSERT_NULL(time_service_get_interface());
}
