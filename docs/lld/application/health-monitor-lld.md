# LLD Companion — HealthMonitor

**Document:** `docs/lld/application/health-monitor-lld.md`
**Version:** 0.3 (Gateway implemented and hardware-validated)
**Board:** Gateway (B-L475E-IOT01A) only
**Layer:** Application
**Status:** Implementation-ready — GW implemented and hardware-validated (2026-10-11)
**Date:** October 2026

**HLD anchor:** HealthMonitor in `components.md` (GW application layer)

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Passive health aggregator. Maintains a consolidated `health_snapshot_t` in RAM, updated continuously by producers via IHealthReport, served to consumers via IHealthSnapshot. Drives the board status LED to indicate device state at a glance. | `components.md` |
| PROVIDES (upward) | IHealthSnapshot *(read side)*, IHealthReport *(write side)*, IHealthAdmin *(control — LifecycleController only)* | `components.md` |
| USES (downward) | ILed (LedDriver), ILogger | `components.md` |
| Root requirements | REQ-CC-010, REQ-CC-070, REQ-CC-090, REQ-NF-208 | `SRS.md` |
| HLD refs | `hld.md` §5.5, §6.4; `components.md` §ISP, §DIP, §Metric Producer Pattern; `sequence-diagrams.md` SD-03b; `task-breakdown.md` §5.2, §7 | HLD |

**Responsibility boundary.** HealthMonitor owns:
- Consolidated health snapshot in RAM — one struct, protected by mutex.
- Event dispatch — mapping each `health_event_t` to the corresponding snapshot field update.
- Stats aggregation — copying polled counter-structs from producers into the snapshot.
- LED status indication — translating device state into LED patterns via ILed.
- Stack watermark polling — collecting `uxTaskGetStackHighWaterMark()` for all registered tasks.
- Metrics reset — zeroing counter-type metrics on remote command (REQ-LD-070).

HealthMonitor does **not** own:
- Metric production — each producer decides when and what to report.
- LED blinking timing — LedDriver's FreeRTOS software timers handle blink patterns.
- Health telemetry serialisation — CloudPublisher owns JSON assembly (SD-03b).

**Threading model.** HealthMonitor has no thread. Every operation executes in
the calling task's context — producers push from their own tasks, consumers
pull from their own tasks. A priority-inheritance mutex serialises access.

**DIP role.** `IHealthReport` is Application-layer owned (here) but consumed
by Middleware producers (ConfigStore, TimeProvider, NtpClient,
CircularFlashLog). Middleware depends on the abstraction (opaque handle +
`health_monitor.h` API), not on `HealthMonitor`'s implementation — this
preserves directional layering while allowing bottom-up metric flow (P2).

---

## 2. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| Aggregate device health metrics | REQ-CC-010 | UC-06 |
| Include health in JSON payloads | REQ-CC-070, REQ-CC-090 | UC-06 |
| LED status indication | REQ-NF-208 | — |
| Remote metrics reset | REQ-LD-070 | — |

---

## 3. Public API

### 3.1 ADT pattern

HealthMonitor follows the Gateway ADT default: an opaque handle returned
by `health_monitor_create()` from a static pool of 1. LedDriver is a
singleton accessed directly via `led_set()` — no handle injection needed.
Logger is accessed directly (P4 cross-cutting exception). Decision HM-D1.

### 3.2 Data types

```c
/* health_monitor.h */

#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <stdint.h>
#include <stdbool.h>
#include "led_driver.h"             /* led_set(), led_pattern_t */
#include "lifecycle_controller.h"   /* lifecycle_state_t,
                                       lifecycle_reset_cause_t */
#include "sensor_types.h"           /* SENSOR_ID_COUNT,
                                       alarm_state_t          */
#include "time_provider.h"          /* time_sync_state_t     */
#include "modbus_master.h"          /* modbus_master_stats_t */
#include "mqtt_client.h"            /* mqtt_stats_t          */
#include "FreeRTOS.h"
#include "task.h"

/** @brief Opaque handle to a HealthMonitor instance. */
typedef struct health_monitor_inst *health_monitor_handle_t;

/** @brief Maximum number of tasks tracked for stack watermarks.
 *  Matches the 8 GW application tasks in task-breakdown.md §5.2. */
#define HEALTH_MONITOR_MAX_TASKS    8U

/** @brief Error codes returned by all HealthMonitor public functions. */
typedef enum {
    HEALTH_MONITOR_ERR_OK          = 0,  /**< Success.                     */
    HEALTH_MONITOR_ERR_NULL_PTR    = 1,  /**< Required pointer is NULL.    */
    HEALTH_MONITOR_ERR_NO_RESOURCE = 2,  /**< Static pool or task table
                                              exhausted.                   */
    HEALTH_MONITOR_ERR_NOT_INIT    = 3,  /**< Called before create().      */
} health_monitor_err_t;
```

Decision HM-D2: single error enum replaces the v0.1 split
(`health_monitor_err_t` + `health_admin_err_t`). All public functions
return `health_monitor_err_t`.

```c
/**
 * @brief Named health events pushed by producers.
 *
 * Each event maps to a specific snapshot field update (see §4 dispatch
 * table). Events are fire-and-forget — push_event is best-effort (HM-D7).
 */
typedef enum {
    HEALTH_EVENT_TIME_SYNC_ACQUIRED   = 0,   /**< TimeProvider: sync OK.     */
    HEALTH_EVENT_TIME_SYNC_LOST       = 1,   /**< TimeProvider: sync lost.   */
    HEALTH_EVENT_CONFIG_WRITE_FAIL    = 2,   /**< ConfigStore: write failed. */
    HEALTH_EVENT_SENSOR_FAIL          = 3,   /**< SensorService: sensor fail.
                                                  param = sensor_id.         */
    HEALTH_EVENT_SENSOR_RECOVERED     = 4,   /**< SensorService: sensor OK.
                                                  param = sensor_id.         */
    HEALTH_EVENT_NTP_SYNC_FAILED      = 5,   /**< NtpClient: sync attempt
                                                  failed.                    */
    HEALTH_EVENT_NTP_SYNC_OK          = 6,   /**< NtpClient: sync succeeded.
                                                  param = epoch seconds.     */
    HEALTH_EVENT_BUFFER_OVERFLOW      = 7,   /**< CFL/SAF: oldest entry
                                                  dropped.                   */
    HEALTH_EVENT_MODBUS_LINK_UP       = 8,   /**< ModbusPoller: field device
                                                  link established.          */
    HEALTH_EVENT_MODBUS_LINK_DOWN     = 9,   /**< ModbusPoller: field device
                                                  link lost.                 */
    HEALTH_EVENT_CLOUD_CONNECTED      = 10,  /**< CloudPublisher: MQTT
                                                  broker connected.          */
    HEALTH_EVENT_CLOUD_DISCONNECTED   = 11,  /**< CloudPublisher: MQTT
                                                  broker disconnected.       */
    HEALTH_EVENT_ALARM_RAISED         = 12,  /**< AlarmService: alarm raised.
                                                  param = sensor_id.         */
    HEALTH_EVENT_ALARM_CLEARED        = 13,  /**< AlarmService: alarm cleared.
                                                  param = sensor_id.         */
    HEALTH_EVENT_FAULT                = 14,  /**< Any: fault condition.
                                                  param = fault code. Logged
                                                  only; LED driven by
                                                  set_led_fault().           */
    HEALTH_EVENT_NTP_BAD_RESPONSE     = 15,  /**< NtpClient: reply failed the sanity
                                                  checks. Logged only.       */
    HEALTH_EVENT_COUNT                       /**< Sentinel — not a valid
                                                  event.                     */
} health_event_t;
```

