/**
 * @file test_imu_driver.c
 * @brief Unity unit tests for ImuDriver — Gateway (LSM6DSL over I2C2).
 *
 * The II2c vtable, GpioDriver and ExtiDriver are replaced by fakes defined in this file
 * (the stub headers keep the real driver .c files out of the link). The I2C fake serves
 * STATUS_REG and the output registers from a 256-byte register map with auto-increment.
 *
 * Test IDs: TC-IM-001..011 (companion §13) plus TC-IM-012..024 for the register sequence,
 * pin / EXTI configuration, two-phase init edge cases and the stale-INT1 release.
 */

#include "unity.h"

#include "exti_driver_stub.h"
#include "gpio_driver_stub.h"
#include "imu_driver.h"
#include "imu_driver_internal.h"

/* ===================================================================== */
/* Fake I2C bus                                                          */
/* ===================================================================== */

#define FAKE_REG_COUNT (256U)
#define FAKE_LOG_DEPTH (12U)
#define LSM6DSL_ADDR (0x6AU)
#define REG_STATUS (0x1EU)
#define STATUS_XLDA (0x01U)
#define STATUS_GDA (0x02U)
#define SENTINEL (0x5A5A)

static uint8_t s_regs[FAKE_REG_COUNT];

static uint32_t s_write_calls;
static uint32_t s_write_fail_call; /* 1-based; 0 = never fail */
static uint8_t s_write_addr[FAKE_LOG_DEPTH];
static uint8_t s_write_reg[FAKE_LOG_DEPTH];
static uint8_t s_write_val[FAKE_LOG_DEPTH];

static uint32_t s_wr_calls;
static uint32_t s_wr_fail_call; /* 1-based; 0 = never fail */
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
    TEST_ASSERT_EQUAL_HEX8(LSM6DSL_ADDR, dev_addr);
    TEST_ASSERT_EQUAL_UINT16(1U, tx_len);
    if (s_wr_calls < FAKE_LOG_DEPTH)
    {
        s_wr_reg[s_wr_calls] = tx_data[0];
        s_wr_len[s_wr_calls] = rx_len;
    }
    s_wr_calls++;
    if (s_wr_calls == s_wr_fail_call)
    {
        return I2C_ERR_NACK;
    }
    for (uint16_t i = 0U; i < rx_len; i++)
    {
        rx_buf[i] = s_regs[tx_data[0] + i];
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
/* Fake GpioDriver / ExtiDriver                                          */
/* ===================================================================== */

static uint32_t s_gpio_cfg_calls;
static gpio_pin_config_t s_gpio_cfg;
static gpio_err_t s_gpio_cfg_ret;
static gpio_level_t s_pin_level;
static gpio_err_t s_gpio_read_ret;

static uint32_t s_exti_cfg_calls;
static uint8_t s_exti_cfg_line;
static exti_port_t s_exti_cfg_port;
static exti_edge_t s_exti_cfg_edge;
static exti_err_t s_exti_cfg_ret;
static uint32_t s_exti_en_calls;
static uint8_t s_exti_en_line;
static uint32_t s_exti_en_prio;
static exti_err_t s_exti_en_ret;

gpio_err_t gpio_configure_pin(const gpio_pin_config_t *config)
{
    s_gpio_cfg_calls++;
    s_gpio_cfg = *config;
    return s_gpio_cfg_ret;
}

gpio_err_t gpio_read_pin(gpio_port_t port, uint8_t pin, gpio_level_t *out_level)
{
    TEST_ASSERT_EQUAL_INT(GPIO_PORT_D, port);
    TEST_ASSERT_EQUAL_UINT8(11U, pin);
    *out_level = s_pin_level;
    return s_gpio_read_ret;
}

exti_err_t exti_configure(uint8_t line, exti_port_t port, exti_edge_t edge)
{
    s_exti_cfg_calls++;
    s_exti_cfg_line = line;
    s_exti_cfg_port = port;
    s_exti_cfg_edge = edge;
    return s_exti_cfg_ret;
}

exti_err_t exti_enable(uint8_t line, uint32_t nvic_priority)
{
    s_exti_en_calls++;
    s_exti_en_line = line;
    s_exti_en_prio = nvic_priority;
    return s_exti_en_ret;
}

/* ===================================================================== */
/* Callback fixture                                                      */
/* ===================================================================== */

static uint32_t s_cb_calls;
static void *s_cb_ctx;
static int s_ctx_token;

static void test_cb(void *ctx)
{
    s_cb_calls++;
    s_cb_ctx = ctx;
}

/* ===================================================================== */
/* Helpers                                                               */
/* ===================================================================== */

static imu_data_t sentinel_data(void)
{
    imu_data_t d = {.accel_x = SENTINEL,
                    .accel_y = SENTINEL,
                    .accel_z = SENTINEL,
                    .gyro_x = SENTINEL,
                    .gyro_y = SENTINEL,
                    .gyro_z = SENTINEL};
    return d;
}

static void assert_unchanged(const imu_data_t *d)
{
    TEST_ASSERT_EQUAL_INT16(SENTINEL, d->accel_x);
    TEST_ASSERT_EQUAL_INT16(SENTINEL, d->accel_y);
    TEST_ASSERT_EQUAL_INT16(SENTINEL, d->accel_z);
    TEST_ASSERT_EQUAL_INT16(SENTINEL, d->gyro_x);
    TEST_ASSERT_EQUAL_INT16(SENTINEL, d->gyro_y);
    TEST_ASSERT_EQUAL_INT16(SENTINEL, d->gyro_z);
}

static void put_le16(uint8_t reg, int16_t value)
{
    s_regs[reg] = (uint8_t) ((uint16_t) value & 0xFFU);
    s_regs[reg + 1U] = (uint8_t) (((uint16_t) value >> 8) & 0xFFU);
}

/** Gyro X/Y/Z at 0x22/24/26 then accel X/Y/Z at 0x28/2A/2C (datasheet order). */
static void set_axes(int16_t gx, int16_t gy, int16_t gz, int16_t ax, int16_t ay, int16_t az)
{
    put_le16(0x22U, gx);
    put_le16(0x24U, gy);
    put_le16(0x26U, gz);
    put_le16(0x28U, ax);
    put_le16(0x2AU, ay);
    put_le16(0x2CU, az);
}

static void init_ok(void)
{
    TEST_ASSERT_EQUAL_INT(IMU_ERR_OK, imu_init());
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
        s_wr_reg[i] = 0U;
        s_wr_len[i] = 0U;
    }
    s_regs[REG_STATUS] = STATUS_XLDA | STATUS_GDA;
    s_write_calls = 0U;
    s_write_fail_call = 0U;
    s_wr_calls = 0U;
    s_wr_fail_call = 0U;
    s_gpio_cfg_calls = 0U;
    s_gpio_cfg_ret = GPIO_OK;
    s_pin_level = GPIO_LEVEL_LOW;
    s_gpio_read_ret = GPIO_OK;
    s_exti_cfg_calls = 0U;
    s_exti_cfg_ret = EXTI_ERR_OK;
    s_exti_en_calls = 0U;
    s_exti_en_ret = EXTI_ERR_OK;
    s_cb_calls = 0U;
    s_cb_ctx = NULL;
    imu_reset_for_test();
}

