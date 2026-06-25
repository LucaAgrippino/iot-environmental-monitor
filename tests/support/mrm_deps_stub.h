/**
 * @file mrm_deps_stub.h
 * @brief Consolidated type-only stub for ModbusRegisterMap unit tests.
 *
 * Provides all type definitions needed by modbus_register_map.h and
 * modbus_register_map.c when compiled under TEST. Replaces all real module
 * headers (sensor_service.h, alarm_service.h, config_service.h,
 * health_monitor.h, time_provider.h) to prevent Ceedling from auto-linking
 * their implementation files and cascading driver dependencies.
 *
 * Key difference from sensor_service_stub.h: sensor_reading_t.value is
 * int32_t (fixed-point, matching production). sensor_service_stub.h uses
 * float and is NOT compatible with MRM tests.
 *
 * Basename: mrm_deps_stub — does NOT match any production .c file.
 */

#ifndef MRM_DEPS_STUB_H
#define MRM_DEPS_STUB_H

#include <stdint.h>
#include <stdbool.h>

/* config_params.h is safe to include directly — it has no transitive deps
 * that would cause Ceedling to auto-link an implementation file.        */
#include "config_service/config_params.h"



#include "lifecycle_controller/ilifecycle.h"
/* ===================================================================== */
/* time_provider types                                                   */
/* ===================================================================== */

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

typedef enum
{
    TIME_PROVIDER_ERR_OK = 0,
    TIME_PROVIDER_ERR_NOT_INIT = 1,
    TIME_PROVIDER_ERR_RTC_FAIL = 2,
    TIME_PROVIDER_ERR_NULL_ARG = 3,
} time_provider_err_t;

typedef struct
{
    time_provider_err_t (*get)(time_provider_ts_t *ts_out);
    time_provider_err_t (*set_time)(uint32_t new_epoch);
    time_provider_err_t (*mark_unsynchronised)(void);
    time_sync_state_t (*get_sync_state)(void);
} itime_provider_t;

/* ===================================================================== */
/* sensor_service types                                                  */
/* ===================================================================== */

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

/* NOTE: value is int32_t (fixed-point) — NOT float.
 * sensor_service_stub.h incorrectly uses float; this stub is correct. */
typedef struct
{
    int32_t value; /* fixed-point; units per sensor_id_t  */
    bool valid;    /* false on driver error or range fail  */
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

typedef struct
{
    sensor_service_err_t (*init)(void);
    sensor_service_err_t (*run_cycle)(void);
    sensor_service_err_t (*get_snapshot)(sensor_snapshot_t *snap);
    sensor_service_err_t (*subscribe)(void (*cb)(const sensor_snapshot_t *));
    sensor_service_err_t (*read_on_demand)(void);
    bool (*is_ready)(void);
} isensor_service_t;

/* ===================================================================== */
/* alarm_service types                                                   */
/* ===================================================================== */

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
    ALARM_SERVICE_ERR_OK = 0,
    ALARM_SERVICE_ERR_NOT_INIT = 1,
    ALARM_SERVICE_ERR_NULL_ARG = 2,
    ALARM_SERVICE_ERR_NO_SUB = 3,
} alarm_service_err_t;

typedef struct
{
    alarm_service_err_t (*init)(void);
    alarm_service_err_t (*get_state)(sensor_id_t sensor, alarm_state_t *state_out);
    alarm_service_err_t (*get_all_states)(alarm_state_t states[SENSOR_ID_COUNT]);
    alarm_service_err_t (*subscribe)(void (*cb)(sensor_id_t, int, const sensor_reading_t *));
    alarm_service_err_t (*ack_all)(void);
} ialarm_service_t;

/* ===================================================================== */
/* config_service types (config_params_t already included above)        */
/* ===================================================================== */

typedef enum
{
    CONFIG_SERVICE_OK = 0,
    CONFIG_SERVICE_ERR_NOT_INIT = 1,
    CONFIG_SERVICE_ERR_NULL_ARG = 2,
    CONFIG_SERVICE_ERR_INVALID = 3,
    CONFIG_SERVICE_ERR_PERSIST = 4,
} config_service_err_t;

