/**
 * @file debug_uart_stub.h
 * @brief Narrow stub for DebugUartDriver in console_service unit tests.
 *
 * Provides idebug_uart_t, debug_uart_err_t, debug_uart_line_flag_t, and
 * debug_uart_line_callback_t — the only types console_service.c needs.
 * Prevents Ceedling from auto-linking debug_uart_driver.c (CMSIS peripheral
 * access not available on host).
 *
 * The test TU creates a spy implementation of idebug_uart_t and passes it
 * to console_service_init(). No real peripheral is exercised.
 *
 * Basename: debug_uart_stub — does NOT match debug_uart_driver.c.
 */

#ifndef DEBUG_UART_STUB_H
#define DEBUG_UART_STUB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define DEBUG_UART_LINE_MAX_LEN (128U)

typedef enum
{
    DEBUG_UART_OK = 0,
    DEBUG_UART_ERR_NOT_INITIALISED = 1,
    DEBUG_UART_ERR_NULL_POINTER = 2,
    DEBUG_UART_ERR_INVALID_PARAM = 3,
    DEBUG_UART_ERR_TX_TIMEOUT = 4,
    DEBUG_UART_ERR_NO_LINE_AVAILABLE = 5,
    DEBUG_UART_ERR_RX_ALREADY_ATTACHED = 6,
} debug_uart_err_t;

typedef enum
{
    DEBUG_UART_LINE_OK = 0,
    DEBUG_UART_LINE_TRUNCATED = 1,
} debug_uart_line_flag_t;

typedef void (*debug_uart_line_callback_t)(void *context);

typedef struct idebug_uart_s
{
    debug_uart_err_t (*send)(const uint8_t *data, size_t length, uint32_t timeout_ms);
    debug_uart_err_t (*read_line)(uint8_t *out_buf, size_t buf_size, size_t *out_length,
                                  debug_uart_line_flag_t *out_flag);
    debug_uart_err_t (*attach_rx)(debug_uart_line_callback_t callback, void *context);
} idebug_uart_t;

#endif /* DEBUG_UART_STUB_H */
