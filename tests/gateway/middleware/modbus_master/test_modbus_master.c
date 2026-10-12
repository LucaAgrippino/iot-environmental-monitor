/**
 * @file test_modbus_master.c
 * @brief Unity tests for ModbusMaster (Gateway).
 *
 * The UART driver is replaced by inline stubs (modbus_uart_driver_stub.h keeps
 * the real driver .c out of the link).  Each transmit() consumes one scripted
 * step that decides how that attempt ends: TX failure, notification timeout,
 * or a response frame delivered through the (mock) notification.
 *
 * Test IDs follow companion §13 (TC-MM-001..017) plus TC-MM-018.. additions.
 */

#include <string.h>

#include "freertos_mock.h"
#include "modbus_master.h"
#include "modbus_uart_driver_stub.h"
#include "unity.h"

#define NOTIFY_RX_DONE (1UL << 0)
#define NOTIFY_RX_ERROR (1UL << 1)
#define MAX_STEPS (8U)

/* ===================================================================== */
/* Independent reference CRC (table-free, MSB-reflected form)            */
/* ===================================================================== */

static uint16_t ref_crc16(const uint8_t *d, uint16_t n)
{
    uint16_t crc = 0xFFFFU;

    for (uint16_t i = 0U; i < n; i++)
    {
        crc = (uint16_t) (crc ^ d[i]);
        for (uint8_t b = 0U; b < 8U; b++)
        {
            if ((crc & 1U) != 0U)
            {
                crc = (uint16_t) ((crc >> 1U) ^ 0xA001U);
            }
            else
            {
                crc = (uint16_t) (crc >> 1U);
            }
        }
    }
    return crc;
}

/** Build a response frame (addr, fc, payload, CRC) and return its length. */
static uint16_t make_frame(uint8_t *out, uint8_t addr, uint8_t fc, const uint8_t *payload,
                           uint16_t n)
{
    uint16_t len = 0U;

    out[len++] = addr;
    out[len++] = fc;
    if (n > 0U)
    {
        memcpy(&out[len], payload, n);
        len = (uint16_t) (len + n);
    }
    const uint16_t crc = ref_crc16(out, len);

    out[len++] = (uint8_t) (crc & 0xFFU);
    out[len++] = (uint8_t) (crc >> 8U);
    return len;
}

/* ===================================================================== */
/* UART stub                                                             */
/* ===================================================================== */

typedef struct
{
    modbus_uart_err_t tx_err;
    bool notify_ok; /* false = xTaskNotifyWait() times out */
    uint32_t bits;  /* notification bits delivered */
    uint8_t rx[MODBUS_MAX_FRAME_BYTES];
    uint16_t rx_len;
} step_t;

static step_t s_steps[MAX_STEPS];
static uint32_t s_step_count;
static uint32_t s_tx_calls;
static uint32_t s_get_rx_calls;
static uint32_t s_attach_calls;
static uint8_t s_last_wire[MODBUS_MAX_FRAME_BYTES];
static uint16_t s_last_wire_len;
static bool s_rx_armed;
static uint8_t s_armed_rx[MODBUS_MAX_FRAME_BYTES];
static uint16_t s_armed_rx_len;
static modbus_uart_rx_cb_t s_captured_cb;

void modbus_uart_attach_rx(modbus_uart_rx_cb_t callback, void *context)
{
    (void) context;
    s_attach_calls++;
    s_captured_cb = callback;
}

modbus_uart_err_t modbus_uart_transmit(const uint8_t *frame, uint16_t len)
{
    const step_t *step = &s_steps[(s_tx_calls < s_step_count) ? s_tx_calls : (s_step_count - 1U)];

    s_tx_calls++;
    memcpy(s_last_wire, frame, len);
    s_last_wire_len = len;

    g_mock_xTaskNotifyWait_return = step->notify_ok ? pdTRUE : pdFALSE;
    g_mock_xTaskNotifyWait_next_value = step->bits;
    memcpy(s_armed_rx, step->rx, step->rx_len);
    s_armed_rx_len = step->rx_len;
    s_rx_armed = true;
    return step->tx_err;
}

