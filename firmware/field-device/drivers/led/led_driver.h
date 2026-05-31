/**
 * @file led_driver.h
 * @brief LED driver — on/off/toggle for fitted board LEDs.
 *
 * Provides ILed (per components.md). GPIO access is delegated to GpioDriver;
 * this module never touches peripheral registers directly.
 *
 * The caller supplies a pin table (led_pin_t array, one entry per led_id_t)
 * to led_init(). Active-level polarity is encoded per-entry in the active_high
 * field — the driver contains no board-conditional #if directives.
 *
 * @note See docs/lld/drivers/led-driver.md for the full design.
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
 * LED_GREEN is present on both boards.
 * LED_RED is present on the Field Device only. Calling any API function
 * with LED_RED on the Gateway (where the pin table marks it not fitted)
 * returns LED_ERR_INVALID_ID.
 */
typedef enum
{
    LED_GREEN = 0U, /**< Green status LED. */
    LED_RED = 1U,   /**< Red alarm LED.   */
    LED_COUNT       /**< Sentinel — never pass to API functions. */
} led_id_t;

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Error codes returned by LedDriver operations.
 */
typedef enum
{
    LED_ERR_OK = 0,         /**< Operation succeeded. */
    LED_ERR_INVALID_ID = 1, /**< LED not fitted on this board. */
    LED_ERR_NOT_INIT = 2,   /**< led_init() not yet called. */
    LED_ERR_NULL_ARG = 3,   /**< NULL argument passed. */
} led_err_t;

/* ------------------------------------------------------------------ */
/* LED state                                                           */
/* ------------------------------------------------------------------ */

typedef enum
{
    LED_STATE_OFF = 0U, /**< LED is off. */
    LED_STATE_ON = 1U,  /**< LED is on.  */
} led_state_t;

/* ------------------------------------------------------------------ */
/* Pin descriptor                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Hardware descriptor for one LED pin.
 *
 * The caller (board-level config) provides an array of these to led_init().
 * The driver reads polarity from the active_high field — it never hard-codes
 * board identity or active-level assumptions internally.
 */
typedef struct
{
    uint8_t port;     /**< GPIO port identifier (gpio_port_t from gpio_driver.h). */
    uint8_t pin;      /**< GPIO pin number (0–15). */
    bool active_high; /**< true → GPIO high = LED on; false → GPIO low = LED on. */
    bool fitted;      /**< false → API calls for this ID return LED_ERR_INVALID_ID. */
} led_pin_t;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise LedDriver from a caller-supplied pin table.
 *
 * Configures the GPIO pins for all fitted LEDs via GpioDriver and sets the
 * initial state to off. Must be called once, after gpio_init().
 *
 * The pin table is owned by the caller. The driver stores only a pointer —
 * the caller must ensure the array's lifetime exceeds the driver's lifetime
 * (static storage in practice).
 *
 * @param pins   Array of LED pin descriptors, one entry per led_id_t,
 *               indexed by the led_id_t value.
 * @param count  Number of entries in @p pins. Must equal LED_COUNT.
 * @return LED_ERR_OK on success; LED_ERR_NULL_ARG if @p pins is NULL;
 *         LED_ERR_INVALID_ID if @p count != LED_COUNT.
 * @note Threading: task-context only, non-blocking. Call before the
 *       scheduler starts.
 */
led_err_t led_init(const led_pin_t *pins, uint8_t count);

/**
 * @brief Turn an LED on.
 *
 * @param id  LED to turn on.
 * @return LED_ERR_OK; LED_ERR_NOT_INIT; LED_ERR_INVALID_ID.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
led_err_t led_on(led_id_t id);

/**
 * @brief Turn an LED off.
 *
 * @param id  LED to turn off.
 * @return LED_ERR_OK; LED_ERR_NOT_INIT; LED_ERR_INVALID_ID.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
led_err_t led_off(led_id_t id);

/**
 * @brief Toggle an LED.
 *
 * @param id  LED to toggle.
 * @return LED_ERR_OK; LED_ERR_NOT_INIT; LED_ERR_INVALID_ID.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
led_err_t led_toggle(led_id_t id);

/**
 * @brief Query the current logical state of an LED.
 *
 * @param id     LED to query.
 * @param state  Output — LED_STATE_ON or LED_STATE_OFF.
 * @return LED_ERR_OK; LED_ERR_NOT_INIT; LED_ERR_INVALID_ID;
 *         LED_ERR_NULL_ARG if @p state is NULL.
 * @note Threading: task-context only, non-blocking.
 */
led_err_t led_get_state(led_id_t id, led_state_t *state);

/* ------------------------------------------------------------------ */
/* ILed vtable (P2 — Dependency Inversion)                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Vtable exposing the LedDriver operational surface.
 *
 * HealthMonitor depends on this interface; the concrete driver is injected as
 * a const pointer to a single static instance. led_init() is called directly
 * at startup — it is not part of this vtable.
 */
typedef struct
{
    led_err_t (*on)(led_id_t id);
    led_err_t (*off)(led_id_t id);
    led_err_t (*toggle)(led_id_t id);
    led_err_t (*get_state)(led_id_t id, led_state_t *state);
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
