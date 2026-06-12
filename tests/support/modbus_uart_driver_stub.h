/**
 * @file modbus_uart_driver_stub.h
 * @brief Minimal ModbusUartDriver declarations for middleware unit tests.
 *
 * Swapped in place of the real modbus_uart_driver/modbus_uart_driver.h
 * via #ifndef TEST in modbus_slave.c. This prevents Ceedling from
 * auto-linking modbus_uart_driver.c (which requires CMSIS hardware mocks
 * incompatible with middleware-layer test defines).
 *
 * Stub implementations are defined inline in the test TU:
 *   modbus_uart_attach_rx  — captures the callback pointer
 *   modbus_uart_get_rx_frame — returns a pre-loaded test frame
 *   modbus_uart_transmit    — captures the transmitted frame for assertion
 */

#ifndef MODBUS_UART_DRIVER_STUB_H
#define MODBUS_UART_DRIVER_STUB_H

#include <stdint.h>

#define MODBUS_UART_BUF_SIZE (256U)

typedef enum
{
    MODBUS_UART_ERR_OK      = 0,
    MODBUS_UART_ERR_TIMEOUT = 1,
    MODBUS_UART_ERR_BUSY    = 2,
} modbus_uart_err_t;

typedef enum
{
    MODBUS_UART_EVENT_RX_DONE  = 0,
    MODBUS_UART_EVENT_RX_ERROR = 1,
} modbus_uart_event_t;

typedef void (*modbus_uart_rx_cb_t)(modbus_uart_event_t event, void *context);

void              modbus_uart_attach_rx(modbus_uart_rx_cb_t callback,
                                        void *context);
modbus_uart_err_t modbus_uart_get_rx_frame(uint8_t *buf, uint16_t *len);
modbus_uart_err_t modbus_uart_transmit(const uint8_t *frame, uint16_t len);

#endif /* MODBUS_UART_DRIVER_STUB_H */
