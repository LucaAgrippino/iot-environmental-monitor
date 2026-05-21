# LLD Companion — HealthMonitor

**Layer:** Application  
**Boards:** Field Device (FD) · Gateway (GW)  
**Provides:** `IHealthSnapshot` *(read side)*, `IHealthReport` *(write side)*, `IHealthAdmin` *(control — LLD-D15)*  
**Consumes:** `ILed` (LedDriver), `ILogger`  
**SRS traces:** REQ-CC-010, REQ-CC-070, REQ-CC-090, REQ-NF-208  
**HLD ref:** `components.md` §Application — HealthMonitor; §"Metric Producer Pattern" (P5); §DIP; `hld.md` §5.5, §6.4; `sequence-diagrams.md` SD-03b
**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** HealthMonitor in `components.md` (FD + GW application layer)

---

## 1. Sources

HealthMonitor is a passive aggregator. It maintains a consolidated
`device_health_snapshot_t` in RAM, updated continuously by producers
via `IHealthReport`, and serves it to consumers via `IHealthSnapshot`.
It also drives the board LEDs to indicate device status at a glance.

HealthMonitor has **no thread**. Every operation executes in the calling
task's context — producers push from their own tasks, consumers pull
from their own tasks. A mutex protects the snapshot.

**DIP role:** `IHealthReport` is Application-layer owned (here) but
consumed by Middleware producers. Middleware depends on the abstraction,
not on `HealthMonitor`'s implementation — this preserves directional
layering while allowing bottom-up metric flow.

---

## 2. Health snapshot struct

All fields are populated continuously. GW-only fields are conditional.

```c
/* health_monitor.h */

typedef struct {
    /* ── System ── */
    uint32_t                uptime_s;          /* seconds since last boot */
    lifecycle_state_t       lifecycle_state;
    lifecycle_reset_cause_t last_reset_cause;

    /* ── Sensors ── */
    bool                    sensor_valid[SENSOR_ID_COUNT]; /* per-sensor fail flag */
    uint32_t                sensor_fail_count;  /* cumulative since boot */
    time_sync_state_t       time_sync_state;

    /* ── Alarms ── */
    alarm_state_t           alarm_state[SENSOR_ID_COUNT];
    uint32_t                alarm_raise_count;  /* cumulative alarm raises */

    /* ── Config ── */
    bool                    config_write_failed; /* set on any ConfigStore write fail */

    /* ── Modbus — Field Device ── */
    uint32_t                modbus_valid_frames;
    uint32_t                modbus_crc_errors;
    uint32_t                modbus_addr_mismatches;
    uint32_t                modbus_exception_responses;

    /* ── Modbus — Gateway ── */
#if defined(BOARD_GATEWAY)
    uint32_t                modbus_transactions_ok;
    uint32_t                modbus_timeouts;
    bool                    modbus_link_online;     /* field device link state */

    /* ── Cloud (GW only) ── */
    uint32_t                mqtt_publishes_sent;
    uint32_t                mqtt_publishes_failed;
    uint32_t                mqtt_reconnect_count;
    int32_t                 wifi_rssi_dbm;
    bool                    cloud_connected;

    /* ── Store-and-forward buffer (GW only) ── */
    uint32_t                buffer_entry_count;
    uint32_t                buffer_overflow_count;  /* cumulative drops */

    /* ── NTP (GW only) ── */
    uint32_t                ntp_sync_fail_count;
    uint32_t                last_ntp_sync_epoch;    /* 0 if never synced */
#endif

    /* ── RTOS task stack watermarks ── */
    uint16_t                stack_watermark_words[HEALTH_TASK_COUNT];
} device_health_snapshot_t;

#define HEALTH_TASK_COUNT    7U   /* matches task list in task-breakdown.md */
```

Stack watermarks are polled from `uxTaskGetStackHighWaterMark()` at each
health-report interval by a dedicated health-poll function (§5).

---

## 3. Data types