void tearDown(void)
{
}

/* ===================================================================== */
/* Tests                                                                 */
/* ===================================================================== */

/* TC-IM-001: nominal read; distinct values prove gyro-first ordering and axis mapping. */
void test_TC_IM_001_nominal_read(void)
{
    imu_data_t d = sentinel_data();

    init_ok();
    set_axes(11, -22, 33, 1000, -2000, 16384);

    TEST_ASSERT_EQUAL_INT(IMU_ERR_OK, imu_read(&d));
    TEST_ASSERT_EQUAL_INT16(11, d.gyro_x);
    TEST_ASSERT_EQUAL_INT16(-22, d.gyro_y);
    TEST_ASSERT_EQUAL_INT16(33, d.gyro_z);
    TEST_ASSERT_EQUAL_INT16(1000, d.accel_x);
    TEST_ASSERT_EQUAL_INT16(-2000, d.accel_y);
    TEST_ASSERT_EQUAL_INT16(16384, d.accel_z);
    TEST_ASSERT_EQUAL_HEX8(REG_STATUS, s_wr_reg[0]);
    TEST_ASSERT_EQUAL_HEX8(0x22U, s_wr_reg[1]);
    TEST_ASSERT_EQUAL_UINT16(12U, s_wr_len[1]);
}

/* TC-IM-002: GDA clear -> NOT_READY, no data burst. */
void test_TC_IM_002_gda_clear(void)
{
    imu_data_t d = sentinel_data();

    init_ok();
    s_regs[REG_STATUS] = STATUS_XLDA;

    TEST_ASSERT_EQUAL_INT(IMU_ERR_NOT_READY, imu_read(&d));
    assert_unchanged(&d);
    TEST_ASSERT_EQUAL_UINT32(1U, s_wr_calls);
}

