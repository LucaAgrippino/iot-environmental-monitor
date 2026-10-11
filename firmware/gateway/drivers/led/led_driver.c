/**
 * @file led_driver.c
 * @brief LedDriver implementation (Gateway).
 *
 * GPIO access is delegated entirely to GpioDriver; no CMSIS registers are
 * touched here. Polarity comes from the caller-supplied pin table at runtime —
 * the driver contains no board-conditional #if directives.
 *
 * @see docs/lld/drivers/led-driver-lld.md
 */

#include "led_driver.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "timers.h"

#ifdef TEST
/* Keep the real GpioDriver out of the host link unit: Ceedling auto-links any
 * .c whose header a translation unit includes. */
#include "gpio_driver_stub.h"
#define LED_TEST_VISIBLE
#else
#include "gpio/gpio_driver.h"
#define LED_TEST_VISIBLE static
#endif /* TEST */

#define LED_TIMER_DEFAULT_PERIOD_MS (500U)
#define LED_HALF_PERIOD_FAST_MS (100U)
#define LED_HALF_PERIOD_MEDIUM_MS (250U)
#define LED_HALF_PERIOD_SLOW_MS (500U)
#define LED_TIMER_NAME_LEN (8U)
#define LED_NO_BLOCK_TICKS (0U)

/* ====================================================================== */
/* Module state                                                           */
/* ====================================================================== */

typedef struct
{
    const led_pin_t *pins;              /**< Caller-supplied pin table (pointer only). */
    uint8_t count;                      /**< Validated == LED_COUNT.                    */
    led_pattern_t pattern[LED_COUNT];   /**< Current pattern per LED.                  */
    volatile bool gpio_on[LED_COUNT];   /**< Logical on-state; also set by the timer.  */
    TimerHandle_t timer[LED_COUNT];     /**< Blink timer per fitted LED.                */
    StaticTimer_t timer_buf[LED_COUNT]; /**< Static timer storage.                   */
    char timer_name[LED_COUNT][LED_TIMER_NAME_LEN]; /**< Names must outlive the timers. */
    bool initialised;
} led_driver_t;

static led_driver_t s_led;

/* ====================================================================== */
/* Helpers                                                                */
/* ====================================================================== */

/** @brief Blink half-period in ticks for a blink pattern. */
static TickType_t blink_half_period(led_pattern_t pattern)
{
    uint32_t ms = LED_TIMER_DEFAULT_PERIOD_MS;

    switch (pattern)
    {
    case LED_PATTERN_BLINK_FAST:
        ms = LED_HALF_PERIOD_FAST_MS;
        break;
    case LED_PATTERN_BLINK_MEDIUM:
        ms = LED_HALF_PERIOD_MEDIUM_MS;
        break;
    case LED_PATTERN_BLINK_SLOW:
        ms = LED_HALF_PERIOD_SLOW_MS;
        break;
    default:
        break;
    }

    return pdMS_TO_TICKS(ms);
}

/** @brief Drive the LED to its on-level. */
static void led_gpio_on(led_id_t id)
{
    const led_pin_t *p = &s_led.pins[id];
    gpio_level_t level = p->active_high ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW;

    (void) gpio_write_pin((gpio_port_t) p->port, p->pin, level);
    s_led.gpio_on[id] = true;
}

/** @brief Drive the LED to its off-level. */
static void led_gpio_off(led_id_t id)
{
    const led_pin_t *p = &s_led.pins[id];
    gpio_level_t level = p->active_high ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH;

    (void) gpio_write_pin((gpio_port_t) p->port, p->pin, level);
    s_led.gpio_on[id] = false;
}

/** @brief True for the blink patterns. */
static bool pattern_is_blink(led_pattern_t pattern)
{
    return (pattern >= LED_PATTERN_BLINK_FAST) && (pattern < LED_PATTERN_COUNT);
}

/**
 * @brief Blink timer callback — toggles the LED once per half-period.
 *
 * Runs in the FreeRTOS timer daemon task. The LED is found by comparing the
 * timer handle with the driver's own table, so no timer ID has to round-trip
 * through a pointer.
 *
 * xTimerStop() is asynchronous, so one more firing can arrive after a switch to
 * a solid pattern; the pattern check turns that residual firing into a no-op
 * (LED-D10).
 */
LED_TEST_VISIBLE void led_blink_callback(TimerHandle_t timer)
{
    for (uint8_t i = 0U; i < (uint8_t) LED_COUNT; i++)
    {
        if ((s_led.timer[i] != NULL) && (s_led.timer[i] == timer))
        {
            led_id_t id = (led_id_t) i;

            if (!pattern_is_blink(s_led.pattern[id]))
            {
                return;
            }

            if (s_led.gpio_on[id])
            {
                led_gpio_off(id);
            }
            else
            {
                led_gpio_on(id);
            }

            return;
        }
    }
}

