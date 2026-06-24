/**
 * @file modbus_slave_iface_stub.h
 * @brief Narrow stub for IModbusSlave vtable in LifecycleController FD tests.
 *
 * Provides imodbus_slave_t with set_address() — the only method
 * LifecycleController (FD) calls on ModbusSlave. Distinct from
 * modbus_uart_driver_stub.h which is the UART hardware driver layer.
 */

#ifndef MODBUS_SLAVE_IFACE_STUB_H
#define MODBUS_SLAVE_IFACE_STUB_H

#include <stdint.h>

typedef enum
{
    MODBUS_SLAVE_ERR_OK           = 0,
    MODBUS_SLAVE_ERR_NOT_INIT     = 1,
    MODBUS_SLAVE_ERR_NULL_ARG     = 2,
    MODBUS_SLAVE_ERR_INVALID_ADDR = 3,
} modbus_slave_err_t;

typedef struct
{
    modbus_slave_err_t (*set_address)(uint8_t new_addr);
} imodbus_slave_t;

#endif /* MODBUS_SLAVE_IFACE_STUB_H */
