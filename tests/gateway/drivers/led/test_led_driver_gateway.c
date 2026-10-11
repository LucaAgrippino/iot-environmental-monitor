/**
 * @file test_led_driver_gateway.c
 * @brief Unit tests for LedDriver, Gateway build (host, Unity).
 *
 * Test IDs follow docs/lld/drivers/led-driver-lld.md §8 (LED-T01..T27); LED-T28
 * onward cover guards the plan does not name.
 *
 * Mocking strategy:
 *   - GpioDriver -> gpio_driver_stub.h; the stub bodies are spies defined below
 *   - FreeRTOS   -> freertos_mock.h (auto-links freertos_mock.c); the timer
 *                   calls are counted by the mock and the blink callback is
 *                   invoked directly through the TEST hook.
 *
 * Runs under tests/project_gateway.yml because the Gateway and Field Device
 * drivers share the basename led_driver (see that file's header). The shared
 * FreeRTOS mock returns one dummy handle for every timer, so the callback is
 * only exercised on the first fitted LED.
 *
 * Build defines: STM32L475xx, BOARD_GATEWAY, TEST (:test_led_driver_gateway:).
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "gpio_driver_stub.h"

#include "FreeRTOS.h"
#include "freertos_mock.h" /* causes Ceedling to auto-link freertos_mock.c */

#include "led_driver.h"

/* ====================================================================== */
/* Pin tables                                                             */
/* ====================================================================== */

/** Field Device style: two fitted LEDs, active-low. */
static const led_pin_t k_pins_two_active_low[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_G, .pin = 6U, .active_high = false, .fitted = true},
    [LED_RED] = {.port = GPIO_PORT_D, .pin = 5U, .active_high = false, .fitted = true},
};

/** Gateway: LD2 on PB14, active-high; LD1 (PA5) not usable. */
static const led_pin_t k_pins_gw[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_B, .pin = 14U, .active_high = true, .fitted = true},
    [LED_RED] = {.port = GPIO_PORT_A, .pin = 5U, .active_high = true, .fitted = false},
};

/* ====================================================================== */
/* GpioDriver spies                                                       */
/* ====================================================================== */

#define SPY_MAX_CALLS (16U)

typedef struct
{
    gpio_port_t port;
    uint8_t pin;
    gpio_level_t level;
} write_call_t;

static gpio_pin_config_t g_cfg_calls[SPY_MAX_CALLS];
static uint8_t g_cfg_count;
static gpio_err_t g_cfg_ret;

static write_call_t g_write_calls[SPY_MAX_CALLS];
static uint8_t g_write_count;

gpio_err_t gpio_init(void)
{
    return GPIO_OK;
}

gpio_err_t gpio_configure_pin(const gpio_pin_config_t *config)
{
    if ((config != NULL) && (g_cfg_count < SPY_MAX_CALLS))
    {
        g_cfg_calls[g_cfg_count] = *config;
    }

    g_cfg_count++;

    return g_cfg_ret;
}

gpio_err_t gpio_read_pin(gpio_port_t port, uint8_t pin, gpio_level_t *out_level)
{
    (void) port;
    (void) pin;
    (void) out_level;

    return GPIO_OK;
}

gpio_err_t gpio_write_pin(gpio_port_t port, uint8_t pin, gpio_level_t level)
{
    if (g_write_count < SPY_MAX_CALLS)
    {
        g_write_calls[g_write_count].port = port;
        g_write_calls[g_write_count].pin = pin;
        g_write_calls[g_write_count].level = level;
    }

    g_write_count++;

    return GPIO_OK;
}

gpio_err_t gpio_toggle_pin(gpio_port_t port, uint8_t pin)
{
    (void) port;
    (void) pin;

    return GPIO_OK;
}

static void spies_reset(void)
{
    (void) memset(g_cfg_calls, 0, sizeof(g_cfg_calls));
    (void) memset(g_write_calls, 0, sizeof(g_write_calls));
    g_cfg_count = 0U;
    g_write_count = 0U;
    g_cfg_ret = GPIO_OK;
}

static const write_call_t *last_write(void)
{
    TEST_ASSERT_TRUE(g_write_count > 0U);

    return &g_write_calls[g_write_count - 1U];
}

/* ====================================================================== */
/* Fixture                                                                */
/* ====================================================================== */

void setUp(void)
{
    mock_freertos_reset();
    spies_reset();
    led_reset_for_test();

    /* led_reset_for_test() stops timers; start every test from zero counts. */
    g_mock_xTimerStop_call_count = 0U;
}

void tearDown(void)
{
}

static void init_two(void)
{
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_init(k_pins_two_active_low, LED_COUNT));
    spies_reset();
    mock_freertos_reset();
}