```c
/* health_monitor.h */

typedef enum {
    HEALTH_MONITOR_ERR_OK        = 0,
    HEALTH_MONITOR_ERR_NOT_INIT  = 1,
    HEALTH_MONITOR_ERR_NULL_ARG  = 2,
} health_monitor_err_t;

typedef enum {
    HEALTH_ADMIN_ERR_OK       = 0,
    HEALTH_ADMIN_ERR_NOT_INIT = 1,
} health_admin_err_t;

/* Event constants used by IHealthReport push functions */
typedef enum {
    HEALTH_EVENT_TIME_SYNC_ACQUIRED        = 0,
    HEALTH_EVENT_TIME_SYNC_LOST            = 1,
    HEALTH_EVENT_CONFIG_WRITE_FAIL         = 2,
    HEALTH_EVENT_CONFIG_READ_FAIL          = 3,
    HEALTH_EVENT_CONFIG_NO_VALID_SLOT      = 4,
    HEALTH_EVENT_SENSOR_FAIL               = 5,
    HEALTH_EVENT_NTP_SYNC_FAILED           = 6,   /* GW */
    HEALTH_EVENT_NTP_BAD_RESPONSE          = 7,   /* GW */
    HEALTH_EVENT_BUFFER_OVERFLOW           = 8,   /* GW */
    HEALTH_EVENT_BUFFER_FLASH_ERR          = 9,   /* GW */
    HEALTH_EVENT_MODBUS_LINK_UP            = 10,  /* GW */
    HEALTH_EVENT_MODBUS_NODE_OFFLINE       = 11,  /* GW */
    HEALTH_EVENT_ALARM_RAISED              = 12,
    HEALTH_EVENT_ALARM_CLEARED             = 13,
    HEALTH_EVENT_FAULT                     = 14,
} health_event_t;
```

---

## 2. Public API

### 4.1 `IHealthReport` — write side (used by producers)

```c
/**
 * @brief  Initialise HealthMonitor.
 *
 * Creates the mutex. Zeroes the snapshot. Must be called once during
 * Init before any producer calls IHealthReport.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero error code on failure.
 */
health_monitor_err_t health_monitor_init(void);

/**
 * @brief  Push a named event into the snapshot.
 *
 * Thread-safe. Acquires mutex, sets the corresponding flag or increments
 * the corresponding counter in the snapshot, releases mutex, then calls
 * update_led_state() if the event affects LED indication.
 *
 * Safe to call from any task context. Not safe from ISR.
 *
 * @param  event  Event identifier.
 * @param  param  Event-specific parameter (sensor_id, fault_code, etc.).
 *                Pass 0 if not applicable.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero error code on failure.
 */
health_monitor_err_t health_monitor_push_event(health_event_t event,
                                                uint32_t       param);

/**
 * @brief  Update Modbus slave stats in the snapshot (FD).
 *
 * Called by ModbusRegisterMap each cycle after polling IModbusSlaveStats.
 * Acquires mutex; copies stats fields atomically.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero error code on failure.
 */
health_monitor_err_t health_monitor_update_modbus_slave_stats(
    const modbus_slave_stats_t *stats);

/**
 * @brief  Update Modbus master stats in the snapshot (GW).
 *
 * Called by ModbusPoller each health-report cycle after polling
 * IModbusMasterStats.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_modbus_master_stats(
    const modbus_master_stats_t *stats);

/**
 * @brief  Update MQTT stats in the snapshot (GW).
 *
 * Called by CloudPublisher each health-report cycle after polling
 * IMqttStats.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_mqtt_stats(
    const mqtt_stats_t *stats);

/**
 * @brief  Update store-and-forward buffer occupancy (GW).
 *
 * Called by StoreAndForward when occupancy changes (after each
 * enqueue or consume).
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_buffer_occupancy(
    uint32_t entry_count);

/**
 * @brief  Update RTOS task stack watermarks.
 *
 * Called by the health-poll function (§5) on each health-report interval.
 * One call covers all tasks.
 *
 * @param  watermarks  Array of HEALTH_TASK_COUNT watermark values (words).
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero error code on failure.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
health_monitor_err_t health_monitor_update_stack_watermarks(
    const uint16_t watermarks[HEALTH_TASK_COUNT]);
```

### 4.2 `IHealthSnapshot` — read side (used by consumers)

