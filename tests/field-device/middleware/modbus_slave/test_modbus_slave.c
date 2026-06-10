/**
 * @file test_modbus_slave.c
 * @brief Unity unit tests for ModbusSlave — TC-MBS-001 to TC-MBS-023.
 *
 * Compiled with -DSTM32F469xx -DTEST -DLOG_LEVEL_MIN=-1.
 *
 * Test isolation strategy:
 *   - IModbusRegisterMap vtable: stub functions defined in this TU.
 *   - modbus_uart_*: stub implementations defined in this TU (declared in
 *     modbus_uart_driver_stub.h, swapped into modbus_slave.c via #ifndef TEST).
 *   - FreeRTOS: freertos_mock.h auto-links freertos_mock.c.
 *   - Logger: LOG_LEVEL_MIN=-1 collapses all LOG_* to ((void)0) — no link.
 *   - modbus_crc16(): real implementation auto-linked via modbus_crc.c.
 */

#include "unity.h"
#include "freertos_mock.h"
#include "modbus_uart_driver_stub.h"
#include "modbus_slave.h"
#include "modbus_crc.h"

#include <string.h>

extern void modbus_slave_reset_for_test(void);

/* ===================================================================== */
/* Stub state — modbus_uart_driver                                       */
/* ===================================================================== */

static uint8_t           s_stub_rx_frame[MODBUS_UART_BUF_SIZE];
static uint16_t          s_stub_rx_len;
static uint8_t           s_stub_tx_frame[MODBUS_UART_BUF_SIZE];
static uint16_t          s_stub_tx_len;
static modbus_uart_rx_cb_t s_captured_rx_cb;

void modbus_uart_attach_rx(modbus_uart_rx_cb_t callback, void *context)
{
    (void) context;
    s_captured_rx_cb = callback;
}

modbus_uart_err_t modbus_uart_get_rx_frame(uint8_t *buf, uint16_t *len)
{
    (void) memcpy(buf, s_stub_rx_frame, s_stub_rx_len);
    *len = s_stub_rx_len;
    return MODBUS_UART_ERR_OK;
}

modbus_uart_err_t modbus_uart_transmit(const uint8_t *frame, uint16_t len)
{
    (void) memcpy(s_stub_tx_frame, frame, len);
    s_stub_tx_len = len;
    return MODBUS_UART_ERR_OK;
}

/* ===================================================================== */
/* Stub state — IModbusRegisterMap                                       */
/* ===================================================================== */

#define WRITE_LOG_MAX (8U)

static modbus_slave_err_t s_read_input_return;
static uint16_t           s_read_input_value;
static modbus_slave_err_t s_read_holding_return;
static uint16_t           s_read_holding_value;
static modbus_slave_err_t s_write_holding_return;
static uint16_t           s_write_addrs[WRITE_LOG_MAX];
static uint16_t           s_write_values[WRITE_LOG_MAX];
static uint32_t           s_write_count;

static modbus_slave_err_t stub_read_input(uint16_t addr, uint16_t *out)
{
    (void) addr;
    *out = s_read_input_value;
    return s_read_input_return;
}

static modbus_slave_err_t stub_read_holding(uint16_t addr, uint16_t *out)
{
    (void) addr;
    *out = s_read_holding_value;
    return s_read_holding_return;
}

static modbus_slave_err_t stub_write_holding(uint16_t addr, uint16_t value)
{
    if (s_write_count < WRITE_LOG_MAX)
    {
        s_write_addrs[s_write_count]  = addr;
        s_write_values[s_write_count] = value;
    }
    s_write_count++;
    return s_write_holding_return;
}

static const IModbusRegisterMap k_reg_map = {
    .read_input    = stub_read_input,
    .read_holding  = stub_read_holding,
    .write_holding = stub_write_holding,
};

/* Dummy task handle — never dereferenced in tests. */
static int           s_task_sentinel;
static TaskHandle_t  s_task_handle = (TaskHandle_t)(&s_task_sentinel);

/* ===================================================================== */
/* Test helpers                                                          */
/* ===================================================================== */

static void do_init(uint8_t addr)
{
    modbus_slave_err_t err = modbus_slave_init(&k_reg_map, addr, s_task_handle);
    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, err);
}

