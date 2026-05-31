/**
 * @file led_driver.c
 * @brief LedDriver implementation for STM32F469 (Field Device) and
 *        STM32L475 (Gateway).
 *
 * GPIO access is delegated entirely to GpioDriver; no CMSIS registers are
 * touched here. Active-high polarity is hardcoded in the compile-time pin
 * table (P1: polarity knowledge stays in the driver).
 *
 * Layout:
 *   §1  Includes and configuration
 *   §2  Private types
 *   §3  Pin table (compile-time constant)
 *   §4  Module state
 *   §5  Internal helpers
 *   §6  Public API
 *   §7  Singleton vtable
 *   §8  Test-only hooks
 */

#include "led_driver.h"

#include <stdbool.h>
#include <stdint.h>

#include "gpio/gpio_driver.h"

/* ===================================================================== */
/* §1. Configuration                                                     */
/* ===================================================================== */

#if !defined(STM32F469xx) && !defined(STM32L475xx)
#error "LedDriver: define STM32F469xx (Field Device) or STM32L475xx (Gateway)."
#endif

/* ===================================================================== */
/* §2. Private types                                                     */
/* ===================================================================== */

/** Maps a logical LED identifier to the GpioDriver pin descriptor. */
typedef struct
{
    gpio_port_t port; /**< GPIO port. */
    uint8_t pin;      /**< Pin number, 0..15. */
    bool fitted;      /**< False when this LED is absent on this board variant. */
} led_pin_t;

/* ===================================================================== */
/* §3. Pin table (compile-time constant)                                 */
/* ===================================================================== */

/*
 * Active-high on both discovery boards (UM1932 §6.5, UM2153 §6.9).
 * A false 'fitted' entry marks an LED absent on this board variant;
 * public functions return LED_ERR_INVALID_ID for those entries.
 */
#if defined(STM32F469xx)
static const led_pin_t s_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_G, .pin = 6U, .fitted = true},
    [LED_RED] = {.port = GPIO_PORT_D, .pin = 5U, .fitted = true},
};
#else /* STM32L475xx */
static const led_pin_t s_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_B, .pin = 14U, .fitted = true},
    [LED_RED] = {.port = GPIO_PORT_A, .pin = 5U, .fitted = false},
};
#endif

/* ===================================================================== */
/* §4. Module state                                                      */
/* ===================================================================== */

typedef struct
{
    bool initialised; /**< Set true by led_init(); guards against duplicate init. */
} led_driver_t;

static led_driver_t s_led;

/* ===================================================================== */
/* §5. Internal helpers                                                  */
/* ===================================================================== */

/**
 * @brief Validate an led_id_t argument.
 *
 * Returns LED_ERR_INVALID_ID if id is out of range or not fitted on the
 * active board variant.
 */
LED_TEST_VISIBLE led_err_t validate_id(led_id_t id)
{
    if ((uint8_t) id >= (uint8_t) LED_COUNT)
    {
        return LED_ERR_INVALID_ID;
    }
    if (!s_led_pins[(uint8_t) id].fitted)
    {
        return LED_ERR_INVALID_ID;
    }
    return LED_ERR_OK;
}

/* ===================================================================== */
/* §6. Public API                                                        */
/* ===================================================================== */

led_err_t led_init(void)
{
    if (s_led.initialised)
    {
        return LED_ERR_OK;
    }

    for (uint8_t i = 0U; i < (uint8_t) LED_COUNT; i++)
    {
        if (!s_led_pins[i].fitted)
        {
            continue;
        }

        gpio_pin_config_t cfg = {
            .port = s_led_pins[i].port,
            .pin = s_led_pins[i].pin,
            .mode = GPIO_MODE_OUTPUT,
            .otype = GPIO_OTYPE_PUSH_PULL,
            .speed = GPIO_SPEED_MEDIUM,
            .pull = GPIO_PULL_NONE,
            .alternate = 0U,
        };
        (void) gpio_configure_pin(&cfg);
        (void) gpio_write_pin(s_led_pins[i].port, s_led_pins[i].pin, GPIO_LEVEL_HIGH);
    }

    s_led.initialised = true;
    return LED_ERR_OK;
}

led_err_t led_on(led_id_t id)
{
    led_err_t err = validate_id(id);
    if (err != LED_ERR_OK)
    {
        return err;
    }
    (void) gpio_write_pin(s_led_pins[(uint8_t) id].port, s_led_pins[(uint8_t) id].pin,
                          GPIO_LEVEL_LOW);
    return LED_ERR_OK;
}

led_err_t led_off(led_id_t id)
{
    led_err_t err = validate_id(id);
    if (err != LED_ERR_OK)
    {
        return err;
    }
    (void) gpio_write_pin(s_led_pins[(uint8_t) id].port, s_led_pins[(uint8_t) id].pin,
                          GPIO_LEVEL_HIGH);
    return LED_ERR_OK;
}

led_err_t led_toggle(led_id_t id)
{
    led_err_t err = validate_id(id);
    if (err != LED_ERR_OK)
    {
        return err;
    }
    (void) gpio_toggle_pin(s_led_pins[(uint8_t) id].port, s_led_pins[(uint8_t) id].pin);
    return LED_ERR_OK;
}

/* ===================================================================== */
/* §7. Singleton vtable                                                  */
/* ===================================================================== */

static const iled_t s_led_vtable = {
    .init = led_init,
    .on = led_on,
    .off = led_off,
    .toggle = led_toggle,
};

const iled_t *const led_driver = &s_led_vtable;

/* ===================================================================== */
/* §8. Test-only hooks                                                   */
/* ===================================================================== */

#ifdef TEST
void led_reset_for_test(void)
{
    s_led.initialised = false;
}
#endif