```c
/**
 * @brief  Get a copy of the current health snapshot.
 *
 * Thread-safe. Acquires mutex; copies the entire snapshot under lock;
 * releases mutex. Returns a value copy — caller owns the copy.
 *
 * Called by: LcdUi (FD, periodic display update), ConsoleService (both,
 * CLI query), CloudPublisher (GW, periodic health publish — SD-03b).
 *
 * @param[out] snap_out  Filled with the current snapshot.
 * @return HEALTH_MONITOR_ERR_OK on success; non-zero error code on failure.
 */
health_monitor_err_t health_monitor_get_snapshot(
    device_health_snapshot_t *snap_out);

/**
 * @brief  Set the LED fault pattern explicitly.
 *
 * Called by LifecycleController on entering Faulted. Overrides the
 * normal LED state machine.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
void health_monitor_set_led_fault(void);

/* ------------------------------------------------------------------ */
/* IHealthAdmin — control side (LLD-D15)                               */
/* ------------------------------------------------------------------ */

/**
 * @brief  Reset all counter-type metrics to zero (LLD-D15).
 *
 * Dispatched by LifecycleController when CMD_RESET_METRICS arrives via
 * Modbus (ModbusRegisterMap → lifecycle_controller->handle_remote_command).
 *
 * Zeroes: Modbus RX OK, CRC errors, timeouts, sensor read errors,
 *         MQTT publish counts, reconnect count.
 * Preserves: uptime, event-based flags (sync flags, persistence-failure
 *            flags), lifecycle state.
 *
 * Thread-safe — acquires internal mutex.
 * @return HEALTH_ADMIN_ERR_OK on success; non-zero error code on failure.
 */
health_admin_err_t health_monitor_reset_metrics(void);

/* ------------------------------------------------------------------ */
/* Singleton vtable interfaces (LLD-D10, LLD-D15)                      */
/* ------------------------------------------------------------------ */

/** IHealthReport — write side (producers). */
typedef struct {
    health_monitor_err_t (*init)(void);
    health_monitor_err_t (*push_event)(health_event_t event, uint32_t param);
    health_monitor_err_t (*update_modbus_slave_stats)(
                             const modbus_slave_stats_t *stats);
    health_monitor_err_t (*update_modbus_master_stats)(
                             const modbus_master_stats_t *stats);
    health_monitor_err_t (*update_mqtt_stats)(const mqtt_stats_t *stats);
    health_monitor_err_t (*update_buffer_occupancy)(uint32_t entry_count);
    health_monitor_err_t (*update_stack_watermarks)(
                             const uint16_t watermarks[HEALTH_TASK_COUNT]);
    void                 (*set_led_fault)(void);
} ihealth_report_t;

/** Singleton pointer — write side. */
extern const ihealth_report_t * const health_report;

/** IHealthSnapshot — read side (consumers). */
typedef struct {
    health_monitor_err_t (*get_snapshot)(device_health_snapshot_t *snap_out);
} ihealth_snapshot_t;

/** Singleton pointer — read side. */
extern const ihealth_snapshot_t * const health_snapshot;

/** IHealthAdmin — control side (LLD-D15, P3 ISP — one method, one consumer). */
typedef struct {
    health_admin_err_t (*reset_metrics)(void);
} ihealth_admin_t;

/** Singleton pointer — control side. Consumed only by LifecycleController. */
extern const ihealth_admin_t * const health_admin;
```

---

## 5. Health-poll function

Some metrics cannot be pushed (they require active querying). The
health-poll function runs on each health-report interval, called
from the component that owns the interval timer:

- **FD:** called from ModbusRegisterMap's cycle (or a separate timer
  in LifecycleTask — see HM-O1).
- **GW:** called from CloudPublisher on each health publish tick (600 s).

```c
void health_monitor_poll(void)
{
    /* Stack watermarks */
    uint16_t wm[HEALTH_TASK_COUNT];
    wm[0] = (uint16_t)uxTaskGetStackHighWaterMark(s_task_handles[0]);
    /* ... per task ... */
    health_monitor_update_stack_watermarks(wm);

    /* Uptime */
    uint32_t uptime = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
    /* Acquire mutex and update snapshot.uptime_s */
}
```

`s_task_handles[]` is a static array of all task handles, populated
at boot by `health_monitor_register_task(name, handle)` calls from
each task at creation time.

---

## 6. LED status — both boards

LED indication is updated inside `health_monitor_push_event()` and
`health_monitor_init()` after every snapshot write that affects
device status.

