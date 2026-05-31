/**
 * @file test_led_driver_gw.c
 * @brief Unit tests for LedDriver — Gateway (STM32L475) pin table.
 *
 * Covers TC-LED-GW-01..05 per docs/lld/drivers/led-driver.md §7.
 *
 * GpioDriver calls are intercepted via stubs defined inline below.
 * The Gateway board has LED_GREEN (PB14, active-high) fitted only;
 * LED_RED is not fitted.
 *
 * Build: STM32L475xx and TEST must be defined (project.yml :test_led_driver_gw:).
 */

#include "unity.h"

#include <string.h>
#include <stdint.h>

#include "gpio_driver_stub.h"
#include "led_driver.h"

/* ===================================================================== */
/* Gateway pin table                                                     */
/* ===================================================================== */

static const led_pin_t k_test_pins_gw[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_B, .pin = 14U, .active_high = true,  .fitted = true},
    [LED_RED]   = {.port = GPIO_PORT_A, .pin =  5U, .active_high = true,  .fitted = false},
};

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

static gpio_pin_config_t g_configure_calls[GPIO_SPY_MAX_CALLS];
static uint8_t           g_configure_call_count;

static gpio_write_call_t g_write_calls[GPIO_SPY_MAX_CALLS];
static uint8_t           g_write_call_count;

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
    (void) port;
    (void) pin;
    return GPIO_OK;
}

gpio_err_t gpio_read_pin(gpio_port_t port, uint8_t pin, gpio_level_t *out_level)
{
    (void) port;
    (void) pin;
    (void) out_level;
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
    (void) memset(g_configure_calls, 0, sizeof(g_configure_calls));
    (void) memset(g_write_calls,     0, sizeof(g_write_calls));
}

void tearDown(void)
{
    /* State reset in setUp(). */
}

/* ===================================================================== */
/* TC-LED-GW-01: init configures only the fitted green LED              */
/* ===================================================================== */

void test_TC_LED_GW_01_init_configures_only_green_led_and_drives_active_high_off(void)
{
    led_err_t err = led_init(k_test_pins_gw, LED_COUNT);

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);

    /* GW has one fitted LED → one configure call. */
    TEST_ASSERT_EQUAL_UINT8(1U, g_configure_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_B, g_configure_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(14U,   g_configure_calls[0].pin);

    /* active_high = true → "off" = GPIO_LEVEL_LOW. */
    TEST_ASSERT_EQUAL_UINT8(1U, g_write_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_B,   g_write_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(14U,     g_write_calls[0].pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, g_write_calls[0].level);
}

/* ===================================================================== */
/* TC-LED-GW-02: led_on(LED_GREEN) drives PB14 HIGH (active-high)       */
/* ===================================================================== */

void test_TC_LED_GW_02_led_on_green_drives_pb14_high(void)
{
    (void) led_init(k_test_pins_gw, LED_COUNT);
    g_write_call_count = 0U;

    led_err_t err = led_on(LED_GREEN);

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT8(1U, g_write_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_B,     g_write_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(14U,       g_write_calls[0].pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, g_write_calls[0].level);
}

/* ===================================================================== */
/* TC-LED-GW-03: led_off(LED_GREEN) drives PB14 LOW (active-high)       */
/* ===================================================================== */

void test_TC_LED_GW_03_led_off_green_drives_pb14_low(void)
{
    (void) led_init(k_test_pins_gw, LED_COUNT);
    g_write_call_count = 0U;

    led_err_t err = led_off(LED_GREEN);

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT8(1U, g_write_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_B,   g_write_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(14U,     g_write_calls[0].pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, g_write_calls[0].level);
}

/* ===================================================================== */
/* TC-LED-GW-04: LED_RED not fitted — returns LED_ERR_INVALID_ID        */
/* ===================================================================== */

void test_TC_LED_GW_04_led_on_red_returns_invalid_id_no_gpio_call(void)
{
    (void) led_init(k_test_pins_gw, LED_COUNT);
    uint8_t write_before = g_write_call_count;

    led_err_t err = led_on(LED_RED);

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, err);
    TEST_ASSERT_EQUAL_UINT8(write_before, g_write_call_count);
}

void test_TC_LED_GW_04_led_off_red_returns_invalid_id_no_gpio_call(void)
{
    (void) led_init(k_test_pins_gw, LED_COUNT);
    uint8_t write_before = g_write_call_count;

    led_err_t err = led_off(LED_RED);

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, err);
    TEST_ASSERT_EQUAL_UINT8(write_before, g_write_call_count);
}

/* ===================================================================== */
/* TC-LED-GW-05: led_toggle from on state drives PB14 LOW               */
/* ===================================================================== */

void test_TC_LED_GW_05_led_toggle_green_from_on_drives_pb14_low(void)
{
    (void) led_init(k_test_pins_gw, LED_COUNT);
    (void) led_on(LED_GREEN);   /* put green into ON state */
    g_write_call_count = 0U;

    led_err_t err = led_toggle(LED_GREEN);

    /* toggle OFF: active_high = true → GPIO_LEVEL_LOW. */
    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT8(1U, g_write_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_B,   g_write_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(14U,     g_write_calls[0].pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, g_write_calls[0].level);
}
