/**
 * @file test_barometer_driver.c
 * @brief Unity unit tests for BarometerDriver — Gateway (LPS22HB over I2C2).
 *
 * The II2c vtable is replaced by a register-map fake defined in this file, so
 * neither I2cDriver nor any CMSIS peripheral is involved. vTaskDelay is the
 * counting mock in tests/support/freertos_mock.c.
 *
 * Test IDs: TC-BA-001..009 (companion §13) plus TC-BA-010..016 for register
 * values, failure points, temperature sign and the vtable.
 */

#include "unity.h"

#include "barometer_driver.h"
#include "freertos_mock.h"

/* ===================================================================== */
/* Fake I2C bus                                                          */
/* ===================================================================== */

#define FAKE_REG_COUNT (256U)
#define FAKE_LOG_DEPTH (8U)
#define FAKE_STATUS_DEPTH (4U)

#define LPS22HB_ADDR (0x5DU)
#define REG_STATUS (0x27U)
#define STATUS_READY (0x03U)
#define STATUS_P_ONLY (0x01U)
#define SENTINEL (0x7EADBEEF)

static uint8_t s_regs[FAKE_REG_COUNT];
static uint8_t s_status_seq[FAKE_STATUS_DEPTH];
static uint32_t s_status_seq_len;
static uint32_t s_status_idx;
static uint8_t s_status_default;

static uint32_t s_write_calls;
static uint32_t s_write_fail_call; /* 1-based; 0 = never fail */
static uint8_t s_write_addr[FAKE_LOG_DEPTH];
static uint8_t s_write_reg[FAKE_LOG_DEPTH];
static uint8_t s_write_val[FAKE_LOG_DEPTH];

static uint32_t s_wr_calls;
static uint32_t s_wr_fail_call; /* 1-based; 0 = never fail */
static uint8_t s_wr_addr[FAKE_LOG_DEPTH];
static uint8_t s_wr_reg[FAKE_LOG_DEPTH];
static uint16_t s_wr_len[FAKE_LOG_DEPTH];

static i2c_err_t fake_init(void)
{
    return I2C_ERR_OK;
}

static i2c_err_t fake_write(uint8_t dev_addr, const uint8_t *data, uint16_t len)
{
    TEST_ASSERT_EQUAL_UINT16(2U, len);
    if (s_write_calls < FAKE_LOG_DEPTH)
    {
        s_write_addr[s_write_calls] = dev_addr;
        s_write_reg[s_write_calls] = data[0];
        s_write_val[s_write_calls] = data[1];
    }
    s_write_calls++;
    return (s_write_calls == s_write_fail_call) ? I2C_ERR_NACK : I2C_ERR_OK;
}

static i2c_err_t fake_read(uint8_t dev_addr, uint8_t *buf, uint16_t len)
{
    (void) dev_addr;
    (void) buf;
    (void) len;
    return I2C_ERR_NACK;
}

static i2c_err_t fake_write_read(uint8_t dev_addr, const uint8_t *tx_data, uint16_t tx_len,
                                 uint8_t *rx_buf, uint16_t rx_len)
{
    uint8_t reg = tx_data[0];

    TEST_ASSERT_EQUAL_UINT16(1U, tx_len);
    if (s_wr_calls < FAKE_LOG_DEPTH)
    {
        s_wr_addr[s_wr_calls] = dev_addr;
        s_wr_reg[s_wr_calls] = reg;
        s_wr_len[s_wr_calls] = rx_len;
    }
    s_wr_calls++;
    if (s_wr_calls == s_wr_fail_call)
    {
        return I2C_ERR_NACK;
    }

    if (reg == REG_STATUS)
    {
        rx_buf[0] =
            (s_status_idx < s_status_seq_len) ? s_status_seq[s_status_idx] : s_status_default;
        s_status_idx++;
        return I2C_ERR_OK;
    }
    for (uint16_t i = 0U; i < rx_len; i++)
    {
        rx_buf[i] = s_regs[reg + i];
    }
    return I2C_ERR_OK;
}

static const ii2c_t s_fake_i2c = {
    .init = fake_init,
    .write = fake_write,
    .read = fake_read,
    .write_read = fake_write_read,
};

const ii2c_t *const i2c_driver = &s_fake_i2c;

/* ===================================================================== */
/* Helpers                                                               */
/* ===================================================================== */

/** Store a 24-bit pressure word (XL, L, H at 0x28..0x2A). */
static void set_pressure_raw(uint32_t raw24)
{
    s_regs[0x28] = (uint8_t) (raw24 & 0xFFU);
    s_regs[0x29] = (uint8_t) ((raw24 >> 8) & 0xFFU);
    s_regs[0x2A] = (uint8_t) ((raw24 >> 16) & 0xFFU);
}

/** Store the 16-bit temperature word (L, H at 0x2B..0x2C). */
static void set_temperature_raw(int16_t raw)
{
    s_regs[0x2B] = (uint8_t) ((uint16_t) raw & 0xFFU);
    s_regs[0x2C] = (uint8_t) (((uint16_t) raw >> 8) & 0xFFU);
}

