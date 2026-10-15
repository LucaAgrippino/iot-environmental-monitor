/**
 * @file sensor_service_internal.h
 * @brief Driver headers consumed by SensorService (Gateway). Not for other modules.
 *
 * Grouped here so the unit test can see the driver types without including a driver
 * header directly: Ceedling links a .c file for every header basename in the test
 * translation unit, and this basename matches no .c file (SS-D11).
 */

#ifndef SENSOR_SERVICE_INTERNAL_H
#define SENSOR_SERVICE_INTERNAL_H

#include "barometer_driver/barometer_driver.h"
#include "humidity_temp_driver/humidity_temp_driver.h"
#include "imu_driver/imu_driver.h"
#include "magnetometer_driver/magnetometer_driver.h"

#endif /* SENSOR_SERVICE_INTERNAL_H */
