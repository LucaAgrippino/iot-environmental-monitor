/**
 * @file health_monitor.h
 * @brief HealthMonitor — consolidated device-health snapshot and status LED (Gateway).
 *
 * A passive aggregator with no thread of its own. Producers push events and
 * counters through the IHealthReport functions, consumers copy the snapshot
 * through IHealthSnapshot, and LifecycleController uses the IHealthAdmin
 * functions. One priority-inheritance mutex serialises every caller.
 *
 * HealthMonitor also drives the Gateway's single user LED (LD2) through
 * LedDriver: the blink pattern encodes device state (companion §6, HM-D9).
 *
 * This is the Gateway build (B-L475E-IOT01A); the Field Device has its own
 * HealthMonitor with a different API (docs/lld/application/health-monitor-lld.md,
 * HM-D3). The two share nothing but the basename.
 *
 * @note See docs/lld/application/health-monitor-lld.md for the full design.
 */

#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "modbus_master/modbus_master.h"
#include "mqtt_client.h"

/* ====================================================================== */
/* Placeholder dependency types                                           */
/*                                                                        */
/* The Gateway has no LifecycleController, SensorService or AlarmService   */
/* yet, so the types the snapshot needs stand in here. Each   */
/* block uses the Field Device header's include guard and its enumerator   */
/* names and values, so the real Gateway header supersedes it without a    */
/* duplicate-definition error once it exists.                              */
/* ====================================================================== */

#ifndef LIFECYCLE_STATE_DEFINED
#define LIFECYCLE_STATE_DEFINED
typedef enum
{
    LIFECYCLE_STATE_INIT = 0,
    LIFECYCLE_STATE_OPERATIONAL = 1,
    LIFECYCLE_STATE_EDITING_CONFIG = 2,
    LIFECYCLE_STATE_RESTARTING = 3,
    LIFECYCLE_STATE_UPDATING_FW = 4,
    LIFECYCLE_STATE_FAULTED = 5,
} lifecycle_state_t;
#endif /* LIFECYCLE_STATE_DEFINED */

#ifndef LIFECYCLE_RESET_CAUSE_DEFINED
#define LIFECYCLE_RESET_CAUSE_DEFINED
typedef enum
{
    LIFECYCLE_RESET_POWER_ON = 0,
    LIFECYCLE_RESET_SOFT = 1,
    LIFECYCLE_RESET_WATCHDOG = 2,
    LIFECYCLE_RESET_UNKNOWN = 3,
} lifecycle_reset_cause_t;
#endif /* LIFECYCLE_RESET_CAUSE_DEFINED */

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

#ifndef TIME_SYNC_STATE_DEFINED
#define TIME_SYNC_STATE_DEFINED
typedef enum
{
    TIME_SYNC_UNSYNCHRONISED = 0,
    TIME_SYNC_SYNCHRONISED = 1,
} time_sync_state_t;
#endif /* TIME_SYNC_STATE_DEFINED */

/* ====================================================================== */
/* Handle, limits, errors                                                 */
/* ====================================================================== */

#ifndef HEALTH_MONITOR_HANDLE_T_DEFINED
#define HEALTH_MONITOR_HANDLE_T_DEFINED
/** @brief Opaque handle to a HealthMonitor instance. */
typedef struct health_monitor_inst *health_monitor_handle_t;
#endif /* HEALTH_MONITOR_HANDLE_T_DEFINED */

/**
 * @brief Maximum number of tasks tracked for stack watermarks.
 *
 * Matches the 8 GW application tasks in task-breakdown.md §5.2 (HM-D10).
 */
#define HEALTH_MONITOR_MAX_TASKS (8U)

/** @brief Error codes returned by every HealthMonitor function (HM-D2). */
typedef enum
{
    HEALTH_MONITOR_ERR_OK = 0,          /**< Success.                              */
    HEALTH_MONITOR_ERR_NULL_PTR = 1,    /**< A required pointer is NULL.           */
    HEALTH_MONITOR_ERR_NO_RESOURCE = 2, /**< Pool or task table exhausted.         */
    HEALTH_MONITOR_ERR_NOT_INIT = 3,    /**< Handle does not refer to a created
                                             instance.                             */
} health_monitor_err_t;

/* ====================================================================== */
/* Events                                                                 */
/* ====================================================================== */