```c
/**
 * @brief Consolidated health snapshot.
 *
 * GW-only — no #if BOARD_GATEWAY guards (decision HM-D3). All fields
 * populated continuously; uptime_s computed fresh in get_snapshot()
 * (decision HM-D5).
 */
typedef struct {
    /* ── System ── */
    uint32_t                uptime_s;           /**< Seconds since boot.     */
    lifecycle_state_t       lifecycle_state;
    lifecycle_reset_cause_t last_reset_cause;

    /* ── Sensors ── */
    bool                    sensor_valid[SENSOR_ID_COUNT];
    uint32_t                sensor_fail_count;   /**< Cumulative since boot. */
    time_sync_state_t       time_sync_state;

    /* ── Alarms ── */
    alarm_state_t           alarm_state[SENSOR_ID_COUNT];
    uint32_t                alarm_raise_count;   /**< Cumulative raises.     */

    /* ── Config ── */
    bool                    config_write_failed; /**< Sticky flag.           */

    /* ── Modbus master ── */
    uint32_t                modbus_transactions_ok;
    uint32_t                modbus_timeouts;
    bool                    modbus_link_online;

    /* ── Cloud ── */
    uint32_t                mqtt_publishes_sent;
    uint32_t                mqtt_publishes_failed;
    uint32_t                mqtt_reconnect_count;
    int32_t                 wifi_rssi_dbm;
    bool                    cloud_connected;

    /* ── Store-and-forward buffer ── */
    uint32_t                buffer_entry_count;
    uint32_t                buffer_overflow_count; /**< Cumulative drops.    */

    /* ── NTP ── */
    uint32_t                ntp_sync_fail_count;
    uint32_t                last_ntp_sync_epoch;   /**< 0 if never synced.  */

    /* ── RTOS task stack watermarks ── */
    uint16_t                stack_watermark_words[HEALTH_MONITOR_MAX_TASKS];
} health_snapshot_t;
```

```c
/**
 * @brief HealthMonitor creation configuration.
 *
 * boot_reset_cause is immutable after create (decision HM-D4).
 */
typedef struct {
    lifecycle_reset_cause_t boot_reset_cause; /**< Determined by
                                                   LifecycleController at
                                                   boot from RCC flags.    */
} health_monitor_config_t;
```

**External types consumed (for reference):**

| Type | Source header | Expected fields |
|---|---|---|
| `modbus_master_stats_t` | `modbus_master.h` | `transactions_ok` (uint32_t), `timeouts` (uint32_t) — the real ModbusMaster counters; the other five are ignored. The link flag is event-driven (`MODBUS_LINK_UP` / `_DOWN`), no longer part of the stats |
| `mqtt_stats_t` | `mqtt_client.h` | `publishes_sent` (uint32_t), `publishes_failed` (uint32_t), `reconnect_count` (uint32_t) |
| `lifecycle_state_t` | `lifecycle_controller.h` | Enum: INIT, OPERATIONAL, FAULTED, etc. |
| `lifecycle_reset_cause_t` | `lifecycle_controller.h` | Enum: POWER_ON, WATCHDOG, SOFTWARE, etc. |
| `alarm_state_t` | `sensor_types.h` | Enum: ALARM_CLEAR, ALARM_ACTIVE, etc. |
| `time_sync_state_t` | `time_provider.h` | Enum: SYNCED, UNSYNCED, etc. |
| `SENSOR_ID_COUNT` | `sensor_types.h` | Number of sensor channels |

### 3.3 Lifecycle functions

```c
/**
 * @brief Create and initialise a HealthMonitor instance.
 *
 * Creates the mutex (priority-inheritance). Zeroes the snapshot.
 * Sets last_reset_cause from config. Sets initial lifecycle_state
 * to INIT. Calls update_led_state() to set the init LED pattern.
 *
 * @param[in]  config  Injected dependencies and boot parameters.
 * @param[out] handle  Receives the created handle on success.
 * @return HEALTH_MONITOR_ERR_OK on success;
 *         HEALTH_MONITOR_ERR_NULL_PTR if config or handle is NULL;
 *         HEALTH_MONITOR_ERR_NO_RESOURCE if pool exhausted.
 * @note Threading: call once during boot before any producer starts.
 */
health_monitor_err_t health_monitor_create(
    const health_monitor_config_t *config,
    health_monitor_handle_t       *handle);

/**
 * @brief Register a FreeRTOS task for stack watermark polling.
 *
 * Called by each task at creation time (before the scheduler starts or
 * early in the task body). Must be called once per task, up to
 * HEALTH_MONITOR_MAX_TASKS times.
 *
 * @param[in] handle       HealthMonitor handle.
 * @param[in] name         Null-terminated task name (≤ 15 chars).
 *                         Stored internally for diagnostics.
 * @param[in] task_handle  FreeRTOS task handle.
 * @return HEALTH_MONITOR_ERR_OK on success;
 *         HEALTH_MONITOR_ERR_NULL_PTR if handle or name is NULL;
 *         HEALTH_MONITOR_ERR_NO_RESOURCE if MAX_TASKS reached.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_register_task(
    health_monitor_handle_t handle,
    const char             *name,
    TaskHandle_t            task_handle);

/**
 * @brief Poll stack watermarks for all registered tasks.
 *
 * Calls uxTaskGetStackHighWaterMark() for each registered task and
 * updates the snapshot under mutex.
 *
 * Called by CloudPublisher on each health publish tick (600 s) before
 * get_snapshot().
 *
 * @param[in] handle  HealthMonitor handle.
 * @return HEALTH_MONITOR_ERR_OK on success;
 *         HEALTH_MONITOR_ERR_NULL_PTR if handle is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT if not created.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_poll(health_monitor_handle_t handle);
```

### 3.4 IHealthReport — write side (producers)

```c
/**
 * @brief Push a named event into the snapshot.
 *
 * Thread-safe. Acquires mutex, updates the field(s) mapped by the event
 * dispatch table (§4), releases mutex, then calls update_led_state()
 * for events that affect LED indication.
 *
 * Best-effort (decision HM-D7): out-of-range param values (e.g.,
 * sensor_id >= SENSOR_ID_COUNT) are logged at WARN and return ERR_OK.
 * Out-of-range event IDs are logged at WARN and return ERR_OK.
 *
 * @param[in] handle  HealthMonitor handle.
 * @param[in] event   Event identifier.
 * @param[in] param   Event-specific parameter. Pass 0 if not
 *                    applicable. See §4 for per-event meaning.
 * @return HEALTH_MONITOR_ERR_OK on success (including best-effort
 *         skips); HEALTH_MONITOR_ERR_NULL_PTR if handle is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT if not created.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_push_event(
    health_monitor_handle_t handle,
    health_event_t          event,
    uint32_t                param);

/**
 * @brief Update Modbus master stats in the snapshot.
 *
 * Called by ModbusPoller each health-report cycle after polling
 * IModbusMasterStats. Acquires mutex; copies stats fields atomically.
 *
 * @param[in] handle  HealthMonitor handle.
 * @param[in] stats   Modbus master counter snapshot.
 * @return HEALTH_MONITOR_ERR_OK on success;
 *         HEALTH_MONITOR_ERR_NULL_PTR if handle or stats is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT if not created.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_modbus_master_stats(
    health_monitor_handle_t      handle,
    const modbus_master_stats_t *stats);

/**
 * @brief Update MQTT stats in the snapshot.
 *
 * Called by CloudPublisher each health-report cycle after polling
 * IMqttStats.
 *
 * @param[in] handle  HealthMonitor handle.
 * @param[in] stats   MQTT client counter snapshot.
 * @return HEALTH_MONITOR_ERR_OK on success;
 *         HEALTH_MONITOR_ERR_NULL_PTR if handle or stats is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT if not created.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_mqtt_stats(
    health_monitor_handle_t handle,
    const mqtt_stats_t     *stats);

/**
 * @brief Update store-and-forward buffer occupancy.
 *
 * Called by StoreAndForward after each enqueue or consume.
 *
 * @param[in] handle       HealthMonitor handle.
 * @param[in] entry_count  Current number of entries in the buffer.
 * @return HEALTH_MONITOR_ERR_OK on success;
 *         HEALTH_MONITOR_ERR_NULL_PTR if handle is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT if not created.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_buffer_occupancy(
    health_monitor_handle_t handle,
    uint32_t                entry_count);

/**
 * @brief Update RTOS task stack watermarks.
 *
 * Called internally by health_monitor_poll(). Also available for
 * direct use if a caller has already collected watermarks.
 *
 * @param[in] handle      HealthMonitor handle.
 * @param[in] watermarks  Array of task_count watermark values (words).
 * @return HEALTH_MONITOR_ERR_OK on success;
 *         HEALTH_MONITOR_ERR_NULL_PTR if handle or watermarks is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT if not created.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_stack_watermarks(
    health_monitor_handle_t handle,
    const uint16_t         *watermarks);

/**
 * @brief Update lifecycle state in the snapshot.
 *
 * Called by LifecycleController on each state transition. Calls
 * update_led_state() after updating the snapshot.
 *
 * @param[in] handle  HealthMonitor handle.
 * @param[in] state   New lifecycle state.
 * @return HEALTH_MONITOR_ERR_OK on success;
 *         HEALTH_MONITOR_ERR_NULL_PTR if handle is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT if not created.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_lifecycle(
    health_monitor_handle_t handle,
    lifecycle_state_t       state);

/**
 * @brief Update WiFi RSSI in the snapshot.
 *
 * Called by CloudPublisher or WifiDriver when a new RSSI reading
 * is available.
 *
 * @param[in] handle    HealthMonitor handle.
 * @param[in] rssi_dbm  WiFi RSSI in dBm (typically −30 to −90).
 * @return HEALTH_MONITOR_ERR_OK on success;
 *         HEALTH_MONITOR_ERR_NULL_PTR if handle is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT if not created.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_wifi_rssi(
    health_monitor_handle_t handle,
    int32_t                 rssi_dbm);
```

