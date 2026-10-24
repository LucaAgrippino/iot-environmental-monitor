/**
 * @file time_service_internal.h
 * @brief Private dependency headers for time_service.c and its host tests.
 *
 * Ceedling links a .c file for every header basename a test includes directly. Pulling
 * the TimeProvider and ModbusPoller headers in through this wrapper gives the test their
 * types without linking the real modules, which the test replaces with spies.
 */

#ifndef TIME_SERVICE_INTERNAL_H
#define TIME_SERVICE_INTERNAL_H

#include "modbus_poller/modbus_poller.h"
#include "time_provider/time_provider.h"

#endif /* TIME_SERVICE_INTERNAL_H */
