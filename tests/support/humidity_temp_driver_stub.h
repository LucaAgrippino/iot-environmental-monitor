/**
 * @file humidity_temp_driver_stub.h
 * @brief Narrow stub for HumidityTempDriver in sensor_service unit tests.
 *
 * Declares only the symbols sensor_service.c calls through. Including this
 * header (via #ifdef TEST in sensor_service.c) instead of the real
 * humidity_temp_driver.h prevents Ceedling from auto-linking
 * humidity_temp_driver.c.
 *
 * Basename: humidity_temp_driver_stub — does NOT match humidity_temp_driver.c.
 */

#ifndef HUMIDITY_TEMP_DRIVER_STUB_H
#define HUMIDITY_TEMP_DRIVER_STUB_H

#include <stdint.h>
#include <stdbool.h>

typedef enum
{
    HT_ERR_OK    = 0,
    HT_ERR_FAULT = 1,
} ht_err_t;

typedef struct
{
    int32_t  temperature_x100;
    uint32_t humidity_x100;
} ht_reading_t;

ht_err_t humidity_temp_init(void);
ht_err_t humidity_temp_read(ht_reading_t *reading);

#endif /* HUMIDITY_TEMP_DRIVER_STUB_H */
