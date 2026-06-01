/**
 * @file sensor_service.h
 * @brief SensorService — application-layer sensor acquisition and ISensorService vtable.
 *
 * Drives BarometerDriver and HumidityTempDriver (FD) through a configurable
 * IIR filter pipeline. Exposes a pull interface (get_snapshot) and a push
 * interface (subscribe). AlarmService registers as the first subscriber.
 *
 * Boards: Field Device (STM32F469I-DISCO) and Gateway (B-L475E-IOT01A).
 *
 * @see docs/lld/application/sensor-alarm-service.md
 */

#ifndef SENSOR_SERVICE_H
#define SENSOR_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

#include "time_provider/time_provider.h"

/* ======================================================================= */
/* Data types                                                               */
/* ======================================================================= */

#ifndef SENSOR_ID_DEFINED
#define SENSOR_ID_DEFINED
typedef enum
{
    SENSOR_ID_TEMPERATURE = 0,
    SENSOR_ID_HUMIDITY = 1,
    SENSOR_ID_PRESSURE = 2,
    SENSOR_ID_ACCEL_X = 3, /**< GW only */
    SENSOR_ID_ACCEL_Y = 4, /**< GW only */
    SENSOR_ID_ACCEL_Z = 5, /**< GW only */
    SENSOR_ID_GYRO_X = 6,  /**< GW only */
    SENSOR_ID_GYRO_Y = 7,  /**< GW only */
    SENSOR_ID_GYRO_Z = 8,  /**< GW only */
    SENSOR_ID_MAG_X = 9,   /**< GW only */
    SENSOR_ID_MAG_Y = 10,  /**< GW only */
    SENSOR_ID_MAG_Z = 11,  /**< GW only */
    SENSOR_ID_COUNT = 12,
} sensor_id_t;
#endif /* SENSOR_ID_DEFINED */

/**
 * @brief One processed sensor sample (value after IIR filter, with validity flag).
 */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- consumed by AlarmService, LcdUi, CloudPublisher */
    float value; /**< Engineering units (°C, %RH, hPa, m/s², dps, µT). */
    /* cppcheck-suppress unusedStructMember -- consumed by AlarmService */
    bool valid; /**< false if driver returned error or range check failed. */
    /* cppcheck-suppress unusedStructMember -- consumed by CloudPublisher */
    time_provider_ts_t timestamp; /**< Timestamp at acquisition time (REQ-SA-100). */
} sensor_reading_t;

/**
 * @brief Snapshot of all sensor readings produced by one acquisition cycle.
 */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- accessed by get_snapshot() callers */
    sensor_reading_t readings[SENSOR_ID_COUNT];
    /* cppcheck-suppress unusedStructMember -- read by LcdUi, ModbusRegisterMap */
    uint32_t cycle_count; /**< Increments on every sensor_service_run_cycle() call. */
} sensor_snapshot_t;

typedef enum
{
    SENSOR_SERVICE_ERR_OK = 0,
    SENSOR_SERVICE_ERR_NOT_INIT = 1,
    SENSOR_SERVICE_ERR_NULL_ARG = 2,
    SENSOR_SERVICE_ERR_NO_SUB = 3, /**< Subscriber table full. */
} sensor_service_err_t;

/* ======================================================================= */
/* Constants                                                                */
/* ======================================================================= */

/** Maximum number of new-reading subscribers (compile-time). */
#define SENSOR_MAX_SUBSCRIBERS (4U)

/* ======================================================================= */
/* Public API — ISensorService                                              */
/* ======================================================================= */

/**
 * @brief Initialise SensorService.
 *
 * Reads polling interval and filter parameters from IConfigProvider (falls
 * back to defaults if absent — REQ-SA-010, SA-050). Attempts to initialise
 * each driver; logs failures and continues (REQ-SA-040, SA-060). Marks
 * permanently failed sensors invalid so every subsequent cycle skips them.
 * Creates the 100 ms poll software timer but does not start the scheduler.
 *
 * @return SENSOR_SERVICE_ERR_OK on success; non-zero otherwise.
 * @note   Must be called before the FreeRTOS scheduler starts.
 */
sensor_service_err_t sensor_service_init(void);

/**
 * @brief Run one full acquisition cycle.
 *
 * Reads all active drivers, applies the full processing pipeline (range
 * validation, clamping, IIR filter), updates the internal snapshot, then
 * calls all registered new-reading callbacks in registration order.
 *
 * @return SENSOR_SERVICE_ERR_OK on success; non-zero otherwise.
 * @note   Task-context only, not ISR-safe.
 */
sensor_service_err_t sensor_service_run_cycle(void);