static void init_ok(void)
{
    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_OK, barometer_init());
}

static barometer_data_t sentinel_data(void)
{
    barometer_data_t d = {.pressure_x10 = SENTINEL, .temperature_x100 = SENTINEL};
    return d;
}

static void assert_unchanged(const barometer_data_t *d)
{
    TEST_ASSERT_EQUAL_INT32(SENTINEL, d->pressure_x10);
    TEST_ASSERT_EQUAL_INT32(SENTINEL, d->temperature_x100);
}

void setUp(void)
{
    for (uint32_t i = 0U; i < FAKE_REG_COUNT; i++)
    {
        s_regs[i] = 0U;
    }
    for (uint32_t i = 0U; i < FAKE_LOG_DEPTH; i++)
    {
        s_write_addr[i] = 0U;
        s_write_reg[i] = 0U;
        s_write_val[i] = 0U;
        s_wr_addr[i] = 0U;
        s_wr_reg[i] = 0U;
        s_wr_len[i] = 0U;
    }
    s_status_seq_len = 0U;
    s_status_idx = 0U;
    s_status_default = STATUS_READY;
    s_write_calls = 0U;
    s_write_fail_call = 0U;
    s_wr_calls = 0U;
    s_wr_fail_call = 0U;
    mock_freertos_reset();
    barometer_reset_for_test();
}

void tearDown(void)
{
}

/* ===================================================================== */
/* Tests                                                                 */
/* ===================================================================== */

/* TC-BA-001: nominal read — 1013.2 hPa and 23.50 degC. */
void test_TC_BA_001_nominal_read(void)
{
    barometer_data_t d = sentinel_data();

    init_ok();
    set_pressure_raw(4150068U); /* 1013.2 hPa x 4096 = 4150067.2, rounded up */
    set_temperature_raw(2350);

    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_OK, barometer_read(&d));
    TEST_ASSERT_EQUAL_INT32((4150068 * 10) / 4096, d.pressure_x10);
    TEST_ASSERT_EQUAL_INT32(10132, d.pressure_x10);
    TEST_ASSERT_EQUAL_INT32(2350, d.temperature_x100);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_vTaskDelay_call_count);
}

/* TC-BA-002: sign extension of a negative 24-bit word truncates toward zero. */
void test_TC_BA_002_pressure_sign_extension(void)
{
    barometer_data_t d = sentinel_data();

    init_ok();
    set_pressure_raw(0xFFFFFFU); /* -1 after sign extension */
    set_temperature_raw(0);

    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_OK, barometer_read(&d));
    TEST_ASSERT_EQUAL_INT32(0, d.pressure_x10);
}

/* TC-BA-003: I2C failure on the 5-byte burst read. */
void test_TC_BA_003_output_burst_i2c_failure(void)
{
    barometer_data_t d = sentinel_data();

    init_ok();
    s_wr_fail_call = s_wr_calls + 2U; /* status ok, burst fails */

    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_I2C, barometer_read(&d));
    assert_unchanged(&d);
    TEST_ASSERT_EQUAL_HEX8(0x28U, s_wr_reg[1]); /* no address-bit-7: IF_ADD_INC does the work */
    TEST_ASSERT_EQUAL_UINT16(5U, s_wr_len[1]);
}

/* TC-BA-004: STATUS_REG never ready -> NOT_READY after exactly one 2 ms retry. */
void test_TC_BA_004_status_never_ready(void)
{
    barometer_data_t d = sentinel_data();

    init_ok();
    s_status_default = 0x00U;

    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_NOT_READY, barometer_read(&d));
    assert_unchanged(&d);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_vTaskDelay_call_count);
    TEST_ASSERT_EQUAL_UINT32(2U, (uint32_t) g_mock_vTaskDelay_last_ticks);
    TEST_ASSERT_EQUAL_UINT32(2U, s_wr_calls); /* two STATUS_REG reads, no burst */
}

/* TC-BA-005: NULL out pointer. */
void test_TC_BA_005_null_out_pointer(void)
{
    init_ok();
    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_INVALID_ARG, barometer_read(NULL));
}

/* TC-BA-006: maximum pressure (1260 hPa) does not overflow. */
void test_TC_BA_006_max_pressure_boundary(void)
{
    barometer_data_t d = sentinel_data();

    init_ok();
    set_pressure_raw(5160960U); /* 1260 x 4096 */

    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_OK, barometer_read(&d));
    TEST_ASSERT_EQUAL_INT32(12600, d.pressure_x10);
}

/* TC-BA-007: minimum pressure (260 hPa). */
void test_TC_BA_007_min_pressure_boundary(void)
{
    barometer_data_t d = sentinel_data();

    init_ok();
    set_pressure_raw(1064960U); /* 260 x 4096 */

    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_OK, barometer_read(&d));
    TEST_ASSERT_EQUAL_INT32(2600, d.pressure_x10);
}

