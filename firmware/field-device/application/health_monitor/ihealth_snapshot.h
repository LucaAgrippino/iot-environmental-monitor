/**
 * @file ihealth_snapshot.h
 * @brief IHealthSnapshot — read-side vtable and device_health_snapshot_t.
 *
 * Provides the consolidated health snapshot struct and the read-side vtable.
 * Self-contained: defines the sensor/alarm/time enums it needs locally
 * (with inclusion guards so they do not conflict when the owning module
 * headers are also included).
 *
 * @see health_monitor.h for the full API.
 * @see docs/lld/application/health-monitor.md
 */

#ifndef IHEALTH_SNAPSHOT_H
#define IHEALTH_SNAPSHOT_H

#include <stdint.h>
#include <stdbool.h>

#include "lifecycle_controller/ilifecycle.h" /* lifecycle_state_t, lifecycle_reset_cause_t */
#include "health_monitor/ihealth_report.h"   /* health_monitor_err_t, HEALTH_TASK_COUNT */

/* ======================================================================= */
/* Supporting types — authoritative definitions live in the owning module  */
/* headers; redeclared here with inclusion guards for self-containment.    */
/* ======================================================================= */

/** time_sync_state_t — authoritative definition in time_provider.h. */
#ifndef TIME_SYNC_STATE_DEFINED
#define TIME_SYNC_STATE_DEFINED
typedef enum
{
    TIME_SYNC_UNSYNCHRONISED = 0,
    TIME_SYNC_SYNCHRONISED = 1,
} time_sync_state_t;
#endif /* TIME_SYNC_STATE_DEFINED */

/** sensor_id_t — authoritative definition in sensor_service.h. */
#ifndef SENSOR_ID_DEFINED
#define SENSOR_ID_DEFINED
typedef enum
{
    SENSOR_ID_TEMPERATURE = 0,
    SENSOR_ID_HUMIDITY = 1,
    SENSOR_ID_PRESSURE = 2,
    SENSOR_ID_ACCEL_X = 3, /* GW only */
    SENSOR_ID_ACCEL_Y = 4, /* GW only */
    SENSOR_ID_ACCEL_Z = 5, /* GW only */
    SENSOR_ID_GYRO_X = 6,  /* GW only */
    SENSOR_ID_GYRO_Y = 7,  /* GW only */
    SENSOR_ID_GYRO_Z = 8,  /* GW only */
    SENSOR_ID_MAG_X = 9,   /* GW only */
    SENSOR_ID_MAG_Y = 10,  /* GW only */
    SENSOR_ID_MAG_Z = 11,  /* GW only */
    SENSOR_ID_COUNT = 12,
} sensor_id_t;
#endif /* SENSOR_ID_DEFINED */

/** alarm_state_t — authoritative definition in alarm_service.h. */
#ifndef ALARM_STATE_DEFINED
#define ALARM_STATE_DEFINED
typedef enum
{
    ALARM_STATE_CLEAR = 0,
    ALARM_STATE_ACTIVE_HIGH = 1,
    ALARM_STATE_ACTIVE_LOW = 2,
} alarm_state_t;
#endif /* ALARM_STATE_DEFINED */

/* ======================================================================= */
/* Health snapshot                                                         */
/* ======================================================================= */

/**
 * @brief Consolidated device health state, updated continuously by producers.
 *
 * GW-only fields are conditional on BOARD_GATEWAY.
 */
typedef struct
{
    /* ── System ── */
    uint32_t uptime_s;
    lifecycle_state_t lifecycle_state;
    lifecycle_reset_cause_t last_reset_cause;

    /* ── Sensors ── */
    bool sensor_valid[SENSOR_ID_COUNT];
    uint32_t sensor_fail_count;
    time_sync_state_t time_sync_state;

    /* ── Alarms ── */
    alarm_state_t alarm_state[SENSOR_ID_COUNT];
    uint32_t alarm_raise_count;

    /* ── Config ── */
    bool config_write_failed;

    /* ── Modbus slave (FD) ── */
    uint32_t modbus_valid_frames;
    uint32_t modbus_crc_errors;
    uint32_t modbus_addr_mismatches;
    uint32_t modbus_exception_responses;
    bool modbus_slave_ok;

#if defined(BOARD_GATEWAY)
    /* ── Modbus master (GW) ── */
    uint32_t modbus_transactions_ok;
    uint32_t modbus_timeouts;
    bool modbus_link_online;

    /* ── Cloud (GW) ── */
    uint32_t mqtt_publishes_sent;
    uint32_t mqtt_publishes_failed;
    uint32_t mqtt_reconnect_count;
    int32_t wifi_rssi_dbm;
    bool cloud_connected;

    /* ── Store-and-forward (GW) ── */
    uint32_t buffer_entry_count;
    uint32_t buffer_overflow_count;

    /* ── NTP (GW) ── */
    uint32_t ntp_sync_fail_count;
    uint32_t last_ntp_sync_epoch;
#endif /* BOARD_GATEWAY */

    /* ── RTOS task stack watermarks ── */
    uint16_t stack_watermark_words[HEALTH_TASK_COUNT];
} device_health_snapshot_t;

/* ======================================================================= */
/* IHealthSnapshot vtable — read side                                      */
/* ======================================================================= */

typedef struct ihealth_snapshot_s
{
    health_monitor_err_t (*get_snapshot)(device_health_snapshot_t *snap_out);
} ihealth_snapshot_t;

/** Singleton pointer — read side. */
extern const ihealth_snapshot_t *const health_snapshot;

#endif /* IHEALTH_SNAPSHOT_H */
