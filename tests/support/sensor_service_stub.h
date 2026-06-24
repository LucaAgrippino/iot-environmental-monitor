/**
 * @file sensor_service_stub.h
 * @brief Narrow stub for SensorService in unit tests.
 *
 * Provides the data types and declarations that alarm_service.c and
 * console_service.c depend on (via #ifdef TEST substitution). Prevents
 * Ceedling from auto-linking sensor_service.c and its entire driver chain.
 *
 * Provides time_provider_ts_t and time_sync_state_t inline so that
 * time_provider.h (and its dependencies) are not pulled in.
 *
 * Basename: sensor_service_stub — does NOT match sensor_service.c.
 */

#ifndef SENSOR_SERVICE_STUB_H
#define SENSOR_SERVICE_STUB_H

#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------- */
/* time_provider_ts_t (binary-compatible copy; guarded against include   */
/* conflicts when time_provider.h is also transitively included)         */
/* --------------------------------------------------------------------- */

#ifndef TIME_SYNC_STATE_DEFINED
#define TIME_SYNC_STATE_DEFINED
typedef enum
{
    TIME_SYNC_UNSYNCHRONISED = 0,
    TIME_SYNC_SYNCHRONISED = 1,
} time_sync_state_t;
#endif /* TIME_SYNC_STATE_DEFINED */

#ifndef TIME_PROVIDER_TS_DEFINED
#define TIME_PROVIDER_TS_DEFINED
typedef struct
{
    uint32_t epoch;
    time_sync_state_t sync_state;
} time_provider_ts_t;
#endif /* TIME_PROVIDER_TS_DEFINED */

/* --------------------------------------------------------------------- */
/* sensor_id_t, sensor_reading_t, sensor_snapshot_t                      */
/* --------------------------------------------------------------------- */

#ifndef SENSOR_ID_DEFINED
#define SENSOR_ID_DEFINED
typedef enum
{
    SENSOR_ID_TEMPERATURE = 0,
    SENSOR_ID_HUMIDITY = 1,
    SENSOR_ID_PRESSURE = 2,
    SENSOR_ID_ACCEL_X = 3,
    SENSOR_ID_ACCEL_Y = 4,
    SENSOR_ID_ACCEL_Z = 5,
    SENSOR_ID_GYRO_X = 6,
    SENSOR_ID_GYRO_Y = 7,
    SENSOR_ID_GYRO_Z = 8,
    SENSOR_ID_MAG_X = 9,
    SENSOR_ID_MAG_Y = 10,
    SENSOR_ID_MAG_Z = 11,
    SENSOR_ID_COUNT = 12,
} sensor_id_t;
#endif /* SENSOR_ID_DEFINED */

typedef struct
{
    int32_t value; /* fixed-point; units per sensor_id_t (see sensor_service.h) */
    bool valid;
    time_provider_ts_t timestamp;
} sensor_reading_t;

typedef struct
{
    sensor_reading_t readings[SENSOR_ID_COUNT];
    uint32_t cycle_count;
} sensor_snapshot_t;

typedef enum
{
    SENSOR_SERVICE_ERR_OK = 0,
    SENSOR_SERVICE_ERR_NOT_INIT = 1,
    SENSOR_SERVICE_ERR_NULL_ARG = 2,
    SENSOR_SERVICE_ERR_NO_SUB = 3,
} sensor_service_err_t;

#define SENSOR_MAX_SUBSCRIBERS (4U)

/* --------------------------------------------------------------------- */
/* sensor_service_subscribe declaration (defined inline in test TU)       */
/* --------------------------------------------------------------------- */

sensor_service_err_t sensor_service_subscribe(void (*cb)(const sensor_snapshot_t *snap));

/* --------------------------------------------------------------------- */
/* isensor_service_t — vtable consumed by LcdUi, ModbusRegisterMap,     */
/* and ConsoleService.                                                   */
/* --------------------------------------------------------------------- */

typedef struct
{
    sensor_service_err_t (*init)(void);
    sensor_service_err_t (*run_cycle)(void);
    sensor_service_err_t (*get_snapshot)(sensor_snapshot_t *snap);
    sensor_service_err_t (*subscribe)(void (*cb)(const sensor_snapshot_t *));
    sensor_service_err_t (*read_on_demand)(void);
    bool                 (*is_ready)(void);
    sensor_service_err_t (*reconfigure)(void);
} isensor_service_t;

#endif /* SENSOR_SERVICE_STUB_H */