/* TC-BA-008: init CTRL_REG1 write fails -> I2C error, driver stays uninitialised. */
void test_TC_BA_008_init_i2c_failure(void)
{
    barometer_data_t d = sentinel_data();

    s_write_fail_call = 1U;

    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_I2C, barometer_init());
    TEST_ASSERT_EQUAL_UINT32(1U, s_write_calls); /* aborted at the first failure */
    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_NOT_READY, barometer_read(&d));
    assert_unchanged(&d);
    TEST_ASSERT_EQUAL_UINT32(0U, s_wr_calls);
}

/* TC-BA-009: reset_for_test() clears the state. */
void test_TC_BA_009_reset_clears_state(void)
{
    barometer_data_t d = sentinel_data();

    init_ok();
    barometer_reset_for_test();

    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_NOT_READY, barometer_read(&d));
    assert_unchanged(&d);
}

/* TC-BA-010: init writes the corrected register values (IF_ADD_INC kept set). */
void test_TC_BA_010_init_register_sequence(void)
{
    init_ok();

    TEST_ASSERT_EQUAL_UINT32(2U, s_write_calls);
    TEST_ASSERT_EQUAL_HEX8(LPS22HB_ADDR, s_write_addr[0]);
    TEST_ASSERT_EQUAL_HEX8(0x10U, s_write_reg[0]); /* CTRL_REG1 */
    TEST_ASSERT_EQUAL_HEX8(0x12U, s_write_val[0]); /* ODR 1 Hz | BDU */
    TEST_ASSERT_EQUAL_HEX8(LPS22HB_ADDR, s_write_addr[1]);
    TEST_ASSERT_EQUAL_HEX8(0x11U, s_write_reg[1]); /* CTRL_REG2 */
    TEST_ASSERT_EQUAL_HEX8(0x10U, s_write_val[1]); /* IF_ADD_INC; 0x00 would break burst reads */
    TEST_ASSERT_EQUAL_UINT32(0U, s_wr_calls);
}

/* TC-BA-011: CTRL_REG2 write fails. */
void test_TC_BA_011_init_ctrl2_write_fails(void)
{
    barometer_data_t d = sentinel_data();

    s_write_fail_call = 2U;

    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_I2C, barometer_init());
    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_NOT_READY, barometer_read(&d));
}

/* TC-BA-012: STATUS_REG read fails. */
void test_TC_BA_012_status_read_i2c_failure(void)
{
    barometer_data_t d = sentinel_data();

    init_ok();
    s_wr_fail_call = s_wr_calls + 1U;

    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_I2C, barometer_read(&d));
    assert_unchanged(&d);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_vTaskDelay_call_count);
}

/* TC-BA-013: not ready first, ready on the retry; status read at the right address. */
void test_TC_BA_013_status_ready_on_retry(void)
{
    barometer_data_t d = sentinel_data();

    init_ok();
    set_pressure_raw(4150068U);
    set_temperature_raw(2350);
    s_status_seq[0] = STATUS_P_ONLY; /* T_DA missing */
    s_status_seq_len = 1U;

    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_OK, barometer_read(&d));
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_vTaskDelay_call_count);
    TEST_ASSERT_EQUAL_HEX8(LPS22HB_ADDR, s_wr_addr[0]);
    TEST_ASSERT_EQUAL_HEX8(REG_STATUS, s_wr_reg[0]);
    TEST_ASSERT_EQUAL_INT32(10132, d.pressure_x10);
}

/* TC-BA-014: negative temperature is sign-extended from 16 bits. */
void test_TC_BA_014_negative_temperature(void)
{
    barometer_data_t d = sentinel_data();

    init_ok();
    set_pressure_raw(4150068U);
    set_temperature_raw(-1250); /* -12.50 degC */

    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_OK, barometer_read(&d));
    TEST_ASSERT_EQUAL_INT32(-1250, d.temperature_x100);
}

/* TC-BA-015: read before init is refused without touching the bus. */
void test_TC_BA_015_read_before_init(void)
{
    barometer_data_t d = sentinel_data();

    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_NOT_READY, barometer_read(&d));
    assert_unchanged(&d);
    TEST_ASSERT_EQUAL_UINT32(0U, s_write_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_wr_calls);
}

/* TC-BA-016: the singleton vtable dispatches to the public functions. */
void test_TC_BA_016_vtable_dispatch(void)
{
    barometer_data_t d = sentinel_data();

    TEST_ASSERT_NOT_NULL(barometer_driver);
    TEST_ASSERT_TRUE(barometer_driver->init == barometer_init);
    TEST_ASSERT_TRUE(barometer_driver->read == barometer_read);

    set_pressure_raw(5160960U);
    set_temperature_raw(100);
    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_OK, barometer_driver->init());
    TEST_ASSERT_EQUAL_INT(BAROMETER_ERR_OK, barometer_driver->read(&d));
    TEST_ASSERT_EQUAL_INT32(12600, d.pressure_x10);
}
