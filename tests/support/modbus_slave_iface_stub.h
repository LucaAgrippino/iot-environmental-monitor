/**
 * @file modbus_slave_iface_stub.h
 * @brief Narrow stub for IModbusSlave vtable in LifecycleController FD tests.
 *
 * Delegates to the canonical leaf header so that the type definition is
 * shared between production and test builds. Ceedling sees this file (not
 * modbus_slave.h) and does not auto-link modbus_slave.c or its UART
 * driver dependencies.
 */

#ifndef MODBUS_SLAVE_IFACE_STUB_H
#define MODBUS_SLAVE_IFACE_STUB_H

#include "modbus_slave/imodbus_slave.h"

#endif /* MODBUS_SLAVE_IFACE_STUB_H */
