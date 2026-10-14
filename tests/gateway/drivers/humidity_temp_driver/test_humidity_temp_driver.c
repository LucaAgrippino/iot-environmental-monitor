/**
 * @file test_humidity_temp_driver.c
 * @brief Unity unit tests for HumidityTempDriver — Gateway (HTS221 over I2C2).
 *
 * The II2c vtable is replaced by a register-map fake defined in this file, so
 * neither I2cDriver nor any CMSIS peripheral is involved. vTaskDelay is the
 * counting mock in tests/support/freertos_mock.c.
 *
 * Test IDs: TC-HT-001..010 (companion §13) plus TC-HT-011..018 for register
 * values, failure points, clamping and the vtable.
 */

#include "unity.h"

#include "freertos_mock.h"
#include "humidity_temp_driver.h"

/* ===================================================================== */
/* Fake I2C bus                                                          */
/* ===================================================================== */

#define FAKE_REG_COUNT (256U)
#define FAKE_LOG_DEPTH (8U)
#define FAKE_STATUS_DEPTH (4U)

#define HTS221_ADDR (0x5FU)
#define REG_STATUS (0x27U)
#define STATUS_READY (0x03U)
#define STATUS_T_ONLY (0x01U)
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
    uint8_t reg = (uint8_t) (tx_data[0] & 0x7FU);

    TEST_ASSERT_EQUAL_UINT16(1U, tx_len);
    if (s_wr_calls < FAKE_LOG_DEPTH)
    {
        s_wr_addr[s_wr_calls] = dev_addr;
        s_wr_reg[s_wr_calls] = tx_data[0];
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
/* Calibration fixtures                                                  */
/* ===================================================================== */

typedef struct
{
    uint8_t h0_rh_x2;
    uint8_t h1_rh_x2;
    uint16_t t0_degc_x8;
    uint16_t t1_degc_x8;
    int16_t h0_t0_out;
    int16_t h1_t0_out;
    int16_t t0_out;
    int16_t t1_out;
} cal_fixture_t;

/* H: 40 -> 80 %RH over -200 -> 7800 counts. T: 20 -> 40 degC over -500 -> 2500.
 * T1 = 320 (0x140) exercises the MSB assembly from register 0x35. */
static const cal_fixture_t k_cal_nominal = {
    .h0_rh_x2 = 80U,
    .h1_rh_x2 = 160U,
    .t0_degc_x8 = 160U,
    .t1_degc_x8 = 320U,
    .h0_t0_out = -200,
    .h1_t0_out = 7800,
    .t0_out = -500,
    .t1_out = 2500,
};

/* Odd H*_rH_x2 values (35.5 -> 72.5 %RH) and fractional T points (22.75 / 37.5 degC),
 * representative of real parts; expected values come from an independent
 * floating-point model, not from ST's application-note numbers. */
static const cal_fixture_t k_cal_representative = {
    .h0_rh_x2 = 71U,
    .h1_rh_x2 = 145U,
    .t0_degc_x8 = 182U,
    .t1_degc_x8 = 300U,
    .h0_t0_out = -1500,
    .h1_t0_out = 6100,
    .t0_out = -120,
    .t1_out = 2460,
};

static void put_le16(uint8_t reg, int16_t value)
{
    s_regs[reg] = (uint8_t) ((uint16_t) value & 0xFFU);
    s_regs[reg + 1U] = (uint8_t) (((uint16_t) value >> 8) & 0xFFU);
}

static void load_calibration(const cal_fixture_t *c)
{
    s_regs[0x30] = c->h0_rh_x2;
    s_regs[0x31] = c->h1_rh_x2;
    s_regs[0x32] = (uint8_t) (c->t0_degc_x8 & 0xFFU);
    s_regs[0x33] = (uint8_t) (c->t1_degc_x8 & 0xFFU);
    s_regs[0x35] =
        (uint8_t) ((((c->t1_degc_x8 >> 8) & 0x03U) << 2) | ((c->t0_degc_x8 >> 8) & 0x03U));
    put_le16(0x36, c->h0_t0_out);
    put_le16(0x3A, c->h1_t0_out);
    put_le16(0x3C, c->t0_out);
    put_le16(0x3E, c->t1_out);
}

static void set_raw_outputs(int16_t h_raw, int16_t t_raw)
{
    put_le16(0x28, h_raw);
    put_le16(0x2A, t_raw);
}

static void init_ok(const cal_fixture_t *c)
{
    load_calibration(c);
    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_OK, humidity_temp_init());
}