```c
static void update_led_state(void)
{
    /* Priority order: Faulted > Alarm > Operational > Init */
    if (s_hm.snapshot.lifecycle_state == LIFECYCLE_STATE_FAULTED) {
        led_driver_set(LED_RED,    LED_ON);
        led_driver_set(LED_GREEN,  LED_OFF);
        led_driver_set(LED_ORANGE, LED_OFF);
        return;
    }

    bool any_alarm = false;
    for (int i = 0; i < SENSOR_ID_COUNT; i++) {
        if (s_hm.snapshot.alarm_state[i] != ALARM_STATE_CLEAR) {
            any_alarm = true; break;
        }
    }
    if (any_alarm) {
        led_driver_set(LED_ORANGE, LED_ON);
        led_driver_set(LED_GREEN,  LED_OFF);
        led_driver_set(LED_RED,    LED_OFF);
        return;
    }

    if (s_hm.snapshot.lifecycle_state == LIFECYCLE_STATE_OPERATIONAL) {
        led_driver_set(LED_GREEN,  LED_BLINK_SLOW);
        led_driver_set(LED_ORANGE, LED_OFF);
        led_driver_set(LED_RED,    LED_OFF);
    } else {
        /* Init or other transient states */
        led_driver_set(LED_GREEN,  LED_BLINK_FAST);
        led_driver_set(LED_ORANGE, LED_OFF);
        led_driver_set(LED_RED,    LED_OFF);
    }
}
```

LED blinking is implemented by LedDriver using a hardware timer — not by
HealthMonitor toggling the GPIO in a loop. HealthMonitor only sets the
desired pattern; LedDriver handles the timing.

**Board-specific LED assignment:**

| LED | FD (STM32F469) | GW (STM32L475) |
|-----|----------------|----------------|
| Green | LD1 | LD3 |
| Orange | LD2 | — (GW has 2 LEDs only; orange omitted) |
| Red | LD3 | LD4 (reassigned to fault) |
| Blue | LD4 | — |

GW LED mapping is simplified — use green for Operational, red for Fault,
both blinking for alarm. See HM-O2.

---

## 3. Internal design

```c
/* health_monitor.c */

typedef struct {
    bool                     initialised;
    device_health_snapshot_t snapshot;
    SemaphoreHandle_t        mutex;   /* priority-inheritance */
    TaskHandle_t             task_handles[HEALTH_TASK_COUNT];
    char                     task_names[HEALTH_TASK_COUNT][16];
    uint8_t                  task_count;
} HealthMonitorState;

static HealthMonitorState s_hm;
```

Every `IHealthReport` write and `IHealthSnapshot` read acquires
`s_hm.mutex`. The critical section duration is bounded by the struct
size — a `memcpy` of `device_health_snapshot_t` (~200 bytes) takes
~100 cycles at 80 MHz. Acceptable.

`update_led_state()` is called while the mutex is **held** — it reads
`s_hm.snapshot` fields and calls `led_driver_set()`. LedDriver calls
must not acquire a mutex internally that could be held by another task
(they are simple GPIO writes — safe).

---


### Synchronisation

This component uses an internal mutex (`health_mutex` per task-breakdown.md §7) to serialise concurrent callers. The mutex is created during `_init()` and held only for the duration of each guarded operation (bounded, short hold time). All public functions are task-safe but not ISR-safe.

### health_monitor_init

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### health_monitor_set_led_fault

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### health_monitor_reset_metrics

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### Principles applied

- **P1 (Strict directional layering).** Depends on ILed (driver layer) and Logger; HealthMonitor is at the application layer and has no upward dependencies.
- **P2 (Dependency Inversion).** Exposes three vtable interfaces; consumes ILed via its interface — not the concrete LedDriver module.
- **P3 (Interface Segregation).** Three separate interfaces because distinct consumer sets have non-overlapping access: `IHealthSnapshot` (read-only — CloudPublisher, ConsoleService), `IHealthReport` (write-only — all producers), `IHealthAdmin` (admin reset — LifecycleController, LLD-D15). Split documented in `components.md` ISP section.
- **P4 (Cross-cutting concern exception).** Logger referenced concretely per the cross-cutting exception. HealthMonitor itself IS one of the two cross-cutting concerns defined by P4; its concrete reference by other application components is the P4 exception.
- **P5 (Bounded resources, no dynamic allocation post-init).** All metric counters in a static aggregate struct; LED state mapped from health state at report time; no heap.
- **P6 (Responsibility traces to requirements).** Every metric counter traces to a specific REQ-CC-* monitoring requirement; `reset_metrics()` traces to REQ-CC-090.
- **P8 (Total error propagation, no silent failures).** `health_monitor_err_t` on init and admin functions; report calls return void (best-effort — health events must not create cascading error chains).
- **P9 (BARR-C coding standard).** Metric counters `uint32_t`; health-event codes `uint8_t` enum; no floating-point.
- **P10 (Naming conventions).** Prefix `health_monitor_`; interfaces `IHealthSnapshot` -> `ihealth_snapshot_t`, `IHealthReport` -> `ihealth_report_t`, `IHealthAdmin` -> `ihealth_admin_t`; errors `HEALTH_MONITOR_ERR_*`.


