/**
 * @file test_modbus_poller.c
 * @brief Unity tests for ModbusPoller (Gateway).
 *
 * ModbusMaster, DeviceProfileRegistry and HealthMonitor are replaced by spies
 * defined in this file; the test never includes their headers directly, so
 * Ceedling links none of their .c files.  FreeRTOS comes from freertos_mock.
 *
 * The spy for modbus_master_transact() consumes one scripted step per call and
 * records every request, which is how the tests see command-before-poll order
 * and the exact wire frames.  Test IDs follow companion §12 (TC-MP-001..018);
 * TC-MP-019.. are additions.
 */

#include <setjmp.h>
#include <string.h>

#include "device_profile_registry_stub.h"
#include "freertos_mock.h"
#include "modbus_poller.h"
#include "unity.h"

#define MAX_STEPS (16U)
#define MAX_PROFILES (6U)
#define HEALTH_HANDLE ((health_monitor_handle_t) 0x1234)
#define SLAVE_A (1U)
#define SLAVE_B (2U)
#define POLL_PERIOD_MS (1000U)
#define MOCK_ITEM_CAPTURE (256U)

/* ===================================================================== */
/* ModbusMaster spy                                                      */
/* ===================================================================== */

typedef struct
{
    modbus_master_err_t result;
    uint8_t payload[8];
    uint16_t payload_len;
} step_t;

static step_t s_steps[MAX_STEPS];
static uint32_t s_step_count;
static uint32_t s_transact_calls;
static modbus_frame_t s_requests[MAX_STEPS];
static modbus_master_err_t s_default_result;

static uint32_t s_start_calls;
static modbus_master_err_t s_start_result;

static modbus_master_stats_t s_stats_value;
static modbus_master_err_t s_stats_result;
static uint32_t s_stats_calls;

static jmp_buf s_escape;
static uint32_t s_escape_after; /* longjmp out of run() on this transact call; 0 = never */

modbus_master_err_t modbus_master_start(void)
{
    s_start_calls++;
    return s_start_result;
}

modbus_master_err_t modbus_master_transact(const modbus_frame_t *request, modbus_frame_t *response)
{
    if (s_transact_calls < MAX_STEPS)
    {
        s_requests[s_transact_calls] = *request;
    }

    const uint32_t call = s_transact_calls;

    s_transact_calls++;
    if ((s_escape_after != 0U) && (s_transact_calls == s_escape_after))
    {
        longjmp(s_escape, 1);
    }

    modbus_master_err_t result = s_default_result;

    (void) memset(response, 0, sizeof(*response));
    if (call < s_step_count)
    {
        result = s_steps[call].result;
        response->slave_addr = request->slave_addr;
        response->function_code = request->function_code;
        (void) memcpy(response->data, s_steps[call].payload, s_steps[call].payload_len);
        response->data_len = s_steps[call].payload_len;
    }
    return result;
}

modbus_master_err_t modbus_master_get_stats(modbus_master_stats_t *stats_out)
{
    s_stats_calls++;
    if (s_stats_result == MODBUS_MASTER_ERR_OK)
    {
        *stats_out = s_stats_value;
    }
    return s_stats_result;
}

/* ===================================================================== */
/* DeviceProfileRegistry spy                                             */
/* ===================================================================== */

static device_profile_t s_profiles[MAX_PROFILES];
static uint8_t s_profile_count;

bool device_profile_get_next(const device_profile_t **profile, uint8_t *index)
{
    if (*index >= s_profile_count)
    {
        return false;
    }
    *profile = &s_profiles[*index];
    (*index)++;
    return true;
}

bool device_profile_is_registered(uint8_t slave_addr)
{
    for (uint8_t i = 0U; i < s_profile_count; i++)
    {
        if (s_profiles[i].slave_addr == slave_addr)
        {
            return true;
        }
    }
    return false;
}

/* ===================================================================== */
/* HealthMonitor spy                                                     */
/* ===================================================================== */

static uint32_t s_event_count;
static health_event_t s_event_kind[MAX_STEPS];
static uint32_t s_event_param[MAX_STEPS];
static health_monitor_handle_t s_event_handle;

static uint32_t s_update_calls;
static modbus_master_stats_t s_update_stats;
static health_monitor_handle_t s_update_handle;

