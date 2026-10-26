/**
 * @file lifecycle_wiring_internal.h
 * @brief The module headers the adapters call. Not for other modules.
 *
 * Grouped here so the unit test can see the modules' types without including a module header
 * directly: Ceedling links a .c file for every header basename in the test translation unit,
 * and this basename matches no .c file (the same device as sensor_service_internal.h).
 */

#ifndef LIFECYCLE_WIRING_INTERNAL_H
#define LIFECYCLE_WIRING_INTERNAL_H

#include "alarm_service/alarm_service.h"
#include "config_service/config_service.h"
#include "modbus_poller/modbus_poller.h"
#include "reset_driver/reset_driver.h"
#include "sensor_service/sensor_service.h"
#include "sensor_service/sensor_task.h"
#include "time_service/time_service.h"

#endif /* LIFECYCLE_WIRING_INTERNAL_H */
