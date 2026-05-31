/**
 * @file led_driver.c
 * @brief LedDriver implementation.
 *
 * GPIO access is delegated entirely to GpioDriver; no CMSIS registers are
 * touched here. Active-level polarity is read from the caller-supplied pin
 * table at runtime — the driver contains no board-conditional #if directives.
 *
 * Layout:
 *   §1  Includes
 *   §2  Module state
 *   §3  Internal helpers
 *   §4  Public API
 *   §5  Singleton vtable
 *   §6  Test-only hooks
 */

#include "led_driver.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "gpio/gpio_driver.h"

/* ===================================================================== */
/* §2. Module state                                                      */
/* ===================================================================== */

typedef struct
{
    const led_pin_t *pins;        /**< Caller-supplied pin table (pointer only). */
    uint8_t count;                /**< Validated == LED_COUNT. */
    led_state_t state[LED_COUNT]; /**< Current logical state per LED. */
    bool initialised;
} led_driver_t;

static led_driver_t s_led;

/* ===================================================================== */
/* §3. Internal helpers                                                  */
/* ===================================================================== */

/**
 * @brief Validate an led_id_t argument.
 *
 * Returns LED_ERR_INVALID_ID if id is out of range or not fitted.
 * Callers must check initialised before calling this.
 */
LED_TEST_VISIBLE led_err_t validate_id(led_id_t id)
{
    if ((uint8_t) id >= (uint8_t) LED_COUNT)
    {
        return LED_ERR_INVALID_ID;
    }
    if (!s_led.pins[(uint8_t) id].fitted)
    {
        return LED_ERR_INVALID_ID;
    }
    return LED_ERR_OK;
}

/* ===================================================================== */
/* §4. Public API                                                        */
/* ===================================================================== */

led_err_t led_init(const led_pin_t *pins, uint8_t count)
{
    uint8_t i;

    if (pins == NULL)
    {
        return LED_ERR_NULL_ARG;
    }
    if (count != (uint8_t) LED_COUNT)
    {
        return LED_ERR_INVALID_ID;
    }

    s_led.pins = pins;
    s_led.count = count;

    for (i = 0U; i < (uint8_t) LED_COUNT; i++)
    {
        gpio_level_t off_level;
        gpio_pin_config_t cfg;

        if (!pins[i].fitted)
        {
            continue;
        }

        cfg.port = (gpio_port_t) pins[i].port;
        cfg.pin = pins[i].pin;
        cfg.mode = GPIO_MODE_OUTPUT;
        cfg.otype = GPIO_OTYPE_OPEN_DRAIN;
        cfg.speed = GPIO_SPEED_LOW;
        cfg.pull = GPIO_PULL_NONE;
        cfg.alternate = 0U;
        (void) gpio_configure_pin(&cfg);

        off_level = pins[i].active_high ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH;
        (void) gpio_write_pin((gpio_port_t) pins[i].port, pins[i].pin, off_level);

        s_led.state[i] = LED_STATE_OFF;
    }

    s_led.initialised = true;
    return LED_ERR_OK;
}

led_err_t led_on(led_id_t id)
{
    gpio_level_t level;
    led_err_t err;

    if (!s_led.initialised)
    {
        return LED_ERR_NOT_INIT;
    }
    err = validate_id(id);
    if (err != LED_ERR_OK)
    {
        return err;
    }

    level = s_led.pins[(uint8_t) id].active_high ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW;
    (void) gpio_write_pin((gpio_port_t) s_led.pins[(uint8_t) id].port, s_led.pins[(uint8_t) id].pin,
                          level);
    s_led.state[(uint8_t) id] = LED_STATE_ON;
    return LED_ERR_OK;
}

led_err_t led_off(led_id_t id)
{
    gpio_level_t level;
    led_err_t err;

    if (!s_led.initialised)
    {
        return LED_ERR_NOT_INIT;
    }
    err = validate_id(id);
    if (err != LED_ERR_OK)
    {
        return err;
    }

    level = s_led.pins[(uint8_t) id].active_high ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH;
    (void) gpio_write_pin((gpio_port_t) s_led.pins[(uint8_t) id].port, s_led.pins[(uint8_t) id].pin,
                          level);
    s_led.state[(uint8_t) id] = LED_STATE_OFF;
    return LED_ERR_OK;
}

led_err_t led_toggle(led_id_t id)
{
    led_state_t new_state;
    led_err_t err;
    gpio_level_t level;

    if (!s_led.initialised)
    {
        return LED_ERR_NOT_INIT;
    }
    err = validate_id(id);
    if (err != LED_ERR_OK)
    {
        return err;
    }

    new_state = (s_led.state[(uint8_t) id] == LED_STATE_OFF) ? LED_STATE_ON : LED_STATE_OFF;

    if (new_state == LED_STATE_ON)
    {
        level = s_led.pins[(uint8_t) id].active_high ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW;
    }
    else
    {
        level = s_led.pins[(uint8_t) id].active_high ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH;
    }

    (void) gpio_write_pin((gpio_port_t) s_led.pins[(uint8_t) id].port, s_led.pins[(uint8_t) id].pin,
                          level);
    s_led.state[(uint8_t) id] = new_state;
    return LED_ERR_OK;
}

led_err_t led_get_state(led_id_t id, led_state_t *state)
{
    led_err_t err;

    if (!s_led.initialised)
    {
        return LED_ERR_NOT_INIT;
    }
    if (state == NULL)
    {
        return LED_ERR_NULL_ARG;
    }
    err = validate_id(id);
    if (err != LED_ERR_OK)
    {
        return err;
    }

    *state = s_led.state[(uint8_t) id];
    return LED_ERR_OK;
}

/* ===================================================================== */
/* §5. Singleton vtable                                                  */
/* ===================================================================== */

static const iled_t s_led_vtable = {
    .on = led_on,
    .off = led_off,
    .toggle = led_toggle,
    .get_state = led_get_state,
};

const iled_t *const led_driver = &s_led_vtable;

/* ===================================================================== */
/* §6. Test-only hooks                                                   */
/* ===================================================================== */

#ifdef TEST
void led_reset_for_test(void)
{
    uint8_t i;

    s_led.pins = NULL;
    s_led.count = 0U;
    s_led.initialised = false;
    for (i = 0U; i < (uint8_t) LED_COUNT; i++)
    {
        s_led.state[i] = LED_STATE_OFF;
    }
}
#endif