### 3.5 IHealthSnapshot — read side (consumers)

```c
/**
 * @brief Get a copy of the current health snapshot.
 *
 * Thread-safe. Acquires mutex; computes uptime_s fresh from
 * xTaskGetTickCount() (decision HM-D5); copies the entire snapshot
 * under lock; releases mutex. Returns a value copy — caller owns it.
 *
 * Called by: CloudPublisher (600 s health publish — SD-03b),
 * ConsoleService (CLI query).
 *
 * @param[in]  handle    HealthMonitor handle.
 * @param[out] snap_out  Filled with the current snapshot.
 * @return HEALTH_MONITOR_ERR_OK on success;
 *         HEALTH_MONITOR_ERR_NULL_PTR if handle or snap_out is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT if not created.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_get_snapshot(
    health_monitor_handle_t handle,
    health_snapshot_t      *snap_out);
```

### 3.6 IHealthAdmin — control side (LifecycleController only)

```c
/**
 * @brief Set the LED fault pattern explicitly.
 *
 * Called by LifecycleController on entering Faulted state. Sets a
 * fault_latched flag that overrides all other LED states until reboot
 * (decision HM-D6).
 *
 * @param[in] handle  HealthMonitor handle.
 * @return HEALTH_MONITOR_ERR_OK on success;
 *         HEALTH_MONITOR_ERR_NULL_PTR if handle is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT if not created.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_set_led_fault(
    health_monitor_handle_t handle);

/**
 * @brief Reset all counter-type metrics to zero.
 *
 * Dispatched by LifecycleController when CMD_RESET_METRICS arrives via
 * Modbus (ModbusRegisterMap → lifecycle_controller->handle_remote_command).
 *
 * Zeroes: sensor_fail_count, alarm_raise_count, modbus_transactions_ok,
 * modbus_timeouts, mqtt_publishes_sent, mqtt_publishes_failed,
 * mqtt_reconnect_count, ntp_sync_fail_count, buffer_overflow_count.
 *
 * Preserves: uptime_s, lifecycle_state, last_reset_cause, sensor_valid[],
 * time_sync_state, alarm_state[], config_write_failed, modbus_link_online,
 * wifi_rssi_dbm, cloud_connected, buffer_entry_count, last_ntp_sync_epoch,
 * stack_watermark_words[].
 *
 * @param[in] handle  HealthMonitor handle.
 * @return HEALTH_MONITOR_ERR_OK on success;
 *         HEALTH_MONITOR_ERR_NULL_PTR if handle is NULL;
 *         HEALTH_MONITOR_ERR_NOT_INIT if not created.
 * @note Threading: task-context only. Not ISR-safe.
 */
health_monitor_err_t health_monitor_reset_metrics(
    health_monitor_handle_t handle);

#endif /* HEALTH_MONITOR_H */
```

---

## 4. Health event dispatch

Each `health_event_t` maps to a specific snapshot update. The `param`
column shows how the second argument to `push_event()` is interpreted.

| Event | param meaning | Snapshot update | LED update |
|---|---|---|---|
| `TIME_SYNC_ACQUIRED` | unused (0) | `time_sync_state = SYNCED` | No |
| `TIME_SYNC_LOST` | unused (0) | `time_sync_state = UNSYNCED` | No |
| `CONFIG_WRITE_FAIL` | unused (0) | `config_write_failed = true` | No |
| `SENSOR_FAIL` | sensor_id | `sensor_valid[param] = false; sensor_fail_count++` | No |
| `SENSOR_RECOVERED` | sensor_id | `sensor_valid[param] = true` | No |
| `NTP_SYNC_FAILED` | unused (0) | `ntp_sync_fail_count++` | No |
| `NTP_SYNC_OK` | epoch seconds | `last_ntp_sync_epoch = param` | No |
| `BUFFER_OVERFLOW` | unused (0) | `buffer_overflow_count++` | No |
| `MODBUS_LINK_UP` | unused (0) | `modbus_link_online = true` | No |
| `MODBUS_LINK_DOWN` | unused (0) | `modbus_link_online = false` | No |
| `CLOUD_CONNECTED` | unused (0) | `cloud_connected = true` | No |
| `CLOUD_DISCONNECTED` | unused (0) | `cloud_connected = false` | No |
| `ALARM_RAISED` | sensor_id | `alarm_state[param] = ALARM_ACTIVE; alarm_raise_count++` | Yes |
| `ALARM_CLEARED` | sensor_id | `alarm_state[param] = ALARM_CLEAR` | Yes |
| `NTP_BAD_RESPONSE` | unused (0) | No snapshot field change; NtpClient logs the detail (added with NtpClient) | No |
| `FAULT` | fault code | Logged at ERROR; no snapshot field change. LED driven by `set_led_fault()`, not by this event. | No |

**Param validation.** Events that use `param` as `sensor_id`
(`SENSOR_FAIL`, `SENSOR_RECOVERED`, `ALARM_RAISED`, `ALARM_CLEARED`)
validate `param < SENSOR_ID_COUNT`. An out-of-range value is logged at
WARN and the snapshot is not modified. The function returns ERR_OK
(best-effort — decision HM-D7).

**LED updates.** `update_led_state()` is called after `ALARM_RAISED` and
`ALARM_CLEARED` because these events may change the alarm-active status
that drives LED indication. Other events do not affect LED state.

---

## 5. Internal design

### 5.1 Private struct and static pool

```c
/* health_monitor.c */

#define HM_MAX_INSTANCES    1U
#define HM_TASK_NAME_LEN    16U

struct health_monitor_inst {
    /* Health state */
    health_snapshot_t   snapshot;

    /* Mutex — priority-inheritance */
    SemaphoreHandle_t   mutex;
    StaticSemaphore_t   mutex_buf;

    /* Registered tasks for watermark polling */
    TaskHandle_t        task_handles[HEALTH_MONITOR_MAX_TASKS];
    char                task_names[HEALTH_MONITOR_MAX_TASKS][HM_TASK_NAME_LEN];
    uint8_t             task_count;

    /* LED fault latch — set by set_led_fault(), never cleared (HM-D6) */
    bool                fault_latched;

    bool                in_use;
};

static struct health_monitor_inst g_pool[HM_MAX_INSTANCES];
static uint8_t                    g_count;
```

