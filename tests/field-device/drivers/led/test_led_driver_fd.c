/**
 * @file test_led_driver_fd.c
 * @brief Unit tests for LedDriver — Field Device (STM32F469) build.
 *
 * Covers T-LED-01, T-LED-02, T-LED-03, T-LED-04, T-LED-06, T-LED-07
 * per docs/lld/drivers/led-driver.md §7.
 *
 * GpioDriver calls are intercepted via stubs defined inline below.
 * The stubs record call counts, ports, pins, and levels so that
 * assertions can verify the exact GpioDriver interactions.
 *
 * Build: STM32F469xx must be defined (project.yml :test_led_driver_fd:).
 */

#include "unity.h"

#include <string.h>
#include <stdint.h>

#include "gpio_driver_stub.h"
#include "led_driver.h"

/* ===================================================================== */
/* GPIO spy state                                                        */
/* ===================================================================== */

typedef struct
{
    gpio_port_t  port;
    uint8_t      pin;
    gpio_level_t level;
} gpio_write_call_t;

typedef struct
{
    gpio_port_t port;
    uint8_t     pin;
} gpio_toggle_call_t;

#define GPIO_SPY_MAX_CALLS (8U)

static gpio_pin_config_t  g_configure_calls[GPIO_SPY_MAX_CALLS];
static uint8_t            g_configure_call_count;

static gpio_write_call_t  g_write_calls[GPIO_SPY_MAX_CALLS];
static uint8_t            g_write_call_count;

static gpio_toggle_call_t g_toggle_calls[GPIO_SPY_MAX_CALLS];
static uint8_t            g_toggle_call_count;

/* ===================================================================== */
/* GPIO stubs (satisfy led_driver.c link-time references)               */
/* ===================================================================== */

gpio_err_t gpio_init(void)
{
    return GPIO_OK;
}

gpio_err_t gpio_configure_pin(const gpio_pin_config_t *config)
{
    if (g_configure_call_count < GPIO_SPY_MAX_CALLS)
    {
        g_configure_calls[g_configure_call_count] = *config;
    }
    g_configure_call_count++;
    return GPIO_OK;
}

gpio_err_t gpio_write_pin(gpio_port_t port, uint8_t pin, gpio_level_t level)
{
    if (g_write_call_count < GPIO_SPY_MAX_CALLS)
    {
        g_write_calls[g_write_call_count].port  = port;
        g_write_calls[g_write_call_count].pin   = pin;
        g_write_calls[g_write_call_count].level = level;
    }
    g_write_call_count++;
    return GPIO_OK;
}

gpio_err_t gpio_toggle_pin(gpio_port_t port, uint8_t pin)
{
    if (g_toggle_call_count < GPIO_SPY_MAX_CALLS)
    {
        g_toggle_calls[g_toggle_call_count].port = port;
        g_toggle_calls[g_toggle_call_count].pin  = pin;
    }
    g_toggle_call_count++;
    return GPIO_OK;
}

gpio_err_t gpio_read_pin(gpio_port_t port, uint8_t pin, gpio_level_t *out_level)
{
    (void)port;
    (void)pin;
    (void)out_level;
    return GPIO_OK;
}

/* ===================================================================== */
/* setUp / tearDown                                                      */
/* ===================================================================== */

void setUp(void)
{
    led_reset_for_test();
    g_configure_call_count = 0U;
    g_write_call_count     = 0U;
    g_toggle_call_count    = 0U;
    (void)memset(g_configure_calls, 0, sizeof(g_configure_calls));
    (void)memset(g_write_calls,     0, sizeof(g_write_calls));
    (void)memset(g_toggle_calls,    0, sizeof(g_toggle_calls));
}

void tearDown(void)
{
    /* State reset in setUp(). */
}

/* ===================================================================== */
/* T-LED-01: led_init configures all fitted LEDs and sets initial state */
/* ===================================================================== */

void test_T_LED_01_init_configures_both_fitted_leds_and_drives_off(void)
{
    /* Act */
    led_err_t err = led_init();

    /* Assert: returns OK */
    TEST_ASSERT_EQUAL(LED_ERR_OK, err);

    /* Both LEDs present on FD → two configure calls */
    TEST_ASSERT_EQUAL_UINT8(2U, g_configure_call_count);

    /* Both LEDs driven LOW (off) during init */
    TEST_ASSERT_EQUAL_UINT8(2U, g_write_call_count);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, g_write_calls[0].level);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, g_write_calls[1].level);
}

