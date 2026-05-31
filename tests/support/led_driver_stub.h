/**
 * @file led_driver_stub.h
 * @brief Minimal LED type declarations for test TUs that stub LedDriver.
 *
 * Include this header (not led_driver.h) in middleware or application test
 * TUs that need LED types but must not auto-link led_driver.c. Stub body
 * implementations (led_on, led_off, led_toggle) are defined inline in the
 * consuming test TU if called directly; in HealthMonitor tests the
 * led_driver_set macro intercepts all LED calls before they reach the driver.
 *
 * Type definitions are kept in sync with led_driver.h manually.
 */

#ifndef LED_DRIVER_STUB_H
#define LED_DRIVER_STUB_H

#include <stdint.h>

typedef enum
{
    LED_GREEN = 0,
    LED_RED   = 1,
    LED_COUNT
} led_id_t;

typedef enum
{
    LED_ERR_OK         = 0,
    LED_ERR_INVALID_ID = 1,
    LED_ERR_NOT_INIT   = 2,
    LED_ERR_NULL_ARG   = 3,
} led_err_t;

led_err_t led_on(led_id_t id);
led_err_t led_off(led_id_t id);
led_err_t led_toggle(led_id_t id);

#endif /* LED_DRIVER_STUB_H */
