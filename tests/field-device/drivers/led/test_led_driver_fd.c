/**
 * @file test_led_driver_fd.c
 * @brief Unit tests for LedDriver — Field Device (STM32F469) pin table.
 *
 * Covers TC-LED-FD-01..06 and TC-LED-CMN-01..04
 * per docs/lld/drivers/led-driver.md §7.
 *
 * GpioDriver calls are intercepted via stubs defined inline below.
 * The test passes its own static pin table to led_init() — no board-
 * conditional #if in this file.
 *
 * Build: STM32F469xx and TEST must be defined (project.yml :test_led_driver_fd:).
 */

#include "unity.h"

#include <string.h>
#include <stdint.h>

#include "gpio_driver_stub.h"
#include "led_driver.h"

/* ===================================================================== */
/* Field Device pin table                                                */
/* ===================================================================== */

static const led_pin_t k_test_pins_fd[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_G, .pin =  6U, .active_high = false, .fitted = true},
    [LED_RED]   = {.port = GPIO_PORT_D, .pin =  5U, .active_high = false, .fitted = true},
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
/* TC-LED-FD-01: led_init configures both fitted LEDs and sets off      */
/* ===================================================================== */

void test_TC_LED_FD_01_init_configures_both_leds_and_drives_active_low_off(void)
{
    led_err_t err = led_init(k_test_pins_fd, LED_COUNT);

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);

    /* Both LEDs present on FD → two configure calls. */
    TEST_ASSERT_EQUAL_UINT8(2U, g_configure_call_count);

    /* active_high = false → "off" = GPIO_LEVEL_HIGH for both. */
    TEST_ASSERT_EQUAL_UINT8(2U, g_write_call_count);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, g_write_calls[0].level);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, g_write_calls[1].level);
}

/* ===================================================================== */
/* TC-LED-FD-02: led_on(LED_GREEN) drives PG6 LOW (active-low)          */
/* ===================================================================== */