/* TC-IM-003: XLDA clear -> NOT_READY. */
void test_TC_IM_003_xlda_clear(void)
{
    imu_data_t d = sentinel_data();

    init_ok();
    s_regs[REG_STATUS] = STATUS_GDA;

    TEST_ASSERT_EQUAL_INT(IMU_ERR_NOT_READY, imu_read(&d));
    assert_unchanged(&d);
}

/* TC-IM-004: I2C failure on the 12-byte burst. */
void test_TC_IM_004_burst_i2c_failure(void)
{
    imu_data_t d = sentinel_data();

    init_ok();
    s_wr_fail_call = 2U;

    TEST_ASSERT_EQUAL_INT(IMU_ERR_I2C, imu_read(&d));
    assert_unchanged(&d);
}

/* TC-IM-005: NULL out pointer. */
void test_TC_IM_005_null_out(void)
{
    init_ok();
    TEST_ASSERT_EQUAL_INT(IMU_ERR_INVALID_ARG, imu_read(NULL));
}

/* TC-IM-006: maximum positive value in every position. */
void test_TC_IM_006_max_positive(void)
{
    imu_data_t d = sentinel_data();

    init_ok();
    set_axes(INT16_MAX, INT16_MAX, INT16_MAX, INT16_MAX, INT16_MAX, INT16_MAX);

    TEST_ASSERT_EQUAL_INT(IMU_ERR_OK, imu_read(&d));
    TEST_ASSERT_EQUAL_INT16(32767, d.gyro_x);
    TEST_ASSERT_EQUAL_INT16(32767, d.gyro_y);
    TEST_ASSERT_EQUAL_INT16(32767, d.gyro_z);
    TEST_ASSERT_EQUAL_INT16(32767, d.accel_x);
    TEST_ASSERT_EQUAL_INT16(32767, d.accel_y);
    TEST_ASSERT_EQUAL_INT16(32767, d.accel_z);
}

/* TC-IM-007: maximum negative value in every position. */
void test_TC_IM_007_max_negative(void)
{
    imu_data_t d = sentinel_data();

    init_ok();
    set_axes(INT16_MIN, INT16_MIN, INT16_MIN, INT16_MIN, INT16_MIN, INT16_MIN);

    TEST_ASSERT_EQUAL_INT(IMU_ERR_OK, imu_read(&d));
    TEST_ASSERT_EQUAL_INT16(-32768, d.gyro_x);
    TEST_ASSERT_EQUAL_INT16(-32768, d.gyro_y);
    TEST_ASSERT_EQUAL_INT16(-32768, d.gyro_z);
    TEST_ASSERT_EQUAL_INT16(-32768, d.accel_x);
    TEST_ASSERT_EQUAL_INT16(-32768, d.accel_y);
    TEST_ASSERT_EQUAL_INT16(-32768, d.accel_z);
}

/* TC-IM-008: attach with NULL callback. */
void test_TC_IM_008_attach_null_callback(void)
{
    init_ok();
    TEST_ASSERT_EQUAL_INT(IMU_ERR_INVALID_ARG, imu_attach_int1_callback(NULL, &s_ctx_token));
    TEST_ASSERT_EQUAL_UINT32(0U, s_exti_en_calls);
}

/* TC-IM-009: the ISR handler invokes the callback with the registered context. */
void test_TC_IM_009_isr_invokes_callback(void)
{
    init_ok();
    TEST_ASSERT_EQUAL_INT(IMU_ERR_OK, imu_attach_int1_callback(test_cb, &s_ctx_token));

    imu_int1_irq_handler();

    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_calls);
    TEST_ASSERT_EQUAL_PTR(&s_ctx_token, s_cb_ctx);
}

/* TC-IM-010: the ISR handler tolerates no callback. */
void test_TC_IM_010_isr_without_callback(void)
{
    imu_int1_irq_handler();
    TEST_ASSERT_EQUAL_UINT32(0U, s_cb_calls);
}

/* TC-IM-011: reset clears readiness and the callback. */
void test_TC_IM_011_reset_clears_state(void)
{
    imu_data_t d = sentinel_data();

    init_ok();
    TEST_ASSERT_EQUAL_INT(IMU_ERR_OK, imu_attach_int1_callback(test_cb, &s_ctx_token));
    imu_reset_for_test();

    TEST_ASSERT_EQUAL_INT(IMU_ERR_NOT_READY, imu_read(&d));
    imu_int1_irq_handler();
    TEST_ASSERT_EQUAL_UINT32(0U, s_cb_calls);
}