/** @brief Validate an id; the driver must be initialised. */
static led_err_t validate_id(led_id_t id)
{
    if ((uint8_t) id >= (uint8_t) LED_COUNT)
    {
        return LED_ERR_INVALID_ID;
    }

    if (!s_led.pins[id].fitted)
    {
        return LED_ERR_INVALID_ID;
    }

    return LED_ERR_OK;
}

/* ====================================================================== */
/* Public API                                                             */
/* ====================================================================== */

led_err_t led_init(const led_pin_t *pins, uint8_t count)
{
    if (pins == NULL)
    {
        return LED_ERR_NULL_ARG;
    }

    if (count != (uint8_t) LED_COUNT)
    {
        return LED_ERR_INVALID_ID;
    }

    if (s_led.initialised)
    {
        return LED_ERR_OK;
    }

    s_led.pins = pins;
    s_led.count = count;

    for (uint8_t i = 0U; i < (uint8_t) LED_COUNT; i++)
    {
        led_id_t id = (led_id_t) i;

        if (!pins[i].fitted)
        {
            continue;
        }

        gpio_pin_config_t cfg = {
            .port = (gpio_port_t) pins[i].port,
            .pin = pins[i].pin,
            .mode = GPIO_MODE_OUTPUT,
            .otype = GPIO_OTYPE_PUSH_PULL,
            .speed = GPIO_SPEED_LOW,
            .pull = GPIO_PULL_NONE,
            .alternate = 0U,
        };

        if (gpio_configure_pin(&cfg) != GPIO_OK)
        {
            (void) memset(&s_led, 0, sizeof(s_led));
            return LED_ERR_NOT_INIT;
        }

        led_gpio_off(id);
        s_led.pattern[i] = LED_PATTERN_OFF;

        s_led.timer_name[i][0] = 'L';
        s_led.timer_name[i][1] = 'e';
        s_led.timer_name[i][2] = 'd';
        s_led.timer_name[i][3] = (char) ('0' + i);
        s_led.timer_name[i][4] = '\0';

        s_led.timer[i] =
            xTimerCreateStatic(s_led.timer_name[i], pdMS_TO_TICKS(LED_TIMER_DEFAULT_PERIOD_MS),
                               pdTRUE, NULL, led_blink_callback, &s_led.timer_buf[i]);

        if (s_led.timer[i] == NULL)
        {
            (void) memset(&s_led, 0, sizeof(s_led));
            return LED_ERR_NOT_INIT;
        }
    }

    s_led.initialised = true;

    return LED_ERR_OK;
}

led_err_t led_set(led_id_t id, led_pattern_t pattern)
{
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

    if ((uint8_t) pattern >= (uint8_t) LED_PATTERN_COUNT)
    {
        return LED_ERR_INVALID_PARAM;
    }

    /* The pattern is written first so that a timer callback already in flight
     * sees a solid pattern and returns without toggling. */
    s_led.pattern[id] = pattern;

    switch (pattern)
    {
    case LED_PATTERN_OFF:
        (void) xTimerStop(s_led.timer[id], LED_NO_BLOCK_TICKS);
        led_gpio_off(id);
        break;

    case LED_PATTERN_ON:
        (void) xTimerStop(s_led.timer[id], LED_NO_BLOCK_TICKS);
        led_gpio_on(id);
        break;

    default:
        /* Blink: start in the on-state. xTimerChangePeriod() starts a dormant
         * timer and restarts a running one, so one call covers both. */
        led_gpio_on(id);
        (void) xTimerChangePeriod(s_led.timer[id], blink_half_period(pattern), LED_NO_BLOCK_TICKS);
        break;
    }

    return LED_ERR_OK;
}

led_err_t led_get_pattern(led_id_t id, led_pattern_t *pattern)
{
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

    if (pattern == NULL)
    {
        return LED_ERR_NULL_ARG;
    }

    *pattern = s_led.pattern[id];

    return LED_ERR_OK;
}

#ifdef TEST
void led_reset_for_test(void)
{
    for (uint8_t i = 0U; i < (uint8_t) LED_COUNT; i++)
    {
        if (s_led.timer[i] != NULL)
        {
            (void) xTimerStop(s_led.timer[i], LED_NO_BLOCK_TICKS);
        }
    }

    (void) memset(&s_led, 0, sizeof(s_led));
}
#endif /* TEST */