void test_TC_LED_FD_02_led_on_green_drives_pg6_low(void)
{
    (void) led_init(k_test_pins_fd, LED_COUNT);
    g_write_call_count = 0U;

    led_err_t err = led_on(LED_GREEN);

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT8(1U, g_write_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_G,    g_write_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(6U,       g_write_calls[0].pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, g_write_calls[0].level);
}

/* ===================================================================== */
/* TC-LED-FD-03: led_off(LED_GREEN) drives PG6 HIGH (active-low)        */
/* ===================================================================== */

void test_TC_LED_FD_03_led_off_green_drives_pg6_high(void)
{
    (void) led_init(k_test_pins_fd, LED_COUNT);
    g_write_call_count = 0U;

    led_err_t err = led_off(LED_GREEN);

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT8(1U, g_write_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_G,     g_write_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(6U,        g_write_calls[0].pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, g_write_calls[0].level);
}

/* ===================================================================== */
/* TC-LED-FD-04: led_toggle(LED_GREEN) from off state drives PG6 LOW   */
/* ===================================================================== */

void test_TC_LED_FD_04_led_toggle_green_from_off_drives_pg6_low(void)
{
    /* After init, LED_GREEN is in OFF state. */
    (void) led_init(k_test_pins_fd, LED_COUNT);
    g_write_call_count = 0U;

    led_err_t err = led_toggle(LED_GREEN);

    /* toggle uses gpio_write_pin (state-based), not gpio_toggle_pin. */
    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT8(1U, g_write_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_G,    g_write_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(6U,       g_write_calls[0].pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, g_write_calls[0].level);
}

/* ===================================================================== */
/* TC-LED-FD-05: led_on(LED_RED) drives PD5 LOW (active-low)            */
/* ===================================================================== */

void test_TC_LED_FD_05_led_on_red_drives_pd5_low(void)
{
    (void) led_init(k_test_pins_fd, LED_COUNT);
    g_write_call_count = 0U;

    led_err_t err = led_on(LED_RED);

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT8(1U, g_write_call_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_D,    g_write_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(5U,       g_write_calls[0].pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, g_write_calls[0].level);
}

/* ===================================================================== */
/* TC-LED-FD-06: led_get_state after led_on returns LED_STATE_ON        */
/* ===================================================================== */

void test_TC_LED_FD_06_led_get_state_returns_on_after_led_on(void)
{
    led_state_t state = LED_STATE_OFF;

    (void) led_init(k_test_pins_fd, LED_COUNT);
    (void) led_on(LED_GREEN);

    led_err_t err = led_get_state(LED_GREEN, &state);

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL(LED_STATE_ON, state);
}

void test_TC_LED_FD_06_led_get_state_returns_off_after_led_off(void)
{
    led_state_t state = LED_STATE_ON;

    (void) led_init(k_test_pins_fd, LED_COUNT);
    (void) led_on(LED_GREEN);
    (void) led_off(LED_GREEN);

    led_err_t err = led_get_state(LED_GREEN, &state);

    TEST_ASSERT_EQUAL(LED_ERR_OK, err);
    TEST_ASSERT_EQUAL(LED_STATE_OFF, state);
}

/* ===================================================================== */
/* TC-LED-CMN-01: led_init(NULL, LED_COUNT) returns LED_ERR_NULL_ARG   */
/* ===================================================================== */

void test_TC_LED_CMN_01_init_null_pins_returns_null_arg(void)
{
    led_err_t err = led_init(NULL, LED_COUNT);

    TEST_ASSERT_EQUAL(LED_ERR_NULL_ARG, err);
    TEST_ASSERT_EQUAL_UINT8(0U, g_configure_call_count);
}

/* ===================================================================== */
/* TC-LED-CMN-02: wrong count returns LED_ERR_INVALID_ID                */
/* ===================================================================== */

void test_TC_LED_CMN_02_init_wrong_count_returns_invalid_id(void)
{
    led_err_t err = led_init(k_test_pins_fd, (uint8_t)(LED_COUNT - 1U));

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, err);
    TEST_ASSERT_EQUAL_UINT8(0U, g_configure_call_count);
}

/* ===================================================================== */
/* TC-LED-CMN-03: led_on(LED_COUNT) returns LED_ERR_INVALID_ID          */
/* ===================================================================== */

void test_TC_LED_CMN_03_led_on_out_of_range_returns_invalid_id(void)
{
    (void) led_init(k_test_pins_fd, LED_COUNT);
    uint8_t write_before = g_write_call_count;

    led_err_t err = led_on((led_id_t) LED_COUNT);

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, err);
    TEST_ASSERT_EQUAL_UINT8(write_before, g_write_call_count);
}

void test_TC_LED_CMN_03_led_off_out_of_range_returns_invalid_id(void)
{
    (void) led_init(k_test_pins_fd, LED_COUNT);
    uint8_t write_before = g_write_call_count;

    led_err_t err = led_off((led_id_t) LED_COUNT);

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, err);
    TEST_ASSERT_EQUAL_UINT8(write_before, g_write_call_count);
}

void test_TC_LED_CMN_03_led_toggle_out_of_range_returns_invalid_id(void)
{
    (void) led_init(k_test_pins_fd, LED_COUNT);
    uint8_t write_before = g_write_call_count;

    led_err_t err = led_toggle((led_id_t) LED_COUNT);

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, err);
    TEST_ASSERT_EQUAL_UINT8(write_before, g_write_call_count);
}

/* ===================================================================== */
/* TC-LED-CMN-04: any API call before led_init returns LED_ERR_NOT_INIT */
/* ===================================================================== */

void test_TC_LED_CMN_04_led_on_before_init_returns_not_init(void)
{
    /* setUp() calls led_reset_for_test() — driver is uninitialised. */
    uint8_t write_before = g_write_call_count;

    led_err_t err = led_on(LED_GREEN);

    TEST_ASSERT_EQUAL(LED_ERR_NOT_INIT, err);
    TEST_ASSERT_EQUAL_UINT8(write_before, g_write_call_count);
}

void test_TC_LED_CMN_04_led_off_before_init_returns_not_init(void)
{
    led_err_t err = led_off(LED_GREEN);

    TEST_ASSERT_EQUAL(LED_ERR_NOT_INIT, err);
}

void test_TC_LED_CMN_04_led_toggle_before_init_returns_not_init(void)
{
    led_err_t err = led_toggle(LED_GREEN);

    TEST_ASSERT_EQUAL(LED_ERR_NOT_INIT, err);
}

void test_TC_LED_CMN_04_led_get_state_before_init_returns_not_init(void)
{
    led_state_t state;
    led_err_t   err = led_get_state(LED_GREEN, &state);

    TEST_ASSERT_EQUAL(LED_ERR_NOT_INIT, err);
}