All state is static, allocated in `.bss`. No dynamic allocation (P5).
`StaticSemaphore_t mutex_buf` — FreeRTOS static allocation for the
priority-inheritance mutex.

### 5.2 Algorithms

#### 5.2.1 `health_monitor_create`

```
health_monitor_create(config, handle):
    if config == NULL or handle == NULL:
        return ERR_NULL_PTR
    if g_count >= HM_MAX_INSTANCES:
        return ERR_NO_RESOURCE

    self = &g_pool[g_count++]
    memset(&self->snapshot, 0, sizeof(health_snapshot_t))
    self->snapshot.last_reset_cause = config->boot_reset_cause
    self->snapshot.lifecycle_state  = LIFECYCLE_STATE_INIT
    self->task_count     = 0
    self->fault_latched  = false
    self->in_use         = true

    /* Initialise all sensor_valid to true (healthy until proven otherwise) */
    for i = 0 to SENSOR_ID_COUNT - 1:
        self->sensor_valid[i] = true

    self->mutex = xSemaphoreCreateMutexStatic(&self->mutex_buf)
    /* Static creation cannot fail */

    *handle = self
    update_led_state(self)
    log_info("HM: created, reset_cause=%u", config->boot_reset_cause)
    return ERR_OK
```

#### 5.2.2 `health_monitor_register_task`

```
health_monitor_register_task(handle, name, task_handle):
    if handle == NULL or name == NULL:
        return ERR_NULL_PTR
    if handle->task_count >= HEALTH_MONITOR_MAX_TASKS:
        return ERR_NO_RESOURCE

    idx = handle->task_count
    handle->task_handles[idx] = task_handle
    strncpy(handle->task_names[idx], name, HM_TASK_NAME_LEN - 1)
    handle->task_names[idx][HM_TASK_NAME_LEN - 1] = '\0'
    handle->task_count++
    log_debug("HM: registered task[%u] = '%s'", idx, name)
    return ERR_OK
```

No mutex needed — called sequentially during boot before the scheduler
starts, or early in each task body (single writer per slot).

#### 5.2.3 `health_monitor_push_event`

```
health_monitor_push_event(handle, event, param):
    if handle == NULL:
        return ERR_NULL_PTR
    if not handle->in_use:
        return ERR_NOT_INIT

    if event >= HEALTH_EVENT_COUNT:
        log_warn("HM: unknown event %u", event)
        return ERR_OK       /* best-effort — HM-D7 */

    xSemaphoreTake(handle->mutex, portMAX_DELAY)

    led_update_needed = false
    switch (event):
        case TIME_SYNC_ACQUIRED:
            handle->snapshot.time_sync_state = TIME_SYNC_SYNCED
        case TIME_SYNC_LOST:
            handle->snapshot.time_sync_state = TIME_SYNC_UNSYNCED
        case CONFIG_WRITE_FAIL:
            handle->snapshot.config_write_failed = true
        case SENSOR_FAIL:
            if param < SENSOR_ID_COUNT:
                handle->snapshot.sensor_valid[param] = false
                handle->snapshot.sensor_fail_count++
            else:
                log_warn("HM: SENSOR_FAIL param %u out of range", param)
        case SENSOR_RECOVERED:
            if param < SENSOR_ID_COUNT:
                handle->snapshot.sensor_valid[param] = true
            else:
                log_warn("HM: SENSOR_RECOVERED param %u OOR", param)
        case NTP_SYNC_FAILED:
            handle->snapshot.ntp_sync_fail_count++
        case NTP_SYNC_OK:
            handle->snapshot.last_ntp_sync_epoch = param
        case BUFFER_OVERFLOW:
            handle->snapshot.buffer_overflow_count++
        case MODBUS_LINK_UP:
            handle->snapshot.modbus_link_online = true
        case MODBUS_LINK_DOWN:
            handle->snapshot.modbus_link_online = false
        case CLOUD_CONNECTED:
            handle->snapshot.cloud_connected = true
        case CLOUD_DISCONNECTED:
            handle->snapshot.cloud_connected = false
        case ALARM_RAISED:
            if param < SENSOR_ID_COUNT:
                handle->snapshot.alarm_state[param] = ALARM_STATE_ACTIVE
                handle->snapshot.alarm_raise_count++
                led_update_needed = true
            else:
                log_warn("HM: ALARM_RAISED param %u OOR", param)
        case ALARM_CLEARED:
            if param < SENSOR_ID_COUNT:
                handle->snapshot.alarm_state[param] = ALARM_STATE_CLEAR
                led_update_needed = true
            else:
                log_warn("HM: ALARM_CLEARED param %u OOR", param)
        case FAULT:
            log_error("HM: FAULT event, code=%u", param)

    xSemaphoreGive(handle->mutex)

    if led_update_needed:
        update_led_state(handle)

    return ERR_OK
```

#### 5.2.4 `health_monitor_poll`

```
health_monitor_poll(handle):
    if handle == NULL:
        return ERR_NULL_PTR
    if not handle->in_use:
        return ERR_NOT_INIT

    uint16_t wm[HEALTH_MONITOR_MAX_TASKS]
    for i = 0 to handle->task_count - 1:
        wm[i] = (uint16_t)uxTaskGetStackHighWaterMark(
                     handle->task_handles[i])

    xSemaphoreTake(handle->mutex, portMAX_DELAY)
    for i = 0 to handle->task_count - 1:
        handle->snapshot.stack_watermark_words[i] = wm[i]
    xSemaphoreGive(handle->mutex)

    return ERR_OK
```

Stack watermarks are collected outside the mutex (each
`uxTaskGetStackHighWaterMark` call reads another task's TCB — no shared
state with HealthMonitor). The snapshot copy is then written under mutex.

#### 5.2.5 `health_monitor_get_snapshot`

```
health_monitor_get_snapshot(handle, snap_out):
    if handle == NULL or snap_out == NULL:
        return ERR_NULL_PTR
    if not handle->in_use:
        return ERR_NOT_INIT

    xSemaphoreTake(handle->mutex, portMAX_DELAY)

    /* Compute uptime fresh — not stale from poll (HM-D5) */
    handle->snapshot.uptime_s =
        (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ)

    memcpy(snap_out, &handle->snapshot, sizeof(health_snapshot_t))
    xSemaphoreGive(handle->mutex)

    return ERR_OK
```

#### 5.2.6 `health_monitor_set_led_fault`

```
health_monitor_set_led_fault(handle):
    if handle == NULL:
        return ERR_NULL_PTR
    if not handle->in_use:
        return ERR_NOT_INIT

    handle->fault_latched = true
    update_led_state(handle)
    return ERR_OK
```

#### 5.2.7 `health_monitor_reset_metrics`

```
health_monitor_reset_metrics(handle):
    if handle == NULL:
        return ERR_NULL_PTR
    if not handle->in_use:
        return ERR_NOT_INIT

    xSemaphoreTake(handle->mutex, portMAX_DELAY)

    handle->snapshot.sensor_fail_count       = 0
    handle->snapshot.alarm_raise_count       = 0
    handle->snapshot.modbus_transactions_ok  = 0
    handle->snapshot.modbus_timeouts         = 0
    handle->snapshot.mqtt_publishes_sent     = 0
    handle->snapshot.mqtt_publishes_failed   = 0
    handle->snapshot.mqtt_reconnect_count    = 0
    handle->snapshot.ntp_sync_fail_count     = 0
    handle->snapshot.buffer_overflow_count   = 0

    xSemaphoreGive(handle->mutex)

    log_info("HM: metrics reset")
    return ERR_OK
```

### 5.3 Test reset hook

```c
#ifdef TEST
void health_monitor_reset_for_test(void)
{
    (void)memset(g_pool, 0, sizeof(g_pool));
    g_count = 0U;
}
#endif
```

---

## 6. LED state machine

