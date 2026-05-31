/**
 * @file test_led_driver_gw.c
 * @brief Unit tests for LedDriver — Gateway (STM32L475) build.
 *
 * Covers T-LED-01 (GW variant) and T-LED-05
 * per docs/lld/drivers/led-driver.md §7.
 *
 * GpioDriver calls are intercepted via stubs defined inline below.
 * The Gateway board has LED_GREEN (PB14) only; LED_RED is not fitted.
 *
 * Build: STM32L475xx must be defined (project.yml :test_led_driver_gw:).
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

#define GPIO_SPY_MAX_CALLS (8U)

static gpio_pin_config_t  g_configure_calls[GPIO_SPY_MAX_CALLS];
static uint8_t            g_configure_call_count;

static gpio_write_call_t  g_write_calls[GPIO_SPY_MAX_CALLS];
static uint8_t            g_write_call_count;

/* ===================================================================== */
/* GPIO stubs                                                            */
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
    (void)port;
    (void)pin;
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
    (void)memset(g_configure_calls, 0, sizeof(g_configure_calls));
    (void)memset(g_write_calls,     0, sizeof(g_write_calls));
}

void tearDown(void)
{
    /* State reset in setUp(). */
}

/* ===================================================================== */
/* T-LED-01 (GW): led_init configures only the green LED                */
/* ===================================================================== */

void test_T_LED_01_gw_init_configures_only_green_led_on_pb14(void)
{
    led_err_t err = led_init();

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);

    /* GW has one fitted LED → one configure call */
    TEST_ASSERT_EQUAL_UINT8(1U, g_configure_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_B, g_configure_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(14U,   g_configure_calls[0].pin);

    /* Green LED driven LOW (off) */
    TEST_ASSERT_EQUAL_UINT8(1U, g_write_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_B,   g_write_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(14U,     g_write_calls[0].pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, g_write_calls[0].level);
}

/* ===================================================================== */
/* T-LED-05: LED_RED not fitted on GW → LED_ERR_INVALID_ID              */
/* ===================================================================== */

void test_T_LED_05_led_on_red_gw_returns_invalid_id_no_gpio_call(void)
{
    (void)led_init();
    uint8_t write_before = g_write_call_count;

    led_err_t err = led_on(LED_RED);

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, err);
    TEST_ASSERT_EQUAL_UINT8(write_before, g_write_call_count);
}

void test_T_LED_05_led_off_red_gw_returns_invalid_id_no_gpio_call(void)
{
    (void)led_init();
    uint8_t write_before = g_write_call_count;

    led_err_t err = led_off(LED_RED);

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, err);
    TEST_ASSERT_EQUAL_UINT8(write_before, g_write_call_count);
}

void test_T_LED_05_led_toggle_red_gw_returns_invalid_id(void)
{
    (void)led_init();

    led_err_t err = led_toggle(LED_RED);

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, err);
}
