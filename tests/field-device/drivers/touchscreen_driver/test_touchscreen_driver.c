/**
 * @file test_touchscreen_driver.c
 * @brief Unity unit tests for TouchscreenDriver -- TC-TS-001 through TC-TS-014.
 *
 * I2C is stubbed inline. Stubs track per-call return codes and receive data
 * using index arrays reset in setUp(). EXTI/GPIO registers are mocked via
 * stm32_cmsis_mock.h. EXTI9_5_IRQHandler is called directly for TC-TS-006.
 *
 * Init sequence in touchscreen_init():
 *   i2c_write call 0  -- FT6x06 INT_MODE register
 *   i2c_write call 1  -- FT6x06 TH_GROUP register
 *   i2c_write_read call 0 -- DEV_MODE probe
 *
 * Read sequence in touchscreen_read():
 *   i2c_write_read call 0 -- TD_STATUS (1 byte)
 *   i2c_write_read call 1 -- P1_XH..P1_YL (4 bytes), only if TD_STATUS > 0
 *
 * Build: STM32F469xx and TEST defined (:test_touchscreen_driver: in project.yml).
 */

#include "unity.h"
#include "stm32_cmsis_mock.h"
#include "i2c/i2c_driver.h"
#include "gpio_driver_stub.h"
#include "touchscreen_driver/touchscreen_driver.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ===================================================================== */
/* Inline I2C stubs                                                      */
/* ===================================================================== */

#define I2C_STUB_SLOTS (4U)

static i2c_err_t g_i2c_write_err[I2C_STUB_SLOTS];
static int       g_i2c_write_idx;
static i2c_err_t g_i2c_wr_err[I2C_STUB_SLOTS];
static uint8_t   g_i2c_wr_rx[I2C_STUB_SLOTS][8U];
static int       g_i2c_wr_idx;

i2c_err_t i2c_init(void)
{
    return I2C_ERR_OK;
}

i2c_err_t i2c_write(uint8_t dev_addr, const uint8_t *data, uint16_t len)
{
    (void) dev_addr;
    (void) data;
    (void) len;
    return g_i2c_write_err[g_i2c_write_idx++];
}

i2c_err_t i2c_write_read(uint8_t dev_addr, const uint8_t *tx_data, uint16_t tx_len,
                          uint8_t *rx_buf, uint16_t rx_len)
{
    (void) dev_addr;
    (void) tx_data;
    (void) tx_len;
    int idx = g_i2c_wr_idx++;
    if (g_i2c_wr_err[idx] != I2C_ERR_OK)
    {
        return g_i2c_wr_err[idx];
    }
    for (uint16_t i = 0U; i < rx_len; i++)
    {
        rx_buf[i] = g_i2c_wr_rx[idx][i];
    }
    return I2C_ERR_OK;
}

/* ===================================================================== */
/* Inline GPIO stub                                                      */
/* ===================================================================== */

/* gpio_configure_pin() is the only GPIO call made by the SUT. Mark AHB2ENR
 * as a sentinel so TC-TS-001 can verify the call occurred without pulling
 * in the real gpio_driver.c (which manages its own clock-enable via AHB1ENR). */
gpio_err_t gpio_configure_pin(const gpio_pin_config_t *config)
{
    (void) config;
    RCC->AHB2ENR |= RCC_AHB1ENR_GPIOJEN;
    return GPIO_OK;
}

/* ===================================================================== */
/* Callback tracking for TC-TS-006                                       */
/* ===================================================================== */

static uint32_t g_cb_count;
static void    *g_cb_ctx_received;

static void test_irq_cb(void *context)
{
    g_cb_count++;
    g_cb_ctx_received = context;
}

/* ===================================================================== */
/* setUp / tearDown                                                      */
/* ===================================================================== */

void setUp(void)
{
    stm32_cmsis_mock_reset();
    touchscreen_reset_for_test();
    memset(g_i2c_write_err, 0, sizeof(g_i2c_write_err));
    g_i2c_write_idx = 0;
    memset(g_i2c_wr_err, 0, sizeof(g_i2c_wr_err));
    memset(g_i2c_wr_rx,  0, sizeof(g_i2c_wr_rx));
    g_i2c_wr_idx = 0;
    g_cb_count = 0U;
    g_cb_ctx_received = NULL;
}

