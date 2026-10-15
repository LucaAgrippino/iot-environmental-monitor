/**
 * @file sensor_service.h
 * @brief SensorService (Gateway) — onboard sensor acquisition and ISensorService vtable.
 *
 * Reads the four Gateway sensor drivers every 100 ms (HumidityTemp, Barometer,
 * Imu, Magnetometer), converts raw driver output to fixed-point engineering units,
 * validates, clamps and low-pass filters each channel, then publishes one
 * snapshot to a pull interface (get_snapshot) and a push interface (subscribe).
 * AlarmService registers as the first subscriber.
 *
 * Units of sensor_reading_t.value (fixed-point int32_t), per sensor ID:
 *   - TEMPERATURE: 0.01 degC      - HUMIDITY: 0.01 %RH     - PRESSURE: 0.1 hPa
 *   - ACCEL_x:     mg             - GYRO_x:   mdps         - MAG_x:    mgauss
 * The IMU and magnetometer units match the CloudPublisher wire format (SS-D10).
 *
 * @note Companion: docs/lld/application/sensor-alarm-service.md (Gateway sections)
 */

#ifndef SENSOR_SERVICE_H
#define SENSOR_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "health_monitor/health_monitor.h" /* health_monitor_handle_t, sensor_id_t */
#include "time_provider/time_provider.h"

/* ======================================================================= */
/* Data types                                                               */
/* ======================================================================= */

/* sensor_id_t is shared with HealthMonitor and defined behind SENSOR_ID_DEFINED
 * in health_monitor.h; both enumerations are identical (HM companion). */

/**
 * @brief One processed sensor sample (value after the IIR filter, with validity flag).
 */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- consumed by AlarmService, CloudPublisher */
    int32_t value; /**< Fixed-point; units depend on sensor_id_t (see file comment). */
    /* cppcheck-suppress unusedStructMember -- consumed by AlarmService */
    bool valid; /**< false if the driver errored, the sample was out of range, or none yet. */
    /* cppcheck-suppress unusedStructMember -- consumed by CloudPublisher */
    time_provider_ts_t timestamp; /**< Time of the last accepted acquisition (REQ-SA-100). */
} sensor_reading_t;

/**
 * @brief Snapshot of every sensor reading produced by one acquisition cycle.
 */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- accessed by get_snapshot() callers */
    sensor_reading_t readings[SENSOR_ID_COUNT];
    /* cppcheck-suppress unusedStructMember -- read by CloudPublisher */
    uint32_t cycle_count; /**< Increments on every sensor_service_run_cycle() call. */
} sensor_snapshot_t;

/** @brief Error codes returned by SensorService operations. */
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
 * @brief Initialise SensorService and the four sensor drivers.
 *
 * Calls humidity_temp_init(), barometer_init(), imu_init() and magnetometer_init().
 * A driver that fails is marked permanently failed, logged, and reported to
 * HealthMonitor as HEALTH_EVENT_SENSOR_FAIL; the others carry on (REQ-SA-040,
 * REQ-SA-060). Filter and range parameters are compile-time defaults because the
 * Gateway has no IConfigProvider yet (SS-O6).
 *
 * @param[in] health  HealthMonitor handle receiving sensor fail / recover events.
 * @return SENSOR_SERVICE_ERR_OK; SENSOR_SERVICE_ERR_NULL_ARG if @p health is NULL.
 * @note Threading: task-context only. Call after i2c_init() and gpio_init(), before
 *       the scheduler starts.
 */
sensor_service_err_t sensor_service_init(health_monitor_handle_t health);

/**
 * @brief Run one acquisition cycle.
 *
 * Reads every non-failed driver, applies the per-channel pipeline (convert, stamp,
 * range-validate, clamp, IIR filter), commits the snapshot and calls the subscribers
 * in registration order. Both Group B sensors are read on every cycle even when the
 * sample is discarded, so their latched DRDY / INT1 lines are always released (GPB-O6).
 *
 * @return SENSOR_SERVICE_ERR_OK; SENSOR_SERVICE_ERR_NOT_INIT.
 * @note Threading: SensorTask only. Not ISR-safe.
 */