health_monitor_err_t health_monitor_push_event(health_monitor_handle_t handle, health_event_t event,
                                               uint32_t param)
{
    if (s_event_count < MAX_STEPS)
    {
        s_event_kind[s_event_count] = event;
        s_event_param[s_event_count] = param;
    }
    s_event_count++;
    s_event_handle = handle;
    return HEALTH_MONITOR_ERR_OK;
}

health_monitor_err_t health_monitor_update_modbus_master_stats(health_monitor_handle_t handle,
                                                               const modbus_master_stats_t *stats)
{
    s_update_calls++;
    s_update_handle = handle;
    s_update_stats = *stats;
    return HEALTH_MONITOR_ERR_OK;
}

/* ===================================================================== */
/* Helpers                                                               */
/* ===================================================================== */

static StaticQueue_t s_resp_storage;
static uint8_t s_resp_buf[sizeof(modbus_command_response_t)];
static QueueHandle_t s_resp_queue;

void setUp(void)
{
    mock_freertos_reset();
    g_mock_xQueueReceive_available = 0U; /* empty command queue unless a test feeds one */
    modbus_poller_reset_for_test();

    (void) memset(s_steps, 0, sizeof(s_steps));
    (void) memset(s_requests, 0, sizeof(s_requests));
    (void) memset(s_profiles, 0, sizeof(s_profiles));
    s_step_count = 0U;
    s_transact_calls = 0U;
    s_default_result = MODBUS_MASTER_ERR_OK;
    s_start_calls = 0U;
    s_start_result = MODBUS_MASTER_ERR_OK;
    (void) memset(&s_stats_value, 0, sizeof(s_stats_value));
    s_stats_result = MODBUS_MASTER_ERR_OK;
    s_stats_calls = 0U;
    s_escape_after = 0U;
    s_event_count = 0U;
    s_event_handle = NULL;
    s_update_calls = 0U;
    s_update_handle = NULL;
    (void) memset(&s_update_stats, 0, sizeof(s_update_stats));

    s_profiles[0] = (device_profile_t){
        .slave_addr = SLAVE_A, .input_reg_start = 0x0010U, .input_reg_count = 3U};
    s_profile_count = 1U;
}

void tearDown(void)
{
}

static void init_ok(void)
{
    TEST_ASSERT_EQUAL(MODBUS_POLLER_ERR_OK, modbus_poller_init(POLL_PERIOD_MS, HEALTH_HANDLE));
    /* The response queue is created after the command queue, so the mock calls it "queue 2". */
    s_resp_queue =
        xQueueCreateStatic(1U, sizeof(modbus_command_response_t), s_resp_buf, &s_resp_storage);
    g_mock_xQueueSend2_last_item_size = sizeof(modbus_command_response_t);
}

/** Run `n` poll cycles with the scripted results `results[0..n-1]` (one transact each). */
static void cycles_with(const modbus_master_err_t *results, uint32_t n)
{
    for (uint32_t i = 0U; i < n; i++)
    {
        s_steps[0].result = results[i];
        s_step_count = 1U;
        s_transact_calls = 0U;
        modbus_poller_cycle_for_test();
    }
}

static void poll_n(modbus_master_err_t result, uint32_t n)
{
    for (uint32_t i = 0U; i < n; i++)
    {
        cycles_with(&result, 1U);
    }
}

static const modbus_slave_state_t *slot0(void)
{
    return modbus_poller_slave_state_for_test(0U);
}

/** Queue a single command for the next cycle's drain. */
static void feed_command(const modbus_command_t *cmd)
{
    g_mock_xQueueReceive_return = pdTRUE;
    g_mock_xQueueReceive_available = 1U;
    g_mock_xQueueReceive_next_item_size = MOCK_ITEM_CAPTURE;
    (void) memcpy(g_mock_xQueueReceive_next_item, cmd, MOCK_ITEM_CAPTURE);
}

static modbus_command_t fc06_command(void)
{
    modbus_command_t cmd = {0};

    cmd.response_queue = s_resp_queue;
    cmd.slave_addr = SLAVE_A;
    cmd.function_code = MODBUS_POLLER_FC_WRITE_SINGLE;
    cmd.start_reg = 0x0100U;
    cmd.reg_count = 1U;
    cmd.data[0] = 0xABCDU;
    return cmd;
}