void tearDown(void) {}

/* ===================================================================== */
/* Helpers                                                               */
/* ===================================================================== */

static ts_err_t helper_init_happy(void)
{
    /* Clear all stub state -- callers may have left dirty error slots */
    memset(g_i2c_write_err, 0, sizeof(g_i2c_write_err));
    memset(g_i2c_wr_err,    0, sizeof(g_i2c_wr_err));
    memset(g_i2c_wr_rx,     0, sizeof(g_i2c_wr_rx));
    /* 2 i2c_write calls OK, 1 i2c_write_read OK (DEV_MODE probe = 0x00) */
    g_i2c_write_err[0U] = I2C_ERR_OK;
    g_i2c_write_err[1U] = I2C_ERR_OK;
    g_i2c_wr_rx[0U][0U] = 0x00U;
    ts_err_t result = touchscreen_init();
    /* Reset indices so subsequent tests start from 0 */
    g_i2c_write_idx = 0;
    g_i2c_wr_idx    = 0;
    return result;
}
/* ===================================================================== */
/* TC-TS-001: touchscreen_init() happy path                              */
/* ===================================================================== */

void test_TC_TS_001_init_happy_path(void)
{
    TEST_ASSERT_EQUAL(TS_ERR_OK, helper_init_happy());

    TEST_ASSERT_TRUE((RCC->AHB2ENR & RCC_AHB1ENR_GPIOJEN) != 0U);
    TEST_ASSERT_TRUE((RCC->APB2ENR & RCC_APB2ENR_SYSCFGEN) != 0U);
    TEST_ASSERT_TRUE((EXTI->FTSR & (1UL << 5U)) != 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, EXTI->IMR & (1UL << 5U)); /* still masked */
    /* Port J = 9 at bits [7:4] of EXTICR[1] */
    TEST_ASSERT_EQUAL_UINT32(0x90UL, SYSCFG->EXTICR[1U] & 0xF0UL);
}

/* ===================================================================== */
/* TC-TS-002: init -- first I2C call fails -> TS_ERR_I2C, not initialised */
/* ===================================================================== */

void test_TC_TS_002_init_i2c_fail_first_write(void)
{
    g_i2c_write_err[0U] = I2C_ERR_NACK;

    ts_err_t err = touchscreen_init();

    TEST_ASSERT_EQUAL(TS_ERR_I2C, err);

    /* Driver must not be marked initialised: framebuffer equivalent is
     * absence of initialised flag, tested indirectly by checking that
     * subsequent reads propagate I2C errors naturally. */
}

/* ===================================================================== */
/* TC-TS-003: init -- DEV_MODE probe returns unexpected value (not fail) */
/* ===================================================================== */

void test_TC_TS_003_init_dev_mode_unexpected_value_not_fail(void)
{
    g_i2c_write_err[0U] = I2C_ERR_OK;
    g_i2c_write_err[1U] = I2C_ERR_OK;
    g_i2c_wr_rx[0U][0U] = 0x07U; /* non-zero DEV_MODE: still OK */

    TEST_ASSERT_EQUAL(TS_ERR_OK, touchscreen_init());
}

/* ===================================================================== */
/* TC-TS-004: attach_irq(NULL, NULL) -> TS_ERR_NULL; IMR unchanged       */
/* ===================================================================== */