static void init_gw(void)
{
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_init(k_pins_gw, LED_COUNT));
    spies_reset();
    mock_freertos_reset();
}

/* ====================================================================== */
/* Group 1 — init lifecycle                                               */
/* ====================================================================== */

void test_LED_T01_init_two_fitted_leds_configures_both_and_drives_them_off(void)
{
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_init(k_pins_two_active_low, LED_COUNT));

    TEST_ASSERT_EQUAL_UINT8(2U, g_cfg_count);
    TEST_ASSERT_EQUAL_UINT8(2U, g_write_count);
    /* Active-low: off is the HIGH level. */
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, g_write_calls[0].level);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, g_write_calls[1].level);
    TEST_ASSERT_EQUAL_UINT32(2U, g_mock_xTimerCreateStatic_call_count);

    led_pattern_t pat = LED_PATTERN_ON;

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_get_pattern(LED_GREEN, &pat));
    TEST_ASSERT_EQUAL(LED_PATTERN_OFF, pat);
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_get_pattern(LED_RED, &pat));
    TEST_ASSERT_EQUAL(LED_PATTERN_OFF, pat);
}

void test_LED_T02_init_gateway_configures_only_the_fitted_led(void)
{
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_init(k_pins_gw, LED_COUNT));

    TEST_ASSERT_EQUAL_UINT8(1U, g_cfg_count);
    TEST_ASSERT_EQUAL(GPIO_PORT_B, g_cfg_calls[0].port);
    TEST_ASSERT_EQUAL_UINT8(14U, g_cfg_calls[0].pin);
    TEST_ASSERT_EQUAL(GPIO_MODE_OUTPUT, g_cfg_calls[0].mode);
    TEST_ASSERT_EQUAL(GPIO_OTYPE_PUSH_PULL, g_cfg_calls[0].otype);
    TEST_ASSERT_EQUAL(GPIO_SPEED_LOW, g_cfg_calls[0].speed);
    TEST_ASSERT_EQUAL(GPIO_PULL_NONE, g_cfg_calls[0].pull);

    TEST_ASSERT_EQUAL_UINT8(1U, g_write_count);
    /* Active-high: off is the LOW level. */
    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, g_write_calls[0].level);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTimerCreateStatic_call_count);
}

void test_LED_T03_init_null_table_is_rejected(void)
{
    TEST_ASSERT_EQUAL(LED_ERR_NULL_ARG, led_init(NULL, LED_COUNT));
    TEST_ASSERT_EQUAL_UINT8(0U, g_cfg_count);
    TEST_ASSERT_EQUAL_UINT8(0U, g_write_count);
}

void test_LED_T04_init_wrong_count_is_rejected(void)
{
    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, led_init(k_pins_gw, (uint8_t) (LED_COUNT - 1U)));
    TEST_ASSERT_EQUAL_UINT8(0U, g_cfg_count);
    TEST_ASSERT_EQUAL_UINT8(0U, g_write_count);
}

/* ====================================================================== */
/* Group 2 — solid patterns, active-low                                   */
/* ====================================================================== */

void test_LED_T05_on_active_low_drives_low_and_stops_the_timer(void)
{
    led_pattern_t pat;

    init_two();

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_ON));

    TEST_ASSERT_TRUE(g_mock_xTimerStop_call_count >= 1U);
    TEST_ASSERT_EQUAL(GPIO_PORT_G, last_write()->port);
    TEST_ASSERT_EQUAL_UINT8(6U, last_write()->pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, last_write()->level);
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_get_pattern(LED_GREEN, &pat));
    TEST_ASSERT_EQUAL(LED_PATTERN_ON, pat);
}

void test_LED_T06_off_active_low_drives_high_and_stops_the_timer(void)
{
    led_pattern_t pat;

    init_two();
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_ON));
    g_mock_xTimerStop_call_count = 0U;

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_OFF));

    TEST_ASSERT_TRUE(g_mock_xTimerStop_call_count >= 1U);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, last_write()->level);
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_get_pattern(LED_GREEN, &pat));
    TEST_ASSERT_EQUAL(LED_PATTERN_OFF, pat);
}

void test_LED_T07_second_led_uses_its_own_pin(void)
{
    init_two();

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_RED, LED_PATTERN_ON));

    TEST_ASSERT_TRUE(g_mock_xTimerStop_call_count >= 1U);
    TEST_ASSERT_EQUAL(GPIO_PORT_D, last_write()->port);
    TEST_ASSERT_EQUAL_UINT8(5U, last_write()->pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, last_write()->level);
}

/* ====================================================================== */
/* Group 3 — solid patterns, active-high (Gateway)                        */
/* ====================================================================== */