sensor_service_err_t sensor_service_run_cycle(void);

/**
 * @brief Copy the latest snapshot to @p snap.
 *
 * @param[out] snap  Destination; must not be NULL.
 * @return SENSOR_SERVICE_ERR_OK; SENSOR_SERVICE_ERR_NOT_INIT; SENSOR_SERVICE_ERR_NULL_ARG.
 * @note Threading: any task. The copy is made inside a critical section.
 */
sensor_service_err_t sensor_service_get_snapshot(sensor_snapshot_t *snap);

/**
 * @brief Register a new-reading callback.
 *
 * The callback runs in SensorTask context after each cycle, so it must be short.
 *
 * @param[in] cb  Callback; must remain valid for the system lifetime.
 * @return SENSOR_SERVICE_ERR_OK; SENSOR_SERVICE_ERR_NOT_INIT; SENSOR_SERVICE_ERR_NULL_ARG;
 *         SENSOR_SERVICE_ERR_NO_SUB if SENSOR_MAX_SUBSCRIBERS callbacks are registered.
 * @note Threading: call before the scheduler starts.
 */
sensor_service_err_t sensor_service_subscribe(void (*cb)(const sensor_snapshot_t *snap));

/**
 * @brief Report whether every non-failed sensor produced a valid reading last cycle.
 *
 * @return true if initialised and no live sensor is invalid; false otherwise.
 * @note Threading: any task.
 */
bool sensor_service_is_ready(void);

/**
 * @brief Run one additional acquisition cycle outside the periodic timer (REQ-SA-170).
 *
 * Executes synchronously in the caller's context and blocks for up to the cycle WCET
 * (about 5 ms). The result is pushed to the subscribers exactly like a periodic cycle.
 *
 * @return SENSOR_SERVICE_ERR_OK; SENSOR_SERVICE_ERR_NOT_INIT.
 * @note Threading: callers must not run concurrently with SensorTask; the Gateway has no
 *       caller yet (SS-O7).
 */
sensor_service_err_t sensor_service_read_on_demand(void);

/* ======================================================================= */
/* Singleton vtable — ISensorService                                        */
/* ======================================================================= */

typedef struct
{
    /* cppcheck-suppress unusedStructMember -- called via vtable by the system layer */
    sensor_service_err_t (*init)(health_monitor_handle_t health);
    /* cppcheck-suppress unusedStructMember -- called via vtable by SensorTask */
    sensor_service_err_t (*run_cycle)(void);
    /* cppcheck-suppress unusedStructMember -- called via vtable by CloudPublisher */
    sensor_service_err_t (*get_snapshot)(sensor_snapshot_t *snap);
    /* cppcheck-suppress unusedStructMember -- called via vtable by AlarmService */
    sensor_service_err_t (*subscribe)(void (*cb)(const sensor_snapshot_t *));
    /* cppcheck-suppress unusedStructMember -- called via vtable by on-demand read paths */
    sensor_service_err_t (*read_on_demand)(void);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleController */
    bool (*is_ready)(void);
} isensor_service_t;

/** Singleton pointer to the SensorService vtable. */
extern const isensor_service_t *const sensor_service;

#ifdef TEST
/** @brief Reset all module state (unit tests only). */
void sensor_service_reset_for_test(void);

/** @brief Seed the IIR coefficient as num/den (unit tests only). */
void sensor_service_set_alpha_for_test(uint8_t num, uint8_t den);

/** @brief Read Group A every @p cycles cycles instead of the default 10 (unit tests only). */
void sensor_service_set_group_a_period_for_test(uint32_t cycles);

/** @brief Convert one raw driver sample to engineering units (unit tests only). */
int32_t sensor_service_convert_for_test(sensor_id_t id, int16_t raw);
#endif /* TEST */

#endif /* SENSOR_SERVICE_H */