modbus_uart_err_t modbus_uart_get_rx_frame(uint8_t *buf, uint16_t *len)
{
    s_get_rx_calls++;
    if (s_rx_armed)
    {
        memcpy(buf, s_armed_rx, s_armed_rx_len);
        *len = s_armed_rx_len;
        s_rx_armed = false;
    }
    else
    {
        *len = 0U; /* the pre-TX drain finds nothing */
    }
    return MODBUS_UART_OK;
}

/* ===================================================================== */
/* Script helpers                                                        */
/* ===================================================================== */

static void script_reset(void)
{
    memset(s_steps, 0, sizeof(s_steps));
    s_step_count = 0U;
}

static step_t *script_add(void)
{
    step_t *step = &s_steps[s_step_count];

    s_step_count++;
    step->tx_err = MODBUS_UART_OK;
    step->notify_ok = true;
    step->bits = NOTIFY_RX_DONE;
    return step;
}

static void script_timeout(void)
{
    step_t *step = script_add();

    step->notify_ok = false;
    step->bits = 0U;
}

static void script_response(uint8_t addr, uint8_t fc, const uint8_t *payload, uint16_t n)
{
    step_t *step = script_add();

    step->rx_len = make_frame(step->rx, addr, fc, payload, n);
}

static void script_corrupt_response(uint8_t addr, uint8_t fc, const uint8_t *payload, uint16_t n)
{
    step_t *step = script_add();

    step->rx_len = make_frame(step->rx, addr, fc, payload, n);
    step->rx[step->rx_len - 1U] ^= 0xFFU;
}

/* ===================================================================== */
/* Fixtures                                                              */
/* ===================================================================== */

static const uint8_t k_read_1_reg[] = {0x00U, 0x00U, 0x00U, 0x01U};
static const uint8_t k_reply_1_reg[] = {0x02U, 0x12U, 0x34U};

static modbus_frame_t make_request(uint8_t addr, uint8_t fc, const uint8_t *data, uint16_t n)
{
    modbus_frame_t req = {.slave_addr = addr, .function_code = fc, .data_len = n};

    if (n > 0U)
    {
        memcpy(req.data, data, n);
    }
    return req;
}

static void start_master(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_init(200U, 3U));
    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_start());
}

void setUp(void)
{
    mock_freertos_reset();
    modbus_master_reset_for_test();
    script_reset();
    s_tx_calls = 0U;
    s_get_rx_calls = 0U;
    s_attach_calls = 0U;
    s_last_wire_len = 0U;
    s_rx_armed = false;
    s_armed_rx_len = 0U;
    s_captured_cb = NULL;
}

void tearDown(void)
{
}

/* ===================================================================== */
/* TC-MM-001..003: init / start ordering                                 */
/* ===================================================================== */

void test_TC_MM_001_init_then_start_registers_uart_callback_once(void)
{
    start_master();
    TEST_ASSERT_EQUAL_UINT32(1U, s_attach_calls);
    TEST_ASSERT_NOT_NULL(s_captured_cb);
}

void test_TC_MM_002_start_without_init_returns_not_init(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_NOT_INIT, modbus_master_start());
    TEST_ASSERT_EQUAL_UINT32(0U, s_attach_calls);
}

void test_TC_MM_003_transact_before_start_returns_not_init(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_init(200U, 3U));
    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_NOT_INIT, modbus_master_transact(&req, &rsp));
    TEST_ASSERT_EQUAL_UINT32(0U, s_tx_calls);
}

void test_TC_MM_018_init_rejects_zero_timeout(void)
{
    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_INVALID_ARG, modbus_master_init(0U, 3U));
    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_NOT_INIT, modbus_master_start());
}

void test_TC_MM_019_second_start_is_rejected(void)
{
    start_master();
    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_ALREADY_STARTED, modbus_master_start());
    TEST_ASSERT_EQUAL_UINT32(1U, s_attach_calls);
}

/* ===================================================================== */
/* TC-MM-004: successful round trip, wire format                         */
/* ===================================================================== */

