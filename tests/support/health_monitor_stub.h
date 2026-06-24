/**
 * @file health_monitor_stub.h
 * @brief Narrow stub for HealthMonitor in unit tests.
 *
 * Provides ihealth_report_t (TimeProvider), ihealth_snapshot_t, and
 * device_health_snapshot_t (LcdUi, ConsoleService). Prevents Ceedling from
 * auto-linking health_monitor.c (which cascades to led_driver → gpio → CMSIS).
 *
 * The struct tag ihealth_report_s matches the definition in health_monitor.h,
 * so production code can include both headers without redefinition errors.
 *
 * Basename: health_monitor_stub — does NOT match health_monitor.c.
 */

#ifndef HEALTH_MONITOR_STUB_H
#define HEALTH_MONITOR_STUB_H

#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------- */
/* Sensor and alarm enumerations (guards match health_monitor.h)         */
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

#ifndef ALARM_STATE_DEFINED
#define ALARM_STATE_DEFINED
typedef enum
{
    ALARM_STATE_CLEAR = 0,
    ALARM_STATE_ACTIVE_HIGH = 1,
    ALARM_STATE_ACTIVE_LOW = 2,
} alarm_state_t;
#endif /* ALARM_STATE_DEFINED */

/* Forward typedef so ihealth_report_t can be used in the extern declaration
 * below even when time_provider.h has not yet been included.
 * Redefinition is valid in C99+ when both typedef refer to the same type. */
#ifndef IHEALTH_REPORT_T_DEFINED
#define IHEALTH_REPORT_T_DEFINED
struct ihealth_report_s;
typedef struct ihealth_report_s ihealth_report_t;
#endif /* IHEALTH_REPORT_T_DEFINED */

/* --------------------------------------------------------------------- */
/* Minimal types — only what TimeProvider needs from health_monitor.h    */
/* --------------------------------------------------------------------- */

typedef enum
{
    HEALTH_MONITOR_ERR_OK = 0,
    HEALTH_MONITOR_ERR_NOT_INIT = 1,
    HEALTH_MONITOR_ERR_NULL_ARG = 2,
} health_monitor_err_t;

typedef enum
{
    HEALTH_EVENT_TIME_SYNC_ACQUIRED = 0,
    HEALTH_EVENT_TIME_SYNC_LOST = 1,
    HEALTH_EVENT_CONFIG_WRITE_FAIL = 2,
    HEALTH_EVENT_CONFIG_READ_FAIL = 3,
    HEALTH_EVENT_CONFIG_NO_VALID_SLOT = 4,
    HEALTH_EVENT_SENSOR_FAIL = 5,
    HEALTH_EVENT_LCD_FAIL = 15, /* mirrors health_monitor.h */
} health_event_t;

/* --------------------------------------------------------------------- */
/* IHealthReport vtable (struct tag matches health_monitor.h)            */
/* --------------------------------------------------------------------- */

/* Complete the struct body. The typedef name ihealth_report_t is declared in
 * time_provider.h (which the test TU always includes alongside this stub).
 * Including a struct body for a forward-declared tag is always valid in C. */
struct ihealth_report_s
{
    health_monitor_err_t (*init)(void);
    health_monitor_err_t (*push_event)(health_event_t event, uint32_t param);
    /* Remaining function pointers intentionally omitted — callers must not
     * access beyond push_event in stub builds. */
};

/* Singleton pointer — declared here, defined in the test TU that needs it. */
extern const ihealth_report_t *const health_report;

/* --------------------------------------------------------------------- */
/* device_health_snapshot_t — superset covering LcdUi and ConsoleService */
/* Fields match health_monitor.h exactly (same names, same order).       */
/* --------------------------------------------------------------------- */

#define HEALTH_STUB_TASK_COUNT (7U) /* mirrors HEALTH_TASK_COUNT */

typedef struct
{
    /* System */
    uint32_t uptime_s;

    /* Sensors */
    bool sensor_valid[SENSOR_ID_COUNT]; /* ConsoleService: sensors cmd    */
    uint32_t sensor_fail_count;

    /* Alarms — ConsoleService: alarms cmd */
    alarm_state_t alarm_state[SENSOR_ID_COUNT];
    uint32_t alarm_raise_count;

    /* Config */
    bool config_write_failed;

    /* Modbus slave (FD) */
    uint32_t modbus_valid_frames;
    uint32_t modbus_crc_errors;
    uint32_t modbus_addr_mismatches; /* ConsoleService: modbus status cmd */
    uint32_t modbus_exception_responses;
    bool modbus_slave_ok; /* ConsoleService: selftest comms     */

    /* Stack watermarks */
    uint16_t stack_watermark_words[HEALTH_STUB_TASK_COUNT];
} device_health_snapshot_t;

/* --------------------------------------------------------------------- */
/* ihealth_snapshot_t — read-side vtable                                */
/* --------------------------------------------------------------------- */

typedef struct
{
    health_monitor_err_t (*get_snapshot)(device_health_snapshot_t *snap_out);
} ihealth_snapshot_t;

#endif /* HEALTH_MONITOR_STUB_H */