/**
 * @brief Named health events pushed by producers.
 *
 * Each event maps to a snapshot update (see the dispatch table in the
 * companion §4). Events are fire-and-forget: push_event is best-effort (HM-D7).
 */
typedef enum
{
    HEALTH_EVENT_TIME_SYNC_ACQUIRED = 0,  /**< TimeProvider: sync OK.                  */
    HEALTH_EVENT_TIME_SYNC_LOST = 1,      /**< TimeProvider: sync lost.                */
    HEALTH_EVENT_CONFIG_WRITE_FAIL = 2,   /**< ConfigStore: write failed.              */
    HEALTH_EVENT_SENSOR_FAIL = 3,         /**< SensorService: param = sensor_id.       */
    HEALTH_EVENT_SENSOR_RECOVERED = 4,    /**< SensorService: param = sensor_id.       */
    HEALTH_EVENT_NTP_SYNC_FAILED = 5,     /**< NtpClient: attempt failed.              */
    HEALTH_EVENT_NTP_SYNC_OK = 6,         /**< NtpClient: param = epoch seconds.       */
    HEALTH_EVENT_BUFFER_OVERFLOW = 7,     /**< CFL/SAF: oldest entry dropped.          */
    HEALTH_EVENT_MODBUS_LINK_UP = 8,      /**< ModbusPoller: Field Device link up.     */
    HEALTH_EVENT_MODBUS_LINK_DOWN = 9,    /**< ModbusPoller: Field Device link down.   */
    HEALTH_EVENT_CLOUD_CONNECTED = 10,    /**< CloudPublisher: broker connected.       */
    HEALTH_EVENT_CLOUD_DISCONNECTED = 11, /**< CloudPublisher: broker disconnected.   */
    HEALTH_EVENT_ALARM_RAISED = 12,       /**< AlarmService: param = sensor_id.        */
    HEALTH_EVENT_ALARM_CLEARED = 13,      /**< AlarmService: param = sensor_id.        */
    HEALTH_EVENT_FAULT = 14,              /**< Any: param = fault code. Logged only;
                                               the LED is driven by set_led_fault().   */
    HEALTH_EVENT_NTP_BAD_RESPONSE = 15,   /**< NtpClient: a server's reply failed the sanity
                                               checks.  Logged only; no snapshot field.   */
    HEALTH_EVENT_COUNT                    /**< Sentinel — not a valid event.           */
} health_event_t;

/* ====================================================================== */
/* Snapshot and configuration                                             */
/* ====================================================================== */

/**
 * @brief Consolidated health snapshot.
 *
 * Gateway only, so no board conditionals (HM-D3). uptime_s is computed fresh
 * in health_monitor_get_snapshot() (HM-D5).
 */
typedef struct
{
    /* ── System ── */
    uint32_t uptime_s;                        /**< Seconds since boot.            */
    lifecycle_state_t lifecycle_state;        /**< Current lifecycle state.       */
    lifecycle_reset_cause_t last_reset_cause; /**< Cause of the last reset.   */

    /* ── Sensors ── */
    bool sensor_valid[SENSOR_ID_COUNT]; /**< Per-sensor health flag.        */
    uint32_t sensor_fail_count;         /**< Cumulative since boot.         */
    time_sync_state_t time_sync_state;  /**< Wall-clock synchronisation.    */

    /* ── Alarms ── */
    alarm_state_t alarm_state[SENSOR_ID_COUNT]; /**< Per-sensor alarm state.  */
    uint32_t alarm_raise_count;                 /**< Cumulative raises.             */

    /* ── Config ── */
    bool config_write_failed; /**< Sticky flag.                   */

    /* ── Modbus master ── */
    uint32_t modbus_transactions_ok;
    uint32_t modbus_timeouts;
    bool modbus_link_online;

    /* ── Cloud ── */
    uint32_t mqtt_publishes_sent;
    uint32_t mqtt_publishes_failed;
    uint32_t mqtt_reconnect_count;
    int32_t wifi_rssi_dbm;
    bool cloud_connected;

    /* ── Store-and-forward buffer ── */
    uint32_t buffer_entry_count;
    uint32_t buffer_overflow_count; /**< Cumulative drops.              */

    /* ── NTP ── */
    uint32_t ntp_sync_fail_count;
    uint32_t last_ntp_sync_epoch; /**< 0 if never synced.             */

    /* ── RTOS task stack watermarks ── */
    uint16_t stack_watermark_words[HEALTH_MONITOR_MAX_TASKS];
} health_snapshot_t;