void test_TC_MM_004_successful_fc04_round_trip(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;
    modbus_master_stats_t st;

    start_master();
    script_response(1U, 4U, k_reply_1_reg, 3U);

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_transact(&req, &rsp));
    TEST_ASSERT_EQUAL_UINT8(1U, rsp.slave_addr);
    TEST_ASSERT_EQUAL_UINT8(4U, rsp.function_code);
    TEST_ASSERT_EQUAL_UINT16(3U, rsp.data_len);
    TEST_ASSERT_EQUAL_MEMORY(k_reply_1_reg, rsp.data, 3U);

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_get_stats(&st));
    TEST_ASSERT_EQUAL_UINT32(1U, st.transactions_ok);
    TEST_ASSERT_EQUAL_UINT32(1U, st.transactions_sent);
}

void test_TC_MM_020_wire_frame_has_address_function_data_and_crc(void)
{
    /* Known Modbus vectors: 01 04 00 00 00 01 -> CRC bytes 31 CA. */
    const uint8_t expected[] = {0x01U, 0x04U, 0x00U, 0x00U, 0x00U, 0x01U, 0x31U, 0xCAU};
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;

    start_master();
    script_response(1U, 4U, k_reply_1_reg, 3U);
    (void) modbus_master_transact(&req, &rsp);

    TEST_ASSERT_EQUAL_UINT16(sizeof(expected), s_last_wire_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, s_last_wire, sizeof(expected));
}

/* ===================================================================== */
/* TC-MM-005..007: retries                                               */
/* ===================================================================== */

void test_TC_MM_005_timeout_then_success(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;
    modbus_master_stats_t st;

    start_master();
    script_timeout();
    script_response(1U, 4U, k_reply_1_reg, 3U);

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_transact(&req, &rsp));
    (void) modbus_master_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(1U, st.timeouts);
    TEST_ASSERT_EQUAL_UINT32(1U, st.transactions_ok);
    TEST_ASSERT_EQUAL_UINT32(2U, st.transactions_sent);
}

void test_TC_MM_006_three_retries_exhausted_returns_timeout(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;
    modbus_master_stats_t st;

    start_master();
    script_timeout(); /* the last step repeats for every further attempt */

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_TIMEOUT, modbus_master_transact(&req, &rsp));
    (void) modbus_master_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(4U, st.timeouts);
    TEST_ASSERT_EQUAL_UINT32(4U, st.transactions_sent);
    TEST_ASSERT_EQUAL_UINT32(0U, st.transactions_ok);
}

void test_TC_MM_007_crc_error_then_success(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;
    modbus_master_stats_t st;

    start_master();
    script_corrupt_response(1U, 4U, k_reply_1_reg, 3U);
    script_response(1U, 4U, k_reply_1_reg, 3U);

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_transact(&req, &rsp));
    (void) modbus_master_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(1U, st.crc_errors);
    TEST_ASSERT_EQUAL_UINT32(1U, st.transactions_ok);
}

void test_TC_MM_021_persistent_crc_error_returns_crc(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;
    modbus_master_stats_t st;

    start_master();
    script_corrupt_response(1U, 4U, k_reply_1_reg, 3U);

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_CRC, modbus_master_transact(&req, &rsp));
    (void) modbus_master_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(4U, st.crc_errors);
}

void test_TC_MM_022_max_retries_zero_means_single_attempt(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_init(200U, 0U));
    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_start());
    script_timeout();

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_TIMEOUT, modbus_master_transact(&req, &rsp));
    TEST_ASSERT_EQUAL_UINT32(1U, s_tx_calls);
}

/* ===================================================================== */
/* TC-MM-008..010: responses that are not retried                        */
/* ===================================================================== */

void test_TC_MM_008_exception_response_not_retried(void)
{
    const uint8_t code[] = {0x02U};
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;
    modbus_master_stats_t st;

    start_master();
    script_response(1U, 0x84U, code, 1U);

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_EXCEPTION, modbus_master_transact(&req, &rsp));
    TEST_ASSERT_EQUAL_UINT8(0x02U, rsp.data[0]);
    TEST_ASSERT_EQUAL_UINT8(0x84U, rsp.function_code);
    TEST_ASSERT_EQUAL_UINT16(1U, rsp.data_len);
    (void) modbus_master_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(1U, st.exception_responses);
    TEST_ASSERT_EQUAL_UINT32(1U, s_tx_calls);
}