void test_T_LED_01_init_is_idempotent_on_second_call(void)
{
    (void)led_init();
    uint8_t configure_after_first = g_configure_call_count;
    uint8_t write_after_first     = g_write_call_count;

    led_err_t err = led_init();

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT8(configure_after_first, g_configure_call_count);
    TEST_ASSERT_EQUAL_UINT8(write_after_first,     g_write_call_count);
}

/* ===================================================================== */
/* T-LED-02: led_on(LED_GREEN) drives PG13 HIGH                         */
/* ===================================================================== */

void test_T_LED_02_led_on_green_drives_pg13_high(void)
{
    (void)led_init();
    g_write_call_count = 0U;

    led_err_t err = led_on(LED_GREEN);

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT8(1U, g_write_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_G,    g_write_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(6U,      g_write_calls[0].pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, g_write_calls[0].level);
}

/* ===================================================================== */
/* T-LED-03: led_off(LED_GREEN) drives PG13 LOW                         */
/* ===================================================================== */

void test_T_LED_03_led_off_green_drives_pg13_low(void)
{
    (void)led_init();
    g_write_call_count = 0U;

    led_err_t err = led_off(LED_GREEN);

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT8(1U, g_write_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_G,   g_write_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(6U,     g_write_calls[0].pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, g_write_calls[0].level);
}

/* ===================================================================== */
/* T-LED-04: led_toggle(LED_GREEN) calls gpio_toggle on PG13            */
/* ===================================================================== */

void test_T_LED_04_led_toggle_green_calls_gpio_toggle_on_pg13(void)
{
    (void)led_init();

    led_err_t err = led_toggle(LED_GREEN);

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT8(1U, g_toggle_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_G, g_toggle_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(6U,   g_toggle_calls[0].pin);
}

/* ===================================================================== */
/* T-LED-06: led_on(LED_RED) on FD drives PD5 HIGH                      */
/* ===================================================================== */

void test_T_LED_06_led_on_red_fd_drives_pd5_high(void)
{
    (void)led_init();
    g_write_call_count = 0U;

    led_err_t err = led_on(LED_RED);

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT8(1U, g_write_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_D,    g_write_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(5U,       g_write_calls[0].pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, g_write_calls[0].level);
}

void test_T_LED_06_led_off_red_fd_drives_pd5_low(void)
{
    (void)led_init();
    g_write_call_count = 0U;

    led_err_t err = led_off(LED_RED);

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT8(1U, g_write_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_D,   g_write_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(5U,      g_write_calls[0].pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, g_write_calls[0].level);
}

void test_T_LED_06_led_toggle_red_fd_calls_gpio_toggle_on_pd5(void)
{
    (void)led_init();

    led_err_t err = led_toggle(LED_RED);

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT8(1U, g_toggle_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_D, g_toggle_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(5U,    g_toggle_calls[0].pin);
}

/* ===================================================================== */
/* T-LED-07: out-of-range id (LED_COUNT) returns INVALID_ID             */
/* ===================================================================== */

void test_T_LED_07_led_on_with_led_count_returns_invalid_id_no_gpio(void)
{
    (void)led_init();
    uint8_t write_before = g_write_call_count;

    led_err_t err = led_on((led_id_t)LED_COUNT);

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, err);
    TEST_ASSERT_EQUAL_UINT8(write_before, g_write_call_count);
}

void test_T_LED_07_led_off_with_led_count_returns_invalid_id_no_gpio(void)
{
    (void)led_init();
    uint8_t write_before = g_write_call_count;

    led_err_t err = led_off((led_id_t)LED_COUNT);

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, err);
    TEST_ASSERT_EQUAL_UINT8(write_before, g_write_call_count);
}

void test_T_LED_07_led_toggle_with_led_count_returns_invalid_id_no_gpio(void)
{
    (void)led_init();
    uint8_t toggle_before = g_toggle_call_count;

    led_err_t err = led_toggle((led_id_t)LED_COUNT);

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, err);
    TEST_ASSERT_EQUAL_UINT8(toggle_before, g_toggle_call_count);
}
