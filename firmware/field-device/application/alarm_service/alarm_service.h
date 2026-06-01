/**
 * @file alarm_service.h
 * @brief AlarmService — threshold evaluation and IAlarmService vtable.
 *
 * Co-hosted in SensorTask. alarm_service_evaluate() is registered as a
 * SensorService subscriber and runs in SensorTask context after each
 * acquisition cycle. Alarm state writes are atomic word operations on
 * Cortex-M4 — no mutex required.
 *
 * @see docs/lld/application/sensor-alarm-service.md
 */

#ifndef ALARM_SERVICE_H
#define ALARM_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

#ifndef TEST
#include "sensor_service/sensor_service.h"
#else
#include "sensor_service_stub.h"
#endif /* TEST */

/* ======================================================================= */
/* Data types                                                               */
/* ======================================================================= */

#ifndef ALARM_STATE_DEFINED
#define ALARM_STATE_DEFINED
typedef enum
{
    ALARM_STATE_CLEAR = 0,
    ALARM_STATE_ACTIVE_HIGH = 1,
    ALARM_STATE_ACTIVE_LOW = 2,
} alarm_state_t;
#endif /* ALARM_STATE_DEFINED */

typedef enum
{
    ALARM_EVENT_RAISED_HIGH = 0,
    ALARM_EVENT_RAISED_LOW = 1,
    ALARM_EVENT_CLEARED = 2,
} alarm_event_t;

typedef enum
{
    ALARM_SERVICE_ERR_OK = 0,
    ALARM_SERVICE_ERR_NOT_INIT = 1,
    ALARM_SERVICE_ERR_NULL_ARG = 2,
    ALARM_SERVICE_ERR_NO_SUB = 3,
} alarm_service_err_t;

/** Maximum number of alarm-event subscribers (compile-time). */
#define ALARM_MAX_SUBSCRIBERS (4U)

/* ======================================================================= */
/* Public API — IAlarmService                                              */
/* ======================================================================= */

/**
 * @brief Initialise AlarmService.
 *
 * Loads alarm thresholds and hysteresis values from IConfigProvider (falls
 * back to compile-time defaults if absent). Resets all alarm states to
 * ALARM_STATE_CLEAR. Registers alarm_service_evaluate() as a SensorService
 * subscriber.
 *
 * @return ALARM_SERVICE_ERR_OK on success; non-zero otherwise.
 */
alarm_service_err_t alarm_service_init(void);

/**
 * @brief Get the current alarm state for one sensor.
 *
 * Thread-safe — alarm_state is an enum read atomically on Cortex-M4.
 *
 * @param  sensor     Sensor ID to query.
 * @param[out] state_out  Filled with the current alarm_state_t on success.
 * @return ALARM_SERVICE_ERR_OK or ALARM_SERVICE_ERR_NULL_ARG.
 */
alarm_service_err_t alarm_service_get_state(sensor_id_t sensor, alarm_state_t *state_out);

/**
 * @brief Get alarm states for all sensors at once.
 *
 * @param[out] states  Array of SENSOR_ID_COUNT alarm_state_t values.
 * @return ALARM_SERVICE_ERR_OK or ALARM_SERVICE_ERR_NULL_ARG.
 */
alarm_service_err_t alarm_service_get_all_states(alarm_state_t states[SENSOR_ID_COUNT]);

/**
 * @brief Register an alarm-event callback.
 *
 * Fired in SensorTask context when an alarm is raised or cleared.
 * Maximum ALARM_MAX_SUBSCRIBERS callbacks; returns ERR_NO_SUB if full.
 *
 * @param  cb  Callback; must remain valid for the system lifetime.
 * @return ALARM_SERVICE_ERR_OK or ALARM_SERVICE_ERR_NO_SUB.
 */
alarm_service_err_t alarm_service_subscribe(void (*cb)(sensor_id_t sensor, alarm_event_t event,
                                                       const sensor_reading_t *reading));

/**
 * @brief Force-clear all alarm flags (bulk acknowledge, LLD-D14).
 *
 * Clears all per-sensor alarm states regardless of current threshold
 * condition. If a threshold is still breached, the alarm re-raises on the
 * next evaluation cycle. Thread-safe — atomic word writes on Cortex-M4.
 *
 * @return ALARM_SERVICE_ERR_OK on success; non-zero otherwise.
 */
alarm_service_err_t alarm_service_ack_all(void);

/* ======================================================================= */
/* Singleton vtable — IAlarmService (LLD-D10, LLD-D14)                    */
/* ======================================================================= */

typedef struct
{
    /* cppcheck-suppress unusedStructMember -- called via vtable by SensorTask */
    alarm_service_err_t (*init)(void);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LcdUi, ModbusRegisterMap */
    alarm_service_err_t (*get_state)(sensor_id_t sensor, alarm_state_t *state_out);
    /* cppcheck-suppress unusedStructMember -- called via vtable by CloudPublisher */
    alarm_service_err_t (*get_all_states)(alarm_state_t states[SENSOR_ID_COUNT]);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LcdUi, Cloud */
    alarm_service_err_t (*subscribe)(void (*cb)(sensor_id_t, alarm_event_t,
                                                const sensor_reading_t *));
    /* cppcheck-suppress unusedStructMember -- called via vtable by ModbusRegisterMap */
    alarm_service_err_t (*ack_all)(void);
} ialarm_service_t;

/** Singleton pointer to the AlarmService vtable (FD + GW). */
extern const ialarm_service_t *const alarm_service;

/* ======================================================================= */
/* Test visibility macro                                                    */
/* ======================================================================= */

#ifdef ALARM_SERVICE_TEST_VISIBLE
#undef ALARM_SERVICE_TEST_VISIBLE
#endif

#ifdef TEST
#define ALARM_SERVICE_TEST_VISIBLE
#else
#define ALARM_SERVICE_TEST_VISIBLE static
#endif

/* ======================================================================= */
/* Test-only hooks                                                          */
/* ======================================================================= */

#ifdef TEST
/**
 * @brief Reset module state to post-BSS defaults (call from setUp()).
 */
void alarm_service_reset_for_test(void);

/**
 * @brief Directly invoke the evaluation callback with a test snapshot.
 *
 * Allows test TUs to inject snapshots into alarm_service_evaluate() without
 * going through the SensorService subscriber mechanism.
 */
void alarm_service_evaluate(const sensor_snapshot_t *snap);
#endif /* TEST */

#endif /* ALARM_SERVICE_H */