typedef enum
{
    CONFIG_PARAM_POLL_INTERVAL = 0,
    CONFIG_PARAM_FILTER_ALPHA = 1,
    CONFIG_PARAM_TEMP_RANGE_MIN = 2,
    CONFIG_PARAM_TEMP_RANGE_MAX = 3,
    CONFIG_PARAM_HUMIDITY_RANGE_MIN = 4,
    CONFIG_PARAM_HUMIDITY_RANGE_MAX = 5,
    CONFIG_PARAM_PRESSURE_RANGE_MIN = 6,
    CONFIG_PARAM_PRESSURE_RANGE_MAX = 7,
    CONFIG_PARAM_TEMP_ALARM_HIGH = 8,
    CONFIG_PARAM_TEMP_ALARM_LOW = 9,
    CONFIG_PARAM_TEMP_HYSTERESIS = 10,
    CONFIG_PARAM_HUMIDITY_ALARM_HIGH = 11,
    CONFIG_PARAM_HUMIDITY_ALARM_LOW = 12,
    CONFIG_PARAM_HUMIDITY_HYSTERESIS = 13,
    CONFIG_PARAM_PRESSURE_ALARM_HIGH = 14,
    CONFIG_PARAM_PRESSURE_ALARM_LOW = 15,
    CONFIG_PARAM_PRESSURE_HYSTERESIS = 16,
    CONFIG_PARAM_MODBUS_SLAVE_ADDR = 17,
    CONFIG_PARAM_MODBUS_POLL_PERIOD = 18,
    CONFIG_PARAM_COUNT,
} config_param_id_t;

typedef void (*config_change_cb_t)(config_param_id_t param_id);

typedef struct
{
    const config_params_t *(*get_params)(void);
} iconfig_provider_t;

typedef struct
{
    config_service_err_t (*init)(const void *store);
    config_service_err_t (*apply_loaded)(const void *blob, uint32_t len);
    config_service_err_t (*set_param)(config_param_id_t id, const void *value);
    config_service_err_t (*validate_param)(config_param_id_t id, const void *value);
    config_service_err_t (*snapshot)(void);
    config_service_err_t (*restore_snapshot)(void);
    config_service_err_t (*flush)(void);
    config_service_err_t (*register_change_callback)(config_change_cb_t cb);
} iconfig_manager_t;

/* ===================================================================== */
/* modbus_slave_stats_t (forward copy — same layout as modbus_slave.h)  */
/* ===================================================================== */

#ifndef MODBUS_SLAVE_STATS_DEFINED
#define MODBUS_SLAVE_STATS_DEFINED
typedef struct
{
    uint32_t valid_frames;
    uint32_t crc_errors;
    uint32_t address_mismatches;
    uint32_t exception_responses;
    uint32_t unsupported_fc;
    uint32_t successful_responses;
} modbus_slave_stats_t;
#endif /* MODBUS_SLAVE_STATS_DEFINED */

/* ===================================================================== */
/* health_monitor types                                                  */
/* ===================================================================== */

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
    HEALTH_EVENT_ALARM_RAISED = 12,
    HEALTH_EVENT_ALARM_CLEARED = 13,
    HEALTH_EVENT_FAULT = 14,
} health_event_t;

#define HEALTH_TASK_COUNT (7U)

/* Full device_health_snapshot_t layout matches health_monitor.h.
 * Only the subset of fields accessed by MRM needs to be correct; the
 * layout must match so memset / struct-copy operations behave correctly. */
typedef struct
{
    uint32_t uptime_s;
    lifecycle_state_t lifecycle_state;
    lifecycle_reset_cause_t last_reset_cause;
    bool sensor_valid[SENSOR_ID_COUNT];
    uint32_t sensor_fail_count;
    time_sync_state_t time_sync_state;
    alarm_state_t alarm_state[SENSOR_ID_COUNT];
    uint32_t alarm_raise_count;
    bool config_write_failed;
    uint32_t modbus_valid_frames;
    uint32_t modbus_crc_errors;
    uint32_t modbus_addr_mismatches;
    uint32_t modbus_exception_responses;
    uint16_t stack_watermark_words[HEALTH_TASK_COUNT];
} device_health_snapshot_t;

/* ihealth_report_t — struct tag matches health_monitor.h for compatibility */
#ifndef IHEALTH_REPORT_T_DEFINED
#define IHEALTH_REPORT_T_DEFINED
struct ihealth_report_s;
typedef struct ihealth_report_s ihealth_report_t;
#endif /* IHEALTH_REPORT_T_DEFINED */

struct ihealth_report_s
{
    health_monitor_err_t (*init)(void);
    health_monitor_err_t (*push_event)(health_event_t event, uint32_t param);
    health_monitor_err_t (*update_modbus_slave_stats)(const modbus_slave_stats_t *stats);
    health_monitor_err_t (*update_stack_watermarks)(const uint16_t watermarks[HEALTH_TASK_COUNT]);
    void (*set_led_fault)(void);
};

typedef struct ihealth_snapshot_s
{
    health_monitor_err_t (*get_snapshot)(device_health_snapshot_t *snap_out);
} ihealth_snapshot_t;

#endif /* MRM_DEPS_STUB_H */