The GW board (B-L475E-IOT01A) has one user-controllable LED: LD2 (green,
PB14). Decision HM-D9 maps device state to blink-rate patterns on this
single LED.

```c
static void update_led_state(struct health_monitor_inst *self)
{
    /* Priority order: Fault > Alarm > Operational > Init */

    if (self->fault_latched) {
        led_set(LED_GREEN, LED_PATTERN_BLINK_FAST);       /* 200 ms */
        return;
    }

    bool any_alarm = false;
    for (uint8_t i = 0U; i < SENSOR_ID_COUNT; i++) {
        if (self->snapshot.alarm_state[i] != ALARM_STATE_CLEAR) {
            any_alarm = true;
            break;
        }
    }

    if (any_alarm) {
        led_set(LED_GREEN, LED_PATTERN_BLINK_SLOW);       /* 1000 ms */
        return;
    }

    if (self->snapshot.lifecycle_state == LIFECYCLE_STATE_OPERATIONAL) {
        led_set(LED_GREEN, LED_PATTERN_ON);
        return;
    }

    /* Init or other transient states */
    led_set(LED_GREEN, LED_PATTERN_BLINK_MEDIUM);         /* 500 ms */
}
```

**LED pattern summary:**

| Device state | LED_GREEN pattern | Visual |
|---|---|---|
| Fault (latched) | `LED_PATTERN_BLINK_FAST` (200 ms) | Rapid blink |
| Alarm active (any sensor) | `LED_PATTERN_BLINK_SLOW` (1000 ms) | Slow blink |
| Operational | `LED_PATTERN_ON` | Solid green |
| Init / transient | `LED_PATTERN_BLINK_MEDIUM` (500 ms) | Medium blink |

LED blinking is implemented by LedDriver using FreeRTOS software timers —
not by HealthMonitor toggling the GPIO in a loop. HealthMonitor only sets
the desired pattern; LedDriver handles the timing.

`update_led_state()` is called:
- In `health_monitor_create()` — initial state.
- In `push_event()` — after `ALARM_RAISED` or `ALARM_CLEARED`.
- In `update_lifecycle()` — on lifecycle state change.
- In `set_led_fault()` — on fault latch.

`update_led_state()` reads `fault_latched` and `alarm_state[]` without
acquiring the mutex — it is always called from a context that has just
released the mutex (or during init before any concurrent access).
`led_set()` is a simple GPIO write plus timer command through LedDriver —
no blocking, no mutex of its own. LedDriver is a singleton; no handle
needed.

---

## 7. Synchronisation

This component uses an internal priority-inheritance mutex (`health_mutex`
per task-breakdown.md §7) to serialise concurrent callers. The mutex is
created during `health_monitor_create()` and held only for the duration
of each guarded operation.

**Critical section duration.** Bounded by `sizeof(health_snapshot_t)` — a
`memcpy` of ~160 bytes takes ~100 cycles at 80 MHz. Acceptable for all
callers.

**Callers by task (from task-breakdown.md §5.2):**

| Task | Function(s) called | Interface |
|---|---|---|
| `SensorTask` | `push_event(SENSOR_FAIL/RECOVERED)`, `push_event(ALARM_RAISED/CLEARED)` | IHealthReport |
| `ModbusPollerTask` | `update_modbus_master_stats()`, `push_event(MODBUS_LINK_UP/DOWN)` | IHealthReport |
| `CloudPublisherTask` | `poll()`, `get_snapshot()`, `update_mqtt_stats()`, `update_buffer_occupancy()`, `update_wifi_rssi()`, `push_event(CLOUD_CONNECTED/DISCONNECTED)` | IHealthReport, IHealthSnapshot |
| `TimeServiceTask` | `push_event(TIME_SYNC_ACQUIRED/LOST)`, `push_event(NTP_SYNC_FAILED/OK)` | IHealthReport |
| `ConsoleTask` | `get_snapshot()` | IHealthSnapshot |
| `LifecycleTask` | `update_lifecycle()`, `set_led_fault()`, `reset_metrics()` | IHealthReport, IHealthAdmin |

All public functions are task-safe but not ISR-safe.

---

## 8. Sequence integration

See `sequence-diagrams.md` SD-03b for inter-component flows.

| SD | Component role | Key functions |
|---|---|---|
| SD-03b | `CloudPublisher` calls `health_monitor_poll()` then `health_monitor_get_snapshot()` to obtain the current health snapshot for the 600 s periodic health telemetry publish. | `health_monitor_poll()`, `health_monitor_get_snapshot()` |

**Health publish sequence (600 s):**

```
1. Health timer tick → CloudPublisher
2. CloudPublisher → health_monitor_poll(hm_handle)
     → polls stack watermarks for all 8 registered tasks
3. CloudPublisher → update_mqtt_stats(hm_handle, &mqtt_stats)
     → copies latest MQTT counters from IMqttStats poll
4. CloudPublisher → health_monitor_get_snapshot(hm_handle, &snap)
     → returns full snapshot with fresh uptime_s
5. CloudPublisher → serialise snap to JSON (REQ-CC-070, CC-090)
6. CloudPublisher → MqttClient → WifiDriver → AWS IoT Core (QoS 0)
```

---

## 9. Error and fault behaviour

All public functions return `health_monitor_err_t`. Callers must not
ignore non-OK returns except where noted (push_event best-effort).

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `ERR_OK` | Operation succeeded | — | Success | — | — |
| `ERR_NULL_PTR` | Required pointer argument is NULL | Return immediately; no state change | Non-OK return | No retry — programming error | Caller logs at ERROR |
| `ERR_NO_RESOURCE` | Static pool exhausted (second `create()`) or task table full | Return immediately | Non-OK return | No retry — programming error | Caller logs at ERROR |
| `ERR_NOT_INIT` | Function called before `create()` | Return immediately; no state change | Non-OK return | No retry — programming error | Caller logs at ERROR |

**push_event is best-effort (HM-D7):** Out-of-range event IDs and
out-of-range param values for sensor-indexed events return ERR_OK and log
at WARN. Health events must not create cascading error chains in
producers.

---

## 10. Principles applied

- **P1 (Strict directional layering).** Application → Driver: depends on ILed (LedDriver). No upward dependencies. HealthMonitor is at the Application layer.
- **P2 (Dependency Inversion).** Middleware producers depend on the opaque `health_monitor_handle_t` and the `health_monitor.h` API, not on HealthMonitor's implementation. Decision HM-D8.
- **P3 (Interface Segregation).** Three separate interface groups because distinct consumer sets have non-overlapping access: `IHealthSnapshot` (read-only — CloudPublisher, ConsoleService), `IHealthReport` (write-only — all producers), `IHealthAdmin` (admin reset + fault LED — LifecycleController). Documented in `components.md` ISP section.
- **P4 (Cross-cutting exception).** Logger referenced concretely per the cross-cutting convention. HealthMonitor itself IS one of the two cross-cutting concerns defined by P4; its concrete reference by other application components is the P4 exception.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static pool of 1; all metric counters in a static aggregate struct; `StaticSemaphore_t` for mutex; no heap.
- **P6 (Traces to requirements).** Every metric traces to a REQ-CC-* monitoring requirement; `reset_metrics()` traces to REQ-LD-070; LED indication traces to REQ-NF-208.
- **P7 (Pull-based downstream consumption).** CloudPublisher and ConsoleService pull the snapshot on their own schedule; HealthMonitor does not push to consumers.
- **P8 (Total error propagation).** 4 distinct error codes. push_event returns ERR_OK on out-of-range input (best-effort) — documented exception to avoid cascading failures.
- **P9 (BARR-C).** Metric counters `uint32_t`; task count `uint8_t`; RSSI `int32_t`; fixed-width types throughout; no floating-point.
- **P10 (Naming).** Prefix `health_monitor_`; handle `health_monitor_handle_t`; errors `HEALTH_MONITOR_ERR_*`; snapshot `health_snapshot_t`.

---