void test_LED_T08_on_active_high_drives_high(void)
{
    init_gw();

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_ON));

    TEST_ASSERT_TRUE(g_mock_xTimerStop_call_count >= 1U);
    TEST_ASSERT_EQUAL(GPIO_PORT_B, last_write()->port);
    TEST_ASSERT_EQUAL_UINT8(14U, last_write()->pin);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, last_write()->level);
}

void test_LED_T09_off_active_high_drives_low(void)
{
    init_gw();
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_ON));

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_OFF));

    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, last_write()->level);
}

/* ====================================================================== */
/* Group 4 — blink patterns                                               */
/* ====================================================================== */

void test_LED_T10_blink_fast_starts_on_with_a_100_ms_half_period(void)
{
    led_pattern_t pat;

    init_two();

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_BLINK_FAST));

    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, last_write()->level); /* on, active-low */
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTimerChangePeriod_call_count);
    TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(100U), g_mock_xTimerChangePeriod_last_period);
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_get_pattern(LED_GREEN, &pat));
    TEST_ASSERT_EQUAL(LED_PATTERN_BLINK_FAST, pat);
}

void test_LED_T11_blink_medium_uses_a_250_ms_half_period(void)
{
    init_two();

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_BLINK_MEDIUM));
    TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(250U), g_mock_xTimerChangePeriod_last_period);
}

void test_LED_T12_blink_slow_uses_a_500_ms_half_period(void)
{
    init_two();

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_BLINK_SLOW));
    TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(500U), g_mock_xTimerChangePeriod_last_period);
}

/* ====================================================================== */
/* Group 5 — timer callback                                               */
/* ====================================================================== */

void test_LED_T13_callback_toggles_the_led_off_then_T14_back_on(void)
{
    init_two();
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_BLINK_FAST));
    spies_reset();

    led_blink_callback(g_mock_xTimerCreateStatic_return);
    TEST_ASSERT_EQUAL_UINT8(1U, g_write_count);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, last_write()->level); /* off, active-low */

    led_blink_callback(g_mock_xTimerCreateStatic_return);
    TEST_ASSERT_EQUAL_UINT8(2U, g_write_count);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, last_write()->level); /* on again */
}

void test_LED_T15_residual_callback_after_a_solid_pattern_is_ignored(void)
{
    init_two();
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_BLINK_FAST));
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_ON));
    spies_reset();

    led_blink_callback(g_mock_xTimerCreateStatic_return);

    TEST_ASSERT_EQUAL_UINT8(0U, g_write_count);
}

void test_LED_T28_callback_with_an_unknown_timer_does_nothing(void)
{
    init_gw();
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_BLINK_SLOW));
    spies_reset();

    led_blink_callback((TimerHandle_t) 0x1234);

    TEST_ASSERT_EQUAL_UINT8(0U, g_write_count);
}

/* ====================================================================== */
/* Group 6 — pattern transitions                                          */
/* ====================================================================== */

void test_LED_T16_blink_to_solid_on_stops_the_timer(void)
{
    init_two();
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_BLINK_FAST));
    g_mock_xTimerStop_call_count = 0U;

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_ON));

    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTimerStop_call_count);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, last_write()->level);
}

void test_LED_T17_solid_to_blink_slow_starts_the_timer(void)
{
    init_two();
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_ON));

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_BLINK_SLOW));

    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, last_write()->level);
    TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(500U), g_mock_xTimerChangePeriod_last_period);
}

void test_LED_T18_blink_to_blink_restarts_the_timer_with_the_new_period(void)
{
    init_two();
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_BLINK_FAST));
    g_mock_xTimerChangePeriod_call_count = 0U;

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_BLINK_SLOW));

    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTimerChangePeriod_call_count);
    TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(500U), g_mock_xTimerChangePeriod_last_period);
    TEST_ASSERT_EQUAL(GPIO_LEVEL_LOW, last_write()->level);
}

/* ====================================================================== */
/* Group 7 — get pattern                                                  */
/* ====================================================================== */

void test_LED_T19_get_pattern_returns_solid_on(void)
{
    led_pattern_t pat = LED_PATTERN_OFF;

    init_two();
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_ON));

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_get_pattern(LED_GREEN, &pat));
    TEST_ASSERT_EQUAL(LED_PATTERN_ON, pat);
}

void test_LED_T20_get_pattern_returns_a_blink_pattern(void)
{
    led_pattern_t pat = LED_PATTERN_OFF;

    init_two();
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_BLINK_FAST));

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_get_pattern(LED_GREEN, &pat));
    TEST_ASSERT_EQUAL(LED_PATTERN_BLINK_FAST, pat);
}