/** @brief HealthMonitor creation configuration (boot_reset_cause: HM-D4). */
typedef struct
{
    /* cppcheck-suppress unusedStructMember */
    lifecycle_reset_cause_t boot_reset_cause; /**< Read from RCC flags by LifecycleController. */
} health_monitor_config_t;

/* ====================================================================== */
/* Lifecycle functions                                                    */
/* ====================================================================== */

/**
 * @brief Create and initialise a HealthMonitor instance.
 *
 * Zeroes the snapshot, marks every sensor valid, records the boot reset cause,
 * sets the lifecycle state to INIT and puts the LED in its init pattern.
 * LedDriver must already be initialised; if it is not, the LED call is logged
 * and creation still succeeds.
 *
 * @param[in]  config  Boot parameters.
 * @param[out] handle  Receives the created handle on success.
 * @return HEALTH_MONITOR_ERR_OK on success; HEALTH_MONITOR_ERR_NULL_PTR if
 *         config or handle is NULL; HEALTH_MONITOR_ERR_NO_RESOURCE if the pool
 *         is exhausted or the mutex cannot be created.
 * @note Threading: call once during boot before any producer starts.
 */
health_monitor_err_t health_monitor_create(const health_monitor_config_t *config,
                                           health_monitor_handle_t *handle);

/**
 * @brief Register a FreeRTOS task for stack watermark polling.
 *
 * @param[in] handle       HealthMonitor handle.
 * @param[in] name         Null-terminated task name; longer names are truncated
 *                         to 15 characters.
 * @param[in] task_handle  FreeRTOS task handle.
 * @return HEALTH_MONITOR_ERR_OK; HEALTH_MONITOR_ERR_NULL_PTR if handle or name
 *         is NULL; HEALTH_MONITOR_ERR_NOT_INIT; HEALTH_MONITOR_ERR_NO_RESOURCE
 *         once HEALTH_MONITOR_MAX_TASKS tasks are registered.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_register_task(health_monitor_handle_t handle, const char *name,
                                                  TaskHandle_t task_handle);

/**
 * @brief Poll the stack watermarks of every registered task into the snapshot.
 *
 * Called by CloudPublisher on each health publish tick, before get_snapshot().
 *
 * @param[in] handle  HealthMonitor handle.
 * @return HEALTH_MONITOR_ERR_OK; HEALTH_MONITOR_ERR_NULL_PTR;
 *         HEALTH_MONITOR_ERR_NOT_INIT.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_poll(health_monitor_handle_t handle);

/* ====================================================================== */
/* IHealthReport — write side (producers)                                 */
/* ====================================================================== */