/** Build a frame into s_stub_rx_frame with correct CRC appended. */
static void inject_frame(const uint8_t *pdu, uint16_t pdu_len)
{
    (void) memcpy(s_stub_rx_frame, pdu, pdu_len);
    uint16_t crc = modbus_crc16(s_stub_rx_frame, pdu_len);
    s_stub_rx_frame[pdu_len]      = (uint8_t)(crc & 0x00FFU);
    s_stub_rx_frame[pdu_len + 1U] = (uint8_t)(crc >> 8U);
    s_stub_rx_len = pdu_len + 2U;
}

/* ===================================================================== */
/* setUp / tearDown                                                      */
/* ===================================================================== */

void setUp(void)
{
    mock_freertos_reset();
    modbus_slave_reset_for_test();

    (void) memset(s_stub_rx_frame, 0, sizeof(s_stub_rx_frame));
    s_stub_rx_len = 0U;
    (void) memset(s_stub_tx_frame, 0, sizeof(s_stub_tx_frame));
    s_stub_tx_len = 0U;
    s_captured_rx_cb = NULL;

    s_read_input_return   = MODBUS_SLAVE_ERR_OK;
    s_read_input_value    = 0U;
    s_read_holding_return = MODBUS_SLAVE_ERR_OK;
    s_read_holding_value  = 0U;
    s_write_holding_return = MODBUS_SLAVE_ERR_OK;
    s_write_count = 0U;
    (void) memset(s_write_addrs,  0, sizeof(s_write_addrs));
    (void) memset(s_write_values, 0, sizeof(s_write_values));
}

void tearDown(void) {}

/* ===================================================================== */
/* TC-MBS-001: NULL reg_map → ERR_NULL_ARG                              */
/* ===================================================================== */

void test_TC_MBS_001_init_null_reg_map_returns_null_arg(void)
{
    modbus_slave_err_t err = modbus_slave_init(NULL, 1U, s_task_handle);
    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_NULL_ARG, err);
}

/* ===================================================================== */
/* TC-MBS-002: slave_addr == 0 → ERR_INVALID_ADDR                      */
/* ===================================================================== */

void test_TC_MBS_002_init_addr_zero_returns_invalid_addr(void)
{
    modbus_slave_err_t err = modbus_slave_init(&k_reg_map, 0U, s_task_handle);
    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_INVALID_ADDR, err);
}

/* ===================================================================== */
/* TC-MBS-003: slave_addr == 248 → ERR_INVALID_ADDR                    */
/* ===================================================================== */

void test_TC_MBS_003_init_addr_248_returns_invalid_addr(void)
{
    modbus_slave_err_t err = modbus_slave_init(&k_reg_map, 248U, s_task_handle);
    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_INVALID_ADDR, err);
}

/* ===================================================================== */
/* TC-MBS-004: address mismatch → silent drop                           */
/* ===================================================================== */

void test_TC_MBS_004_address_mismatch_silent_drop(void)
{
    do_init(1U);

    /* Frame addressed to slave 2, not 1. */
    const uint8_t pdu[] = {0x02U, 0x03U, 0x00U, 0x00U, 0x00U, 0x01U};
    inject_frame(pdu, sizeof(pdu));

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_process());

    modbus_slave_stats_t stats;
    (void) modbus_slave_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.address_mismatches);
    TEST_ASSERT_EQUAL_UINT16(0U, s_stub_tx_len);
}

/* ===================================================================== */
/* TC-MBS-005: wrong CRC → silent drop                                  */
/* ===================================================================== */

void test_TC_MBS_005_wrong_crc_silent_drop(void)
{
    do_init(1U);

    /* Correct address, wrong CRC — flip last byte. */
    const uint8_t pdu[] = {0x01U, 0x03U, 0x00U, 0x00U, 0x00U, 0x01U};
    inject_frame(pdu, sizeof(pdu));
    s_stub_rx_frame[s_stub_rx_len - 1U] ^= 0xFFU; /* corrupt CRC high byte */

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_process());

    modbus_slave_stats_t stats;
    (void) modbus_slave_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.crc_errors);
    TEST_ASSERT_EQUAL_UINT16(0U, s_stub_tx_len);
}

/* ===================================================================== */
/* TC-MBS-006: FC04 read valid input register → correct response        */
/* ===================================================================== */