void test_LED_T21_get_pattern_null_output_is_rejected(void)
{
    init_two();

    TEST_ASSERT_EQUAL(LED_ERR_NULL_ARG, led_get_pattern(LED_GREEN, NULL));
}

void test_LED_T29_get_pattern_rejects_unfitted_and_out_of_range_ids(void)
{
    led_pattern_t pat = LED_PATTERN_OFF;

    init_gw();

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, led_get_pattern(LED_RED, &pat));
    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, led_get_pattern(LED_COUNT, &pat));
}

/* ====================================================================== */
/* Group 8 — error paths                                                  */
/* ====================================================================== */

void test_LED_T22_gateway_red_led_is_not_fitted(void)
{
    init_gw();

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, led_set(LED_RED, LED_PATTERN_ON));
    TEST_ASSERT_EQUAL_UINT8(0U, g_write_count);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xTimerStop_call_count);
}

void test_LED_T23_out_of_range_id_is_rejected(void)
{
    init_two();

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_ID, led_set(LED_COUNT, LED_PATTERN_ON));
}

void test_LED_T24_set_before_init_is_rejected(void)
{
    TEST_ASSERT_EQUAL(LED_ERR_NOT_INIT, led_set(LED_GREEN, LED_PATTERN_ON));
    TEST_ASSERT_EQUAL_UINT8(0U, g_write_count);
}

void test_LED_T25_get_pattern_before_init_is_rejected(void)
{
    led_pattern_t pat;

    TEST_ASSERT_EQUAL(LED_ERR_NOT_INIT, led_get_pattern(LED_GREEN, &pat));
}

void test_LED_T26_pattern_out_of_range_is_rejected(void)
{
    init_two();

    TEST_ASSERT_EQUAL(LED_ERR_INVALID_PARAM, led_set(LED_GREEN, LED_PATTERN_COUNT));
    TEST_ASSERT_EQUAL_UINT8(0U, g_write_count);
}

/* ====================================================================== */
/* Group 9 — reset for test                                               */
/* ====================================================================== */

void test_LED_T27_reset_stops_timers_and_allows_reinit(void)
{
    init_two();
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_BLINK_FAST));
    g_mock_xTimerStop_call_count = 0U;

    led_reset_for_test();

    TEST_ASSERT_EQUAL_UINT32(2U, g_mock_xTimerStop_call_count);
    TEST_ASSERT_EQUAL(LED_ERR_NOT_INIT, led_set(LED_GREEN, LED_PATTERN_ON));
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_init(k_pins_gw, LED_COUNT));
}

/* ====================================================================== */
/* Added during implementation                                            */
/* ====================================================================== */

void test_LED_T30_second_init_is_idempotent(void)
{
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_init(k_pins_gw, LED_COUNT));
    spies_reset();
    mock_freertos_reset();

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_init(k_pins_gw, LED_COUNT));

    TEST_ASSERT_EQUAL_UINT8(0U, g_cfg_count);
    TEST_ASSERT_EQUAL_UINT8(0U, g_write_count);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xTimerCreateStatic_call_count);
}

void test_LED_T31_gpio_configure_failure_leaves_the_driver_uninitialised(void)
{
    g_cfg_ret = GPIO_ERR_NOT_INITIALISED;

    TEST_ASSERT_EQUAL(LED_ERR_NOT_INIT, led_init(k_pins_gw, LED_COUNT));
    TEST_ASSERT_EQUAL(LED_ERR_NOT_INIT, led_set(LED_GREEN, LED_PATTERN_ON));

    /* A later init, once GPIO is up, succeeds from a clean state. */
    g_cfg_ret = GPIO_OK;
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_init(k_pins_gw, LED_COUNT));
}

void test_LED_T32_timer_creation_failure_leaves_the_driver_uninitialised(void)
{
    g_mock_xTimerCreateStatic_return = NULL;

    TEST_ASSERT_EQUAL(LED_ERR_NOT_INIT, led_init(k_pins_gw, LED_COUNT));
    TEST_ASSERT_EQUAL(LED_ERR_NOT_INIT, led_set(LED_GREEN, LED_PATTERN_ON));
}

void test_LED_T33_blink_never_starts_a_timer_for_a_solid_pattern(void)
{
    init_gw();

    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_ON));
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_set(LED_GREEN, LED_PATTERN_OFF));

    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xTimerChangePeriod_call_count);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xTimerStart_call_count);
}

void test_LED_T34_init_only_creates_timers_for_fitted_leds(void)
{
    TEST_ASSERT_EQUAL(LED_ERR_OK, led_init(k_pins_gw, LED_COUNT));

    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTimerCreateStatic_call_count);
}