static humidity_temp_data_t sentinel_data(void)
{
    humidity_temp_data_t d = {.temperature_x100 = SENTINEL, .humidity_x100 = SENTINEL};
    return d;
}

static void assert_unchanged(const humidity_temp_data_t *d)
{
    TEST_ASSERT_EQUAL_INT32(SENTINEL, d->temperature_x100);
    TEST_ASSERT_EQUAL_INT32(SENTINEL, d->humidity_x100);
}

/* Independent floating-point model of the datasheet formulas. */
static int32_t model_humidity_x100(const cal_fixture_t *c, int16_t raw)
{
    double h0 = c->h0_rh_x2 / 2.0;
    double h1 = c->h1_rh_x2 / 2.0;
    double v = h0 + ((h1 - h0) * (raw - c->h0_t0_out)) / (double) (c->h1_t0_out - c->h0_t0_out);
    return (int32_t) (v * 100.0);
}

static int32_t model_temperature_x100(const cal_fixture_t *c, int16_t raw)
{
    double t0 = c->t0_degc_x8 / 8.0;
    double t1 = c->t1_degc_x8 / 8.0;
    double v = t0 + ((t1 - t0) * (raw - c->t0_out)) / (double) (c->t1_out - c->t0_out);
    return (int32_t) (v * 100.0);
}

/* ===================================================================== */
/* Fixtures                                                              */
/* ===================================================================== */

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
    humidity_temp_reset_for_test();
}

void tearDown(void)
{
}

/* ===================================================================== */
/* Tests                                                                 */
/* ===================================================================== */

/* TC-HT-001: nominal read matches the hand calculation (and the float model). */
void test_TC_HT_001_nominal_read_matches_hand_calculation(void)
{
    humidity_temp_data_t d = sentinel_data();

    init_ok(&k_cal_nominal);
    set_raw_outputs(3800, 1000);

    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_OK, humidity_temp_read(&d));
    /* (80-40)x50x(3800+200)/8000 + 4000 = 6000 ; (320-160)x100x1500/24000 + 2000 = 3000 */
    TEST_ASSERT_EQUAL_INT32(6000, d.humidity_x100);
    TEST_ASSERT_EQUAL_INT32(3000, d.temperature_x100);
    TEST_ASSERT_INT32_WITHIN(1, model_humidity_x100(&k_cal_nominal, 3800), d.humidity_x100);
    TEST_ASSERT_INT32_WITHIN(1, model_temperature_x100(&k_cal_nominal, 1000), d.temperature_x100);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_vTaskDelay_call_count);
}

/* TC-HT-002: I2C failure on the STATUS_REG read. */
void test_TC_HT_002_status_read_i2c_failure(void)
{
    humidity_temp_data_t d = sentinel_data();

    init_ok(&k_cal_nominal);
    s_wr_fail_call = s_wr_calls + 1U; /* the STATUS_REG read */

    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_I2C, humidity_temp_read(&d));
    assert_unchanged(&d);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_vTaskDelay_call_count);
}

/* TC-HT-003: not ready first, ready on the retry after one 2 ms delay. */
void test_TC_HT_003_status_ready_on_retry(void)
{
    humidity_temp_data_t d = sentinel_data();

    init_ok(&k_cal_nominal);
    set_raw_outputs(3800, 1000);
    s_status_seq[0] = 0x00U;
    s_status_seq_len = 1U;

    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_OK, humidity_temp_read(&d));
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_vTaskDelay_call_count);
    TEST_ASSERT_EQUAL_UINT32(2U, (uint32_t) g_mock_vTaskDelay_last_ticks);
    TEST_ASSERT_EQUAL_INT32(6000, d.humidity_x100);
}