static modbus_command_response_t sent_response(void)
{
    modbus_command_response_t r;

    (void) memcpy(&r, g_mock_xQueueSend2_last_item, sizeof(r));
    return r;
}

/* ===================================================================== */
/* init                                                                  */
/* ===================================================================== */

void test_TC_MP_001_init_creates_queue_and_returns_ok(void)
{
    TEST_ASSERT_EQUAL(MODBUS_POLLER_ERR_OK, modbus_poller_init(POLL_PERIOD_MS, HEALTH_HANDLE));
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xQueueCreateStatic_call_count);
    /* Initialised: a valid command is accepted rather than NOT_INIT. */
    modbus_command_t cmd = fc06_command();
    TEST_ASSERT_EQUAL(MODBUS_POLLER_ERR_OK, modbus_poller_send_command(&cmd));
}

void test_TC_MP_019_init_null_health_is_rejected(void)
{
    TEST_ASSERT_EQUAL(MODBUS_POLLER_ERR_NULL_ARG, modbus_poller_init(POLL_PERIOD_MS, NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueCreateStatic_call_count);
}

void test_TC_MP_020_init_zero_period_is_rejected(void)
{
    TEST_ASSERT_EQUAL(MODBUS_POLLER_ERR_INVALID_ARG, modbus_poller_init(0U, HEALTH_HANDLE));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueCreateStatic_call_count);
}

void test_TC_MP_028_reset_for_test_deletes_queue_and_clears_state(void)
{
    init_ok();
    poll_n(MODBUS_MASTER_ERR_OK, 3U);
    TEST_ASSERT_TRUE(slot0()->link_online);

    modbus_poller_reset_for_test();

    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_vQueueDelete_call_count);
    TEST_ASSERT_EQUAL_UINT8(0U, slot0()->slave_addr);
    TEST_ASSERT_FALSE(slot0()->link_online);
    modbus_command_t cmd = fc06_command();
    TEST_ASSERT_EQUAL(MODBUS_POLLER_ERR_NOT_INIT, modbus_poller_send_command(&cmd));
}

/* ===================================================================== */
/* link-state hysteresis                                                 */
/* ===================================================================== */

void test_TC_MP_002_two_successes_from_offline_emit_nothing(void)
{
    init_ok();
    poll_n(MODBUS_MASTER_ERR_OK, 2U);

    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
    TEST_ASSERT_EQUAL_UINT8(2U, slot0()->consecutive_successes);
    TEST_ASSERT_FALSE(slot0()->link_online);
}

void test_TC_MP_003_third_success_emits_link_up_once(void)
{
    init_ok();
    poll_n(MODBUS_MASTER_ERR_OK, 3U);

    TEST_ASSERT_EQUAL_UINT32(1U, s_event_count);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_MODBUS_LINK_UP, s_event_kind[0]);
    TEST_ASSERT_EQUAL_UINT32(SLAVE_A, s_event_param[0]);
    TEST_ASSERT_EQUAL_PTR(HEALTH_HANDLE, s_event_handle);
    TEST_ASSERT_TRUE(slot0()->link_online);
    TEST_ASSERT_EQUAL_UINT8(SLAVE_A, slot0()->slave_addr);
}

void test_TC_MP_004_third_failure_from_online_emits_node_offline_once(void)
{
    init_ok();
    poll_n(MODBUS_MASTER_ERR_OK, 3U);
    s_event_count = 0U;

    poll_n(MODBUS_MASTER_ERR_TIMEOUT, 3U);

    TEST_ASSERT_EQUAL_UINT32(1U, s_event_count);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_MODBUS_LINK_DOWN, s_event_kind[0]);
    TEST_ASSERT_EQUAL_UINT32(SLAVE_A, s_event_param[0]);
    TEST_ASSERT_FALSE(slot0()->link_online);
}

void test_TC_MP_005_success_after_two_failures_keeps_link_online(void)
{
    init_ok();
    poll_n(MODBUS_MASTER_ERR_OK, 3U);
    s_event_count = 0U;
    poll_n(MODBUS_MASTER_ERR_TIMEOUT, 2U);

    poll_n(MODBUS_MASTER_ERR_OK, 1U);

    TEST_ASSERT_EQUAL_UINT8(0U, slot0()->consecutive_failures);
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
    TEST_ASSERT_TRUE(slot0()->link_online);
}

