/**
 * @file alarm_service_stub.h
 * @brief Narrow stub for AlarmService in lcd_ui unit tests.
 *
 * Provides ialarm_service_t with only the methods lcd_ui.c calls
 * (get_all_states). Basename does NOT match alarm_service.c, so Ceedling
 * does not auto-link the real implementation.
 *
 * alarm_state_t is guarded with ALARM_STATE_DEFINED to allow coexistence
 * with alarm_service.h when both headers would otherwise conflict.
 */

#ifndef ALARM_SERVICE_STUB_H
#define ALARM_SERVICE_STUB_H

#include <stdint.h>
#include "sensor_service_stub.h" /* sensor_id_t, SENSOR_ID_COUNT */

/* --------------------------------------------------------------------- */
/* alarm_state_t (must match alarm_service.h exactly)                   */
/* --------------------------------------------------------------------- */

#ifndef ALARM_STATE_DEFINED
#define ALARM_STATE_DEFINED
typedef enum
{
    ALARM_STATE_CLEAR       = 0,
    ALARM_STATE_ACTIVE_HIGH = 1,
    ALARM_STATE_ACTIVE_LOW  = 2,
} alarm_state_t;
#endif /* ALARM_STATE_DEFINED */

typedef enum
{
    ALARM_SERVICE_ERR_OK       = 0,
    ALARM_SERVICE_ERR_NOT_INIT = 1,
    ALARM_SERVICE_ERR_NULL_ARG = 2,
    ALARM_SERVICE_ERR_NO_SUB   = 3,
} alarm_service_err_t;

/* --------------------------------------------------------------------- */
/* ialarm_service_t — only the members lcd_ui.c accesses                */
/* --------------------------------------------------------------------- */

typedef struct
{
    alarm_service_err_t (*init)(void);
    alarm_service_err_t (*get_state)(sensor_id_t sensor, alarm_state_t *state_out);
    alarm_service_err_t (*get_all_states)(alarm_state_t states[SENSOR_ID_COUNT]);
    alarm_service_err_t (*subscribe)(void (*cb)(sensor_id_t, int, const void *));
    alarm_service_err_t (*ack_all)(void);
} ialarm_service_t;

#endif /* ALARM_SERVICE_STUB_H */