/**
 * @brief Push a named event into the snapshot.
 *
 * Best-effort (HM-D7): an out-of-range event or sensor_id param is logged at
 * WARN, changes nothing, and still returns HEALTH_MONITOR_ERR_OK.
 *
 * @param[in] handle  HealthMonitor handle.
 * @param[in] event   Event identifier.
 * @param[in] param   Event-specific parameter; 0 if not applicable.
 * @return HEALTH_MONITOR_ERR_OK; HEALTH_MONITOR_ERR_NULL_PTR if handle is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_push_event(health_monitor_handle_t handle, health_event_t event,
                                               uint32_t param);

/**
 * @brief Copy the Modbus master counters into the snapshot.
 *
 * Copies transactions_ok and timeouts only.  The link state is event-driven:
 * it changes through HEALTH_EVENT_MODBUS_LINK_UP / _DOWN, not through these stats.
 *
 * @param[in] handle  HealthMonitor handle.
 * @param[in] stats   Counter snapshot from ModbusMaster.
 * @return HEALTH_MONITOR_ERR_OK; HEALTH_MONITOR_ERR_NULL_PTR if handle or stats
 *         is NULL; HEALTH_MONITOR_ERR_NOT_INIT.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_modbus_master_stats(health_monitor_handle_t handle,
                                                               const modbus_master_stats_t *stats);

/**
 * @brief Copy the MQTT counters into the snapshot.
 *
 * Takes publishes_sent, publish_failures and reconnect_count from the stats; the
 * companion calls the second one publishes_failed.
 *
 * @param[in] handle  HealthMonitor handle.
 * @param[in] stats   Counter snapshot from MqttClient.
 * @return HEALTH_MONITOR_ERR_OK; HEALTH_MONITOR_ERR_NULL_PTR if handle or stats
 *         is NULL; HEALTH_MONITOR_ERR_NOT_INIT.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_mqtt_stats(health_monitor_handle_t handle,
                                                      const mqtt_stats_t *stats);

/**
 * @brief Set the store-and-forward buffer occupancy.
 *
 * @param[in] handle       HealthMonitor handle.
 * @param[in] entry_count  Current number of buffered entries.
 * @return HEALTH_MONITOR_ERR_OK; HEALTH_MONITOR_ERR_NULL_PTR if handle is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_buffer_occupancy(health_monitor_handle_t handle,
                                                            uint32_t entry_count);

/**
 * @brief Store stack watermarks that the caller has already collected.
 *
 * Copies one value per registered task; also used by health_monitor_poll().
 *
 * @param[in] handle      HealthMonitor handle.
 * @param[in] watermarks  One watermark (words) per registered task.
 * @return HEALTH_MONITOR_ERR_OK; HEALTH_MONITOR_ERR_NULL_PTR if handle or
 *         watermarks is NULL; HEALTH_MONITOR_ERR_NOT_INIT.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_stack_watermarks(health_monitor_handle_t handle,
                                                            const uint16_t *watermarks);

/**
 * @brief Record a lifecycle state change and refresh the LED.
 *
 * @param[in] handle  HealthMonitor handle.
 * @param[in] state   New lifecycle state.
 * @return HEALTH_MONITOR_ERR_OK; HEALTH_MONITOR_ERR_NULL_PTR if handle is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_lifecycle(health_monitor_handle_t handle,
                                                     lifecycle_state_t state);

/**
 * @brief Record the latest WiFi RSSI.
 *
 * @param[in] handle    HealthMonitor handle.
 * @param[in] rssi_dbm  RSSI in dBm (typically -30 to -90).
 * @return HEALTH_MONITOR_ERR_OK; HEALTH_MONITOR_ERR_NULL_PTR if handle is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_wifi_rssi(health_monitor_handle_t handle,
                                                     int32_t rssi_dbm);

/* ====================================================================== */
/* IHealthSnapshot — read side (consumers)                                */
/* ====================================================================== */

/**
 * @brief Copy the current health snapshot.
 *
 * Computes uptime_s from the tick count under the lock (HM-D5) and returns a
 * value copy that the caller owns.
 *
 * @param[in]  handle    HealthMonitor handle.
 * @param[out] snap_out  Receives the snapshot.
 * @return HEALTH_MONITOR_ERR_OK; HEALTH_MONITOR_ERR_NULL_PTR if handle or
 *         snap_out is NULL; HEALTH_MONITOR_ERR_NOT_INIT.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_get_snapshot(health_monitor_handle_t handle,
                                                 health_snapshot_t *snap_out);

/* ====================================================================== */
/* IHealthAdmin — control side (LifecycleController only)                 */
/* ====================================================================== */

/**
 * @brief Latch the fault LED pattern until reboot (HM-D6).
 *
 * @param[in] handle  HealthMonitor handle.
 * @return HEALTH_MONITOR_ERR_OK; HEALTH_MONITOR_ERR_NULL_PTR if handle is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_set_led_fault(health_monitor_handle_t handle);

/**
 * @brief Zero the counter-type metrics (remote CMD_RESET_METRICS).
 *
 * Zeroes sensor_fail_count, alarm_raise_count, modbus_transactions_ok,
 * modbus_timeouts, mqtt_publishes_sent, mqtt_publishes_failed,
 * mqtt_reconnect_count, ntp_sync_fail_count and buffer_overflow_count. Every
 * other field is preserved.
 *
 * @param[in] handle  HealthMonitor handle.
 * @return HEALTH_MONITOR_ERR_OK; HEALTH_MONITOR_ERR_NULL_PTR if handle is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_reset_metrics(health_monitor_handle_t handle);

#ifdef TEST
/** @brief Test hook — forget the instance pool so each test starts clean. */
void health_monitor_reset_for_test(void);
#endif

#endif /* HEALTH_MONITOR_H */