void test_TC_MP_006_failure_after_two_successes_keeps_link_offline(void)
{
    init_ok();
    poll_n(MODBUS_MASTER_ERR_OK, 2U);

    poll_n(MODBUS_MASTER_ERR_CRC, 1U);

    TEST_ASSERT_EQUAL_UINT8(0U, slot0()->consecutive_successes);
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
    TEST_ASSERT_FALSE(slot0()->link_online);
}

void test_TC_MP_012_exception_response_counts_as_failure(void)
{
    init_ok();
    poll_n(MODBUS_MASTER_ERR_OK, 2U);

    poll_n(MODBUS_MASTER_ERR_EXCEPTION, 1U);

    TEST_ASSERT_EQUAL_UINT8(1U, slot0()->consecutive_failures);
    TEST_ASSERT_EQUAL_UINT8(0U, slot0()->consecutive_successes);
}

void test_TC_MP_015_fourth_failure_emits_no_duplicate_and_caps_at_three(void)
{
    init_ok();
    poll_n(MODBUS_MASTER_ERR_OK, 3U);
    s_event_count = 0U;

    poll_n(MODBUS_MASTER_ERR_TIMEOUT, 4U);

    TEST_ASSERT_EQUAL_UINT32(1U, s_event_count);
    TEST_ASSERT_EQUAL_UINT8(3U, slot0()->consecutive_failures);
}

void test_TC_MP_016_fourth_success_emits_no_duplicate_and_caps_at_three(void)
{
    init_ok();

    poll_n(MODBUS_MASTER_ERR_OK, 4U);

    TEST_ASSERT_EQUAL_UINT32(1U, s_event_count);
    TEST_ASSERT_EQUAL_UINT8(3U, slot0()->consecutive_successes);
}

void test_TC_MP_021_each_non_ok_code_counts_as_failure(void)
{
    const modbus_master_err_t codes[] = {MODBUS_MASTER_ERR_TIMEOUT, MODBUS_MASTER_ERR_CRC,
                                         MODBUS_MASTER_ERR_BAD_RESPONSE,
                                         MODBUS_MASTER_ERR_EXCEPTION, MODBUS_MASTER_ERR_RX_ERROR};

    init_ok();
    poll_n(MODBUS_MASTER_ERR_OK, 3U);
    s_event_count = 0U;

    cycles_with(codes, 5U);

    TEST_ASSERT_EQUAL_UINT32(1U, s_event_count);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_MODBUS_LINK_DOWN, s_event_kind[0]);
}

/* ===================================================================== */
/* polling                                                               */
/* ===================================================================== */

void test_TC_MP_022_poll_request_is_fc04_over_the_profile_span(void)
{
    init_ok();
    poll_n(MODBUS_MASTER_ERR_OK, 1U);

    TEST_ASSERT_EQUAL_UINT8(SLAVE_A, s_requests[0].slave_addr);
    TEST_ASSERT_EQUAL_UINT8(0x04U, s_requests[0].function_code);
    TEST_ASSERT_EQUAL_UINT16(4U, s_requests[0].data_len);
    TEST_ASSERT_EQUAL_UINT8(0x00U, s_requests[0].data[0]);
    TEST_ASSERT_EQUAL_UINT8(0x10U, s_requests[0].data[1]);
    TEST_ASSERT_EQUAL_UINT8(0x00U, s_requests[0].data[2]);
    TEST_ASSERT_EQUAL_UINT8(0x03U, s_requests[0].data[3]);
}

void test_TC_MP_023_two_slaves_keep_independent_link_state(void)
{
    s_profiles[1] =
        (device_profile_t){.slave_addr = SLAVE_B, .input_reg_start = 0U, .input_reg_count = 1U};
    s_profile_count = 2U;
    init_ok();

    for (uint32_t i = 0U; i < 3U; i++)
    {
        s_steps[0].result = MODBUS_MASTER_ERR_OK;
        s_steps[1].result = MODBUS_MASTER_ERR_TIMEOUT;
        s_step_count = 2U;
        s_transact_calls = 0U;
        modbus_poller_cycle_for_test();
    }

    TEST_ASSERT_EQUAL_UINT8(SLAVE_A, modbus_poller_slave_state_for_test(0U)->slave_addr);
    TEST_ASSERT_EQUAL_UINT8(SLAVE_B, modbus_poller_slave_state_for_test(1U)->slave_addr);
    TEST_ASSERT_TRUE(modbus_poller_slave_state_for_test(0U)->link_online);
    TEST_ASSERT_FALSE(modbus_poller_slave_state_for_test(1U)->link_online);
    TEST_ASSERT_EQUAL_UINT32(1U, s_event_count);
    TEST_ASSERT_EQUAL_UINT32(SLAVE_A, s_event_param[0]);
}

