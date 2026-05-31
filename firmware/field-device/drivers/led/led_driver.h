/**
 * @file led_driver.h
 * @brief LED driver — on/off/toggle for fitted board LEDs.
 *
 * Provides ILed (per components.md): init, on, off, toggle for LEDs present
 * on the active board variant. GPIO access is delegated to GpioDriver; this
 * module never touches peripheral registers directly.
 *
 * Board variants:
 *   STM32F469 (Field Device): LED_GREEN = PG13, LED_RED = PD5 (both active-high).
 *   STM32L475 (Gateway):      LED_GREEN = PB14, LED_RED not fitted.
 *
 * @note See docs/lld/drivers/led-driver.md for the full design.
 */

#ifndef LED_DRIVER_H
#define LED_DRIVER_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* LED identifiers                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief LED identifier.
 *
 * LED_GREEN is present on both boards.
 * LED_RED is present on the Field Device only; returns LED_ERR_INVALID_ID
 * on the Gateway.
 */
typedef enum
{
    LED_GREEN = 0, /**< Green status LED. FD: LD3 (PG13). GW: LED2 (PB14). */
    LED_RED = 1,   /**< Red alarm LED.   FD: LD4 (PD5).  GW: not fitted.   */
    LED_COUNT      /**< Sentinel — do not pass to API functions. */
} led_id_t;

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Error codes returned by all LedDriver operations.
 */
typedef enum
{
    LED_ERR_OK = 0,         /**< Operation succeeded. */
    LED_ERR_INVALID_ID = 1, /**< LED identifier not fitted on this board. */
} led_err_t;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise LedDriver.
 *
 * Configures the GPIO pins for all fitted LEDs (via GpioDriver) and
 * sets the initial state to off. Must be called once after gpio_init().
 *
 * @return LED_ERR_OK on success.
 * @note Threading: task-context only, non-blocking. Must be called before
 *       the scheduler starts.
 */
led_err_t led_init(void);

/**
 * @brief Turn an LED on.
 *
 * @param id  LED to turn on.
 * @return LED_ERR_OK on success; LED_ERR_INVALID_ID if the LED is not
 *         fitted on this board.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
led_err_t led_on(led_id_t id);

/**
 * @brief Turn an LED off.
 *
 * @param id  LED to turn off.
 * @return LED_ERR_OK on success; LED_ERR_INVALID_ID if the LED is not
 *         fitted on this board.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
led_err_t led_off(led_id_t id);

/**
 * @brief Toggle an LED state.
 *
 * @param id  LED to toggle.
 * @return LED_ERR_OK on success; LED_ERR_INVALID_ID if the LED is not
 *         fitted on this board.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
led_err_t led_toggle(led_id_t id);

/* ------------------------------------------------------------------ */
/* ILed vtable (P2 — Dependency Inversion)                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Vtable exposing the LedDriver public surface.
 *
 * HealthMonitor depends on this interface; the concrete driver is
 * injected as a const pointer to a single static instance.
 */
typedef struct
{
    led_err_t (*init)(void);
    led_err_t (*on)(led_id_t id);
    led_err_t (*off)(led_id_t id);
    led_err_t (*toggle)(led_id_t id);
} iled_t;

/** Singleton pointer to the LedDriver vtable instance. */
extern const iled_t *const led_driver;

/* ------------------------------------------------------------------ */
/* Test-only hooks (#ifdef TEST)                                       */
/* ------------------------------------------------------------------ */

#ifdef TEST
#define LED_TEST_VISIBLE
#else
#define LED_TEST_VISIBLE static
#endif

#ifdef TEST
/**
 * @brief Reset module state for unit tests.
 *
 * Clears the internal s_led state to its post-bss value. Test-only.
 */
void led_reset_for_test(void);
#endif

#endif /* LED_DRIVER_H */
