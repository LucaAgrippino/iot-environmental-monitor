/**
 * @file health_monitor.h
 * @brief HealthMonitor — device health aggregator and LED status indicator.
 *
 * Provides three vtable interfaces:
 *   - IHealthReport  (write side — producers push metric events)
 *   - IHealthSnapshot (read side — consumers copy the consolidated snapshot)
 *   - IHealthAdmin   (control side — LifecycleController resets counters)
 *
 * HealthMonitor has no thread. All operations execute in the calling
 * task's context, serialised by an internal priority-inheritance mutex.
 *
 * @see docs/lld/application/health-monitor.md
 */

#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <stdint.h>
#include <stdbool.h>

/* ======================================================================= */
/* Forward type declarations from modules not yet implemented.             */
/* TODO: replace each block with #include "<module>.h" when available.    */
/* ======================================================================= */

/* lifecycle_controller.h */
typedef enum
{
    LIFECYCLE_STATE_INIT = 0,
    LIFECYCLE_STATE_OPERATIONAL = 1,
    LIFECYCLE_STATE_EDITING_CONFIG = 2,
    LIFECYCLE_STATE_RESTARTING = 3,
    LIFECYCLE_STATE_UPDATING_FW = 4,
    LIFECYCLE_STATE_FAULTED = 5,
} lifecycle_state_t;

typedef enum
{
    LIFECYCLE_RESET_POWER_ON = 0,
    LIFECYCLE_RESET_SOFT = 1,
    LIFECYCLE_RESET_WATCHDOG = 2,
    LIFECYCLE_RESET_UNKNOWN = 3,
} lifecycle_reset_cause_t;

/* time_provider.h */
#ifndef TIME_SYNC_STATE_DEFINED
#define TIME_SYNC_STATE_DEFINED
typedef enum
{
    TIME_SYNC_UNSYNCHRONISED = 0,
    TIME_SYNC_SYNCHRONISED = 1,
} time_sync_state_t;
#endif /* TIME_SYNC_STATE_DEFINED */

/* sensor_service.h */
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

/* alarm_service.h */
#ifndef ALARM_STATE_DEFINED
#define ALARM_STATE_DEFINED
typedef enum
{
    ALARM_STATE_CLEAR = 0,
    ALARM_STATE_ACTIVE_HIGH = 1,
    ALARM_STATE_ACTIVE_LOW = 2,
} alarm_state_t;
#endif /* ALARM_STATE_DEFINED */

/* modbus_slave.h */
typedef struct
{
    uint32_t valid_frames;
    uint32_t crc_errors;
    uint32_t address_mismatches;
    uint32_t exception_responses;
    uint32_t unsupported_fc;
    uint32_t successful_responses;
} modbus_slave_stats_t;

#if defined(BOARD_GATEWAY)
/* modbus_master.h */
typedef struct
{
    uint32_t transactions_ok;
    uint32_t timeouts;
} modbus_master_stats_t;

/* mqtt_client.h */
typedef struct
{
    uint32_t publishes_sent;
    uint32_t publishes_failed;
    uint32_t reconnect_count;
    int32_t rssi_dbm;
} mqtt_stats_t;
#endif /* BOARD_GATEWAY */

/* ======================================================================= */
/* Constants                                                               */
/* ======================================================================= */

/** Number of monitored RTOS tasks — matches task-breakdown.md §7. */
#define HEALTH_TASK_COUNT 7U

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
/* Error codes                                                             */
/* ======================================================================= */

typedef enum
{
    HEALTH_MONITOR_ERR_OK = 0,
    HEALTH_MONITOR_ERR_NOT_INIT = 1,
    HEALTH_MONITOR_ERR_NULL_ARG = 2,
} health_monitor_err_t;

typedef enum
{
    HEALTH_ADMIN_ERR_OK = 0,
    HEALTH_ADMIN_ERR_NOT_INIT = 1,
} health_admin_err_t;

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
/* IHealthReport — write side                                              */
/* ======================================================================= */

/**
 * @brief Initialise HealthMonitor.
 *
 * Creates the internal mutex and zeroes the snapshot. Must be called once
 * before any producer calls IHealthReport.
 *
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t health_monitor_init(void);

/**
 * @brief Push a named event into the snapshot.
 *
 * Thread-safe. Acquires mutex, updates the corresponding counter or flag,
 * releases mutex, then drives update_led_state() if the event affects
 * LED indication. Safe from any task context; not ISR-safe.
 *
 * @param  event  Event identifier.
 * @param  param  Event-specific parameter (sensor_id, fault code, etc.).
 *                Pass 0 if not applicable.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t health_monitor_push_event(health_event_t event, uint32_t param);

/**
 * @brief Update Modbus slave statistics in the snapshot (FD).
 *
 * Called by ModbusRegisterMap each cycle. Acquires mutex; copies stats
 * fields atomically.
 *
 * @param  stats  Pointer to the latest slave statistics. Must not be NULL.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t health_monitor_update_modbus_slave_stats(const modbus_slave_stats_t *stats);

#if defined(BOARD_GATEWAY)
/**
 * @brief Update Modbus master statistics in the snapshot (GW).
 *
 * @param  stats  Pointer to the latest master statistics. Must not be NULL.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t health_monitor_update_modbus_master_stats(const modbus_master_stats_t *stats);

/**
 * @brief Update MQTT statistics in the snapshot (GW).
 *
 * @param  stats  Pointer to the latest MQTT statistics. Must not be NULL.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t health_monitor_update_mqtt_stats(const mqtt_stats_t *stats);

/**
 * @brief Update store-and-forward buffer occupancy (GW).
 *
 * @param  entry_count  Current number of buffered entries.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t health_monitor_update_buffer_occupancy(uint32_t entry_count);
#endif /* BOARD_GATEWAY */