void test_TC_MP_024_slave_beyond_table_size_is_skipped(void)
{
    for (uint8_t i = 0U; i < MAX_PROFILES; i++)
    {
        s_profiles[i] = (device_profile_t){
            .slave_addr = (uint8_t) (i + 1U), .input_reg_start = 0U, .input_reg_count = 1U};
    }
    s_profile_count = 5U; /* table holds 4 */
    init_ok();

    modbus_poller_cycle_for_test();

    TEST_ASSERT_EQUAL_UINT32(4U, s_transact_calls);
    TEST_ASSERT_NULL(modbus_poller_slave_state_for_test(4U));
}

void test_TC_MP_025_empty_registry_polls_nothing_but_still_reports(void)
{
    s_profile_count = 0U;
    init_ok();

    modbus_poller_cycle_for_test();

    TEST_ASSERT_EQUAL_UINT32(0U, s_transact_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_update_calls);
}

/* ===================================================================== */
/* health                                                                */
/* ===================================================================== */

void test_TC_MP_011_cycle_forwards_master_stats_to_health_monitor(void)
{
    s_stats_value = (modbus_master_stats_t){.transactions_sent = 9U,
                                            .transactions_ok = 7U,
                                            .timeouts = 2U,
                                            .crc_errors = 1U,
                                            .rx_errors = 3U,
                                            .exception_responses = 4U,
                                            .bad_responses = 5U};
    init_ok();

    modbus_poller_cycle_for_test();

    TEST_ASSERT_EQUAL_UINT32(1U, s_stats_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_update_calls);
    TEST_ASSERT_EQUAL_PTR(HEALTH_HANDLE, s_update_handle);
    TEST_ASSERT_EQUAL_MEMORY(&s_stats_value, &s_update_stats, sizeof(s_stats_value));
}

void test_TC_MP_026_stats_read_failure_skips_the_update(void)
{
    s_stats_result = MODBUS_MASTER_ERR_NOT_INIT;
    init_ok();

    modbus_poller_cycle_for_test();

    TEST_ASSERT_EQUAL_UINT32(1U, s_stats_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_update_calls);
}

/* ===================================================================== */
/* send_command                                                          */
/* ===================================================================== */

void test_TC_MP_008_unregistered_slave_is_rejected_and_never_transacted(void)
{
    init_ok();
    modbus_command_t cmd = fc06_command();

    cmd.slave_addr = 99U;

    TEST_ASSERT_EQUAL(MODBUS_POLLER_ERR_REJECTED, modbus_poller_send_command(&cmd));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend_call_count);
    TEST_ASSERT_EQUAL_UINT32(0U, s_transact_calls);
}

void test_TC_MP_009_full_queue_returns_queue_full(void)
{
    init_ok();
    modbus_command_t cmd = fc06_command();

    g_mock_xQueueSend_return = pdFALSE;

    TEST_ASSERT_EQUAL(MODBUS_POLLER_ERR_QUEUE_FULL, modbus_poller_send_command(&cmd));
}

void test_TC_MP_010_command_queued_before_run_is_not_executed(void)
{
    init_ok();
    modbus_command_t cmd = fc06_command();

    TEST_ASSERT_EQUAL(MODBUS_POLLER_ERR_OK, modbus_poller_send_command(&cmd));
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xQueueSend_call_count);
    TEST_ASSERT_EQUAL_UINT32(0U, s_transact_calls);
}

void test_TC_MP_013_send_command_before_init_returns_not_init(void)
{
    modbus_command_t cmd = fc06_command();

    TEST_ASSERT_EQUAL(MODBUS_POLLER_ERR_NOT_INIT, modbus_poller_send_command(&cmd));
}

