/**
 * @file test_magnetometer_driver.c
 * @brief Unity unit tests for MagnetometerDriver — Gateway (LIS3MDL over I2C2).
 *
 * The II2c vtable, GpioDriver and ExtiDriver are replaced by fakes defined in this file
 * (the stub headers keep the real driver .c files out of the link). The I2C fake models
 * the LIS3MDL: a multi-byte read only auto-increments when bit 7 of the register address
 * is set, otherwise it returns the same register repeatedly.
 *
 * Test IDs: TC-MG-001..010 (companion §13) plus TC-MG-011..024 for the register sequence,
 * pin / EXTI configuration, two-phase init edge cases and the stale-DRDY release.
 */

#include "unity.h"

#include "exti_driver_stub.h"
#include "gpio_driver_stub.h"
#include "magnetometer_driver.h"
#include "magnetometer_driver_internal.h"

/* ===================================================================== */
/* Fake I2C bus                                                          */
/* ===================================================================== */

#define FAKE_REG_COUNT (256U)
#define FAKE_LOG_DEPTH (12U)
#define LIS3MDL_ADDR (0x1EU)
#define REG_STATUS (0x27U)
#define STATUS_ZYXDA (0x08U)
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
    const uint8_t reg = (uint8_t) (tx_data[0] & 0x7FU);
    const bool auto_inc = (tx_data[0] & 0x80U) != 0U;

    TEST_ASSERT_EQUAL_HEX8(LIS3MDL_ADDR, dev_addr);
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
        rx_buf[i] = s_regs[auto_inc ? (reg + i) : reg];
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
    TEST_ASSERT_EQUAL_INT(GPIO_PORT_C, port);
    TEST_ASSERT_EQUAL_UINT8(8U, pin);
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

static magnetometer_data_t sentinel_data(void)
{
    magnetometer_data_t d = {.x = SENTINEL, .y = SENTINEL, .z = SENTINEL};
    return d;
}

static void assert_unchanged(const magnetometer_data_t *d)
{
    TEST_ASSERT_EQUAL_INT16(SENTINEL, d->x);
    TEST_ASSERT_EQUAL_INT16(SENTINEL, d->y);
    TEST_ASSERT_EQUAL_INT16(SENTINEL, d->z);
}

static void set_axes(int16_t x, int16_t y, int16_t z)
{
    s_regs[0x28] = (uint8_t) ((uint16_t) x & 0xFFU);
    s_regs[0x29] = (uint8_t) (((uint16_t) x >> 8) & 0xFFU);
    s_regs[0x2A] = (uint8_t) ((uint16_t) y & 0xFFU);
    s_regs[0x2B] = (uint8_t) (((uint16_t) y >> 8) & 0xFFU);
    s_regs[0x2C] = (uint8_t) ((uint16_t) z & 0xFFU);
    s_regs[0x2D] = (uint8_t) (((uint16_t) z >> 8) & 0xFFU);
}

static void init_ok(void)
{
    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_OK, magnetometer_init());
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
    s_regs[REG_STATUS] = STATUS_ZYXDA;
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
    magnetometer_reset_for_test();
}

void tearDown(void)
{
}

/* ===================================================================== */
/* Tests                                                                 */
/* ===================================================================== */

/* TC-MG-001: nominal read; distinct values prove the axis order. */
void test_TC_MG_001_nominal_read(void)
{
    magnetometer_data_t d = sentinel_data();

    init_ok();
    set_axes(0x1234, -2000, 0x0102);

    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_OK, magnetometer_read(&d));
    TEST_ASSERT_EQUAL_INT16(0x1234, d.x);
    TEST_ASSERT_EQUAL_INT16(-2000, d.y);
    TEST_ASSERT_EQUAL_INT16(0x0102, d.z);
}

