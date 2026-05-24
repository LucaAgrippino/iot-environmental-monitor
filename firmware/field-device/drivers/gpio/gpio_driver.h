/**
 * @file gpio_driver.h
 * @brief CMSIS-level GPIO driver — pin configuration and digital I/O.
 *
 * Provides IGpio (per components.md): configure, read, write, and toggle
 * single-pin digital I/O. Used by every driver that owns physical pins.
 *
 * @note See docs/lld/gpio-driver.md for the full design specification.
 */

#ifndef GPIO_DRIVER_H
#define GPIO_DRIVER_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief GPIO driver result codes.
 */
typedef enum
{
    GPIO_OK                  =  0, /**< Success. */
    GPIO_ERR_NOT_INITIALISED =  1, /**< gpio_init() has not been called. */
    GPIO_ERR_INVALID_PORT    =  2, /**< Port value out of range or not enabled. */
    GPIO_ERR_INVALID_PIN     =  3, /**< Pin number outside 0..15. */
    GPIO_ERR_INVALID_MODE    =  4, /**< Mode value out of range. */
    GPIO_ERR_INVALID_CONFIG  =  5, /**< Config struct combination not permitted. */
    GPIO_ERR_NULL_POINTER    =  6  /**< Required output pointer is NULL. */
} gpio_err_t;

/* ------------------------------------------------------------------ */
/* Enumerations                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief GPIO port identifiers.
 *
 * The set is the superset across both boards. A build-time check rejects
 * use of a port not present on the current target.
 */
typedef enum
{
    GPIO_PORT_A = 0,
    GPIO_PORT_B,
    GPIO_PORT_C,
    GPIO_PORT_D,
    GPIO_PORT_E,
    GPIO_PORT_F,
    GPIO_PORT_G,
    GPIO_PORT_H,
    GPIO_PORT_I,  /**< STM32F469 only. */
    GPIO_PORT_J,  /**< STM32F469 only. */
    GPIO_PORT_K,  /**< STM32F469 only. */
    GPIO_PORT_COUNT
} gpio_port_t;

/**
 * @brief Pin operating mode.
 */
typedef enum
{
    GPIO_MODE_INPUT     = 0,
    GPIO_MODE_OUTPUT    = 1,
    GPIO_MODE_ALTERNATE = 2,
    GPIO_MODE_ANALOGUE  = 3
} gpio_mode_t;

/**
 * @brief Output driver type.
 */
typedef enum
{
    GPIO_OTYPE_PUSH_PULL  = 0,
    GPIO_OTYPE_OPEN_DRAIN = 1
} gpio_otype_t;

/**
 * @brief Output slew rate.
 */
typedef enum
{
    GPIO_SPEED_LOW       = 0,
    GPIO_SPEED_MEDIUM    = 1,
    GPIO_SPEED_HIGH      = 2,
    GPIO_SPEED_VERY_HIGH = 3
} gpio_speed_t;

/**
 * @brief Internal pull configuration.
 */
typedef enum
{
    GPIO_PULL_NONE = 0,
    GPIO_PULL_UP   = 1,
    GPIO_PULL_DOWN = 2
} gpio_pull_t;

/**
 * @brief Logical pin level.
 */
typedef enum
{
    GPIO_LEVEL_LOW  = 0,
    GPIO_LEVEL_HIGH = 1
} gpio_level_t;

/* ------------------------------------------------------------------ */
/* Configuration struct                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Per-pin configuration descriptor.
 *
 * Passed by value-via-pointer to gpio_configure_pin(). All fields are
 * inspected; callers must initialise every field, including @c alternate
 * when @c mode is not GPIO_MODE_ALTERNATE (set to 0 in that case).
 */