void test_TC_MP_014_send_command_null_returns_null_arg(void)
{
    init_ok();

    TEST_ASSERT_EQUAL(MODBUS_POLLER_ERR_NULL_ARG, modbus_poller_send_command(NULL));
}

void test_TC_MP_029_unsupported_function_code_is_invalid(void)
{
    init_ok();
    modbus_command_t cmd = fc06_command();

    cmd.function_code = 0x03U;

    TEST_ASSERT_EQUAL(MODBUS_POLLER_ERR_INVALID_ARG, modbus_poller_send_command(&cmd));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend_call_count);
}

void test_TC_MP_030_fc16_register_count_bounds_are_enforced(void)
{
    init_ok();
    modbus_command_t cmd = fc06_command();

    cmd.function_code = MODBUS_POLLER_FC_WRITE_MULTIPLE;

    cmd.reg_count = 0U;
    TEST_ASSERT_EQUAL(MODBUS_POLLER_ERR_INVALID_ARG, modbus_poller_send_command(&cmd));
    cmd.reg_count = MODBUS_POLLER_FC16_MAX_COUNT + 1U;
    TEST_ASSERT_EQUAL(MODBUS_POLLER_ERR_INVALID_ARG, modbus_poller_send_command(&cmd));
    cmd.reg_count = MODBUS_POLLER_FC16_MAX_COUNT;
    TEST_ASSERT_EQUAL(MODBUS_POLLER_ERR_OK, modbus_poller_send_command(&cmd));
}

/* ===================================================================== */
/* command execution                                                     */
/* ===================================================================== */

void test_TC_MP_007_command_runs_before_the_poll_and_result_is_posted(void)
{
    init_ok();
    modbus_command_t cmd = fc06_command();

    /* Write response echoes register and value: 0x0100, 0xABCD. */
    s_steps[0] = (step_t){
        .result = MODBUS_MASTER_ERR_OK, .payload = {0x01U, 0x00U, 0xABU, 0xCDU}, .payload_len = 4U};
    s_step_count = 1U;
    feed_command(&cmd);

    modbus_poller_cycle_for_test();

    TEST_ASSERT_EQUAL_UINT32(2U, s_transact_calls);
    TEST_ASSERT_EQUAL_UINT8(MODBUS_POLLER_FC_WRITE_SINGLE, s_requests[0].function_code);
    TEST_ASSERT_EQUAL_UINT8(0x04U, s_requests[1].function_code);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xQueueSend2_call_count);

    const modbus_command_response_t r = sent_response();

    TEST_ASSERT_EQUAL(MODBUS_MASTER_ERR_OK, r.status);
    TEST_ASSERT_EQUAL_UINT16(2U, r.data_len);
    TEST_ASSERT_EQUAL_HEX16(0x0100U, r.data[0]);
    TEST_ASSERT_EQUAL_HEX16(0xABCDU, r.data[1]);
}

void test_TC_MP_017_fc06_frame_is_built_correctly(void)
{
    init_ok();
    modbus_command_t cmd = fc06_command();

    feed_command(&cmd);
    modbus_poller_cycle_for_test();

    TEST_ASSERT_EQUAL_UINT8(SLAVE_A, s_requests[0].slave_addr);
    TEST_ASSERT_EQUAL_UINT8(0x06U, s_requests[0].function_code);
    TEST_ASSERT_EQUAL_UINT16(4U, s_requests[0].data_len);
    TEST_ASSERT_EQUAL_HEX8(0x01U, s_requests[0].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_requests[0].data[1]);
    TEST_ASSERT_EQUAL_HEX8(0xABU, s_requests[0].data[2]);
    TEST_ASSERT_EQUAL_HEX8(0xCDU, s_requests[0].data[3]);
}

void test_TC_MP_018_fc16_frame_is_built_correctly(void)
{
    init_ok();
    modbus_command_t cmd = fc06_command();

    cmd.function_code = MODBUS_POLLER_FC_WRITE_MULTIPLE;
    cmd.start_reg = 0x0110U;
    cmd.reg_count = 3U;
    cmd.data[0] = 0x1122U;
    cmd.data[1] = 0x3344U;
    cmd.data[2] = 0x5566U;
    feed_command(&cmd);

    modbus_poller_cycle_for_test();

    const uint8_t expect[] = {0x01U, 0x10U, 0x00U, 0x03U, 0x06U, 0x11U,
                              0x22U, 0x33U, 0x44U, 0x55U, 0x66U};

    TEST_ASSERT_EQUAL_UINT8(0x10U, s_requests[0].function_code);
    TEST_ASSERT_EQUAL_UINT16(sizeof(expect), s_requests[0].data_len);
    TEST_ASSERT_EQUAL_MEMORY(expect, s_requests[0].data, sizeof(expect));
}

