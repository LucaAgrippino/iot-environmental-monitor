/**
 * @file barometer_driver_stub.h
 * @brief Narrow stub for BarometerDriver in sensor_service unit tests.
 *
 * Declares only the symbols sensor_service.c calls through. Including this
 * header (via #ifdef TEST in sensor_service.c) instead of the real
 * barometer_driver.h prevents Ceedling from auto-linking barometer_driver.c.
 *
 * Basename: barometer_driver_stub — does NOT match barometer_driver.c.
 */

#ifndef BAROMETER_DRIVER_STUB_H
#define BAROMETER_DRIVER_STUB_H

#include <stdint.h>
#include <stdbool.h>

typedef enum
{
    BARO_ERR_OK    = 0,
    BARO_ERR_FAULT = 1,
} baro_err_t;

typedef struct
{
    int32_t pressure_x10;
} baro_reading_t;

baro_err_t barometer_init(void);
baro_err_t barometer_read(baro_reading_t *reading);

#endif /* BAROMETER_DRIVER_STUB_H */
