/**
 * @file gpio_driver_stub.h
 * @brief Minimal GPIO type declarations for test TUs that stub GpioDriver.
 *
 * Include this header (not gpio_driver.h) in middleware or application test
 * TUs that need GPIO types but must not auto-link gpio_driver.c. Stub body
 * implementations (gpio_configure_pin, gpio_write_pin, gpio_toggle_pin, etc.)
 * are defined inline in the consuming test TU.
 *
 * This header intentionally does NOT include gpio_driver.h to prevent
 * Ceedling from adding gpio_driver.c to the test link list.
 *
 * Type definitions are kept in sync with gpio_driver.h manually.
 */

#ifndef GPIO_DRIVER_STUB_H
#define GPIO_DRIVER_STUB_H

#include <stdint.h>

typedef enum
{
    GPIO_OK = 0,
    GPIO_ERR_NOT_INITIALISED = 1,
    GPIO_ERR_INVALID_PORT    = 2,
    GPIO_ERR_INVALID_PIN     = 3,
    GPIO_ERR_INVALID_MODE    = 4,
    GPIO_ERR_INVALID_CONFIG  = 5,
    GPIO_ERR_NULL_POINTER    = 6
} gpio_err_t;

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
    GPIO_PORT_I,
    GPIO_PORT_J,
    GPIO_PORT_K,
    GPIO_PORT_COUNT
} gpio_port_t;

typedef enum
{
    GPIO_MODE_INPUT     = 0,
    GPIO_MODE_OUTPUT    = 1,
    GPIO_MODE_ALTERNATE = 2,
    GPIO_MODE_ANALOGUE  = 3
} gpio_mode_t;

typedef enum
{
    GPIO_OTYPE_PUSH_PULL  = 0,
    GPIO_OTYPE_OPEN_DRAIN = 1
} gpio_otype_t;

typedef enum
{
    GPIO_SPEED_LOW       = 0,
    GPIO_SPEED_MEDIUM    = 1,
    GPIO_SPEED_HIGH      = 2,
    GPIO_SPEED_VERY_HIGH = 3
} gpio_speed_t;

typedef enum
{
    GPIO_PULL_NONE = 0,
    GPIO_PULL_UP   = 1,
    GPIO_PULL_DOWN = 2
} gpio_pull_t;

typedef enum
{
    GPIO_LEVEL_LOW   = 0,
    GPIO_LEVEL_HIGH  = 1,
    GPIO_LEVEL_UNDEF = 1,
} gpio_level_t;

typedef struct
{
    gpio_port_t  port;
    uint8_t      pin;
    gpio_mode_t  mode;
    gpio_otype_t otype;
    gpio_speed_t speed;
    gpio_pull_t  pull;
    uint8_t      alternate;
} gpio_pin_config_t;

gpio_err_t gpio_init(void);
gpio_err_t gpio_configure_pin(const gpio_pin_config_t *config);
gpio_err_t gpio_read_pin(gpio_port_t port, uint8_t pin, gpio_level_t *out_level);
gpio_err_t gpio_write_pin(gpio_port_t port, uint8_t pin, gpio_level_t level);
gpio_err_t gpio_toggle_pin(gpio_port_t port, uint8_t pin);

#endif /* GPIO_DRIVER_STUB_H */