void test_TC_MM_009_wrong_slave_address_not_retried(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;
    modbus_master_stats_t st;

    start_master();
    script_response(2U, 4U, k_reply_1_reg, 3U);

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_BAD_RESPONSE, modbus_master_transact(&req, &rsp));
    (void) modbus_master_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(1U, st.bad_responses);
    TEST_ASSERT_EQUAL_UINT32(1U, s_tx_calls);
}

void test_TC_MM_010_wrong_function_code_not_retried(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;
    modbus_master_stats_t st;

    start_master();
    script_response(1U, 3U, k_reply_1_reg, 3U);

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_BAD_RESPONSE, modbus_master_transact(&req, &rsp));
    (void) modbus_master_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(1U, st.bad_responses);
    TEST_ASSERT_EQUAL_UINT32(1U, s_tx_calls);
}

void test_TC_MM_023_runt_exception_frame_is_bad_response(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;

    start_master();
    script_response(1U, 0x84U, NULL, 0U); /* exception flag but no exception code */

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_BAD_RESPONSE, modbus_master_transact(&req, &rsp));
}

/* ===================================================================== */
/* TC-MM-011..012: argument checks                                       */
/* ===================================================================== */

void test_TC_MM_011_null_request(void)
{
    modbus_frame_t rsp;

    start_master();
    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_NULL_ARG, modbus_master_transact(NULL, &rsp));
}

void test_TC_MM_012_null_response(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);

    start_master();
    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_NULL_ARG, modbus_master_transact(&req, NULL));
}

void test_TC_MM_024_oversize_request_payload_rejected(void)
{
    modbus_frame_t req = make_request(1U, 16U, NULL, 0U);
    modbus_frame_t rsp;

    start_master();
    req.data_len = MODBUS_MAX_DATA_BYTES + 1U;
    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_INVALID_ARG, modbus_master_transact(&req, &rsp));
    TEST_ASSERT_EQUAL_UINT32(0U, s_tx_calls);
}

void test_TC_MM_025_maximum_request_and_response_payload(void)
{
    uint8_t payload[MODBUS_MAX_DATA_BYTES];
    modbus_frame_t req = make_request(1U, 16U, NULL, 0U);
    modbus_frame_t rsp;

    memset(payload, 0xA5, sizeof(payload));
    start_master();
    req.data_len = MODBUS_MAX_DATA_BYTES;
    memcpy(req.data, payload, sizeof(payload));
    script_response(1U, 16U, payload, MODBUS_MAX_DATA_BYTES);

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_transact(&req, &rsp));
    TEST_ASSERT_EQUAL_UINT16(MODBUS_MAX_FRAME_BYTES, s_last_wire_len);
    TEST_ASSERT_EQUAL_UINT16(MODBUS_MAX_DATA_BYTES, rsp.data_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, rsp.data, MODBUS_MAX_DATA_BYTES);
}

/* ===================================================================== */
/* TC-MM-013..014: statistics                                            */
/* ===================================================================== */

void test_TC_MM_013_get_stats_snapshot_and_argument_checks(void)
{
    modbus_master_stats_t st;

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_NOT_INIT, modbus_master_get_stats(&st));
    start_master();
    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_NULL_ARG, modbus_master_get_stats(NULL));
    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_get_stats(&st));
    TEST_ASSERT_EQUAL_UINT32(0U, st.transactions_sent);
}

void test_TC_MM_014_reset_stats_zeroes_every_counter(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;
    modbus_master_stats_t st;
    modbus_master_stats_t zero;

    memset(&zero, 0, sizeof(zero));
    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_NOT_INIT, modbus_master_reset_stats());
    start_master();
    script_timeout();
    (void) modbus_master_transact(&req, &rsp);

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_reset_stats());
    (void) modbus_master_get_stats(&st);
    TEST_ASSERT_EQUAL_MEMORY(&zero, &st, sizeof(st));
}

/* ===================================================================== */
/* TC-MM-015..017: transport failures and FC03                           */
/* ===================================================================== */

void test_TC_MM_015_tx_failure_is_a_retried_timeout(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;
    modbus_master_stats_t st;
    step_t *fail;

    start_master();
    fail = script_add();
    fail->tx_err = MODBUS_UART_ERR_TIMEOUT;
    script_response(1U, 4U, k_reply_1_reg, 3U);

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_transact(&req, &rsp));
    (void) modbus_master_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(1U, st.timeouts);
    TEST_ASSERT_EQUAL_UINT32(2U, st.transactions_sent);
}