/* TC-MG-002: ZYXDA clear -> NOT_READY, no data burst. */
void test_TC_MG_002_zyxda_clear(void)
{
    magnetometer_data_t d = sentinel_data();

    init_ok();
    s_regs[REG_STATUS] = 0x07U; /* every other status bit set, ZYXDA clear */

    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_NOT_READY, magnetometer_read(&d));
    assert_unchanged(&d);
    TEST_ASSERT_EQUAL_UINT32(1U, s_wr_calls);
}

/* TC-MG-003: I2C failure on the STATUS_REG read. */
void test_TC_MG_003_status_read_i2c_failure(void)
{
    magnetometer_data_t d = sentinel_data();

    init_ok();
    s_wr_fail_call = 1U;

    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_I2C, magnetometer_read(&d));
    assert_unchanged(&d);
}

/* TC-MG-004: I2C failure on the data burst. */
void test_TC_MG_004_burst_i2c_failure(void)
{
    magnetometer_data_t d = sentinel_data();

    init_ok();
    s_wr_fail_call = 2U;

    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_I2C, magnetometer_read(&d));
    assert_unchanged(&d);
}

/* TC-MG-005: NULL out pointer. */
void test_TC_MG_005_null_out(void)
{
    init_ok();
    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_INVALID_ARG, magnetometer_read(NULL));
}

/* TC-MG-006: negative axes are sign-extended; extremes survive. */
void test_TC_MG_006_sign_extension_and_extremes(void)
{
    magnetometer_data_t d = sentinel_data();

    init_ok();
    set_axes(-1, INT16_MAX, INT16_MIN);

    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_OK, magnetometer_read(&d));
    TEST_ASSERT_EQUAL_INT16(-1, d.x);
    TEST_ASSERT_EQUAL_INT16(32767, d.y);
    TEST_ASSERT_EQUAL_INT16(-32768, d.z);
}

/* TC-MG-007: attach with NULL callback. */
void test_TC_MG_007_attach_null_callback(void)
{
    init_ok();
    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_INVALID_ARG,
                          magnetometer_attach_drdy_callback(NULL, &s_ctx_token));
    TEST_ASSERT_EQUAL_UINT32(0U, s_exti_en_calls);
}

/* TC-MG-008: the ISR handler invokes the callback with the registered context. */
void test_TC_MG_008_isr_invokes_callback(void)
{
    init_ok();
    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_OK,
                          magnetometer_attach_drdy_callback(test_cb, &s_ctx_token));

    magnetometer_drdy_irq_handler();

    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_calls);
    TEST_ASSERT_EQUAL_PTR(&s_ctx_token, s_cb_ctx);
}

/* TC-MG-009: the ISR handler tolerates no callback. */
void test_TC_MG_009_isr_without_callback(void)
{
    magnetometer_drdy_irq_handler();
    TEST_ASSERT_EQUAL_UINT32(0U, s_cb_calls);
}

/* TC-MG-010: reset clears readiness and the callback. */
void test_TC_MG_010_reset_clears_state(void)
{
    magnetometer_data_t d = sentinel_data();

    init_ok();
    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_OK,
                          magnetometer_attach_drdy_callback(test_cb, &s_ctx_token));
    magnetometer_reset_for_test();

    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_NOT_READY, magnetometer_read(&d));
    magnetometer_drdy_irq_handler();
    TEST_ASSERT_EQUAL_UINT32(0U, s_cb_calls);
}

/* TC-MG-011: init writes the corrected register values; CTRL_REG3 (leave power-down) last. */
void test_TC_MG_011_init_register_sequence(void)
{
    init_ok();

    TEST_ASSERT_EQUAL_UINT32(5U, s_write_calls); /* no INT_CFG write */
    const uint8_t expect_reg[5] = {0x20U, 0x21U, 0x23U, 0x24U, 0x22U};
    const uint8_t expect_val[5] = {0x70U, 0x00U, 0x0CU, 0x40U, 0x00U};
    for (uint32_t i = 0U; i < 5U; i++)
    {
        TEST_ASSERT_EQUAL_HEX8(LIS3MDL_ADDR, s_write_addr[i]);
        TEST_ASSERT_EQUAL_HEX8(expect_reg[i], s_write_reg[i]);
        TEST_ASSERT_EQUAL_HEX8(expect_val[i], s_write_val[i]);
    }
}