## 11. Memory and sizing

| Item | Size |
|---|---|
| `health_snapshot_t` | 112 B (measured; estimated ~160 B, HM-O1) |
| `task_handles[8]` | 32 B |
| `task_names[8][16]` | 128 B |
| `StaticSemaphore_t mutex_buf` | 80 B |
| `fault_latched`, `in_use`, `task_count` | 3 B |
| Misc (padding) | ~6 B |
| **Total RAM per instance** | **~410 B estimated, 360 B measured** |

No flash footprint beyond code. HealthMonitor has no persistent storage.

---

## 12. Unit-test plan

Test file: `tests/gateway/application/health_monitor/test_health_monitor.c`

### 12.1 Framework and mock strategy

| Item | Value |
|---|---|
| Framework | Unity (via Ceedling 0.31) |
| Mocking | CMock (auto-generated from headers) |
| LedDriver | Mocked via CMock from `led_driver.h`. Stubs verify `led_set(led_id_t, led_pattern_t)` call parameters (2-arg form, singleton — no handle). |
| FreeRTOS | Mocked via CMock. `xSemaphoreCreateMutexStatic` returns a stub handle. `xSemaphoreTake`/`xSemaphoreGive` are no-ops. `xTaskGetTickCount` returns a controllable value. `uxTaskGetStackHighWaterMark` returns controllable values per task. |
| ILogger | Mocked via CMock (no-op stubs). |
| Test isolation | `health_monitor_reset_for_test()` called in `setUp()` to clear static pool. |

### 12.2 Test cases

#### Group 1: Create / lifecycle

| ID | Precondition | Action | Expected |
|---|---|---|---|
| HM-T01 | `led_set` mock succeeds | `health_monitor_create(&cfg, &h)` | `ERR_OK`; snapshot zeroed; `lifecycle_state == INIT`; `last_reset_cause == cfg.boot_reset_cause`; all `sensor_valid[] == true`; `led_set(LED_GREEN, LED_PATTERN_BLINK_MEDIUM)` called |
| HM-T02 | — | `health_monitor_create(NULL, &h)` | `ERR_NULL_PTR`; handle unchanged |
| HM-T03 | — | `health_monitor_create(&cfg, NULL)` | `ERR_NULL_PTR` |
| HM-T04 | One handle already created | `health_monitor_create(&cfg, &h2)` | `ERR_NO_RESOURCE` |
| HM-T05 | `reset_for_test()`; no create | `health_monitor_push_event(h_stale, ev, 0)` | `ERR_NOT_INIT` |

#### Group 2: Register task

| ID | Precondition | Action | Expected |
|---|---|---|---|
| HM-T06 | Handle created | `health_monitor_register_task(h, "SensorTask", xTask)` | `ERR_OK`; task_count == 1; name stored |
| HM-T07 | Register 8 tasks sequentially | After 8th `register_task` | `ERR_OK`; task_count == 8 |
| HM-T08 | 8 tasks registered | `health_monitor_register_task(h, "NinthTask", xTask9)` | `ERR_NO_RESOURCE`; task_count stays 8 |

#### Group 3: Push event — happy path

| ID | Precondition | Action | Expected |
|---|---|---|---|
| HM-T09 | Handle created | `push_event(h, CONFIG_WRITE_FAIL, 0)` | `ERR_OK`; `snapshot.config_write_failed == true` |
| HM-T10 | Handle created | `push_event(h, SENSOR_FAIL, 0)` | `ERR_OK`; `sensor_valid[0] == false`; `sensor_fail_count == 1` |
| HM-T11 | HM-T10 done | `push_event(h, SENSOR_RECOVERED, 0)` | `ERR_OK`; `sensor_valid[0] == true` |
| HM-T12 | Handle created | `push_event(h, TIME_SYNC_ACQUIRED, 0)` | `ERR_OK`; `time_sync_state == SYNCED` |
| HM-T13 | Handle created | `push_event(h, NTP_SYNC_OK, 1700000000)` | `ERR_OK`; `last_ntp_sync_epoch == 1700000000` |
| HM-T14 | Handle created | `push_event(h, CLOUD_CONNECTED, 0)` then `push_event(h, CLOUD_DISCONNECTED, 0)` | `cloud_connected` transitions: `false → true → false` |
| HM-T15 | Handle created | `push_event(h, ALARM_RAISED, 0)` | `ERR_OK`; `alarm_state[0] == ACTIVE`; `alarm_raise_count == 1`; `led_set(LED_GREEN, LED_PATTERN_BLINK_SLOW)` called |
| HM-T16 | Handle created | `push_event(h, HEALTH_EVENT_COUNT, 0)` (out-of-range) | `ERR_OK`; snapshot unchanged; WARN logged |

#### Group 4: Stats update

| ID | Precondition | Action | Expected |
|---|---|---|---|
| HM-T17 | Handle created; stats = {ok=100, timeouts=5} | `update_modbus_master_stats(h, &stats)` | `ERR_OK`; snapshot `modbus_transactions_ok == 100`, `modbus_timeouts == 5`, `modbus_link_online` unchanged (`false`) |
| HM-T18 | Handle created; stats = {sent=50, failed=2, reconnect=3} | `update_mqtt_stats(h, &stats)` | `ERR_OK`; snapshot fields match |
| HM-T19 | Handle created | `update_buffer_occupancy(h, 42)` | `ERR_OK`; `buffer_entry_count == 42` |
| HM-T20 | Handle created | `update_wifi_rssi(h, -65)` | `ERR_OK`; `wifi_rssi_dbm == -65` |
| HM-T21 | Handle created | `update_lifecycle(h, LIFECYCLE_STATE_OPERATIONAL)` | `ERR_OK`; `lifecycle_state == OPERATIONAL`; `led_set(LED_GREEN, LED_PATTERN_ON)` called |
| HM-T22 | Handle created; 3 tasks registered; FreeRTOS mock: watermarks = {100, 200, 150} | `health_monitor_poll(h)` | `ERR_OK`; `stack_watermark_words[0..2]` == {100, 200, 150}; `uxTaskGetStackHighWaterMark` called 3 times |

#### Group 5: Snapshot

| ID | Precondition | Action | Expected |
|---|---|---|---|
| HM-T23 | Handle created; push several events | `get_snapshot(h, &snap)`, then `push_event(h, NTP_SYNC_FAILED, 0)` | Snapshot copy unaffected by subsequent push |
| HM-T24 | Handle created; FreeRTOS mock: `xTaskGetTickCount` returns 80000 (80 s at 1 kHz) | `get_snapshot(h, &snap)` | `snap.uptime_s == 80` |
| HM-T25 | Handle created | `get_snapshot(h, NULL)` | `ERR_NULL_PTR` |
| HM-T26 | Handle created; push events from two pthreads concurrently | `get_snapshot` from a third pthread | No torn reads; all fields consistent |

#### Group 6: LED state machine

| ID | Precondition | Action | Expected |
|---|---|---|---|
| HM-T27 | Handle just created (lifecycle_state == INIT) | Check `led_set` call in create | `led_set(LED_GREEN, LED_PATTERN_BLINK_MEDIUM)` called |
| HM-T28 | Handle created | `update_lifecycle(h, OPERATIONAL)` | `led_set(LED_GREEN, LED_PATTERN_ON)` called |
| HM-T29 | Operational | `push_event(h, ALARM_RAISED, 0)` | `led_set(LED_GREEN, LED_PATTERN_BLINK_SLOW)` called |
| HM-T30 | Alarm active on sensor 0 | `push_event(h, ALARM_CLEARED, 0)` | `led_set(LED_GREEN, LED_PATTERN_ON)` called (back to operational; no other alarms) |
| HM-T31 | Operational | `set_led_fault(h)`, then `push_event(h, ALARM_RAISED, 1)` | First: `led_set(LED_GREEN, LED_PATTERN_BLINK_FAST)`. Second: LED stays `LED_PATTERN_BLINK_FAST` (fault is latched — HM-D6) |