typedef struct
{
    gpio_port_t  port;       /**< Port. */
    uint8_t      pin;        /**< Pin number, 0..15. */
    gpio_mode_t  mode;       /**< Operating mode. */
    gpio_otype_t otype;      /**< Output type (ignored if mode is INPUT or ANALOGUE). */
    gpio_speed_t speed;      /**< Slew rate (ignored if mode is INPUT or ANALOGUE). */
    gpio_pull_t  pull;       /**< Pull configuration. */
    uint8_t      alternate;  /**< AF0..AF15, valid only when mode is ALTERNATE. */
} gpio_pin_config_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise the GPIO subsystem.
 *
 * Enables peripheral clocks for every GPIO port the current build target
 * exposes. Must be called once, from main(), before the FreeRTOS scheduler
 * is started, and before any other GPIO function. Subsequent calls are
 * no-ops and return GPIO_OK.
 *
 * @return GPIO_OK on success.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
gpio_err_t gpio_init(void);

/**
 * @brief Configure a single pin.
 *
 * Applies the full config in one call. The order of register writes is
 * chosen so that the pin is not transiently driven incorrectly during
 * reconfiguration (mode is set last among configuration writes).
 *
 * @param[in] config Pointer to a fully populated config descriptor.
 *
 * @return GPIO_OK on success; GPIO_ERR_NOT_INITIALISED, GPIO_ERR_INVALID_PORT,
 *         GPIO_ERR_INVALID_PIN, GPIO_ERR_INVALID_MODE, GPIO_ERR_INVALID_CONFIG,
 *         or GPIO_ERR_NULL_POINTER on failure.
 *
 * @note Threading: task-context only, non-blocking. NOT ISR-safe — performs
 *       read-modify-write on shared per-port registers. NOT safe to call
 *       concurrently on pins of the same port; serialise externally if
 *       configuration must occur after the scheduler has started.
 */
gpio_err_t gpio_configure_pin(const gpio_pin_config_t *config);

/**
 * @brief Read a pin's input level.
 *
 * @param[in]  port      Port containing the pin.
 * @param[in]  pin       Pin number, 0..15.
 * @param[out] out_level Resulting level (HIGH or LOW).
 *
 * @return GPIO_OK on success; GPIO_ERR_NOT_INITIALISED, GPIO_ERR_INVALID_PORT,
 *         GPIO_ERR_INVALID_PIN, or GPIO_ERR_NULL_POINTER on failure.
 *
 * @note Threading: ISR-safe. The implementation is a single 32-bit register
 *       read.
 */
gpio_err_t gpio_read_pin(gpio_port_t port, uint8_t pin, gpio_level_t *out_level);

/**
 * @brief Drive an output pin to the requested level.
 *
 * @param[in] port  Port containing the pin.
 * @param[in] pin   Pin number, 0..15.
 * @param[in] level Target level.
 *
 * @return GPIO_OK on success; GPIO_ERR_NOT_INITIALISED, GPIO_ERR_INVALID_PORT,
 *         or GPIO_ERR_INVALID_PIN on failure.
 *
 * @note Threading: ISR-safe. The implementation is a single 32-bit write to
 *       the bit-set/reset register (BSRR), which is atomic with respect to
 *       other writers on the same port.
 */
gpio_err_t gpio_write_pin(gpio_port_t port, uint8_t pin, gpio_level_t level);

/**
 * @brief Invert an output pin's current level.
 *
 * @param[in] port Port containing the pin.
 * @param[in] pin  Pin number, 0..15.
 *
 * @return GPIO_OK on success; GPIO_ERR_NOT_INITIALISED, GPIO_ERR_INVALID_PORT,
 *         or GPIO_ERR_INVALID_PIN on failure.
 *
 * @note Threading: task-context only, non-blocking. NOT ISR-safe — performs
 *       read-modify-write on ODR. If toggling from an ISR is required,
 *       use gpio_write_pin() with an externally tracked desired level.
 */
gpio_err_t gpio_toggle_pin(gpio_port_t port, uint8_t pin);

#endif /* GPIO_DRIVER_H */