/* TC-MG-012: init configures PC8 as a floating input and EXTI line 8 on a rising edge. */
void test_TC_MG_012_init_pin_and_exti(void)
{
    init_ok();

    TEST_ASSERT_EQUAL_UINT32(1U, s_gpio_cfg_calls);
    TEST_ASSERT_EQUAL_INT(GPIO_PORT_C, s_gpio_cfg.port);
    TEST_ASSERT_EQUAL_UINT8(8U, s_gpio_cfg.pin);
    TEST_ASSERT_EQUAL_INT(GPIO_MODE_INPUT, s_gpio_cfg.mode);
    TEST_ASSERT_EQUAL_INT(GPIO_PULL_NONE, s_gpio_cfg.pull);

    TEST_ASSERT_EQUAL_UINT32(1U, s_exti_cfg_calls);
    TEST_ASSERT_EQUAL_UINT8(8U, s_exti_cfg_line);
    TEST_ASSERT_EQUAL_INT(EXTI_PORT_C, s_exti_cfg_port);
    TEST_ASSERT_EQUAL_INT(EXTI_EDGE_RISING, s_exti_cfg_edge);
    TEST_ASSERT_EQUAL_UINT32(0U, s_exti_en_calls); /* Phase 1 must not unmask */
}

/* TC-MG-013: a failed register write aborts init before any pin work. */
void test_TC_MG_013_init_write_failure_at_each_step(void)
{
    for (uint32_t fail = 1U; fail <= 5U; fail++)
    {
        magnetometer_data_t d = sentinel_data();

        setUp();
        s_write_fail_call = fail;

        TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_I2C, magnetometer_init());
        TEST_ASSERT_EQUAL_UINT32(fail, s_write_calls);
        TEST_ASSERT_EQUAL_UINT32(0U, s_gpio_cfg_calls);
        TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_NOT_READY, magnetometer_read(&d));
    }
}

/* TC-MG-014: GPIO configuration failure. */
void test_TC_MG_014_init_gpio_failure(void)
{
    s_gpio_cfg_ret = GPIO_ERR_NOT_INITIALISED;

    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_HW, magnetometer_init());
    TEST_ASSERT_EQUAL_UINT32(0U, s_exti_cfg_calls);
}

/* TC-MG-015: a second init (REQ-NF-205) tolerates an already-configured EXTI line. */
void test_TC_MG_015_reinit_accepts_exti_conflict(void)
{
    init_ok();
    s_exti_cfg_ret = EXTI_ERR_CONFLICT;

    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_OK, magnetometer_init());
}

/* TC-MG-016: any other EXTI error is a hardware failure. */
void test_TC_MG_016_init_exti_failure(void)
{
    magnetometer_data_t d = sentinel_data();

    s_exti_cfg_ret = EXTI_ERR_INVALID_ARG;

    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_HW, magnetometer_init());
    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_NOT_READY, magnetometer_read(&d));
}

/* TC-MG-017: re-init keeps the registered callback. */
void test_TC_MG_017_reinit_keeps_callback(void)
{
    init_ok();
    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_OK,
                          magnetometer_attach_drdy_callback(test_cb, &s_ctx_token));
    s_exti_cfg_ret = EXTI_ERR_CONFLICT;
    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_OK, magnetometer_init());

    magnetometer_drdy_irq_handler();
    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_calls);
}

/* TC-MG-018: attach before init is refused and does not touch the EXTI. */
void test_TC_MG_018_attach_before_init(void)
{
    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_NOT_READY,
                          magnetometer_attach_drdy_callback(test_cb, &s_ctx_token));
    TEST_ASSERT_EQUAL_UINT32(0U, s_exti_en_calls);
    magnetometer_drdy_irq_handler();
    TEST_ASSERT_EQUAL_UINT32(0U, s_cb_calls);
}