#### Group 7: Admin

| ID | Precondition | Action | Expected |
|---|---|---|---|
| HM-T32 | Handle created; push various events to increment counters; set lifecycle to OPERATIONAL | `reset_metrics(h)`, then `get_snapshot(h, &snap)` | Counters zeroed (sensor_fail_count, alarm_raise_count, modbus_*, mqtt_*, ntp_sync_fail_count, buffer_overflow_count). Preserved: lifecycle_state (OPERATIONAL), sensor_valid[], alarm_state[], config_write_failed, uptime_s, wifi_rssi_dbm, cloud_connected, buffer_entry_count, last_ntp_sync_epoch |
| HM-T33 | Handle created | `set_led_fault(h)` | `ERR_OK`; fault_latched == true; `led_set(LED_GREEN, LED_PATTERN_BLINK_FAST)` called |
| HM-T34 | `reset_for_test()`; no create | `reset_metrics(h_stale)` | `ERR_NOT_INIT` |
| HM-T35 | `reset_for_test()`; no create | `set_led_fault(h_stale)` | `ERR_NOT_INIT` |

### 12.3 Error-code cross-check

Every `health_monitor_err_t` value must be exercised by at least one
test case.

| Error code | Value | Covering test(s) |
|---|---|---|
| `ERR_OK` | 0 | HM-T01, T06–T07, T09–T15, T17–T24, T28–T33 |
| `ERR_NULL_PTR` | 1 | HM-T02, T03, T25 |
| `ERR_NO_RESOURCE` | 2 | HM-T04, T08 |
| `ERR_NOT_INIT` | 3 | HM-T05, T34, T35 |

**All 4 error codes covered.** ✓

### 12.4 Coverage target

Branch coverage target: **90 %** on `health_monitor.c`. The event dispatch
switch has 15 cases, each exercised by at least one test. The LED state
machine has 4 branches, each exercised.

### 12.5 Cannot be host-tested

| Scenario | Reason | Verification method |
|---|---|---|
| Real LED blink on LD2 | Host has no GPIO hardware | Visual check on board |
| Priority-inheritance mutex behaviour under contention | Host pthreads do not replicate FreeRTOS scheduling | Integration test with real tasks |
| `uxTaskGetStackHighWaterMark` accuracy | Requires real FreeRTOS TCBs | Integration test on hardware |

---

## 13. Open items

| ID | Item | Status | Resolution path |
|---|---|---|---|
| **HM-O1** | `health_snapshot_t` size — estimate ~160 bytes. Verify with `sizeof()` at coding time; add a `static_assert` to catch unexpected growth beyond 256 bytes. | **Resolved** | Measured 112 bytes on the target; `_Static_assert(sizeof(health_snapshot_t) <= 256U)` added. One instance takes 360 bytes of RAM. |
| **HM-O2** | GW LD2 hardware verification — confirm LD2 (PB14) is the user-controllable LED and is not shared with SWD/debug or WiFi module signals. | **Open** | The bring-up test leaves LD2 blinking in the fault pattern; confirm by eye (same item as LED-O7 in the LedDriver companion). The pin was PA5 in v0.2; corrected to PB14 (PA5 is LD1, shared with SPI1_SCK). |
| **HM-O3** | `HEALTH_EVENT_ALARM_RAISED` carries only a sensor id, so the direction of the alarm is lost: the snapshot records `ALARM_STATE_ACTIVE_HIGH` for any raised alarm. | **Open** | Decide at the AlarmService / CloudPublisher health payload whether the event needs a direction (for example in the upper bits of `param`). |
| **HM-O4** | **Producers still call placeholders, not this module.** CircularFlashLog (`health_report_push_buffer_event`), StoreAndForward (`health_update_buffer_occupancy`), CloudPublisher (`health_report_*`) and TimeProvider (`ihealth_report_t`) each declare their own stand-in for IHealthReport. None calls `health_monitor_*` yet. TimeProvider's placeholder also defines `health_event_t` and `health_monitor_err_t`, so a translation unit that includes both `time_provider.h` and `health_monitor.h` will not compile. | **Open** | One follow-up per producer: replace the stand-in with a `health_monitor_handle_t` call and drop its placeholder. TimeProvider's `time_provider_init(const ihealth_report_t *)` signature changes with it (CFL-O5, SAF-O4, TP-O4). |

---

## 14. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| HM-D1 | ADT pattern (opaque handle, static pool of 1) | Gateway default. LedDriver is a singleton — accessed directly via `led_set(led_id_t, led_pattern_t)`, no handle injection. Logger accessed directly (P4). Consistent with CFL, SAF, and all gateway Application-layer modules. |
| HM-D2 | Single error enum `health_monitor_err_t` (4 values) | v0.1 had two enums (`health_monitor_err_t` + `health_admin_err_t`). All public functions share the same failure modes (NULL, not init, no resource). One enum simplifies callers and testing. |
| HM-D3 | GW-only companion | FD has its own HealthMonitor implementation (field-device-v1.0). The GW snapshot has different fields (Modbus master, MQTT, WiFi, NTP, SAF buffer). No `#if defined(BOARD_GATEWAY)` guards needed. |
| HM-D4 | `boot_reset_cause` injected via config struct | Determined once at boot by LifecycleController from RCC reset flags. Immutable after init — does not need a runtime update function. |
| HM-D5 | Uptime computed fresh in `get_snapshot()` | v0.1 updated uptime in poll(). This gives stale values (up to 600 s behind). Computing from `xTaskGetTickCount()` under mutex in `get_snapshot()` is always current with negligible cost. |
| HM-D6 | Fault LED latched until reboot | Once LifecycleController enters Faulted, the device requires a reboot to recover. The LED must reflect this permanently. A `fault_latched` bool overrides all other LED patterns. |
| HM-D7 | `push_event` is best-effort | Health events are diagnostic signals. A producer that receives an error from push_event would have no useful recovery action. Returning ERR_OK for out-of-range inputs (with WARN log) prevents cascading error handling in every producer. |
| HM-D8 | No vtable — ADT handle is the DIP seam | Middleware producers depend on `health_monitor.h` (public API), not on the implementation. The opaque handle satisfies P2 (DIP) without the RAM and complexity cost of function-pointer tables. Consistent with SAF-D5 and CFL ADT approach. |
| HM-D9 | GW LED mapping — single LED (LD2) with blink-rate encoding | The B-L475E-IOT01A has one fitted user LED (LD2 green at PB14). Blink rate encodes severity: rapid (fault), slow (alarm), solid (operational), medium (init). Resolves HM-O2 from v0.1. |
| HM-D10 | `HEALTH_MONITOR_MAX_TASKS = 8U` | Matches the 8 GW application tasks in task-breakdown.md §5.2. v0.1 used 7 — incorrect; the WifiTask was missing from the count. |

---

## 15. File layout

```
firmware/gateway/application/health_monitor/
├── health_monitor.h        /* public API — handle, config, error enum,
                               event enum, snapshot struct, all
                               interface functions */
└── health_monitor.c        /* implementation — event dispatch, LED state
                               machine, stats aggregation, poll */

tests/gateway/application/health_monitor/
└── test_health_monitor.c   /* Unity + CMock (LedDriver, FreeRTOS, Logger) */
```

---

## 15.1 Gateway implementation notes

Where the code settles something this companion left open, or differs from its
pseudocode. The public API in §3 is unchanged.