void test_TC_MM_016_rx_error_bit_retried_then_reported_as_rx_error(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;
    modbus_master_stats_t st;
    step_t *err;

    start_master();
    err = script_add();
    err->bits = NOTIFY_RX_ERROR; /* the last step repeats for every attempt */

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_RX_ERROR, modbus_master_transact(&req, &rsp));
    (void) modbus_master_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(4U, st.rx_errors);
    TEST_ASSERT_EQUAL_UINT32(0U, st.bad_responses);
    TEST_ASSERT_EQUAL_UINT32(4U, s_tx_calls);
}

void test_TC_MM_029_rx_error_then_success(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;
    modbus_master_stats_t st;
    step_t *err;

    start_master();
    err = script_add();
    err->bits = NOTIFY_RX_ERROR;
    script_response(1U, 4U, k_reply_1_reg, 3U);

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_transact(&req, &rsp));
    (void) modbus_master_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(1U, st.rx_errors);
    TEST_ASSERT_EQUAL_UINT32(1U, st.transactions_ok);
}

void test_TC_MM_026_runt_frame_is_a_retried_bad_response(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;
    step_t *runt;

    start_master();
    runt = script_add();
    runt->rx_len = 3U;
    script_response(1U, 4U, k_reply_1_reg, 3U);

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_transact(&req, &rsp));
    TEST_ASSERT_EQUAL_UINT32(2U, s_tx_calls);
}

void test_TC_MM_017_fc03_round_trip(void)
{
    const uint8_t read_hr[] = {0x00U, 0x10U, 0x00U, 0x02U};
    const uint8_t reply[] = {0x04U, 0x00U, 0x01U, 0x00U, 0x02U};
    modbus_frame_t req = make_request(7U, 3U, read_hr, 4U);
    modbus_frame_t rsp;

    start_master();
    script_response(7U, 3U, reply, 5U);

    TEST_ASSERT_EQUAL_INT(MODBUS_MASTER_ERR_OK, modbus_master_transact(&req, &rsp));
    TEST_ASSERT_EQUAL_UINT16(5U, rsp.data_len);
    TEST_ASSERT_EQUAL_MEMORY(reply, rsp.data, 5U);
}

/* ===================================================================== */
/* TC-MM-027..028: stale-state handling and the ISR callback             */
/* ===================================================================== */

void test_TC_MM_027_stale_rx_is_flushed_before_every_transmit(void)
{
    modbus_frame_t req = make_request(1U, 4U, k_read_1_reg, 4U);
    modbus_frame_t rsp;

    start_master();
    script_timeout();
    script_response(1U, 4U, k_reply_1_reg, 3U);
    (void) modbus_master_transact(&req, &rsp);

    /* Per attempt: one UART drain + one non-blocking notify flush before the TX,
     * then one blocking wait and (when a frame arrived) one frame read. */
    TEST_ASSERT_EQUAL_UINT32(2U, s_tx_calls);
    TEST_ASSERT_EQUAL_UINT32(2U * 2U, g_mock_xTaskNotifyWait_call_count);
    TEST_ASSERT_EQUAL_UINT32(3U, s_get_rx_calls);
}

void test_TC_MM_028_uart_callback_maps_events_to_notification_bits(void)
{
    start_master();
    TEST_ASSERT_NOT_NULL(s_captured_cb);

    s_captured_cb(MODBUS_UART_EVENT_RX_DONE, NULL);
    TEST_ASSERT_EQUAL_UINT32(NOTIFY_RX_DONE, g_mock_xTaskNotifyFromISR_last_value);
    TEST_ASSERT_EQUAL_PTR(g_mock_xTaskGetCurrentTaskHandle_return,
                          g_mock_xTaskNotifyFromISR_last_handle);

    s_captured_cb(MODBUS_UART_EVENT_RX_ERROR, NULL);
    TEST_ASSERT_EQUAL_UINT32(NOTIFY_RX_ERROR, g_mock_xTaskNotifyFromISR_last_value);
    TEST_ASSERT_EQUAL_UINT32(2U, g_mock_xTaskNotifyFromISR_call_count);
}