/* TC-MG-019: attach unmasks EXTI line 8 at a FreeRTOS-safe priority. */
void test_TC_MG_019_attach_enables_exti(void)
{
    init_ok();

    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_OK,
                          magnetometer_attach_drdy_callback(test_cb, &s_ctx_token));
    TEST_ASSERT_EQUAL_UINT32(1U, s_exti_en_calls);
    TEST_ASSERT_EQUAL_UINT8(8U, s_exti_en_line);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(5U, s_exti_en_prio); /* >= configMAX_SYSCALL (5) */
}

/* TC-MG-020: EXTI enable failure is reported. */
void test_TC_MG_020_attach_exti_enable_failure(void)
{
    init_ok();
    s_exti_en_ret = EXTI_ERR_NOT_CONFIGURED;

    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_HW,
                          magnetometer_attach_drdy_callback(test_cb, &s_ctx_token));
}

/* TC-MG-021: DRDY low after arming -> no stale-release read. */
void test_TC_MG_021_attach_with_drdy_low_does_not_read(void)
{
    init_ok();
    s_pin_level = GPIO_LEVEL_LOW;

    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_OK,
                          magnetometer_attach_drdy_callback(test_cb, &s_ctx_token));
    TEST_ASSERT_EQUAL_UINT32(0U, s_wr_calls);
}

/* TC-MG-022: DRDY already high -> output registers read once so a later edge can occur. */
void test_TC_MG_022_attach_with_drdy_high_releases_line(void)
{
    init_ok();
    s_pin_level = GPIO_LEVEL_HIGH;

    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_OK,
                          magnetometer_attach_drdy_callback(test_cb, &s_ctx_token));
    TEST_ASSERT_EQUAL_UINT32(1U, s_wr_calls);
    TEST_ASSERT_EQUAL_HEX8(0xA8U, s_wr_reg[0]); /* OUT_X_L | auto-increment */
    TEST_ASSERT_EQUAL_UINT16(6U, s_wr_len[0]);
    TEST_ASSERT_EQUAL_UINT32(1U, s_exti_en_calls); /* armed before the release */
}

/* TC-MG-023: failures while releasing a stale DRDY are reported. */
void test_TC_MG_023_stale_release_failures(void)
{
    init_ok();
    s_pin_level = GPIO_LEVEL_HIGH;
    s_wr_fail_call = 1U;
    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_I2C,
                          magnetometer_attach_drdy_callback(test_cb, &s_ctx_token));

    s_wr_fail_call = 0U;
    s_gpio_read_ret = GPIO_ERR_INVALID_PIN;
    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_HW,
                          magnetometer_attach_drdy_callback(test_cb, &s_ctx_token));
}

/* TC-MG-024: the burst read sets address bit 7; the singleton vtable dispatches. */
void test_TC_MG_024_burst_uses_autoincrement_and_vtable(void)
{
    magnetometer_data_t d = sentinel_data();

    TEST_ASSERT_NOT_NULL(magnetometer_driver);
    TEST_ASSERT_TRUE(magnetometer_driver->init == magnetometer_init);
    TEST_ASSERT_TRUE(magnetometer_driver->attach_drdy_callback ==
                     magnetometer_attach_drdy_callback);
    TEST_ASSERT_TRUE(magnetometer_driver->read == magnetometer_read);

    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_OK, magnetometer_driver->init());
    set_axes(100, 200, 300);
    TEST_ASSERT_EQUAL_INT(MAGNETOMETER_ERR_OK, magnetometer_driver->read(&d));

    TEST_ASSERT_EQUAL_HEX8(REG_STATUS, s_wr_reg[0]);
    TEST_ASSERT_EQUAL_HEX8(0xA8U, s_wr_reg[1]);
    TEST_ASSERT_EQUAL_INT16(100, d.x);
    TEST_ASSERT_EQUAL_INT16(200, d.y);
    TEST_ASSERT_EQUAL_INT16(300, d.z);
}
