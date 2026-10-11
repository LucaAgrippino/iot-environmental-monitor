/**
 * @file led_driver.h
 * @brief LedDriver — on/off and blink patterns for the board LEDs (Gateway).
 *
 * Provides ILed (per components.md). GPIO access is delegated to GpioDriver;
 * this module never touches peripheral registers directly. Blink patterns run
 * on one FreeRTOS static software timer per fitted LED, so no hardware timer
 * peripheral is consumed.
 *
 * The caller supplies a pin table (one led_pin_t per led_id_t) to led_init().
 * Active-level polarity is encoded per entry in the active_high field, so the
 * driver contains no board-conditional #if directives.
 *
 * This is the Gateway build (B-L475E-IOT01A): one fitted LED, LD2 on PB14. The
 * Field Device counterpart lives in firmware/field-device/drivers/led/ — the
 * boards are separate CubeIDE projects, so each keeps its own copy.
 *
 * @note See docs/lld/drivers/led-driver-lld.md for the full design.
 */

#ifndef LED_DRIVER_H
#define LED_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* LED identifiers                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief LED logical identifier.
 *
 * LED_GREEN is present on both boards. LED_RED is present on the Field Device
 * only: on the Gateway the pin table marks it not fitted, so every API call
 * with LED_RED returns LED_ERR_INVALID_ID.
 */
typedef enum
{
    LED_GREEN = 0U, /**< Green status LED. */
    LED_RED = 1U,   /**< Red alarm LED.    */
    LED_COUNT       /**< Sentinel — never pass to API functions. */
} led_id_t;

/* ------------------------------------------------------------------ */
/* Output patterns                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief LED output pattern.
 *
 * Blink periods are full on-off cycles. The timer fires at half the period,
 * e.g. LED_PATTERN_BLINK_FAST fires every 100 ms for a 200 ms cycle.
 */
typedef enum
{
    LED_PATTERN_OFF = 0U,          /**< Solid off.                 */
    LED_PATTERN_ON = 1U,           /**< Solid on.                  */
    LED_PATTERN_BLINK_FAST = 2U,   /**< 200 ms full cycle (5 Hz).  */
    LED_PATTERN_BLINK_MEDIUM = 3U, /**< 500 ms full cycle (2 Hz).  */
    LED_PATTERN_BLINK_SLOW = 4U,   /**< 1000 ms full cycle (1 Hz). */
    LED_PATTERN_COUNT              /**< Sentinel — never pass to API functions. */
} led_pattern_t;

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

/** @brief Result codes returned by every LedDriver function. */
typedef enum
{
    LED_ERR_OK = 0,           /**< Operation succeeded.                    */
    LED_ERR_INVALID_ID = 1,   /**< LED not fitted on this board.           */
    LED_ERR_NOT_INIT = 2,     /**< led_init() not yet called, or failed.   */
    LED_ERR_NULL_ARG = 3,     /**< NULL argument passed.                   */
    LED_ERR_INVALID_PARAM = 4 /**< Pattern value out of range.             */
} led_err_t;

/* ------------------------------------------------------------------ */
/* Pin table                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Hardware descriptor for one LED pin.
 *
 * The board configuration (or a bring-up main) provides an array of these to
 * led_init(). The driver reads polarity from active_high and never hard-codes
 * board identity.
 */
typedef struct
{
    /* cppcheck-suppress unusedStructMember */
    uint8_t port; /**< GPIO port identifier (gpio_port_t).          */
    /* cppcheck-suppress unusedStructMember */
    uint8_t pin; /**< GPIO pin number, 0..15.                       */
    /* cppcheck-suppress unusedStructMember */
    bool active_high; /**< true: GPIO high = LED on.                */
    /* cppcheck-suppress unusedStructMember */
    bool fitted; /**< false: API calls return LED_ERR_INVALID_ID.   */
} led_pin_t;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise LedDriver from a caller-supplied pin table.
 *
 * Configures the GPIO pin of every fitted LED through GpioDriver, drives each
 * LED off, and creates one static FreeRTOS software timer per fitted LED for
 * blink support. Must be called after gpio_init(). The driver stores only a
 * pointer to the table, which must outlive the driver (a static const array at
 * board-configuration scope).
 *
 * A second call while initialised returns LED_ERR_OK without side effects.
 *
 * @param[in] pins   Array of LED pin descriptors, indexed by led_id_t.
 * @param[in] count  Number of entries; must equal LED_COUNT.
 * @return LED_ERR_OK on success; LED_ERR_NULL_ARG if pins is NULL;
 *         LED_ERR_INVALID_ID if count != LED_COUNT; LED_ERR_NOT_INIT if a GPIO
 *         pin cannot be configured (for example gpio_init() was not called) or
 *         a timer cannot be created. The driver is left uninitialised then.
 * @note Threading: task-context only, non-blocking. Call before the scheduler
 *       starts.
 */
led_err_t led_init(const led_pin_t *pins, uint8_t count);

/**
 * @brief Set the output pattern for an LED.
 *
 * Solid patterns take effect immediately. A blink pattern starts the LED in
 * the on-state and starts the blink timer; switching between blink patterns
 * restarts the timer with the new period; switching to a solid pattern stops
 * the timer.
 *
 * May be called before the scheduler starts: the timer command is queued and
 * the LED sits on until the timer task begins to run (LED-D9).
 *
 * @param[in] id       LED to control.
 * @param[in] pattern  Desired output pattern.
 * @return LED_ERR_OK; LED_ERR_NOT_INIT; LED_ERR_INVALID_ID (out of range or not
 *         fitted); LED_ERR_INVALID_PARAM if pattern >= LED_PATTERN_COUNT.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
led_err_t led_set(led_id_t id, led_pattern_t pattern);

/**
 * @brief Query the current pattern of an LED.
 *
 * @param[in]  id       LED to query.
 * @param[out] pattern  Receives the active pattern.
 * @return LED_ERR_OK; LED_ERR_NOT_INIT; LED_ERR_INVALID_ID; LED_ERR_NULL_ARG if
 *         pattern is NULL.
 * @note Threading: task-context only, non-blocking.
 */
led_err_t led_get_pattern(led_id_t id, led_pattern_t *pattern);

#ifdef TEST
#include "FreeRTOS.h"
#include "timers.h"

/**
 * @brief Test hook — stop all timers and return the driver to its post-bss state.
 *
 * Production code must never call this.
 */
void led_reset_for_test(void);

/** @brief Test hook — the blink timer callback, normally static. */
void led_blink_callback(TimerHandle_t timer);
#endif

#endif /* LED_DRIVER_H */