/* TC-IM-012: init writes the register values; ODR registers (which start conversion) last. */
void test_TC_IM_012_init_register_sequence(void)
{
    init_ok();

    TEST_ASSERT_EQUAL_UINT32(4U, s_write_calls);
    const uint8_t expect_reg[4] = {0x12U, 0x0DU, 0x10U, 0x11U};
    const uint8_t expect_val[4] = {0x44U, 0x03U, 0x40U, 0x40U};
    for (uint32_t i = 0U; i < 4U; i++)
    {
        TEST_ASSERT_EQUAL_HEX8(LSM6DSL_ADDR, s_write_addr[i]);
        TEST_ASSERT_EQUAL_HEX8(expect_reg[i], s_write_reg[i]);
        TEST_ASSERT_EQUAL_HEX8(expect_val[i], s_write_val[i]);
    }
}

/* TC-IM-013: init configures PD11 as a floating input and EXTI line 11 on a rising edge. */
void test_TC_IM_013_init_pin_and_exti(void)
{
    init_ok();

    TEST_ASSERT_EQUAL_UINT32(1U, s_gpio_cfg_calls);
    TEST_ASSERT_EQUAL_INT(GPIO_PORT_D, s_gpio_cfg.port);
    TEST_ASSERT_EQUAL_UINT8(11U, s_gpio_cfg.pin);
    TEST_ASSERT_EQUAL_INT(GPIO_MODE_INPUT, s_gpio_cfg.mode);
    TEST_ASSERT_EQUAL_INT(GPIO_PULL_NONE, s_gpio_cfg.pull);

    TEST_ASSERT_EQUAL_UINT32(1U, s_exti_cfg_calls);
    TEST_ASSERT_EQUAL_UINT8(11U, s_exti_cfg_line);
    TEST_ASSERT_EQUAL_INT(EXTI_PORT_D, s_exti_cfg_port);
    TEST_ASSERT_EQUAL_INT(EXTI_EDGE_RISING, s_exti_cfg_edge);
    TEST_ASSERT_EQUAL_UINT32(0U, s_exti_en_calls); /* Phase 1 must not unmask */
}

/* TC-IM-014: a failed register write aborts init before any pin work. */
void test_TC_IM_014_init_write_failure_at_each_step(void)
{
    for (uint32_t fail = 1U; fail <= 4U; fail++)
    {
        imu_data_t d = sentinel_data();

        setUp();
        s_write_fail_call = fail;

        TEST_ASSERT_EQUAL_INT(IMU_ERR_I2C, imu_init());
        TEST_ASSERT_EQUAL_UINT32(fail, s_write_calls);
        TEST_ASSERT_EQUAL_UINT32(0U, s_gpio_cfg_calls);
        TEST_ASSERT_EQUAL_INT(IMU_ERR_NOT_READY, imu_read(&d));
    }
}

/* TC-IM-015: GPIO configuration failure. */
void test_TC_IM_015_init_gpio_failure(void)
{
    s_gpio_cfg_ret = GPIO_ERR_NOT_INITIALISED;

    TEST_ASSERT_EQUAL_INT(IMU_ERR_HW, imu_init());
    TEST_ASSERT_EQUAL_UINT32(0U, s_exti_cfg_calls);
}

/* TC-IM-016: a second init (REQ-NF-205) tolerates an already-configured EXTI line. */
void test_TC_IM_016_reinit_accepts_exti_conflict(void)
{
    init_ok();
    s_exti_cfg_ret = EXTI_ERR_CONFLICT;

    TEST_ASSERT_EQUAL_INT(IMU_ERR_OK, imu_init());
}

/* TC-IM-017: any other EXTI error is a hardware failure. */
void test_TC_IM_017_init_exti_failure(void)
{
    imu_data_t d = sentinel_data();

    s_exti_cfg_ret = EXTI_ERR_INVALID_ARG;

    TEST_ASSERT_EQUAL_INT(IMU_ERR_HW, imu_init());
    TEST_ASSERT_EQUAL_INT(IMU_ERR_NOT_READY, imu_read(&d));
}