void test_TC_MP_031_failed_command_reports_status_and_no_data(void)
{
    init_ok();
    modbus_command_t cmd = fc06_command();

    s_steps[0].result = MODBUS_MASTER_ERR_TIMEOUT;
    s_step_count = 1U;
    feed_command(&cmd);

    modbus_poller_cycle_for_test();

    const modbus_command_response_t r = sent_response();

    TEST_ASSERT_EQUAL(MODBUS_MASTER_ERR_TIMEOUT, r.status);
    TEST_ASSERT_EQUAL_UINT16(0U, r.data_len);
}

void test_TC_MP_032_command_without_response_queue_does_not_post(void)
{
    init_ok();
    modbus_command_t cmd = fc06_command();

    cmd.response_queue = NULL;
    feed_command(&cmd);

    modbus_poller_cycle_for_test();

    TEST_ASSERT_EQUAL_UINT32(2U, s_transact_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend2_call_count);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend_call_count);
}

void test_TC_MP_037_corrupt_queued_command_is_answered_without_a_transaction(void)
{
    init_ok();
    modbus_command_t cmd = fc06_command();

    cmd.function_code = MODBUS_POLLER_FC_WRITE_MULTIPLE;
    cmd.reg_count = MODBUS_POLLER_FC16_MAX_COUNT + 1U;
    feed_command(&cmd);

    modbus_poller_cycle_for_test();

    TEST_ASSERT_EQUAL_UINT32(1U, s_transact_calls); /* only the poll */
    TEST_ASSERT_EQUAL(MODBUS_MASTER_ERR_INVALID_ARG, sent_response().status);
}

void test_TC_MP_033_command_does_not_touch_link_state(void)
{
    init_ok();
    modbus_command_t cmd = fc06_command();

    s_steps[0].result = MODBUS_MASTER_ERR_TIMEOUT;
    s_step_count = 1U;
    feed_command(&cmd);

    modbus_poller_cycle_for_test();

    /* The command failed; the poll that followed succeeded and is the only counted result. */
    TEST_ASSERT_EQUAL_UINT8(1U, slot0()->consecutive_successes);
    TEST_ASSERT_EQUAL_UINT8(0U, slot0()->consecutive_failures);
}

void test_TC_MP_034_response_words_are_decoded_big_endian(void)
{
    init_ok();
    modbus_command_t cmd = fc06_command();

    s_steps[0] = (step_t){.result = MODBUS_MASTER_ERR_OK,
                          .payload = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U},
                          .payload_len = 8U};
    s_step_count = 1U;
    feed_command(&cmd);

    modbus_poller_cycle_for_test();

    const modbus_command_response_t r = sent_response();

    TEST_ASSERT_EQUAL_UINT16(4U, r.data_len);
    TEST_ASSERT_EQUAL_HEX16(0x0708U, r.data[3]);
}

/* ===================================================================== */
/* run                                                                   */
/* ===================================================================== */

void test_TC_MP_035_run_starts_master_polls_and_delays_by_the_period(void)
{
    init_ok();
    s_escape_after = 2U; /* leave on the second cycle's poll */

    if (setjmp(s_escape) == 0)
    {
        modbus_poller_run();
        TEST_FAIL_MESSAGE("modbus_poller_run() returned");
    }

    TEST_ASSERT_EQUAL_UINT32(1U, s_start_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_vTaskDelayUntil_call_count);
    TEST_ASSERT_EQUAL_UINT32(POLL_PERIOD_MS, g_mock_vTaskDelayUntil_last_increment);
    TEST_ASSERT_EQUAL_UINT32(1U, s_update_calls);
}

void test_TC_MP_036_slave_state_accessor_rejects_out_of_range_slot(void)
{
    TEST_ASSERT_NULL(modbus_poller_slave_state_for_test(4U));
}