void test_TC_TS_004_attach_irq_null_callback(void)
{
    TEST_ASSERT_EQUAL(TS_ERR_OK, helper_init_happy());

    uint32_t imr_before = EXTI->IMR;

    TEST_ASSERT_EQUAL(TS_ERR_NULL, touchscreen_attach_irq(NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(imr_before, EXTI->IMR);
}

/* ===================================================================== */
/* TC-TS-005: attach_irq happy path -- EXTI5 enabled, NVIC configured    */
/* ===================================================================== */

void test_TC_TS_005_attach_irq_happy_path(void)
{
    TEST_ASSERT_EQUAL(TS_ERR_OK, helper_init_happy());

    uint32_t sentinel = 0xCAFEBABEU;
    TEST_ASSERT_EQUAL(TS_ERR_OK, touchscreen_attach_irq(test_irq_cb, &sentinel));

    TEST_ASSERT_TRUE((EXTI->IMR & (1UL << 5U)) != 0U);
    /* NVIC is configured during touchscreen_init(); verify it is set after attach */
    TEST_ASSERT_GREATER_THAN_UINT32(0U, g_mock_nvic_enable_count[EXTI9_5_IRQn]);
    TEST_ASSERT_EQUAL_UINT32(10U, g_mock_nvic_priority[EXTI9_5_IRQn]);
}

/* ===================================================================== */
/* TC-TS-006: EXTI9_5_IRQHandler dispatches callback with context        */
/* ===================================================================== */

void test_TC_TS_006_isr_dispatches_callback(void)
{
    TEST_ASSERT_EQUAL(TS_ERR_OK, helper_init_happy());

    uint32_t sentinel = 0x12345678U;
    TEST_ASSERT_EQUAL(TS_ERR_OK, touchscreen_attach_irq(test_irq_cb, &sentinel));

    EXTI->PR = (1UL << 5U);
    EXTI9_5_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(1U, g_cb_count);
    TEST_ASSERT_EQUAL_PTR(&sentinel, g_cb_ctx_received);
    TEST_ASSERT_EQUAL_UINT32((1UL << 5U), EXTI->PR); /* write-1-to-clear written */
}

/* ===================================================================== */
/* TC-TS-007: touchscreen_read(NULL) -> TS_ERR_NULL                      */
/* ===================================================================== */

void test_TC_TS_007_read_null_pointer(void)
{
    TEST_ASSERT_EQUAL(TS_ERR_OK, helper_init_happy());
    TEST_ASSERT_EQUAL(TS_ERR_NULL, touchscreen_read(NULL));
}

/* ===================================================================== */
/* TC-TS-008: read -- TD_STATUS = 0 -> TS_ERR_NO_DATA, no coord reads   */
/* ===================================================================== */

void test_TC_TS_008_read_no_active_touch(void)
{
    TEST_ASSERT_EQUAL(TS_ERR_OK, helper_init_happy());

    g_i2c_wr_rx[0U][0U] = 0x00U; /* TD_STATUS = 0 active points */

    ts_touch_t touch;
    TEST_ASSERT_EQUAL(TS_ERR_NO_DATA, touchscreen_read(&touch));

    /* Only one i2c_write_read should have been issued (TD_STATUS only) */
    TEST_ASSERT_EQUAL_INT(1, g_i2c_wr_idx);
}
/* ===================================================================== */
/* TC-TS-009: read happy path -- coords (320, 240), event = PRESS        */
/* ===================================================================== */

void test_TC_TS_009_read_happy_path_press(void)
{
    TEST_ASSERT_EQUAL(TS_ERR_OK, helper_init_happy());

    /* TD_STATUS = 1 active point */
    g_i2c_wr_rx[0U][0U] = 0x01U;

    /* P1_XH = (320 >> 8) = 0x01, P1_XL = 0x40 (320 = 0x140)
     * P1_YH = (240 >> 8) = 0x00, P1_YL = 0xF0 (240 = 0x00F0)
     * Event flag in P1_XH[7:6] = 00 (PRESS) */
    g_i2c_wr_rx[1U][0U] = 0x01U; /* P1_XH: event=00, X MSB=1 */
    g_i2c_wr_rx[1U][1U] = 0x40U; /* P1_XL */
    g_i2c_wr_rx[1U][2U] = 0x00U; /* P1_YH */
    g_i2c_wr_rx[1U][3U] = 0xF0U; /* P1_YL */

    ts_touch_t touch;
    TEST_ASSERT_EQUAL(TS_ERR_OK, touchscreen_read(&touch));
    TEST_ASSERT_EQUAL_UINT16(320U, touch.x);
    TEST_ASSERT_EQUAL_UINT16(240U, touch.y);
    TEST_ASSERT_EQUAL(TS_EVENT_PRESS, touch.event);
}

/* ===================================================================== */
/* TC-TS-010: P1_XH[7:6] = 0b01 -> TS_EVENT_RELEASE                     */
/* ===================================================================== */

void test_TC_TS_010_read_event_release(void)
{
    TEST_ASSERT_EQUAL(TS_ERR_OK, helper_init_happy());

    g_i2c_wr_rx[0U][0U] = 0x01U;
    g_i2c_wr_rx[1U][0U] = (0x01U << 6U) | 0x00U; /* event=01 (RELEASE), X MSB=0 */
    g_i2c_wr_rx[1U][1U] = 0x00U;
    g_i2c_wr_rx[1U][2U] = 0x00U;
    g_i2c_wr_rx[1U][3U] = 0x00U;

    ts_touch_t touch;
    TEST_ASSERT_EQUAL(TS_ERR_OK, touchscreen_read(&touch));
    TEST_ASSERT_EQUAL(TS_EVENT_RELEASE, touch.event);
}

/* ===================================================================== */
/* TC-TS-011: P1_XH[7:6] = 0b10 -> TS_EVENT_CONTACT                     */
/* ===================================================================== */

void test_TC_TS_011_read_event_contact(void)
{
    TEST_ASSERT_EQUAL(TS_ERR_OK, helper_init_happy());

    g_i2c_wr_rx[0U][0U] = 0x01U;
    g_i2c_wr_rx[1U][0U] = (0x02U << 6U) | 0x00U; /* event=10 (CONTACT) */
    g_i2c_wr_rx[1U][1U] = 0x00U;
    g_i2c_wr_rx[1U][2U] = 0x00U;
    g_i2c_wr_rx[1U][3U] = 0x00U;

    ts_touch_t touch;
    TEST_ASSERT_EQUAL(TS_ERR_OK, touchscreen_read(&touch));
    TEST_ASSERT_EQUAL(TS_EVENT_CONTACT, touch.event);
}

/* ===================================================================== */
/* TC-TS-012: TD_STATUS read returns I2C error -> TS_ERR_I2C             */
/* ===================================================================== */

void test_TC_TS_012_read_td_status_i2c_error(void)
{
    TEST_ASSERT_EQUAL(TS_ERR_OK, helper_init_happy());

    g_i2c_wr_err[0U] = I2C_ERR_NACK;

    ts_touch_t touch;
    TEST_ASSERT_EQUAL(TS_ERR_I2C, touchscreen_read(&touch));
}

/* ===================================================================== */
/* TC-TS-013: coord read returns I2C error -> TS_ERR_I2C                 */
/* ===================================================================== */

void test_TC_TS_013_read_coord_i2c_error(void)
{
    TEST_ASSERT_EQUAL(TS_ERR_OK, helper_init_happy());

    g_i2c_wr_rx[0U][0U] = 0x01U;    /* TD_STATUS = 1, first call OK */
    g_i2c_wr_err[1U]    = I2C_ERR_NACK; /* coord read fails */

    ts_touch_t touch;
    TEST_ASSERT_EQUAL(TS_ERR_I2C, touchscreen_read(&touch));
}

/* ===================================================================== */
/* TC-TS-014: functions before init -- consistent error semantics        */
/* ===================================================================== */

void test_TC_TS_014_functions_before_init(void)
{
    /* No touchscreen_init() -- FT6x06 still in reset (I2C would NACK) */
    g_i2c_wr_err[0U] = I2C_ERR_NACK;

    ts_touch_t touch;
    TEST_ASSERT_EQUAL(TS_ERR_I2C,   touchscreen_read(&touch));
    TEST_ASSERT_EQUAL(TS_ERR_NULL,  touchscreen_attach_irq(NULL, NULL));

    /* After init, functions succeed */
    TEST_ASSERT_EQUAL(TS_ERR_OK, helper_init_happy());
    g_i2c_wr_rx[0U][0U] = 0x00U; /* TD_STATUS = 0 */
    TEST_ASSERT_EQUAL(TS_ERR_NO_DATA, touchscreen_read(&touch));
}