/* TC-IM-018: re-init keeps the registered callback. */
void test_TC_IM_018_reinit_keeps_callback(void)
{
    init_ok();
    TEST_ASSERT_EQUAL_INT(IMU_ERR_OK, imu_attach_int1_callback(test_cb, &s_ctx_token));
    s_exti_cfg_ret = EXTI_ERR_CONFLICT;
    TEST_ASSERT_EQUAL_INT(IMU_ERR_OK, imu_init());

    imu_int1_irq_handler();
    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_calls);
}

/* TC-IM-019: attach before init is refused and does not touch the EXTI. */
void test_TC_IM_019_attach_before_init(void)
{
    TEST_ASSERT_EQUAL_INT(IMU_ERR_NOT_READY, imu_attach_int1_callback(test_cb, &s_ctx_token));
    TEST_ASSERT_EQUAL_UINT32(0U, s_exti_en_calls);
    imu_int1_irq_handler();
    TEST_ASSERT_EQUAL_UINT32(0U, s_cb_calls);
}

/* TC-IM-020: attach unmasks EXTI line 11 at a FreeRTOS-safe priority. */
void test_TC_IM_020_attach_enables_exti(void)
{
    init_ok();

    TEST_ASSERT_EQUAL_INT(IMU_ERR_OK, imu_attach_int1_callback(test_cb, &s_ctx_token));
    TEST_ASSERT_EQUAL_UINT32(1U, s_exti_en_calls);
    TEST_ASSERT_EQUAL_UINT8(11U, s_exti_en_line);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(5U, s_exti_en_prio); /* >= configMAX_SYSCALL (5) */
}

/* TC-IM-021: EXTI enable failure is reported. */
void test_TC_IM_021_attach_exti_enable_failure(void)
{
    init_ok();
    s_exti_en_ret = EXTI_ERR_NOT_CONFIGURED;

    TEST_ASSERT_EQUAL_INT(IMU_ERR_HW, imu_attach_int1_callback(test_cb, &s_ctx_token));
}

/* TC-IM-022: INT1 low after arming -> no stale-release read. */
void test_TC_IM_022_attach_with_int1_low_does_not_read(void)
{
    init_ok();
    s_pin_level = GPIO_LEVEL_LOW;

    TEST_ASSERT_EQUAL_INT(IMU_ERR_OK, imu_attach_int1_callback(test_cb, &s_ctx_token));
    TEST_ASSERT_EQUAL_UINT32(0U, s_wr_calls);
}

/* TC-IM-023: INT1 already high -> output registers read once so a later edge can occur. */
void test_TC_IM_023_attach_with_int1_high_releases_line(void)
{
    init_ok();
    s_pin_level = GPIO_LEVEL_HIGH;

    TEST_ASSERT_EQUAL_INT(IMU_ERR_OK, imu_attach_int1_callback(test_cb, &s_ctx_token));
    TEST_ASSERT_EQUAL_UINT32(1U, s_wr_calls);
    TEST_ASSERT_EQUAL_HEX8(0x22U, s_wr_reg[0]);
    TEST_ASSERT_EQUAL_UINT16(12U, s_wr_len[0]);
    TEST_ASSERT_EQUAL_UINT32(1U, s_exti_en_calls); /* armed before the release */

    /* Failures while releasing are reported. */
    s_wr_fail_call = s_wr_calls + 1U;
    TEST_ASSERT_EQUAL_INT(IMU_ERR_I2C, imu_attach_int1_callback(test_cb, &s_ctx_token));
    s_wr_fail_call = 0U;
    s_gpio_read_ret = GPIO_ERR_INVALID_PIN;
    TEST_ASSERT_EQUAL_INT(IMU_ERR_HW, imu_attach_int1_callback(test_cb, &s_ctx_token));
}

/* TC-IM-024: the singleton vtable dispatches to the public functions. */
void test_TC_IM_024_vtable_dispatch(void)
{
    imu_data_t d = sentinel_data();

    TEST_ASSERT_NOT_NULL(imu_driver);
    TEST_ASSERT_TRUE(imu_driver->init == imu_init);
    TEST_ASSERT_TRUE(imu_driver->attach_int1_callback == imu_attach_int1_callback);
    TEST_ASSERT_TRUE(imu_driver->read == imu_read);

    TEST_ASSERT_EQUAL_INT(IMU_ERR_OK, imu_driver->init());
    set_axes(1, 2, 3, 4, 5, 6);
    TEST_ASSERT_EQUAL_INT(IMU_ERR_OK, imu_driver->read(&d));
    TEST_ASSERT_EQUAL_INT16(1, d.gyro_x);
    TEST_ASSERT_EQUAL_INT16(6, d.accel_z);
}