/**
 * @brief Update RTOS task stack watermarks in the snapshot.
 *
 * Called by health_monitor_poll() on each health-report interval.
 *
 * @param  watermarks  Array of HEALTH_TASK_COUNT minimum free stack words.
 *                     Must not be NULL.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t
health_monitor_update_stack_watermarks(const uint16_t watermarks[HEALTH_TASK_COUNT]);

/* ======================================================================= */
/* IHealthSnapshot — read side                                             */
/* ======================================================================= */

/**
 * @brief Get a copy of the current health snapshot.
 *
 * Thread-safe. Acquires mutex; copies the entire snapshot; releases mutex.
 * The caller owns the returned copy — subsequent push_event() calls do not
 * affect it.
 *
 * @param[out] snap_out  Destination to fill. Must not be NULL.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero on failure.
 */
health_monitor_err_t health_monitor_get_snapshot(device_health_snapshot_t *snap_out);

/**
 * @brief Override all LEDs to the Faulted pattern immediately.
 *
 * Called by LifecycleController on entering the Faulted state. Bypasses
 * the normal update_led_state() priority logic.
 */
void health_monitor_set_led_fault(void);

/* ======================================================================= */
/* IHealthAdmin — control side                                             */
/* ======================================================================= */

/**
 * @brief Reset all counter-type metrics to zero (LLD-D15).
 *
 * Zeroes cumulative counters (Modbus, sensor fail, alarm raise, MQTT).
 * Preserves event-flags (sync state, persistence-fail), lifecycle state,
 * and uptime.
 *
 * Thread-safe — acquires the internal mutex.
 *
 * @return HEALTH_ADMIN_ERR_OK on success; non-zero on failure.
 */
health_admin_err_t health_monitor_reset_metrics(void);

/* ======================================================================= */
/* Polling and task registration                                           */
/* ======================================================================= */

/**
 * @brief Poll stack watermarks and uptime for all registered tasks.
 *
 * Called by the component that owns the health-report interval
 * (ModbusRegisterMap or LifecycleTask on FD; CloudPublisher on GW).
 * Not ISR-safe.
 */
void health_monitor_poll(void);

/**
 * @brief Register a task handle for stack-watermark polling.
 *
 * Must be called once per task, from the task itself or from main(),
 * before health_monitor_poll() is first called.
 *
 * @param  name         Short task name (up to 15 chars + NUL).
 * @param  task_handle  FreeRTOS TaskHandle_t (passed as void * to avoid
 *                      exposing task.h in this header).
 * @return HEALTH_MONITOR_ERR_OK on success; HEALTH_MONITOR_ERR_NULL_ARG if
 *         either pointer is NULL; HEALTH_MONITOR_ERR_NOT_INIT if called
 *         before health_monitor_init().
 */
health_monitor_err_t health_monitor_register_task(const char *name, void *task_handle);

/* ======================================================================= */
/* Vtable interfaces (DIP)                                                 */
/* ======================================================================= */

/** IHealthReport — write side. */
typedef struct ihealth_report_s
{
    health_monitor_err_t (*init)(void);
    health_monitor_err_t (*push_event)(health_event_t event, uint32_t param);
    health_monitor_err_t (*update_modbus_slave_stats)(const modbus_slave_stats_t *stats);
#if defined(BOARD_GATEWAY)
    health_monitor_err_t (*update_modbus_master_stats)(const modbus_master_stats_t *stats);
    health_monitor_err_t (*update_mqtt_stats)(const mqtt_stats_t *stats);
    health_monitor_err_t (*update_buffer_occupancy)(uint32_t entry_count);
#endif
    health_monitor_err_t (*update_stack_watermarks)(const uint16_t watermarks[HEALTH_TASK_COUNT]);
    void (*set_led_fault)(void);
} ihealth_report_t;

/** Singleton pointer — write side. */
extern const ihealth_report_t *const health_report;

/** IHealthSnapshot — read side. */
typedef struct ihealth_snapshot_s
{
    health_monitor_err_t (*get_snapshot)(device_health_snapshot_t *snap_out);
} ihealth_snapshot_t;

/** Singleton pointer — read side. */
extern const ihealth_snapshot_t *const health_snapshot;

/** IHealthAdmin — control side. */
typedef struct ihealth_admin_s
{
    health_admin_err_t (*reset_metrics)(void);
} ihealth_admin_t;

/** Singleton pointer — control side. Consumed only by LifecycleController. */
extern const ihealth_admin_t *const health_admin;

/* ======================================================================= */
/* Test-only exposure                                                      */
/* ======================================================================= */

#ifdef HEALTH_MONITOR_TEST_VISIBLE
#undef HEALTH_MONITOR_TEST_VISIBLE
#endif

#ifdef TEST
#define HEALTH_MONITOR_TEST_VISIBLE
#else
#define HEALTH_MONITOR_TEST_VISIBLE static
#endif

#ifdef TEST
/**
 * @brief Reset module state between unit tests.
 *
 * Clears s_hm to its post-BSS value. Must be called from setUp().
 */
void health_monitor_reset_for_test(void);
#endif /* TEST */

/* In UNIT_TEST builds, led_driver_set() is macro-replaced with a spy stub
 * so the test TU can inspect which LED calls were made. */
#ifdef UNIT_TEST
/* Forward declaration — definition provided by the test TU. */
void stub_led_set(uint32_t id, uint32_t state);
#define led_driver_set(id, state) stub_led_set((uint32_t) (id), (uint32_t) (state))
#endif /* UNIT_TEST */

#endif /* HEALTH_MONITOR_H */