/**
 * @brief Copy the latest snapshot to @p snap (thread-safe pull model).
 *
 * Uses taskENTER_CRITICAL for the memcpy so any task may call this safely.
 *
 * @param[out] snap  Destination buffer; must not be NULL.
 * @return SENSOR_SERVICE_ERR_OK or SENSOR_SERVICE_ERR_NULL_ARG.
 */
sensor_service_err_t sensor_service_get_snapshot(sensor_snapshot_t *snap);

/**
 * @brief Register a new-reading callback (push model).
 *
 * Callback fires in SensorTask context after each acquisition cycle.
 * Maximum SENSOR_MAX_SUBSCRIBERS callbacks; returns ERR_NO_SUB if full.
 *
 * @param  cb  Callback pointer; must remain valid for the system lifetime.
 * @return SENSOR_SERVICE_ERR_OK or SENSOR_SERVICE_ERR_NO_SUB.
 */
sensor_service_err_t sensor_service_subscribe(void (*cb)(const sensor_snapshot_t *snap));

/**
 * @brief Trigger one additional acquisition cycle (REQ-SA-170).
 *
 * Executes synchronously in the caller's context (~3 ms at 80 MHz).
 *
 * @return SENSOR_SERVICE_ERR_OK on success; non-zero otherwise.
 * @note   Task-context only, not ISR-safe.
 */
sensor_service_err_t sensor_service_read_on_demand(void);

/**
 * @brief Return true if all non-failed sensors produced a valid reading.
 *
 * @return true if all active sensors are valid; false otherwise.
 */
bool sensor_service_is_ready(void);

/* ======================================================================= */
/* Singleton vtable — ISensorService (LLD-D10)                             */
/* ======================================================================= */

typedef struct
{
    /* cppcheck-suppress unusedStructMember -- called via vtable by consumers */
    sensor_service_err_t (*init)(void);
    /* cppcheck-suppress unusedStructMember -- called via vtable by SensorTask */
    sensor_service_err_t (*run_cycle)(void);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LcdUi, Modbus, Cloud */
    sensor_service_err_t (*get_snapshot)(sensor_snapshot_t *snap);
    /* cppcheck-suppress unusedStructMember -- called via vtable by AlarmService */
    sensor_service_err_t (*subscribe)(void (*cb)(const sensor_snapshot_t *));
    /* cppcheck-suppress unusedStructMember -- called via vtable by ModbusRegisterMap */
    sensor_service_err_t (*read_on_demand)(void);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleController */
    bool (*is_ready)(void);
} isensor_service_t;

/** Singleton pointer to the SensorService vtable (FD + GW). */
extern const isensor_service_t *const sensor_service;

/* ======================================================================= */
/* Test visibility macro                                                    */
/* ======================================================================= */

#ifdef SENSOR_SERVICE_TEST_VISIBLE
#undef SENSOR_SERVICE_TEST_VISIBLE
#endif

#ifdef TEST
#define SENSOR_SERVICE_TEST_VISIBLE
#else
#define SENSOR_SERVICE_TEST_VISIBLE static
#endif

/* ======================================================================= */
/* UNIT_TEST stub redirections                                              */
/* ======================================================================= */

#ifdef UNIT_TEST
#include "barometer_driver_stub.h"
#include "humidity_temp_driver_stub.h"

/* Stub function declarations — definitions provided by the test TU */
baro_err_t stub_baro_init(void);
baro_err_t stub_baro_read(baro_reading_t *reading);
ht_err_t stub_ht_init(void);
ht_err_t stub_ht_read(ht_reading_t *reading);
time_provider_err_t stub_time_get(time_provider_ts_t *ts_out);

/* Redirect driver and time calls to stubs */
#define barometer_init() stub_baro_init()
#define barometer_read(r) stub_baro_read(r)
#define humidity_temp_init() stub_ht_init()
#define humidity_temp_read(r) stub_ht_read(r)
#define time_provider_get(ts) stub_time_get(ts)
#endif /* UNIT_TEST */

/* ======================================================================= */
/* Test-only hooks                                                          */
/* ======================================================================= */

#ifdef TEST
/**
 * @brief Reset module state to post-BSS defaults (call from setUp()).
 */
void sensor_service_reset_for_test(void);

/**
 * @brief Set the IIR filter coefficient directly (TC-SS-004).
 */
void sensor_service_set_alpha_for_test(float alpha);

/**
 * @brief Set prev_filtered for one sensor (TC-SS-004).
 */
void sensor_service_set_prev_filtered_for_test(int id, float value);

/**
 * @brief Read back prev_filtered state for all sensors.
 */
void sensor_service_get_prev_filtered_for_test(float out[SENSOR_ID_COUNT]);
#endif /* TEST */

#endif /* SENSOR_SERVICE_H */