/* TC-HT-004: never ready -> NOT_READY after exactly one retry, no output read. */
void test_TC_HT_004_status_never_ready(void)
{
    humidity_temp_data_t d = sentinel_data();

    init_ok(&k_cal_nominal);
    s_status_default = 0x00U;
    uint32_t wr_before = s_wr_calls;

    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_NOT_READY, humidity_temp_read(&d));
    assert_unchanged(&d);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_vTaskDelay_call_count);
    TEST_ASSERT_EQUAL_UINT32(2U, s_wr_calls - wr_before); /* two STATUS_REG reads, no burst */
}

/* TC-HT-005: NULL out pointer. */
void test_TC_HT_005_null_out_pointer(void)
{
    init_ok(&k_cal_nominal);
    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_INVALID_ARG, humidity_temp_read(NULL));
}

/* TC-HT-006: zero calibration span must not divide by zero. */
void test_TC_HT_006_degenerate_calibration_zero_span(void)
{
    cal_fixture_t flat = k_cal_nominal;
    humidity_temp_data_t d = sentinel_data();

    flat.h1_t0_out = flat.h0_t0_out;
    flat.t1_out = flat.t0_out;
    init_ok(&flat);
    set_raw_outputs(1234, -321);

    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_OK, humidity_temp_read(&d));
    TEST_ASSERT_EQUAL_INT32(4000, d.humidity_x100);    /* H0 point: 40.00 %RH */
    TEST_ASSERT_EQUAL_INT32(2000, d.temperature_x100); /* T0 point: 20.00 degC */
}

/* TC-HT-007: representative factory calibration, odd H*_rH_x2, matches the model. */
void test_TC_HT_007_representative_calibration(void)
{
    humidity_temp_data_t d = sentinel_data();

    init_ok(&k_cal_representative);
    set_raw_outputs(2300, 900);

    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_OK, humidity_temp_read(&d));
    TEST_ASSERT_INT32_WITHIN(1, model_humidity_x100(&k_cal_representative, 2300), d.humidity_x100);
    TEST_ASSERT_INT32_WITHIN(1, model_temperature_x100(&k_cal_representative, 900),
                             d.temperature_x100);
}

/* TC-HT-008: CTRL_REG1 write fails -> I2C error, driver stays uninitialised. */
void test_TC_HT_008_init_ctrl_write_fails(void)
{
    humidity_temp_data_t d = sentinel_data();

    load_calibration(&k_cal_nominal);
    s_write_fail_call = 1U;

    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_I2C, humidity_temp_init());
    TEST_ASSERT_EQUAL_UINT32(1U, s_write_calls); /* aborted at the first failure */
    TEST_ASSERT_EQUAL_UINT32(0U, s_wr_calls);

    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_NOT_READY, humidity_temp_read(&d));
    assert_unchanged(&d);
}

/* TC-HT-009: read before init is refused without touching the bus. */
void test_TC_HT_009_read_before_init(void)
{
    humidity_temp_data_t d = sentinel_data();

    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_NOT_READY, humidity_temp_read(&d));
    assert_unchanged(&d);
    TEST_ASSERT_EQUAL_UINT32(0U, s_write_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_wr_calls);
}

/* TC-HT-010: reset_for_test() clears the state. */
void test_TC_HT_010_reset_clears_state(void)
{
    humidity_temp_data_t d = sentinel_data();

    init_ok(&k_cal_nominal);
    humidity_temp_reset_for_test();

    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_NOT_READY, humidity_temp_read(&d));
    assert_unchanged(&d);
}

/* TC-HT-011: init writes the corrected register values to the right addresses. */
void test_TC_HT_011_init_register_sequence(void)
{
    init_ok(&k_cal_nominal);

    TEST_ASSERT_EQUAL_UINT32(2U, s_write_calls);
    TEST_ASSERT_EQUAL_HEX8(HTS221_ADDR, s_write_addr[0]);
    TEST_ASSERT_EQUAL_HEX8(0x20U, s_write_reg[0]); /* CTRL_REG1 */
    TEST_ASSERT_EQUAL_HEX8(0x85U, s_write_val[0]); /* PD | BDU | ODR 1 Hz */
    TEST_ASSERT_EQUAL_HEX8(HTS221_ADDR, s_write_addr[1]);
    TEST_ASSERT_EQUAL_HEX8(0x10U, s_write_reg[1]); /* AV_CONF */
    TEST_ASSERT_EQUAL_HEX8(0x1BU, s_write_val[1]);

    TEST_ASSERT_EQUAL_UINT32(1U, s_wr_calls);
    TEST_ASSERT_EQUAL_HEX8(HTS221_ADDR, s_wr_addr[0]);
    TEST_ASSERT_EQUAL_HEX8(0xB0U, s_wr_reg[0]); /* 0x30 | auto-increment */
    TEST_ASSERT_EQUAL_UINT16(16U, s_wr_len[0]);
}

