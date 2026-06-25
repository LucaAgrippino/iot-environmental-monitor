/**
 * @file imodbus_slave.h
 * @brief IModbusSlave — leaf interface header (set_address capability).
 *
 * Extracted from modbus_slave.h so that modbus_slave.c (middleware) can
 * implement the vtable without an upward include of modbus_register_map.h
 * (application), and so lifecycle_controller.h can inject the interface
 * without pulling in the full ModbusSlave header.
 *
 * Consumers: lifecycle_controller.h (FD), modbus_register_map.h.
 * Implementor: modbus_slave.c (defines modbus_slave singleton).
 *
 * @see docs/lld/middleware/modbus-slave.md
 */

#ifndef IMODBUS_SLAVE_H
#define IMODBUS_SLAVE_H

#include <stdint.h>

/* ===================================================================== */
/* ModbusSlave error codes                                               */
/* ===================================================================== */

#ifndef MODBUS_SLAVE_ERR_DEFINED
#define MODBUS_SLAVE_ERR_DEFINED
typedef enum
{
    MODBUS_SLAVE_ERR_OK = 0,
    MODBUS_SLAVE_ERR_NOT_INIT = 1,
    MODBUS_SLAVE_ERR_NULL_ARG = 2,
    MODBUS_SLAVE_ERR_INVALID_ADDR = 3,
} modbus_slave_err_t;
#endif /* MODBUS_SLAVE_ERR_DEFINED */

/* ===================================================================== */
/* IModbusSlave vtable                                                   */
/* ===================================================================== */

typedef struct
{
    modbus_slave_err_t (*set_address)(uint8_t new_addr);
} imodbus_slave_t;

/** Singleton — implemented by modbus_slave.c. */
extern const imodbus_slave_t *const modbus_slave;

#endif /* IMODBUS_SLAVE_H */