void test_TC_MBS_006_fc04_read_input_happy_path(void)
{
    do_init(1U);
    s_read_input_value = 0x1234U;

    /* FC04: read 1 input register at address 0x0000. */
    const uint8_t pdu[] = {0x01U, 0x04U, 0x00U, 0x00U, 0x00U, 0x01U};
    inject_frame(pdu, sizeof(pdu));

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_process());

    /* Response: [addr][0x04][byte_count=2][hi][lo][CRC×2] = 7 bytes. */
    TEST_ASSERT_EQUAL_UINT16(7U, s_stub_tx_len);
    TEST_ASSERT_EQUAL_HEX8(0x01U, s_stub_tx_frame[0]);
    TEST_ASSERT_EQUAL_HEX8(0x04U, s_stub_tx_frame[1]);
    TEST_ASSERT_EQUAL_HEX8(0x02U, s_stub_tx_frame[2]); /* byte count */
    TEST_ASSERT_EQUAL_HEX8(0x12U, s_stub_tx_frame[3]); /* value hi */
    TEST_ASSERT_EQUAL_HEX8(0x34U, s_stub_tx_frame[4]); /* value lo */

    modbus_slave_stats_t stats;
    (void) modbus_slave_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.successful_responses);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.valid_frames);
}

/* ===================================================================== */
/* TC-MBS-007: FC04 invalid address → exception 0x02                   */
/* ===================================================================== */

void test_TC_MBS_007_fc04_invalid_addr_exception_02(void)
{
    do_init(1U);
    s_read_input_return = MODBUS_SLAVE_ERR_INVALID_ADDR;

    const uint8_t pdu[] = {0x01U, 0x04U, 0x00U, 0x64U, 0x00U, 0x01U};
    inject_frame(pdu, sizeof(pdu));

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_process());

    /* Exception response: [addr][FC|0x80][exception][CRC×2] = 5 bytes. */
    TEST_ASSERT_EQUAL_UINT16(5U, s_stub_tx_len);
    TEST_ASSERT_EQUAL_HEX8(0x01U, s_stub_tx_frame[0]);
    TEST_ASSERT_EQUAL_HEX8(0x84U, s_stub_tx_frame[1]); /* 0x04 | 0x80 */
    TEST_ASSERT_EQUAL_HEX8(0x02U, s_stub_tx_frame[2]); /* exception code */

    modbus_slave_stats_t stats;
    (void) modbus_slave_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.exception_responses);
}

/* ===================================================================== */
/* TC-MBS-008: FC03 read valid holding register → correct response      */
/* ===================================================================== */

void test_TC_MBS_008_fc03_read_holding_happy_path(void)
{
    do_init(1U);
    s_read_holding_value = 0xABCDU;

    const uint8_t pdu[] = {0x01U, 0x03U, 0x00U, 0x00U, 0x00U, 0x01U};
    inject_frame(pdu, sizeof(pdu));

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_process());

    TEST_ASSERT_EQUAL_UINT16(7U, s_stub_tx_len);
    TEST_ASSERT_EQUAL_HEX8(0x01U, s_stub_tx_frame[0]);
    TEST_ASSERT_EQUAL_HEX8(0x03U, s_stub_tx_frame[1]);
    TEST_ASSERT_EQUAL_HEX8(0x02U, s_stub_tx_frame[2]);
    TEST_ASSERT_EQUAL_HEX8(0xABU, s_stub_tx_frame[3]);
    TEST_ASSERT_EQUAL_HEX8(0xCDU, s_stub_tx_frame[4]);
}

/* ===================================================================== */
/* TC-MBS-009: FC06 write valid → write_holding called; echo response  */
/* ===================================================================== */

void test_TC_MBS_009_fc06_write_single_happy_path(void)
{
    do_init(1U);

    /* Write value 0x0042 to address 0x0010. */
    const uint8_t pdu[] = {0x01U, 0x06U, 0x00U, 0x10U, 0x00U, 0x42U};
    inject_frame(pdu, sizeof(pdu));

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_process());

    TEST_ASSERT_EQUAL_UINT32(1U, s_write_count);
    TEST_ASSERT_EQUAL_UINT16(0x0010U, s_write_addrs[0]);
    TEST_ASSERT_EQUAL_UINT16(0x0042U, s_write_values[0]);

    /* Echo response: [addr][0x06][addr_hi][addr_lo][val_hi][val_lo][CRC×2]. */
    TEST_ASSERT_EQUAL_UINT16(8U, s_stub_tx_len);
    TEST_ASSERT_EQUAL_HEX8(0x01U, s_stub_tx_frame[0]);
    TEST_ASSERT_EQUAL_HEX8(0x06U, s_stub_tx_frame[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_stub_tx_frame[2]);
    TEST_ASSERT_EQUAL_HEX8(0x10U, s_stub_tx_frame[3]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_stub_tx_frame[4]);
    TEST_ASSERT_EQUAL_HEX8(0x42U, s_stub_tx_frame[5]);
}

