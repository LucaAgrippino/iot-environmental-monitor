/**
 * @file ihealth_report.h
 * @brief IHealthReport — write-side vtable for HealthMonitor producers.
 *
 * Contains only the vtable struct, error codes, event enum, and minimal
 * forward declarations. Consumers that push health events include this
 * header rather than health_monitor.h, avoiding include-graph cycles.
 *
 * @see health_monitor.h for the full API (init, poll, snapshot, admin).
 * @see docs/lld/application/health-monitor.md
 */

#ifndef IHEALTH_REPORT_H
#define IHEALTH_REPORT_H

#include <stdint.h>

/* ======================================================================= */
/* Constants                                                               */
/* ======================================================================= */

/** Number of monitored RTOS tasks — matches task-breakdown.md §7. */
#define HEALTH_TASK_COUNT 7U

/* ======================================================================= */
/* Error codes                                                             */
/* ======================================================================= */

typedef enum
{
    HEALTH_MONITOR_ERR_OK = 0,
    HEALTH_MONITOR_ERR_NOT_INIT = 1,
    HEALTH_MONITOR_ERR_NULL_ARG = 2,
} health_monitor_err_t;

/* ======================================================================= */
/* Health events                                                           */
/* ======================================================================= */

typedef enum
{
    HEALTH_EVENT_TIME_SYNC_ACQUIRED = 0,
    HEALTH_EVENT_TIME_SYNC_LOST = 1,
    HEALTH_EVENT_CONFIG_WRITE_FAIL = 2,
    HEALTH_EVENT_CONFIG_READ_FAIL = 3,
    HEALTH_EVENT_CONFIG_NO_VALID_SLOT = 4,
    HEALTH_EVENT_SENSOR_FAIL = 5,
    HEALTH_EVENT_NTP_SYNC_FAILED = 6,      /* GW */
    HEALTH_EVENT_NTP_BAD_RESPONSE = 7,     /* GW */
    HEALTH_EVENT_BUFFER_OVERFLOW = 8,      /* GW */
    HEALTH_EVENT_BUFFER_FLASH_ERR = 9,     /* GW */
    HEALTH_EVENT_MODBUS_LINK_UP = 10,      /* GW */
    HEALTH_EVENT_MODBUS_NODE_OFFLINE = 11, /* GW */
    HEALTH_EVENT_ALARM_RAISED = 12,
    HEALTH_EVENT_ALARM_CLEARED = 13,
    HEALTH_EVENT_FAULT = 14,
    HEALTH_EVENT_LCD_FAIL = 15, /* FD: LCD UI init or refresh error */
} health_event_t;

/* ======================================================================= */
/* Modbus / cloud statistics types                                         */
/* ======================================================================= */

/* Complete definition so consumers (health_monitor.h, health_monitor.c)
 * can allocate modbus_slave_stats_t on the stack.  The guard prevents a
 * duplicate-definition error when modbus_slave.h is also included in the
 * same translation unit (e.g. in lifecycle_controller.h production build). */
#ifndef MODBUS_SLAVE_STATS_S_DEFINED
#define MODBUS_SLAVE_STATS_S_DEFINED
typedef struct modbus_slave_stats_s
{
    uint32_t valid_frames;
    uint32_t crc_errors;
    uint32_t address_mismatches;
    uint32_t exception_responses;
    uint32_t unsupported_fc;
    uint32_t successful_responses;
} modbus_slave_stats_t;
#endif /* MODBUS_SLAVE_STATS_S_DEFINED */

#if defined(BOARD_GATEWAY)
/** GW-only placeholder until modbus_master.h is implemented. */
typedef struct modbus_master_stats_s
{
    uint32_t transactions_ok;
    uint32_t timeouts;
} modbus_master_stats_t;

/** GW-only placeholder until mqtt_client.h is implemented. */
typedef struct mqtt_stats_s
{
    uint32_t publishes_sent;
    uint32_t publishes_failed;
    uint32_t reconnect_count;
    int32_t rssi_dbm;
} mqtt_stats_t;
#endif /* BOARD_GATEWAY */

/* ======================================================================= */
/* IHealthReport vtable — write side                                       */
/* ======================================================================= */

typedef struct ihealth_report_s
{
    health_monitor_err_t (*init)(void);
    health_monitor_err_t (*push_event)(health_event_t event, uint32_t param);
    health_monitor_err_t (*update_modbus_slave_stats)(const modbus_slave_stats_t *stats);
#if defined(BOARD_GATEWAY)
    health_monitor_err_t (*update_modbus_master_stats)(const modbus_master_stats_t *stats);
    health_monitor_err_t (*update_mqtt_stats)(const mqtt_stats_t *stats);
    health_monitor_err_t (*update_buffer_occupancy)(uint32_t entry_count);
#endif /* BOARD_GATEWAY */
    health_monitor_err_t (*update_stack_watermarks)(const uint16_t watermarks[HEALTH_TASK_COUNT]);
    void (*set_led_fault)(void);
} ihealth_report_t;

/** Singleton pointer — write side. */
extern const ihealth_report_t *const health_report;

#endif /* IHEALTH_REPORT_H */
