/**
 * @file exti_driver_stub.h
 * @brief Minimal EXTI type declarations for test TUs that stub ExtiDriver.
 *
 * Include this header (not exti_driver.h) in driver, middleware or application test
 * TUs that need the EXTI types but must not auto-link exti_driver.c. Stub body
 * implementations (exti_configure, exti_enable, ...) are defined inline in the
 * consuming test TU.
 *
 * Type definitions are kept in sync with firmware/gateway/drivers/exti/exti_driver.h
 * manually.
 */

#ifndef EXTI_DRIVER_STUB_H
#define EXTI_DRIVER_STUB_H

#include <stdint.h>

typedef enum
{
    EXTI_ERR_OK = 0,
    EXTI_ERR_INVALID_ARG = 1,
    EXTI_ERR_CONFLICT = 2,
    EXTI_ERR_NOT_CONFIGURED = 3
} exti_err_t;

typedef enum
{
    EXTI_PORT_A = 0U,
    EXTI_PORT_B = 1U,
    EXTI_PORT_C = 2U,
    EXTI_PORT_D = 3U,
    EXTI_PORT_E = 4U,
    EXTI_PORT_F = 5U,
    EXTI_PORT_G = 6U,
    EXTI_PORT_H = 7U
} exti_port_t;

typedef enum
{
    EXTI_EDGE_RISING = 0U,
    EXTI_EDGE_FALLING = 1U,
    EXTI_EDGE_BOTH = 2U
} exti_edge_t;

exti_err_t exti_configure(uint8_t line, exti_port_t port, exti_edge_t edge);
exti_err_t exti_enable(uint8_t line, uint32_t nvic_priority);
exti_err_t exti_disable(uint8_t line);
void exti_clear_pending(uint8_t line);

#endif /* EXTI_DRIVER_STUB_H */