/* TC-HT-012: AV_CONF write fails. */
void test_TC_HT_012_init_av_conf_write_fails(void)
{
    load_calibration(&k_cal_nominal);
    s_write_fail_call = 2U;

    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_I2C, humidity_temp_init());
    TEST_ASSERT_EQUAL_UINT32(0U, s_wr_calls); /* calibration never read */
}

/* TC-HT-013: calibration burst read fails. */
void test_TC_HT_013_init_calibration_read_fails(void)
{
    humidity_temp_data_t d = sentinel_data();

    load_calibration(&k_cal_nominal);
    s_wr_fail_call = 1U;

    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_I2C, humidity_temp_init());
    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_NOT_READY, humidity_temp_read(&d));
}

/* TC-HT-014: STATUS_REG read fails on the retry. */
void test_TC_HT_014_status_retry_i2c_failure(void)
{
    humidity_temp_data_t d = sentinel_data();

    init_ok(&k_cal_nominal);
    s_status_seq[0] = 0x00U;
    s_status_seq_len = 1U;
    s_wr_fail_call = s_wr_calls + 2U; /* first status read ok (not ready), retry fails */

    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_I2C, humidity_temp_read(&d));
    assert_unchanged(&d);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_vTaskDelay_call_count);
}

/* TC-HT-015: output burst read fails; burst uses auto-increment. */
void test_TC_HT_015_output_burst_failure(void)
{
    humidity_temp_data_t d = sentinel_data();

    init_ok(&k_cal_nominal);
    s_wr_fail_call = s_wr_calls + 2U; /* status ok, burst fails */

    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_I2C, humidity_temp_read(&d));
    assert_unchanged(&d);
    TEST_ASSERT_EQUAL_HEX8(REG_STATUS, s_wr_reg[1]);
    TEST_ASSERT_EQUAL_HEX8(0xA8U, s_wr_reg[2]); /* 0x28 | auto-increment */
    TEST_ASSERT_EQUAL_UINT16(4U, s_wr_len[2]);
}

/* TC-HT-016: only one of H_DA / T_DA set counts as not ready. */
void test_TC_HT_016_partial_status_is_not_ready(void)
{
    humidity_temp_data_t d = sentinel_data();

    init_ok(&k_cal_nominal);
    s_status_default = STATUS_T_ONLY;

    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_NOT_READY, humidity_temp_read(&d));
    assert_unchanged(&d);
}

/* TC-HT-017: out-of-range humidity is clamped to 0..100 %RH. */
void test_TC_HT_017_humidity_is_clamped(void)
{
    humidity_temp_data_t d = sentinel_data();

    init_ok(&k_cal_nominal);

    set_raw_outputs(30000, 0);
    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_OK, humidity_temp_read(&d));
    TEST_ASSERT_EQUAL_INT32(10000, d.humidity_x100);

    set_raw_outputs(-20000, 0);
    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_OK, humidity_temp_read(&d));
    TEST_ASSERT_EQUAL_INT32(0, d.humidity_x100);
}

/* TC-HT-018: the singleton vtable dispatches to the public functions. */
void test_TC_HT_018_vtable_dispatch(void)
{
    humidity_temp_data_t d = sentinel_data();

    TEST_ASSERT_NOT_NULL(humidity_temp_driver);
    TEST_ASSERT_TRUE(humidity_temp_driver->init == humidity_temp_init);
    TEST_ASSERT_TRUE(humidity_temp_driver->read == humidity_temp_read);

    load_calibration(&k_cal_nominal);
    set_raw_outputs(3800, 1000);
    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_OK, humidity_temp_driver->init());
    TEST_ASSERT_EQUAL_INT(HUMIDITY_TEMP_ERR_OK, humidity_temp_driver->read(&d));
    TEST_ASSERT_EQUAL_INT32(6000, d.humidity_x100);
}