/* ===================================================================== */
/* TC-MBS-010: FC06 invalid value → exception 0x03                     */
/* ===================================================================== */

void test_TC_MBS_010_fc06_invalid_value_exception_03(void)
{
    do_init(1U);
    s_write_holding_return = MODBUS_SLAVE_ERR_INVALID_VALUE;

    const uint8_t pdu[] = {0x01U, 0x06U, 0x00U, 0x10U, 0xFFU, 0xFFU};
    inject_frame(pdu, sizeof(pdu));

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_process());

    TEST_ASSERT_EQUAL_UINT16(5U, s_stub_tx_len);
    TEST_ASSERT_EQUAL_HEX8(0x86U, s_stub_tx_frame[1]); /* 0x06 | 0x80 */
    TEST_ASSERT_EQUAL_HEX8(0x03U, s_stub_tx_frame[2]); /* exception 0x03 */

    modbus_slave_stats_t stats;
    (void) modbus_slave_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.exception_responses);
}

/* ===================================================================== */
/* TC-MBS-011: FC06 CMD_SOFT_RESTART (0x0202) + 0xA5A5 → accepted     */
/* ===================================================================== */

void test_TC_MBS_011_fc06_cmd_soft_restart_accepted(void)
{
    do_init(1U);
    /* Register map returns OK for this value — magic check is in the map. */
    s_write_holding_return = MODBUS_SLAVE_ERR_OK;

    const uint8_t pdu[] = {0x01U, 0x06U, 0x02U, 0x02U, 0xA5U, 0xA5U};
    inject_frame(pdu, sizeof(pdu));

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_process());

    /* Echo ACK, no exception. */
    TEST_ASSERT_EQUAL_UINT16(8U, s_stub_tx_len);
    TEST_ASSERT_EQUAL_HEX8(0x06U, s_stub_tx_frame[1]);

    modbus_slave_stats_t stats;
    (void) modbus_slave_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.successful_responses);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.exception_responses);
}

/* ===================================================================== */
/* TC-MBS-012: FC06 CMD_SOFT_RESTART with wrong value → exception 0x03 */
/* ===================================================================== */

void test_TC_MBS_012_fc06_cmd_soft_restart_wrong_value_exception_03(void)
{
    do_init(1U);
    /* Register map rejects wrong value with INVALID_VALUE. */
    s_write_holding_return = MODBUS_SLAVE_ERR_INVALID_VALUE;

    const uint8_t pdu[] = {0x01U, 0x06U, 0x02U, 0x02U, 0x00U, 0x01U};
    inject_frame(pdu, sizeof(pdu));

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_process());

    TEST_ASSERT_EQUAL_HEX8(0x86U, s_stub_tx_frame[1]);
    TEST_ASSERT_EQUAL_HEX8(0x03U, s_stub_tx_frame[2]);
}

/* ===================================================================== */
/* TC-MBS-013: FC16 byte count mismatch → exception 0x03               */
/* ===================================================================== */

void test_TC_MBS_013_fc16_byte_count_mismatch_exception_03(void)
{
    do_init(1U);

    /* qty=2 registers, byte_count=3 (should be 4). */
    const uint8_t pdu[] = {0x01U, 0x10U, 0x00U, 0x00U, 0x00U, 0x02U,
                            0x03U, 0x00U, 0x01U, 0x00U};
    inject_frame(pdu, sizeof(pdu));

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_process());

    TEST_ASSERT_EQUAL_UINT16(5U, s_stub_tx_len);
    TEST_ASSERT_EQUAL_HEX8(0x90U, s_stub_tx_frame[1]); /* 0x10 | 0x80 */
    TEST_ASSERT_EQUAL_HEX8(0x03U, s_stub_tx_frame[2]); /* exception 0x03 */
    TEST_ASSERT_EQUAL_UINT32(0U, s_write_count);
}

/* ===================================================================== */
/* TC-MBS-014: FC16 happy path — all writes in order; ACK response      */
/* ===================================================================== */

