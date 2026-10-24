/**
 * @file alarm_service.h
 * @brief AlarmService (Gateway) — threshold evaluation and IAlarmService vtable.
 *
 * Co-hosted in SensorTask. alarm_service_evaluate() is registered as a SensorService
 * subscriber and runs in SensorTask context after every acquisition cycle.
 *
 * Only temperature, humidity and pressure carry thresholds. The IMU and magnetometer
 * channels are never evaluated (SS-O3), because their readings are valid on the
 * Gateway and a zero threshold would alarm on any non-zero value.
 *
 * @note Companion: docs/lld/application/sensor-alarm-service.md (Gateway sections)
 */

#ifndef ALARM_SERVICE_H
#define ALARM_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "sensor_service/sensor_service.h"

/* ======================================================================= */
/* Data types                                                               */
/* ======================================================================= */

/* alarm_state_t is shared with HealthMonitor and defined behind ALARM_STATE_DEFINED
 * in health_monitor.h. */

/** @brief Alarm transition passed to subscribers. */
typedef enum
{
    ALARM_EVENT_RAISED_HIGH = 0,
    ALARM_EVENT_RAISED_LOW = 1,
    ALARM_EVENT_CLEARED = 2,
} alarm_event_t;

/** @brief Error codes returned by AlarmService operations. */
typedef enum
{
    ALARM_SERVICE_ERR_OK = 0,
    ALARM_SERVICE_ERR_NOT_INIT = 1,
    ALARM_SERVICE_ERR_NULL_ARG = 2,
    ALARM_SERVICE_ERR_NO_SUB = 3,      /**< Subscriber table full. */
    ALARM_SERVICE_ERR_INVALID_ARG = 4, /**< Sensor ID out of range. */
} alarm_service_err_t;

/** Maximum number of alarm-event subscribers (compile-time). */
#define ALARM_MAX_SUBSCRIBERS (4U)

/* ======================================================================= */
/* Public API — IAlarmService                                              */
/* ======================================================================= */

/**
 * @brief Initialise AlarmService and register it as a SensorService subscriber.
 *
 * Loads the compile-time default thresholds (no IConfigProvider on the Gateway yet,
 * SS-O6) and clears every alarm. SensorService must already be initialised.
 *
 * @return ALARM_SERVICE_ERR_OK; ALARM_SERVICE_ERR_NOT_INIT if registering with
 *         SensorService fails.
 * @note Threading: call before the scheduler starts.
 */
alarm_service_err_t alarm_service_init(void);

/**
 * @brief Get the alarm state of one sensor.
 *
 * @param[in]  sensor     Sensor ID.
 * @param[out] state_out  Receives the state; must not be NULL.
 * @return ALARM_SERVICE_ERR_OK; _NOT_INIT; _NULL_ARG; _INVALID_ARG if @p sensor is out of range.
 * @note Threading: any task. A single enum is read atomically on Cortex-M4.
 */
alarm_service_err_t alarm_service_get_state(sensor_id_t sensor, alarm_state_t *state_out);

/**
 * @brief Get the alarm states of all sensors.
 *
 * @param[out] states  Array of SENSOR_ID_COUNT entries; must not be NULL.
 * @return ALARM_SERVICE_ERR_OK; _NOT_INIT; _NULL_ARG.
 * @note Threading: any task.
 */
alarm_service_err_t alarm_service_get_all_states(alarm_state_t states[SENSOR_ID_COUNT]);

/**
 * @brief Register an alarm-event callback, run in SensorTask context.
 *
 * @param[in] cb  Callback; must remain valid for the system lifetime.
 * @return ALARM_SERVICE_ERR_OK; _NOT_INIT; _NULL_ARG; _NO_SUB if the table is full.
 * @note Threading: call before the scheduler starts.
 */
alarm_service_err_t alarm_service_subscribe(void (*cb)(sensor_id_t sensor, alarm_event_t event,
                                                       const sensor_reading_t *reading));

/**
 * @brief Get the threshold that a raised alarm event crossed.
 *
 * ALARM_EVENT_RAISED_HIGH yields the high limit and ALARM_EVENT_RAISED_LOW the low
 * limit, in the units of sensor_reading_t.value. The subscriber callback carries the
 * reading but not the limit, so CloudPublisher reads it here for the alarm payload.
 *
 * @param[in]  sensor     Sensor ID.
 * @param[in]  event      ALARM_EVENT_RAISED_HIGH or ALARM_EVENT_RAISED_LOW.
 * @param[out] limit_out  Receives the limit; must not be NULL.
 * @return ALARM_SERVICE_ERR_OK; _NOT_INIT; _NULL_ARG; _INVALID_ARG if @p sensor is out of
 *         range, has no threshold (IMU / magnetometer) or @p event is ALARM_EVENT_CLEARED.
 * @note Threading: any task.
 */
alarm_service_err_t alarm_service_get_limit(sensor_id_t sensor, alarm_event_t event,
                                            int32_t *limit_out);

/**
 * @brief Force-clear every alarm (bulk acknowledge, LLD-D14).
 *
 * A condition that is still breached re-raises on the next cycle.
 *
 * @return ALARM_SERVICE_ERR_OK; _NOT_INIT.
 * @note Threading: any task.
 */
alarm_service_err_t alarm_service_ack_all(void);

/* ======================================================================= */
/* Singleton vtable — IAlarmService                                         */
/* ======================================================================= */

typedef struct
{
    /* cppcheck-suppress unusedStructMember -- called via vtable by the system layer */
    alarm_service_err_t (*init)(void);
    /* cppcheck-suppress unusedStructMember -- called via vtable by consumers */
    alarm_service_err_t (*get_state)(sensor_id_t sensor, alarm_state_t *state_out);
    /* cppcheck-suppress unusedStructMember -- called via vtable by CloudPublisher */
    alarm_service_err_t (*get_all_states)(alarm_state_t states[SENSOR_ID_COUNT]);
    /* cppcheck-suppress unusedStructMember -- called via vtable by CloudPublisher */
    alarm_service_err_t (*subscribe)(void (*cb)(sensor_id_t, alarm_event_t,
                                                const sensor_reading_t *));
    /* cppcheck-suppress unusedStructMember -- called via vtable by CloudPublisher */
    alarm_service_err_t (*get_limit)(sensor_id_t sensor, alarm_event_t event, int32_t *limit_out);
    /* cppcheck-suppress unusedStructMember -- called via vtable by the command path */
    alarm_service_err_t (*ack_all)(void);
} ialarm_service_t;

/** Singleton pointer to the AlarmService vtable. */
extern const ialarm_service_t *const alarm_service;

#ifdef TEST
/** @brief Reset module state (unit tests only). */
void alarm_service_reset_for_test(void);

/** @brief Evaluate a snapshot directly, bypassing SensorService (unit tests only). */
void alarm_service_evaluate(const sensor_snapshot_t *snap);
#endif /* TEST */

#endif /* ALARM_SERVICE_H */