| # | Topic | Gateway behaviour |
|---|---|---|
| 1 | LED update under the lock | `update_led_state()` runs while the mutex is held, in `push_event` (alarm events), `update_lifecycle`, `set_led_fault` and, before the instance is visible, `create`. §6 had it read the snapshot unlocked after release; that lets two tasks choose a pattern, then write it in the opposite order and leave the LED showing the older decision. `led_set()` never blocks and takes no lock, so holding the HealthMonitor mutex across it is safe. |
| 2 | Logging after unlock | `push_event` records what it must log and writes the log line after releasing the mutex, so a slow Logger sink never lengthens the critical section. |
| 3 | `set_led_fault` locking | Takes the mutex (§5.2.6 had none): `fault_latched` is read by every LED update. |
| 4 | `register_task` | Checks `in_use` (returns `ERR_NOT_INIT` for a stale handle) and takes the mutex; the pseudocode did neither. Names are truncated to 15 characters by a bounded copy. |
| 5 | MQTT field name | The real `mqtt_stats_t` calls the failure counter `publish_failures`, not `publishes_failed`; the snapshot field keeps the companion's name. |
| 6 | Alarm state | The Field Device `alarm_state_t` has `ALARM_STATE_ACTIVE_HIGH` and `_LOW`, not a single `ALARM_STATE_ACTIVE`. A raised alarm is recorded as `ACTIVE_HIGH` (HM-O3); the LED treats any value other than `ALARM_STATE_CLEAR` as an alarm, as §6 already did. |
| 7 | Time sync names | The real enumerators are `TIME_SYNC_SYNCHRONISED` / `TIME_SYNC_UNSYNCHRONISED`, not `SYNCED` / `UNSYNCED`. `health_monitor.h` carries the type under TimeProvider's guard (`TIME_SYNC_STATE_DEFINED`) instead of including `time_provider.h`, whose IHealthReport placeholder would clash with this header (HM-O4). |
| 8 | Placeholder types | The Gateway has no LifecycleController, SensorService, AlarmService or ModbusMaster, so `lifecycle_state_t`, `lifecycle_reset_cause_t`, `sensor_id_t`, `alarm_state_t` and `modbus_master_stats_t` are defined in `health_monitor.h` under the Field Device headers' include guards, with the same enumerator names and values, so the real headers supersede them. |
| 9 | Mutex creation failure | `create` returns `ERR_NO_RESOURCE` without consuming the pool slot if the mutex cannot be created (the pseudocode assumed static creation cannot fail). |
| 10 | LedDriver failure | A non-OK `led_set()` is logged at WARN and ignored; the LED is an indicator, not a dependency (LED-O2 in the LedDriver companion). |
| 11 | Stack watermarks | `update_stack_watermarks` copies one value per registered task. `poll` reads the task count under the lock, collects the watermarks outside it, then stores them through that function. |
| 12 | Include paths | `health_monitor.h` includes the real `mqtt_client.h` for `mqtt_stats_t`. The host tests run under `tests/project_gateway.yml` (the Gateway and Field Device modules share the basename `health_monitor`) with `mqtt_client` and `wifi_task` added as include roots, and the Gateway directory is excluded from `tests/project.yml`. The test file is `test_health_monitor_gateway.c`, defines block `:test_health_monitor_gateway:`. |

### 15.2 Gateway test results

`tests/gateway/application/health_monitor/test_health_monitor_gateway.c`: 50
tests, 49 pass, 1 deferred, 0 fail (2026-10-11). HM-T01..T35 follow §12.2;
HM-T36 onward cover every function on a NULL and a stale handle, NULL data
pointers, out-of-range and boundary sensor ids, multiple simultaneous alarms, the
FAULT event, LedDriver and mutex-creation failures, lock balance, empty polls,
direct watermark updates and long task names. HM-T26 (concurrent access) is
deferred to TC-HW-HM-009. Four deliberate mutations (fault latch ignored, a
counter dropped from the reset, an off-by-one in the sensor-id check, the wrong
MQTT field) each failed the intended tests.

Two additive changes to the shared test support: the FreeRTOS mock can return a
different stack watermark per call (needed for HM-T22), and `led_driver_stub.h`
declares the Gateway pattern API (`led_pattern_t`, `led_set`, `LED_ERR_INVALID_PARAM`).

### 15.3 Hardware bring-up

Harness: `firmware/gateway/integration-tests/health_monitor/main_test_health_monitor.c`.
It uses the real LedDriver, the real scheduler and real tasks. The LED cases read
`GPIOB->ODR` and time the transitions with the DWT cycle counter.

| ID | What to observe | Verifies |
|---|---|---|
| TC-HW-HM-001 | After `create()`: INIT state, reset cause recorded, all sensors valid, init pattern set | §5.2.1 |
| TC-HW-HM-002 | The init pattern toggles PB14 every 250 ms | INIT → medium blink |
| TC-HW-HM-003 | `update_lifecycle(OPERATIONAL)`: LD2 solid on and steady | Operational → solid |
| TC-HW-HM-004 | `ALARM_RAISED`: 500 ms toggling; `ALARM_CLEARED`: solid again | Alarm → slow blink |
| TC-HW-HM-005 | `uptime_s` advances with the tick count | HM-D5 |
| TC-HW-HM-006 | `poll()` returns non-zero watermarks below each stack size for three tasks | §5.2.4 on real TCBs |
| TC-HW-HM-007 | Events, MQTT and Modbus stats, occupancy and RSSI round-trip | Snapshot fields |
| TC-HW-HM-008 | `reset_metrics()` zeroes the counters and keeps the rest | §3.6 |
| TC-HW-HM-009 | Two writers and a reader: no torn multi-field MQTT block, no lost event | The mutex |
| TC-HW-HM-010 | Error paths | §9 |
| TC-HW-HM-011 | `set_led_fault()`: 100 ms toggling, still latched after a later alarm and lifecycle change | HM-D6 |

**Status: run on hardware 2026-10-11 — all 11 cases PASS** (B-L475E-IOT01A).
Measured mean half-periods: 244 ms (init, first interval from task start), 500 ms
(alarm), 100 ms (fault). In TC-HW-HM-009 a reader took 3 273 snapshots while 1 000
MQTT updates and 2 000 counting events ran: 0 errors, 0 torn reads, and the final
counters were exact. The harness leaves LD2 in the fault pattern; confirmation by
eye is tracked as HM-O2.

---

## Phase H — Readiness review

| # | Check | Status |
|---|---|---|
| H1 | PROVIDES / USES match `components.md` | PASS — PROVIDES IHealthSnapshot, IHealthReport, IHealthAdmin; USES ILed (LedDriver), ILogger |
| H2 | Root SRS requirements cited | PASS — REQ-CC-010, REQ-CC-070, REQ-CC-090, REQ-NF-208, REQ-LD-070 |
| H3 | All public API functions have complete Doxygen | PASS |
| H4 | ADT pattern applied | PASS — HM-D1 |
| H5 | Error enum covers all failure modes | PASS — 4 error codes, single enum (HM-D2) |
| H6 | Event dispatch table documented with per-event field mapping | PASS — §4 |
| H7 | Snapshot struct documented without conditional compilation | PASS — HM-D3 |
| H8 | LED state machine documented with blink patterns | PASS — §6, HM-D9 |
| H9 | Open items have resolution path | PASS — O1 resolved; O2 (visual check), O3, O4 open with owners |
| H10 | Unit-test plan covers happy + error + boundary cases | PASS — 50 test cases, 49 pass and 1 deferred to hardware; all 4 error codes covered (§12.3) |
| H11 | Test file path follows Gateway convention | PASS |
| H12 | P1–P10 compliance reviewed | PASS — §10 |
| H13 | Thread safety documented | PASS — §7, callers-by-task table |
| H14 | `reset_for_test` hook specified | PASS — §5.3 |
| H15 | Decisions log complete | PASS — 10 decisions |
| H16 | Memory and sizing budget documented | PASS — §11 (~410 B estimated; 360 B measured per instance) |
| H17 | Sequence integration documented | PASS — §8, SD-03b health publish flow |
| H18 | Gateway implemented, host-tested and hardware-validated | PASS — §15.1–15.3: 50 host tests, 11 hardware cases |

**Verdict: PASS — implemented and hardware-validated for Gateway.** No producer calls it yet (HM-O4).

---

*Companion produced during the LLD Application Phase. Authored by Luca
Agrippino; reviewed against the V-Model gate criteria for LLD.*