## 8. Board differences

| Aspect | FD | GW |
|--------|----|----|
| GW-only snapshot fields | Absent | MQTT stats, WiFi RSSI, buffer occupancy, NTP sync info, Modbus master stats, cloud_connected |
| IHealthReport consumers | ConfigStore, TimeProvider, SensorService, AlarmService | + NtpClient, CircularFlashLog, ModbusPoller, CloudPublisher, MqttClient |
| LED count | 4 | 2 |
| Health publish interval | No cloud; snapshot served locally | 600 s (GW health topic) |
| Polling caller | ModbusRegisterMap or LifecycleTask (HM-O1) | CloudPublisher (600 s tick) |

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

### SD trace

| SD | Component role | Key function |
|---|---|---|
| SD-03 | SD-03b: `CloudPublisher` calls `health_monitor_get_snapshot()` to obtain the current health snapshot for the 600 s periodic health telemetry publish | `health_monitor_get_snapshot()` |

---

## 6. Error and fault behaviour

All public functions return `health_monitor_err_t` or `health_admin_err_t`;
callers must not ignore non-OK returns.  HealthMonitor itself never retries —
`push_event()` and counter updates are fire-and-forget.

### health_monitor_err_t

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `HEALTH_MONITOR_ERR_NOT_INIT` | Function called before `health_monitor_init()` | Return error; no state change | Non-OK return | No retry — programming error | Caller logs at ERROR via ILogger |
| `HEALTH_MONITOR_ERR_NULL_ARG` | Null pointer argument | Return error | Non-OK return | No retry — programming error | Caller logs at ERROR via ILogger |

### health_admin_err_t

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `HEALTH_ADMIN_ERR_NOT_INIT` | `health_monitor_reset_stats()` or admin function called before init | Return error; no state change | Non-OK return | No retry — programming error | Caller logs at ERROR via ILogger |


## 7. Unit-test plan

```c
#ifdef UNIT_TEST
#define led_driver_set(id, state)  stub_led_set(id, state)
#endif
```

Minimum test cases:
- `health_monitor_init()` → snapshot zeroed; mutex created.
- `push_event(HEALTH_EVENT_CONFIG_WRITE_FAIL, 0)` → `snapshot.config_write_failed == true`.
- `push_event(HEALTH_EVENT_ALARM_RAISED, SENSOR_ID_TEMPERATURE)` → orange LED set; `alarm_raise_count` incremented.
- `push_event(HEALTH_EVENT_FAULT, 0)` → red LED set; overrides previous state.
- `push_event(HEALTH_EVENT_ALARM_RAISED, ...)` after `HEALTH_EVENT_FAULT` → LED stays red.
- `health_monitor_update_modbus_slave_stats(&stats)` → snapshot modbus fields updated.
- `health_monitor_get_snapshot()` → returns copy; subsequent `push_event()` does not affect the returned copy.
- Concurrent push + get from two threads (host-side pthreads) → no torn reads.

---

## 8. Open items

| ID | Item | Resolution path | Status |
|--------|------|-----------------|--------|
| HM-O1 | FD health-poll caller — stack watermarks on the FD must be polled somewhere. Options: (a) LifecycleTask on a slow timer (10 min), (b) ModbusRegisterMap on each Modbus health register access. Decide at FD integration — does not affect the HealthMonitor API. | Decide health-poll caller at FD integration — LifecycleTask or ModbusRegisterMap | Open |
| HM-O2 | GW LED mapping — the STM32L475 has only LD3 and LD4 (green and blue per UM2153). Orange and red patterns from the FD design cannot be directly replicated. Decide whether to use LD3/LD4 blink codes (e.g., alternating blink = alarm) or omit the alarm LED pattern on the GW. | Decide GW LED blink-code mapping at integration; document in HealthMonitor code | Open |
| HM-O3 | `device_health_snapshot_t` size — estimate ~200 bytes. Verify with `sizeof()` at coding time; add a `static_assert` to catch unexpected growth. | Add static_assert for snapshot size at implementation | Open |