void test_TC_MBS_014_fc16_write_multiple_happy_path(void)
{
    do_init(1U);

    /* Write 2 registers starting at 0x0000: val0=0x0001, val1=0x0002. */
    const uint8_t pdu[] = {0x01U, 0x10U, 0x00U, 0x00U, 0x00U, 0x02U,
                            0x04U, 0x00U, 0x01U, 0x00U, 0x02U};
    inject_frame(pdu, sizeof(pdu));

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_process());

    /* Two writes dispatched in order. */
    TEST_ASSERT_EQUAL_UINT32(2U, s_write_count);
    TEST_ASSERT_EQUAL_UINT16(0x0000U, s_write_addrs[0]);
    TEST_ASSERT_EQUAL_UINT16(0x0001U, s_write_values[0]);
    TEST_ASSERT_EQUAL_UINT16(0x0001U, s_write_addrs[1]);
    TEST_ASSERT_EQUAL_UINT16(0x0002U, s_write_values[1]);

    /* ACK response: [addr][0x10][start_hi][start_lo][qty_hi][qty_lo][CRC×2]. */
    TEST_ASSERT_EQUAL_UINT16(8U, s_stub_tx_len);
    TEST_ASSERT_EQUAL_HEX8(0x01U, s_stub_tx_frame[0]);
    TEST_ASSERT_EQUAL_HEX8(0x10U, s_stub_tx_frame[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_stub_tx_frame[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_stub_tx_frame[3]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_stub_tx_frame[4]);
    TEST_ASSERT_EQUAL_HEX8(0x02U, s_stub_tx_frame[5]);

    modbus_slave_stats_t stats;
    (void) modbus_slave_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.successful_responses);
}

/* ===================================================================== */
/* TC-MBS-015: unsupported FC → exception 0x01; stats.unsupported_fc   */
/* ===================================================================== */

void test_TC_MBS_015_unsupported_fc_exception_01(void)
{
    do_init(1U);

    /* FC 0x01 (read coils) — not supported. */
    const uint8_t pdu[] = {0x01U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U};
    inject_frame(pdu, sizeof(pdu));

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_process());

    TEST_ASSERT_EQUAL_UINT16(5U, s_stub_tx_len);
    TEST_ASSERT_EQUAL_HEX8(0x81U, s_stub_tx_frame[1]); /* 0x01 | 0x80 */
    TEST_ASSERT_EQUAL_HEX8(0x01U, s_stub_tx_frame[2]); /* exception 0x01 */

    modbus_slave_stats_t stats;
    (void) modbus_slave_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.unsupported_fc);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.exception_responses);
}

/* ===================================================================== */
/* TC-MBS-016: response CRC bytes appended low-byte-first               */
/* ===================================================================== */

void test_TC_MBS_016_response_crc_low_byte_first(void)
{
    do_init(1U);
    s_read_holding_value = 0x0000U;

    const uint8_t pdu[] = {0x01U, 0x03U, 0x00U, 0x00U, 0x00U, 0x01U};
    inject_frame(pdu, sizeof(pdu));

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_process());
    TEST_ASSERT_TRUE(s_stub_tx_len >= 2U);

    /* Compute CRC over response payload (all bytes except the 2 CRC bytes). */
    uint16_t expected_crc = modbus_crc16(s_stub_tx_frame, s_stub_tx_len - 2U);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(expected_crc & 0x00FFU),
                            s_stub_tx_frame[s_stub_tx_len - 2U]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(expected_crc >> 8U),
                            s_stub_tx_frame[s_stub_tx_len - 1U]);
}

/* ===================================================================== */
/* TC-MBS-017: get_stats NULL → ERR_NULL_ARG                           */
/* ===================================================================== */

void test_TC_MBS_017_get_stats_null_returns_null_arg(void)
{
    do_init(1U);
    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_NULL_ARG, modbus_slave_get_stats(NULL));
}

/* ===================================================================== */
/* TC-MBS-018: get_stats after several frames → snapshot correct        */
/* ===================================================================== */

void test_TC_MBS_018_get_stats_snapshot_correct(void)
{
    do_init(1U);

    /* Process 1 address-mismatch, 1 CRC error, 1 valid FC03. */

    /* Address mismatch (slave 2, we are 1). */
    const uint8_t pdu_mm[] = {0x02U, 0x03U, 0x00U, 0x00U, 0x00U, 0x01U};
    inject_frame(pdu_mm, sizeof(pdu_mm));
    (void) modbus_slave_process();

    /* CRC error. */
    const uint8_t pdu_crc[] = {0x01U, 0x03U, 0x00U, 0x00U, 0x00U, 0x01U};
    inject_frame(pdu_crc, sizeof(pdu_crc));
    s_stub_rx_frame[s_stub_rx_len - 1U] ^= 0xFFU;
    (void) modbus_slave_process();

    /* Valid FC03. */
    s_read_holding_value = 0x0001U;
    const uint8_t pdu_ok[] = {0x01U, 0x03U, 0x00U, 0x00U, 0x00U, 0x01U};
    inject_frame(pdu_ok, sizeof(pdu_ok));
    (void) modbus_slave_process();

    modbus_slave_stats_t stats;
    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_get_stats(&stats));
    TEST_ASSERT_EQUAL_UINT32(1U, stats.address_mismatches);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.crc_errors);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.valid_frames);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.successful_responses);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.exception_responses);
}

/* ===================================================================== */
/* TC-MBS-019: reset_stats → all counters zero                          */
/* ===================================================================== */

void test_TC_MBS_019_reset_stats_clears_all(void)
{
    do_init(1U);

    /* Accumulate some stats. */
    const uint8_t pdu[] = {0x02U, 0x03U, 0x00U, 0x00U, 0x00U, 0x01U};
    inject_frame(pdu, sizeof(pdu));
    (void) modbus_slave_process();

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_reset_stats());

    modbus_slave_stats_t stats;
    (void) modbus_slave_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.address_mismatches);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.crc_errors);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.valid_frames);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.successful_responses);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.exception_responses);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.unsupported_fc);
}

/* ===================================================================== */
/* TC-MBS-020: set_address(0) → ERR_INVALID_ADDR                       */
/* ===================================================================== */

void test_TC_MBS_020_set_address_zero_returns_invalid_addr(void)
{
    do_init(1U);
    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_INVALID_ADDR,
                          modbus_slave_set_address(0U));
}

/* ===================================================================== */
/* TC-MBS-021: set_address(7) → frames to 7 processed, old dropped     */
/* ===================================================================== */

void test_TC_MBS_021_set_address_update_visible(void)
{
    do_init(1U);

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_set_address(7U));

    /* Frame to address 7 — should be processed. */
    s_read_holding_value = 0x0005U;
    const uint8_t pdu7[] = {0x07U, 0x03U, 0x00U, 0x00U, 0x00U, 0x01U};
    inject_frame(pdu7, sizeof(pdu7));
    (void) modbus_slave_process();

    modbus_slave_stats_t stats;
    (void) modbus_slave_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.valid_frames);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.address_mismatches);

    /* Frame to old address 1 — should be dropped. */
    const uint8_t pdu1[] = {0x01U, 0x03U, 0x00U, 0x00U, 0x00U, 0x01U};
    inject_frame(pdu1, sizeof(pdu1));
    (void) modbus_slave_process();

    (void) modbus_slave_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.address_mismatches);
}

/* ===================================================================== */
/* TC-MBS-022: read_input returns ERR_DEVICE_FAIL → exception 0x04     */
/* ===================================================================== */

void test_TC_MBS_022_read_input_device_fail_exception_04(void)
{
    do_init(1U);
    s_read_input_return = MODBUS_SLAVE_ERR_DEVICE_FAIL;

    const uint8_t pdu[] = {0x01U, 0x04U, 0x00U, 0x00U, 0x00U, 0x01U};
    inject_frame(pdu, sizeof(pdu));

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_process());

    TEST_ASSERT_EQUAL_UINT16(5U, s_stub_tx_len);
    TEST_ASSERT_EQUAL_HEX8(0x84U, s_stub_tx_frame[1]); /* 0x04 | 0x80 */
    TEST_ASSERT_EQUAL_HEX8(0x04U, s_stub_tx_frame[2]); /* exception 0x04 */

    modbus_slave_stats_t stats;
    (void) modbus_slave_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.exception_responses);
}

/* ===================================================================== */
/* TC-MBS-023: all public functions before init → ERR_NOT_INIT          */
/* ===================================================================== */

void test_TC_MBS_023_all_functions_before_init_return_not_init(void)
{
    /* Do NOT call do_init(). */
    modbus_slave_stats_t stats;

    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_NOT_INIT, modbus_slave_process());
    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_NOT_INIT, modbus_slave_set_address(1U));

    /* get_stats and reset_stats are valid before init in this design
     * (they operate on the stats struct, not hardware) — but NOT_INIT
     * is expected if the design requires init first. Check the companion:
     * §8 (per-function behaviour) does not explicitly restrict get_stats
     * to post-init, so they return OK with zeroed stats. Verify process
     * and set_address which DO require init. */
    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_get_stats(&stats));
    TEST_ASSERT_EQUAL_UINT32(0U, stats.valid_frames);
    TEST_ASSERT_EQUAL_INT(MODBUS_SLAVE_ERR_OK, modbus_slave_reset_stats());
}
