# LLD Companion — SensorService · AlarmService

**Layer:** Application  
**Boards:** Field Device (FD) · Gateway (GW)  
**Version:** 1.1  
**Date:** October 2026  
**Status:** Pass H — Implementation ready · Gateway implemented (host-tested; hardware not run)

**SensorService provides:** `ISensorService`  
**AlarmService provides:** `IAlarmService`  
**SensorService consumes (FD):** `IBarometer`, `IHumidityTemp`, `ITimeProvider`, `IConfigProvider`, `IHealthReport`, `ILogger`  
**SensorService consumes (GW):** `IBarometer`, `IHumidityTemp`, `IMagnetometer`, `IImu`, `ITimeProvider`, `IConfigProvider`, `IHealthReport`, `ILogger`  
**AlarmService consumes:** `ISensorService`, `IConfigProvider`, `ILogger`  
**SRS traces:** REQ-SA-000–SA-171; REQ-AM-000–AM-040  
**HLD ref:** `components.md` §Application — SensorService, AlarmService; `hld.md` §5.3; `sequence-diagrams.md` SD-01; `task-breakdown.md` §4.2 (FD), §5.2 (GW)

**HLD anchor:** SensorService + AlarmService in `components.md` (FD + GW application layer)

---

## §1 Purpose and scope

This companion covers the SensorService and AlarmService modules of the
application layer.  Both modules exist on both boards (FD and GW) and
share the same source code, with board-specific conditional compilation
for the sensor inventory.

**SensorService** owns the periodic sensor acquisition cycle: it calls the
driver interfaces, timestamps readings, validates ranges, applies IIR
low-pass filtering, and publishes the processed snapshot for downstream
consumers.  Access is both pull-based (`sensor_service_get_snapshot()` —
P7) and push-based (subscriber callbacks for low-latency notification).

**AlarmService** is co-hosted in `SensorTask` (SS-D1).  Its evaluation
runs as a SensorService subscriber callback — when SensorService completes
a cycle and notifies subscribers, AlarmService's callback fires in the
same task context.  A separate companion would split the description of
one execution flow across two documents.

### §1.1 Sensor inventory by board

| Sensor | Physical device | FD | GW | Driver |
|--------|-----------------|----|----|--------|
| Temperature | HTS221 | Simulated | Real | `HumidityTempDriver` |
| Humidity | HTS221 | Simulated | Real | `HumidityTempDriver` |
| Pressure | LPS22HB | Simulated | Real | `BarometerDriver` |
| Accelerometer (3-axis) | LSM6DSL | — | Real | `ImuDriver` |
| Gyroscope (3-axis) | LSM6DSL | — | Real | `ImuDriver` |
| Magnetometer (3-axis) | LIS3MDL | — | Real | `MagnetometerDriver` |

FD sensors are simulated behind their driver abstraction (Vision §5.1.1).
SensorService calls the same driver interface on both boards — the
simulation detail is invisible above the driver layer.

---

## §2 Sources and consumed interfaces

### §2.1 Sources

| Source | What this companion takes from it |
|--------|----------------------------------|
| `components.md` | SensorService and AlarmService responsibility, layer position |
| `hld.md` §5.3 | Data-flow overview: sensor → processing → alarm → consumers |
| `sequence-diagrams.md` SD-01 | Sensor acquisition cycle flow |
| `task-breakdown.md` §4.2, §5.2 | SensorTask priority, stack, WCET budgets |
| `SRS.md` REQ-SA-000–171 | Sensor acquisition requirements |
| `SRS.md` REQ-AM-000–040 | Alarm management requirements |
| GW Sensor Drivers Group A LLD | `IBarometer`, `IHumidityTemp` vtable signatures |
| GW Sensor Drivers Group B LLD | `IMagnetometer`, `IImu` vtable signatures |
| I2cDriver LLD | LLD-D10 vtable singleton pattern |
| `modbus-register-map.md` | Register layout consumed by ModbusRegisterMap |

### §2.2 Consumed interface signatures

**Driver interfaces — FD + GW:**

```c
/* ibarometer_t (from barometer_driver.h — GW Sensor Drivers Group A LLD) */
typedef struct {
    barometer_err_t (*init)(void);
    barometer_err_t (*read)(barometer_data_t *out);
} ibarometer_t;
extern const ibarometer_t * const barometer_driver;

/* ihumidity_temp_t (from humidity_temp_driver.h — GW Sensor Drivers Group A LLD) */
typedef struct {
    humidity_temp_err_t (*init)(void);
    humidity_temp_err_t (*read)(humidity_temp_data_t *out);
} ihumidity_temp_t;
extern const ihumidity_temp_t * const humidity_temp_driver;
```

**Driver interfaces — GW only:**

```c
/* imagnetometer_t (from magnetometer_driver.h — GW Sensor Drivers Group B LLD) */
typedef struct {
    magnetometer_err_t (*init)(void);
    magnetometer_err_t (*attach_drdy_callback)(
        magnetometer_drdy_cb_t cb, void *ctx);
    magnetometer_err_t (*read)(magnetometer_data_t *out);
} imagnetometer_t;
extern const imagnetometer_t * const magnetometer_driver;

/* iimu_t (from imu_driver.h — GW Sensor Drivers Group B LLD) */
typedef struct {
    imu_err_t (*init)(void);
    imu_err_t (*attach_int1_callback)(imu_int1_cb_t cb, void *ctx);
    imu_err_t (*read)(imu_data_t *out);
} iimu_t;
extern const iimu_t * const imu_driver;
```

**Middleware interfaces (companions pending):**

```c
/* ITimeProvider — provides monotonic and wall-clock timestamps */
time_provider_ts_t time_provider_get(void);

/* IConfigProvider — provides key-value configuration access */
config_err_t config_get_float(const char *key, float *out);
config_err_t config_get_u32(const char *key, uint32_t *out);

/* IHealthReport — pushes health events to HealthMonitor */
void health_report_event(health_event_t event);

/* ILogger — structured logging */
void logger_log(log_level_t level, const char *tag, const char *fmt, ...);
```

> Middleware interface signatures are provisional.  Final signatures will
> be confirmed when those companions are written.

---

## §3 Traceability matrix

| Requirement | API function(s) | Verified by |
|-------------|-----------------|-------------|
| REQ-SA-010 | `sensor_service_init()` — reads polling interval from config | TC-SS-001, TC-SS-002 |
| REQ-SA-040 | `sensor_service_init()` — logs driver init failures, continues | TC-SS-002 |
| REQ-SA-050 | `sensor_service_init()` — falls back to defaults if config absent | TC-SS-001 |
| REQ-SA-060 | `sensor_service_init()` — marks permanently failed sensors | TC-SS-002 |
| REQ-SA-080 | `sensor_service_run_cycle()` — logs sensor read errors | TC-SS-004 |
| REQ-SA-0E1 | `sensor_service_run_cycle()` — sets `reading.valid = false` on error | TC-SS-004 |
| REQ-SA-100 | `sensor_service_run_cycle()` — timestamps each reading | TC-SS-003 |
| REQ-SA-120 | `sensor_service_run_cycle()` — range validation | TC-SS-005 |
| REQ-SA-130 | `sensor_service_run_cycle()` — value clamping | TC-SS-006 |
| REQ-SA-140 | `sensor_service_run_cycle()` — IIR low-pass filter | TC-SS-007, TC-SS-008 |
| REQ-SA-170 | `sensor_service_read_on_demand()` — on-demand acquisition | TC-SS-012 |
| REQ-AM-010 | `alarm_service_init()` — loads thresholds from config | TC-AS-001, TC-AS-002 |
| REQ-AM-020 | `alarm_service_evaluate()` — threshold comparison with hysteresis | TC-AS-003–TC-AS-009 |
| REQ-AM-030 | `alarm_service_subscribe()` — alarm event notification | TC-AS-010 |
| REQ-AM-040 | Alarm notification includes sensor ID, event, value, threshold, timestamp | TC-AS-010 |

---

## §4 Public API — provided interfaces

### §4.1 Data types

```c
/* sensor_service.h */

typedef enum {
    SENSOR_ID_TEMPERATURE   = 0,
    SENSOR_ID_HUMIDITY      = 1,
    SENSOR_ID_PRESSURE      = 2,
    SENSOR_ID_ACCEL_X       = 3,   /* GW only */
    SENSOR_ID_ACCEL_Y       = 4,   /* GW only */
    SENSOR_ID_ACCEL_Z       = 5,   /* GW only */
    SENSOR_ID_GYRO_X        = 6,   /* GW only */
    SENSOR_ID_GYRO_Y        = 7,   /* GW only */
    SENSOR_ID_GYRO_Z        = 8,   /* GW only */
    SENSOR_ID_MAG_X         = 9,   /* GW only */
    SENSOR_ID_MAG_Y         = 10,  /* GW only */
    SENSOR_ID_MAG_Z         = 11,  /* GW only */
    SENSOR_ID_COUNT         = 12,
} sensor_id_t;
```

`SENSOR_ID_COUNT` is 12 on both boards (SS-D8).  On the FD, sensors 3–11
are marked as permanently failed at init; their readings carry
`valid = false`.  This keeps the Modbus register layout consistent across
boards.

```c
typedef struct {
    int32_t              value;      /* fixed-point; units per sensor_id_t (below) */
    bool                 valid;      /* false if driver returned error (REQ-SA-0E1) */
    time_provider_ts_t   timestamp;  /* from TimeProvider (REQ-SA-100) */
} sensor_reading_t;
```

`value` is a fixed-point `int32_t` (SS-D2, corrected in v1.1).  The units are:

| Sensor ID | Unit | Example |
|-----------|------|---------|
| `TEMPERATURE` | 0.01 °C | 2350 = 23.50 °C |
| `HUMIDITY` | 0.01 %RH | 5500 = 55.00 %RH |
| `PRESSURE` | 0.1 hPa | 10132 = 1013.2 hPa |
| `ACCEL_x` (GW) | mg | 1000 = 1 g |
| `GYRO_x` (GW) | mdps | 8750 = 8.75 dps |
| `MAG_x` (GW) | mgauss | 1000 = 1 gauss |

The scalar units are the drivers' native units and the FD implementation's.  The
IMU and magnetometer drivers return raw LSB (GPB-D4), so SensorService converts
them with the documented sensitivities (SS-D10).  The IMU and magnetometer units
are the ones already on the CloudPublisher wire (`accel_*_mg`, `gyro_*_mdps`,
`mag_*_mgauss`), so no conversion is needed at that seam.

```c
typedef struct {
    sensor_reading_t readings[SENSOR_ID_COUNT];
    uint32_t         cycle_count;    /* increments each acquisition cycle */
} sensor_snapshot_t;

typedef enum {
    SENSOR_SERVICE_ERR_OK        = 0,
    SENSOR_SERVICE_ERR_NOT_INIT  = 1,
    SENSOR_SERVICE_ERR_NULL_ARG  = 2,
    SENSOR_SERVICE_ERR_NO_SUB    = 3,  /* subscriber table full */
} sensor_service_err_t;
```

```c
/* alarm_service.h */

typedef enum {
    ALARM_STATE_CLEAR        = 0,
    ALARM_STATE_ACTIVE_HIGH  = 1,
    ALARM_STATE_ACTIVE_LOW   = 2,
} alarm_state_t;

typedef enum {
    ALARM_EVENT_RAISED_HIGH  = 0,
    ALARM_EVENT_RAISED_LOW   = 1,
    ALARM_EVENT_CLEARED      = 2,
} alarm_event_t;

typedef enum {
    ALARM_SERVICE_ERR_OK        = 0,
    ALARM_SERVICE_ERR_NOT_INIT  = 1,
    ALARM_SERVICE_ERR_NULL_ARG  = 2,
    ALARM_SERVICE_ERR_NO_SUB    = 3,
} alarm_service_err_t;
```

### §4.2 ISensorService — provided interface

```c
/**
 * @brief  Initialise SensorService.
 *
 * Reads polling interval and IIR filter alpha from IConfigProvider;
 * falls back to compile-time defaults if config is absent (REQ-SA-010,
 * SA-050).  Attempts to initialise each driver via its vtable init()
 * member; logs failures and marks the driver as permanently failed so
 * every subsequent cycle skips it (REQ-SA-040, SA-060).
 *
 * On the FD, sensors 3-11 (GW-only) are unconditionally marked as
 * permanently failed.
 *
 * @return SENSOR_SERVICE_ERR_OK on success.
 * @pre    Called before the FreeRTOS scheduler starts.
 * @post   SensorService is ready for run_cycle() calls.
 */
sensor_service_err_t sensor_service_init(void);

/**
 * @brief  Run one full acquisition cycle.
 *
 * Called from SensorTask on every 100 ms periodic tick.  For each
 * non-failed sensor: reads the driver, timestamps, range-validates,
 * clamps, and applies the IIR low-pass filter (see 5.1 processing
 * pipeline).  Updates the internal snapshot under a critical section,
 * then calls all registered new-reading callbacks in registration order.
 *
 * WCET budget: ~3 ms (FD) / ~5 ms (GW) at 80 MHz (task-breakdown.md
 * section 8.1).
 *
 * @return SENSOR_SERVICE_ERR_OK on success; ERR_NOT_INIT if called
 *         before sensor_service_init().
 * @pre    SensorService initialised.
 * @note   Task-context only.  Not ISR-safe.
 */
sensor_service_err_t sensor_service_run_cycle(void);

/**
 * @brief  Get the latest sensor snapshot (pull model — P7).
 *
 * Called by LcdUi (FD), ModbusRegisterMap (FD), and CloudPublisher (GW)
 * to read current data.  Thread-safe — snapshot is copied under a
 * critical section.
 *
 * @param[out] snap  Destination for the latest snapshot copy.
 * @return SENSOR_SERVICE_ERR_OK on success; ERR_NULL_ARG if snap is NULL;
 *         ERR_NOT_INIT if called before init.
 */
sensor_service_err_t sensor_service_get_snapshot(sensor_snapshot_t *snap);

/**
 * @brief  Register a new-reading callback (push model).
 *
 * Callback fires in SensorTask context immediately after each cycle
 * completes.  Keep callback execution time minimal — it delays the
 * next cycle.
 *
 * Maximum subscribers: SENSOR_MAX_SUBSCRIBERS (compile-time, default 4).
 *
 * @param  cb  Function pointer; must remain valid for the system lifetime.
 * @return SENSOR_SERVICE_ERR_OK on success; ERR_NO_SUB if table full;
 *         ERR_NULL_ARG if cb is NULL.
 */
sensor_service_err_t sensor_service_subscribe(
    void (*cb)(const sensor_snapshot_t *snap));

/**
 * @brief  Trigger an on-demand sensor read (REQ-SA-170).
 *
 * Runs one additional acquisition cycle outside the periodic timer.
 * The result appears in the snapshot and is pushed to subscribers.
 * Called by ModbusRegisterMap (FD) on FC04 remote read request.
 *
 * Executes synchronously in the calling task's context — caller must
 * tolerate ~3 ms (FD) / ~5 ms (GW) blocking.
 *
 * @return SENSOR_SERVICE_ERR_OK on success.
 * @note   Task-context only.  Not ISR-safe.
 */
sensor_service_err_t sensor_service_read_on_demand(void);

/**
 * @brief  Check readiness — all non-failed sensors produced a valid reading.
 *
 * Called by LifecycleController during the Init.SelfChecking phase
 * (SD-00b) to verify sensor initialisations succeeded.
 *
 * @return true if all non-failed sensors have valid = true in the latest
 *         snapshot; false otherwise.
 */
bool sensor_service_is_ready(void);
```

> **Gateway deviation (SS-D12):** the Gateway build is
> `sensor_service_init(health_monitor_handle_t health)`.  The Gateway HealthMonitor
> is handle-based (`health_monitor_push_event(handle, event, param)`), not the
> provisional `health_report_event()` of §2.2, so the handle is injected.  The
> FD build keeps `init(void)`.

**Vtable — ISensorService (LLD-D10, SS-D5):**

```c
typedef struct {
    sensor_service_err_t (*init)(void);
    sensor_service_err_t (*run_cycle)(void);
    sensor_service_err_t (*get_snapshot)(sensor_snapshot_t *snap);
    sensor_service_err_t (*subscribe)(void (*cb)(const sensor_snapshot_t *));
    sensor_service_err_t (*read_on_demand)(void);
    bool                 (*is_ready)(void);
} isensor_service_t;

/** Singleton pointer — SensorService vtable (FD + GW). */
extern const isensor_service_t * const sensor_service;
```

**Reset-for-test (project convention):**

```c
#ifdef TEST
/**
 * @brief  Reset SensorService static state for unit-test isolation.
 *
 * Zeroes the internal SensorServiceState struct, including the snapshot,
 * filter state, subscriber list, and driver-failed flags.  Does NOT
 * re-initialise drivers.
 */
void sensor_service_reset_for_test(void);
#endif
```

### §4.3 IAlarmService — provided interface

```c
/**
 * @brief  Initialise AlarmService.
 *
 * Reads alarm thresholds and hysteresis values from IConfigProvider for
 * each sensor.  Validates hysteresis: must be positive and less than
 * (threshold_high - threshold_low) / 2; rejects and falls back to
 * defaults on violation.  Initialises all alarm states to CLEAR.
 * Registers alarm_service_evaluate() as a SensorService subscriber.
 *
 * @return ALARM_SERVICE_ERR_OK on success.
 * @pre    SensorService already initialised (subscriber registration required).
 */
alarm_service_err_t alarm_service_init(void);

/**
 * @brief  Get the current alarm state for one sensor.
 *
 * Thread-safe — alarm_state_t is a single enum word, read atomically
 * on Cortex-M4.
 *
 * @param      sensor     The sensor to query.
 * @param[out] state_out  Receives the current alarm state.
 * @return ALARM_SERVICE_ERR_OK on success; ERR_NULL_ARG if state_out is
 *         NULL; ERR_NOT_INIT if called before init.
 */
alarm_service_err_t alarm_service_get_state(sensor_id_t   sensor,
                                             alarm_state_t *state_out);

/**
 * @brief  Get alarm states for all sensors at once.
 *
 * Copies all SENSOR_ID_COUNT alarm states into the caller's array.
 *
 * @param[out] states  Array of SENSOR_ID_COUNT alarm_state_t values.
 * @return ALARM_SERVICE_ERR_OK on success; ERR_NULL_ARG if states is NULL.
 */
alarm_service_err_t alarm_service_get_all_states(
    alarm_state_t states[SENSOR_ID_COUNT]);

/**
 * @brief  Register an alarm event callback.
 *
 * Fired in SensorTask context when an alarm is raised or cleared.
 * Consumers: LcdUi (FD), ModbusRegisterMap (FD — exposes alarm register
 * bits), CloudPublisher (GW — publishes alarm events).
 *
 * Maximum subscribers: ALARM_MAX_SUBSCRIBERS (compile-time, default 4).
 *
 * @param  cb  Callback receiving sensor ID, event type, and triggering reading.
 * @return ALARM_SERVICE_ERR_OK on success; ERR_NO_SUB if table full.
 */
alarm_service_err_t alarm_service_subscribe(
    void (*cb)(sensor_id_t       sensor,
               alarm_event_t     event,
               const sensor_reading_t *reading));

/**
 * @brief  Force-clear all alarm flags (SS-D3).
 *
 * Implements CMD_ACK_ALARM register-map command semantics.  Clears all
 * per-sensor alarm flags regardless of current threshold state.
 *
 * Alarm model is non-latched with manual clear: flags reflect the current
 * evaluation state and are set while the threshold is breached, cleared
 * automatically when the condition resolves.  ack_all() provides a
 * manual force-clear path triggered by CMD_ACK_ALARM.  If a condition is
 * still active when ack_all() runs, the flag clears momentarily and
 * re-raises on the next evaluation cycle.
 *
 * Thread-safe — executes in ModbusSlaveTask context; alarm_state writes
 * are atomic word operations on Cortex-M4.
 *
 * @return ALARM_SERVICE_ERR_OK on success.
 */
alarm_service_err_t alarm_service_ack_all(void);
```

**Vtable — IAlarmService (LLD-D10, LLD-D14, SS-D5):**

```c
typedef struct {
    alarm_service_err_t (*init)(void);
    alarm_service_err_t (*get_state)(sensor_id_t sensor, alarm_state_t *state_out);
    alarm_service_err_t (*get_all_states)(alarm_state_t states[SENSOR_ID_COUNT]);
    alarm_service_err_t (*subscribe)(void (*cb)(sensor_id_t,
                                                alarm_event_t,
                                                const sensor_reading_t *));
    alarm_service_err_t (*ack_all)(void);
} ialarm_service_t;

/** Singleton pointer — AlarmService vtable (FD + GW). */
extern const ialarm_service_t * const alarm_service;
```

**Reset-for-test (project convention):**

```c
#ifdef TEST
/**
 * @brief  Reset AlarmService static state for unit-test isolation.
 *
 * Zeroes the internal AlarmServiceState struct, including alarm states,
 * thresholds, and subscriber list.
 */
void alarm_service_reset_for_test(void);
#endif
```

---

## §5 Internal design

### §5.1 Processing pipeline (SD-01, per sensor per cycle)

Executed by `sensor_service_run_cycle()` for each enabled sensor:

```
1. Call driver read function via vtable (e.g. barometer_driver->read(&data)).
   On error: mark reading.valid = false; log via ILogger (REQ-SA-080, SA-0E1);
             push HEALTH_EVENT_SENSOR_FAIL via IHealthReport if first
             failure this cycle; skip steps 2-5 for this sensor.

2. Stamp: reading.timestamp = time_provider_get()   [REQ-SA-100]

3. Range validate: if value < range_min || value > range_max:
       reading.valid = false  [REQ-SA-120]
       (clamped copy still passes through filter — see step 4)

4. Clamp: value = clamp(value, range_min, range_max)  [REQ-SA-130]

5. Low-pass IIR filter (integer, alpha = num / den):   [REQ-SA-140]
       first accepted sample: prev_filtered[id] = value   (SS-D13)
       sum      = num * value + (den - num) * prev_filtered[id]
       filtered = (sum + sign(sum) * den / 2) / den       (half away from zero)
       prev_filtered[id] = filtered
       reading.value = filtered
```

Two details the v1.0 float form hid (SS-D13): the filter state is **seeded with the
first accepted sample** instead of starting at zero (a pressure channel would
otherwise climb from 0 hPa and raise a false low-pressure alarm on every boot), and
the rounding bias follows the sign of the sum, because C division truncates toward
zero and a plain `+ den / 2` rounds negative values the wrong way.

The filter is applied to the clamped value, not the raw value (SS-D4).
This prevents an out-of-range spike from corrupting the filter state with
an extreme value.

**Alpha parameter (REQ-SA-140 — see SS-O1):** provisionally 1/10 = 0.1 (heavy
smoothing — time constant approximately 1 s at 10 Hz).  The Gateway has no
IConfigProvider yet, so the compile-time default is always used (SS-O6).

After all sensors are processed, `run_cycle()` updates the internal
snapshot under a `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` guard
(brief — just a `memcpy` of `sensor_snapshot_t`), then calls all
subscribers in registration order.

### §5.2 SensorService internal state

```c
/* sensor_service.c (Gateway) */

#define SENSOR_MAX_SUBSCRIBERS  4U

static struct {
    bool                    initialised;
    health_monitor_handle_t health;
    sensor_snapshot_t       snapshot;                       /* critical section */
    int32_t                 prev_filtered[SENSOR_ID_COUNT]; /* same units as value */
    bool                    filter_seeded[SENSOR_ID_COUNT]; /* SS-D13 */
    bool                    driver_failed[SENSOR_ID_COUNT]; /* permanent: init failed */
    bool                    fail_reported[SENSOR_ID_COUNT]; /* FAIL sent, not recovered (SS-D14) */
    uint8_t                 alpha_num;
    uint8_t                 alpha_den;
    void (*subscribers[SENSOR_MAX_SUBSCRIBERS])(const sensor_snapshot_t *);
    uint8_t                 subscriber_count;
} s_ss;
```

`TimerHandle_t poll_timer` is deliberately excluded from
`SensorServiceState` — it belongs at the task level (SS-D6).
SensorService is a pure application service that provides `run_cycle()`;
the task wrapper manages scheduling.

### §5.3 Alarm evaluation logic

`alarm_service_evaluate()` is the SensorService subscriber callback.  It
runs in SensorTask context, called after each acquisition cycle.

```c
static void alarm_service_evaluate(const sensor_snapshot_t *snap)
{
    for (uint8_t i = 0U; i < (uint8_t)SENSOR_ID_COUNT; i++)
    {
        const sensor_reading_t *r = &snap->readings[i];

        if (!s_as.thresholds[i].enabled || !r->valid)
        {
            continue;   /* no thresholds (SS-D15), or invalid reading */
        }

        int32_t threshold_high = s_as.thresholds[i].high;
        int32_t threshold_low  = s_as.thresholds[i].low;
        int32_t hysteresis     = s_as.thresholds[i].hysteresis;

        switch (s_as.alarm_state[i])
        {
        case ALARM_STATE_CLEAR:
            if (r->value > threshold_high)
            {
                set_alarm(i, ALARM_STATE_ACTIVE_HIGH,
                          ALARM_EVENT_RAISED_HIGH, r);
            }
            else if (r->value < threshold_low)
            {
                set_alarm(i, ALARM_STATE_ACTIVE_LOW,
                          ALARM_EVENT_RAISED_LOW, r);
            }
            break;

        case ALARM_STATE_ACTIVE_HIGH:
            if (r->value < (threshold_high - hysteresis))
            {
                set_alarm(i, ALARM_STATE_CLEAR,
                          ALARM_EVENT_CLEARED, r);
            }
            break;

        case ALARM_STATE_ACTIVE_LOW:
            if (r->value > (threshold_low + hysteresis))
            {
                set_alarm(i, ALARM_STATE_CLEAR,
                          ALARM_EVENT_CLEARED, r);
            }
            break;

        default:
            /* Defensive — should never reach here */
            break;
        }
    }
}
```

**Why `enabled` (SS-D15):** on the FD the IMU and magnetometer channels are always
invalid, so `if (!r->valid) continue` was enough.  On the Gateway those channels are
valid, and an all-zero threshold entry would raise a high alarm on any positive
reading.  Only temperature, humidity and pressure are enabled (SS-O3).

`set_alarm()` updates `s_as.alarm_state[i]` and calls all registered
alarm subscribers with the event and the triggering reading.

**Hysteresis constraint:** must be positive and smaller than
`(threshold_high - threshold_low) / 2`.  Validated at
`alarm_service_init()` when loading from ConfigProvider; reject and use
defaults on violation.  This prevents a misconfigured hysteresis from
making alarms impossible to clear.

### §5.4 AlarmService internal state

```c
/* alarm_service.c */

#define ALARM_MAX_SUBSCRIBERS  4U

typedef struct {
    bool    enabled;     /* false: never evaluated - IMU / magnetometer (SS-D15) */
    int32_t high;
    int32_t low;
    int32_t hysteresis;
} alarm_threshold_t;

typedef struct {
    bool              initialised;
    alarm_state_t     alarm_state[SENSOR_ID_COUNT];
    alarm_threshold_t thresholds[SENSOR_ID_COUNT];
    void (*subscribers[ALARM_MAX_SUBSCRIBERS])(sensor_id_t,
                                                alarm_event_t,
                                                const sensor_reading_t *);
    uint8_t           subscriber_count;
} AlarmServiceState;

static AlarmServiceState s_as;
```

---

## §6 Synchronisation

| Operation | Caller task(s) | Protection |
|-----------|---------------|------------|
| `sensor_service_run_cycle()` | SensorTask only | No protection needed — single writer |
| Snapshot write (inside run_cycle) | SensorTask only | `taskENTER_CRITICAL` for the `memcpy` |
| `sensor_service_get_snapshot()` | Any task (LcdUi, Modbus, Cloud) | `taskENTER_CRITICAL` for the `memcpy` |
| `sensor_service_subscribe()` | Init context only | No protection — called before scheduler |
| `alarm_service_evaluate()` | SensorTask only (subscriber callback) | No protection needed — single caller |
| `alarm_service_get_state()` | Any task | Single-word read — atomic on Cortex-M4 |
| `alarm_service_get_all_states()` | Any task | Array copy — not atomic; momentary inconsistency between sensors is acceptable |
| `alarm_service_ack_all()` | ModbusSlaveTask | Atomic word writes on Cortex-M4 |
| Alarm subscriber callbacks | SensorTask only (fired from evaluate) | No protection needed |

Critical section duration for the snapshot copy is bounded: a single
`memcpy` of `sizeof(sensor_snapshot_t)` (~200 bytes).  Maximum interrupt
latency: < 10 us at 80 MHz.

---

## §7 Init ordering and lifecycle

### §7.1 Init sequence

```
1. Driver init
   barometer_driver->init(), humidity_temp_driver->init()
   GW only: magnetometer_driver->init(), imu_driver->init()
   — called by sensor_service_init() via driver vtables.

2. sensor_service_init()
   — reads config, inits drivers, marks failed drivers, zeroes filter state.

3. alarm_service_init()
   — reads alarm thresholds from config, registers as SensorService subscriber.

4. SensorTask timer creation
   — 100 ms software timer, callback sends task notification.
```

**Gateway (SS-D9):** steps 1–3 execute in `sensor_task_create()` before the scheduler
starts; step 4 runs when `SensorTask` first gets the CPU.  **FD:** unchanged from v1.0
(`vSensorTask` initialises after the LifecycleController start-gate).

### §7.2 Task design

Implemented in `sensor_task.c` (`sensor_task_create(health)`), priority 5, 384 words,
static TCB and stack (task-breakdown §5.2).

- `sensor_task_create()` runs **before the scheduler starts** and calls
  `sensor_service_init(health)` then `alarm_service_init()`, because the Group A and
  Group B driver headers require `*_init()` before the scheduler (SS-D9).  This
  replaces the v1.0 text in which `vSensorTask` initialised both after a LifecycleController
  start-gate; the Gateway has no LifecycleController yet.
- The task, on its first run, creates the 100 ms static `xTimer` and starts it
  (SS-D6: the timer lives in the task wrapper, not in `SensorServiceState`).
- The timer callback only calls `xTaskNotifyGive()`; the task loops on
  `ulTaskNotifyTake()` and calls `sensor_service_run_cycle()`.

---


## §8 Sequence integration and SD trace

| SD | Component role | Key function | Notes |
|----|----------------|--------------|-------|
| SD-00b | LifecycleController calls readiness check during Init.SelfChecking phase | `sensor_service_is_ready()` | Confirms all non-failed sensors produced a valid reading |
| SD-01 | SensorService drives the acquisition cycle: calls `barometer_driver->read()`, `humidity_temp_driver->read()`, and on GW `imu_driver->read()`, `magnetometer_driver->read()`; timestamps, validates, filters; notifies AlarmService via subscriber callback | `sensor_service_run_cycle()` | — |
| SD-03a | CloudPublisher (GW) retrieves current snapshot for the 60 s telemetry payload | `sensor_service_get_snapshot()` | Pull model (P7) |
| SD-05 | AlarmService detects threshold breach; alarm subscriber notifies ModbusRegisterMap (FD, sets alarm register bits) and CloudPublisher (GW, publishes alarm event) | `alarm_service_evaluate()`, `alarm_service_ack_all()` | CMD_ACK_ALARM clears all flags |
| SD-06d | OTA: UpdateService requests sensor quiesce before flash step | *Deferred* (SS-D7) | `pause()` / `resume()` not yet in API; designed with OTA companion |

**Corrections from Draft v0.1:**
- SD-00b: `sensor_service_probe()` renamed to `sensor_service_is_ready()` (matches actual API).
- SD-01: Driver calls use vtable notation (`barometer_driver->read()`) not bare function names.
- SD-03a: `sensor_service_get_latest()` renamed to `sensor_service_get_snapshot()` (matches actual API).
- SD-05: `alarm_service_acknowledge()` renamed to `alarm_service_ack_all()` (matches actual API).

---

## §9 Error and fault behaviour

All public functions return `sensor_service_err_t` or
`alarm_service_err_t`.  Callers must not ignore non-OK returns.  Neither
SensorService nor AlarmService retries internally — CloudPublisher and
HealthMonitor handle event-level retry.

### §9.1 SensorService errors

| Error | Cause | Local behaviour | Retry | Observability |
|-------|-------|-----------------|-------|---------------|
| `ERR_NOT_INIT` | Function called before init | Return error | No — programming error | Caller logs ERROR |
| `ERR_NULL_ARG` | NULL pointer argument | Return error | No — programming error | Caller logs ERROR |
| `ERR_NO_SUB` | Subscriber table full | Return error; subscriber not added | No — increase `SENSOR_MAX_SUBSCRIBERS` | Caller logs ERROR |

### §9.2 AlarmService errors

| Error | Cause | Local behaviour | Retry | Observability |
|-------|-------|-----------------|-------|---------------|
| `ERR_NOT_INIT` | Function called before init | Return error | No — programming error | Caller logs ERROR |
| `ERR_NULL_ARG` | NULL pointer argument | Return error | No — programming error | Caller logs ERROR |
| `ERR_NO_SUB` | Subscriber table full | Return error; subscriber not added | No — increase `ALARM_MAX_SUBSCRIBERS` | Caller logs ERROR |

### §9.3 Driver-level error handling

When a driver `read()` returns an error during `run_cycle()`:

1. `reading.valid` set to `false` for that sensor.
2. Error logged via ILogger at WARNING level (transient) or ERROR (permanent).
3. `HEALTH_EVENT_SENSOR_FAIL` pushed to IHealthReport if this is the first
   failure in the current cycle.
4. Processing pipeline steps 2–5 skipped for that sensor.
5. If the driver has failed on every cycle since init, it may be marked as
   permanently failed (`driver_failed[sensor_id] = true`).  Subsequent
   cycles skip the driver entirely.

---

## §10 Board differences

| Aspect | FD | GW |
|--------|----|----|
| Sensor count | 3 (temp, humidity, pressure) | 12 readings across 6 sensor types |
| Drivers called | `HumidityTempDriver`, `BarometerDriver` | + `ImuDriver`, `MagnetometerDriver` |
| IMU/Mag data | Not present (IDs 3–11 always invalid) | 3 accel + 3 gyro + 3 mag readings |
| Alarm-evaluated sensors | Temp, humidity, pressure | Same 3 scalar sensors; IMU alarms deferred (SS-O3) |
| WCET budget | ~3 ms | ~5 ms (more drivers) |
| Sensor backend | Simulated (Vision §5.1.1) | Real hardware via I2C2 |
| Downstream consumers | LcdUi, ModbusRegisterMap | CloudPublisher, ModbusPoller |

The FD alarm evaluation is identical to the GW for the three scalar
sensors.  IMU/magnetometer alarm evaluation is a GW-only extension
deferred to SS-O3 — per-axis and magnitude-threshold semantics are
non-trivial and require customer spec confirmation.

---

## §11 Memory and sizing budget

### §11.1 Static RAM

| Structure | Size (bytes) | Count | Total |
|-----------|-------------|-------|-------|
| `SensorServiceState` | ~300 | 1 | ~300 |
| `AlarmServiceState` | ~220 | 1 | ~220 |
| `s_sensor_task_handle` | 4 | 1 | 4 |
| `s_poll_timer` | 4 | 1 | 4 |
| **Total static .bss** | | | **~528** |

### §11.2 SensorTask stack

| Board | Estimated stack (words) | Provisioned | Justification |
|-------|------------------------|-------------|---------------|
| FD | ~200 | 256 (1024 bytes) | run_cycle + 3 driver calls + subscriber callbacks |
| GW | ~300 | 384 (1536 bytes) | run_cycle + 6 driver calls + subscriber callbacks |

Verify with `uxTaskGetStackHighWaterMark()` at integration time.

### §11.3 Flash (code size estimate)

| Module | Estimated .text (bytes) |
|--------|------------------------|
| `sensor_service.c` | ~1200 |
| `alarm_service.c` | ~800 |
| `sensor_task.c` | ~200 |
| **Total** | **~2200** |

---

## §12 File layout

```
firmware/gateway/application/sensor_service/
├── sensor_service.h            Public API, ISensorService vtable, data types
├── sensor_service.c            Pipeline, filter, unit conversion, vtable instance
├── sensor_service_internal.h   Driver headers (keeps them out of the unit test's own includes)
├── sensor_task.h               sensor_task_create(), task constants
└── sensor_task.c               SensorTask + 100 ms poll timer

firmware/gateway/application/alarm_service/
├── alarm_service.h             Public API, IAlarmService vtable, alarm types
└── alarm_service.c             Evaluation, thresholds, vtable instance

firmware/field-device/application/{sensor_service,alarm_service}/   unchanged FD copies
```

SS-O5 is closed: each board keeps its own copy under its own `application/` tree, as
every other module does, rather than a shared `firmware/common/` (SS-D16).  The
same-basename modules are tested by `tests/project_gateway.yml` and excluded from
`tests/project.yml`.

---

## §13 Unit-test plan

### §13.1 Test infrastructure

```c
#ifdef TEST
void sensor_service_reset_for_test(void);
void alarm_service_reset_for_test(void);
#endif
```

Test files:

| Test file | Module under test |
|-----------|-------------------|
| `test_sensor_service.c` | `sensor_service.c` |
| `test_alarm_service.c` | `alarm_service.c` |

Driver dependencies are stubbed via link-time substitution: test builds
link against stub implementations of `ibarometer_t`, `ihumidity_temp_t`,
`imagnetometer_t`, `iimu_t`, `ITimeProvider`, `IConfigProvider`, and
`ILogger`.

### §13.2 SensorService test cases

| ID | Scenario | Inputs | Expected |
|----|----------|--------|----------|
| TC-SS-001 | Init succeeds — all drivers available | Stub drivers return OK; config provides alpha = 0.1 | `ERR_OK`; all `driver_failed` flags false |
| TC-SS-002 | Init — driver failure graceful | Stub barometer returns error | `ERR_OK`; `driver_failed[SENSOR_ID_PRESSURE]` = true; other sensors unaffected |
| TC-SS-003 | Run cycle — all drivers succeed | Stub drivers return valid data | All `reading.valid` = true; `cycle_count` incremented |
| TC-SS-004 | Run cycle — driver error | Stub humidity returns error | `reading.valid` = false for temp and humidity; cycle continues |
| TC-SS-005 | Range validation — out-of-range | Temperature = 200.0; range_max = 125.0 | `reading.valid` = false |
| TC-SS-006 | Clamping | Pressure = 1200.0 hPa; range_max = 1100.0 | Value clamped to 1100.0; valid = false (out-of-range) |
| TC-SS-007 | IIR filter — single step | alpha = 0.1, value = 25.0, prev = 20.0 | filtered = 0.1 * 25.0 + 0.9 * 20.0 = 20.5 |
| TC-SS-008 | IIR filter on clamped value | Out-of-range raw = 200.0; clamped = 125.0 | Filter fed 125.0, not 200.0 |
| TC-SS-009 | get_snapshot() returns latest copy | Run one cycle, then get_snapshot | Snapshot matches cycle output |
| TC-SS-010 | subscribe() — table full | Register SENSOR_MAX_SUBSCRIBERS + 1 | Fifth returns ERR_NO_SUB |
| TC-SS-011 | Subscriber callback correctness | Register callback, run cycle | Callback receives pointer to current snapshot |
| TC-SS-012 | read_on_demand() | Call read_on_demand | One acquisition cycle runs; snapshot updated |
| TC-SS-013 | is_ready() — all sensors valid | All non-failed sensors have valid = true | Returns true |
| TC-SS-014 | is_ready() — sensor invalid | One non-failed sensor has valid = false | Returns false |
| TC-SS-015 | NULL argument | `get_snapshot(NULL)` | Returns ERR_NULL_ARG |
| TC-SS-016 | Called before init | `run_cycle()` before `init()` | Returns ERR_NOT_INIT |

### §13.3 AlarmService test cases

| ID | Scenario | Inputs | Expected |
|----|----------|--------|----------|
| TC-AS-001 | Init succeeds | Valid thresholds from config | `ERR_OK`; all states CLEAR |
| TC-AS-002 | Hysteresis validation | hysteresis > (high - low) / 2 | Rejected; defaults used |
| TC-AS-003 | Reading within range | temp = 25.0; high = 40.0; low = 10.0 | State stays CLEAR |
| TC-AS-004 | High alarm raised | temp = 41.0; high = 40.0 | State = ACTIVE_HIGH; RAISED_HIGH event |
| TC-AS-005 | Low alarm raised | temp = 9.0; low = 10.0 | State = ACTIVE_LOW; RAISED_LOW event |
| TC-AS-006 | Hysteresis prevents clear | ACTIVE_HIGH; temp = 38.5; hyst = 2.0 | Still ACTIVE_HIGH (38.5 > 40.0 - 2.0) |
| TC-AS-007 | High alarm clears | ACTIVE_HIGH; temp = 37.0; hyst = 2.0 | State = CLEAR; CLEARED event |
| TC-AS-008 | Low alarm hysteresis | ACTIVE_LOW; temp = 11.5; low = 10.0; hyst = 2.0 | Still ACTIVE_LOW (11.5 < 10.0 + 2.0) |
| TC-AS-009 | Invalid reading ignored | reading.valid = false; state was CLEAR | State unchanged |
| TC-AS-010 | Subscriber notification | Register callback; trigger alarm | Callback receives correct sensor_id, event, reading |
| TC-AS-011 | ack_all() clears flags | Two alarms active | Both set to CLEAR |
| TC-AS-012 | ack_all() re-raise | ack_all(); condition still active; next cycle | Alarm re-raises on next evaluate |
| TC-AS-013 | subscribe() — table full | Register ALARM_MAX_SUBSCRIBERS + 1 | Last returns ERR_NO_SUB |
| TC-AS-014 | NULL argument | `get_state(0, NULL)` | Returns ERR_NULL_ARG |
| TC-AS-015 | Called before init | `get_state()` before `init()` | Returns ERR_NOT_INIT |

---

### §13.4 Gateway implementation (host tests)

`tests/gateway/application/sensor_service/test_sensor_service.c` and
`.../alarm_service/test_alarm_service.c`, run with
`CEEDLING_MAIN_PROJECT_FILE=project_gateway.yml ceedling test:test_sensor_service test:test_alarm_service`.
Every plan row above is implemented except **TC-AS-002** (no `IConfigProvider`, SS-O6).
TC-SS-008 is checked inside TC-SS-005/006/008 (the clamped value reaches the filter).
Gateway-only additions use IDs from 100:

| ID | Scenario |
|----|----------|
| TC-SS-100 | `init(NULL)` rejected |
| TC-SS-101 | vtable members point at the public functions |
| TC-SS-102 | IMU / magnetometer raw LSB converted to mg / mdps / mgauss |
| TC-SS-103 | extreme raw values (`INT16_MIN` / `INT16_MAX`) convert without overflow |
| TC-SS-104 | first sample seeds the filter (no ramp from zero) |
| TC-SS-105 | half-way filter results round away from zero for both signs |
| TC-SS-106 | FAIL then RECOVERED health events, once each |
| TC-SS-107 | both Group B drivers read every cycle even when `NOT_READY` (GPB-O6) |
| TC-SS-108 | `NOT_READY` keeps the previous reading, no fault |
| TC-SS-109 | Group B bus error invalidates all axes and reports once per channel |
| TC-SS-110 | Group A `NOT_READY` (1 Hz sensors) keeps the reading, no fault |
| TC-SS-111 | A 2 mg offset is not lost to rounding (no IIR dead band, SS-D18) |
| TC-SS-112 | Group A read every 10th cycle, Group B every cycle (SS-D19) |
| TC-SS-113 | Group A retried every cycle until its first valid sample (SS-D19) |
| TC-AS-100 | `init` fails when SensorService refuses the subscription |
| TC-AS-101 | `get_state` rejects an out-of-range sensor ID (`ERR_INVALID_ARG`) |
| TC-AS-102 | vtable members point at the public functions |
| TC-AS-103 | humidity and pressure use their own thresholds |
| TC-AS-104 | valid IMU / magnetometer readings never alarm (SS-D15) |
| TC-AS-105 | `get_all_states` reports every sensor |

The unit-test plan above lists a plain `ALARM_SERVICE_ERR_INVALID_ARG` nowhere; the
Gateway adds it (value 4) for a sensor ID outside `0..SENSOR_ID_COUNT-1`.

---

### §13.5 Hardware result (2026-10-15)

`integration-tests/sensor_service`, B-L475E-IOT01A, board flat and still: **TC-HW-SS-001..008 PASS**.
20 cycles in 2 s; T 26.3 degC, H 65.9 %RH, P 1020.5 hPa; accel (-166, -90, -950) mg; gyro rest offset
about (1.9, -3.4, 1.1) dps; magnetic field about 0.97 gauss; no alarms; no HealthMonitor failures.
Motion check (49 live lines, board turned over and rotated by hand, then breathed on): az swept
-967..+1000 mg with ax / ay redistributing, gyro peaked near 98 dps, the magnetic vector rotated,
humidity moved 0.33 %RH and relaxed through the filter. Worst `run_cycle` 5081 us (TC-HW-SS-009).
The first run failed TC-HW-SS-008: Group A `NOT_READY` (1 Hz sensors polled every 100 ms) was being
treated as a fault. Fixed and covered by TC-SS-110 (SS-D17).

---

## §14 Open items

| ID | Item | Resolution path | Status |
|----|------|-----------------|--------|
| SS-O1 | IIR filter alpha value — [TBD] in REQ-SA-140. Provisional 0.1. Must be in range (0, 1) exclusive; validated at init. | Confirm with sensor characterisation data or customer spec | Open |
| SS-O2 | REQ-SA-090 "most recent [TBD] readings per sensor" — TBD not resolved. Decision: N=1 (latest reading only). If N > 1 required, add static ring buffer per sensor. | Implement N=1; revisit if historical access required | Open |
| SS-O3 | IMU and magnetometer alarm evaluation (GW) — deferred. Per-axis and magnitude thresholds have different physical interpretations. | Design at coding time once customer spec confirmed | Open |
| SS-O4 | WCET measurement — 3 ms (FD) / 5 ms (GW) budget must be verified by timing `sensor_service_run_cycle()` under debugger at target speed. | Measured on the Gateway 2026-10-15 (TC-HW-SS-009): worst case **5081 us** over 50 timed cycles, 81 us over the estimate. About 4 ms of it is the Group A drivers' two 2 ms not-ready retry sleeps (blocked, not CPU-busy); a cycle with no due Group A read is far shorter. The requirement is to finish well inside the 100 ms period (REQ-NF-101), so the GW budget is restated as 10 ms wall time. Record in task-breakdown.md §8. | Closed (GW) |
| SS-O5 | `firmware/common/` directory — not in original project structure. | Closed 2026-10-15 — per-board copies kept (SS-D16). | Closed |
| SS-O6 | The Gateway has no `IConfigProvider`: alpha, ranges, thresholds and hysteresis are compile-time defaults, and the §5.3 hysteresis-validation path (TC-AS-002) has nothing to validate. | Load from ConfigService once it exists on the Gateway; then implement TC-AS-002. | Open |
| SS-O7 | `read_on_demand()` runs a cycle in the caller's task with no lock; safe only if it never overlaps SensorTask. The Gateway has no caller. | Add a mutex or route the request through SensorTask before the first Gateway caller appears. | Open |
| SS-O8 | CloudPublisher still compiles against placeholder `sensor_reading_t` / `sensor_service_get_latest()` / `alarm_service_subscribe(handle, cb, ctx)` / `alarm_event_t`. All clash with the real headers, and its `int16_t` gyro field overflows (±250 dps = ±250000 mdps). | Reconcile CloudPublisher against this LLD (same job as SAF-O3). Until then `application/sensor_service` and `application/alarm_service` are excluded from the default `.cproject` image, because the `cloud_publisher` bring-up harness defines a placeholder `alarm_service_subscribe` that collides with the real one at link time. | Open |
| SS-O9 | Hardware validation of the pipeline (TC-HW-SS-*), the 5 ms GW WCET (SS-O4) and the 384-word stack. | TC-HW-SS-001..009 PASS on 2026-10-15; tilt / rotate / breathe check done (see §13.5). SensorTask stack: 233 of 384 words free, so it could shrink to 192 words later. | Closed |
| SS-O11 | (Closed 2026-10-15, SS-D18) The integer IIR had a dead band: with alpha 1/10 and rounding to nearest, a step smaller than 5 units rounds to zero. The accelerometer (mg) held -166/-90/-950 for 30 s on the bench while the gyro and magnetometer moved. | Confirm accel responds when tilted; if the dead band matters, keep the filter state with extra fractional bits. | Open |
| SS-O10 | IMU / magnetometer DRDY interrupts are not used: SensorService polls every 100 ms and never calls `attach_*_callback`. | Revisit if lower latency than one cycle is needed. | Open |

---

## §15 Decisions log

| ID | Decision | Rationale |
|----|----------|-----------|
| SS-D1 | Co-host AlarmService in SensorTask as a SensorService subscriber callback | Both share the same execution flow; a separate task would add synchronisation overhead without benefit. |
| SS-D2 | `sensor_reading_t.value` is `int32_t` fixed-point (**corrected in v1.1**; v1.0 said `float`) | The FD implementation was refactored from float to fixed-point, the BARR-C/P9 rule forbids float in this layer, and the CloudPublisher wire format is integer (mg / mdps / mgauss). Integer filter and thresholds are exact and need no FPU context. |
| SS-D3 | Non-latched alarm model with manual clear via `ack_all()` | Flags reflect current evaluation state. CMD_ACK_ALARM provides manual clear. Re-raise on next cycle if condition persists. Trades acknowledge semantics for simplicity. |
| SS-D4 | IIR filter applied to clamped value, not raw | Prevents out-of-range spike from corrupting filter state with an extreme value. |
| SS-D5 | Vtable singleton pattern for both services | Consistent with LLD-D10 (I2cDriver). Enables test substitution via link-time vtable replacement. |
| SS-D6 | `poll_timer` at task level, not in `SensorServiceState` | SensorService is a pure application service providing `run_cycle()`. Timer management is a task-level scheduling concern. |
| SS-D7 | `pause()` / `resume()` deferred to OTA stretch goal | SD-06d requires these for OTA flash quiesce. Not needed until UpdateService companion is written. |
| SS-D8 | `SENSOR_ID_COUNT = 12` on both boards | Keeps Modbus register layout consistent. FD marks sensors 3-11 as permanently failed at init; memory overhead is approximately 200 bytes on a 320 KiB MCU. |
| SS-D9 | Gateway initialises both services in `sensor_task_create()`, before the scheduler | The Group A / Group B driver headers say `*_init()` must run before the scheduler starts; v1.0 §7.1 ran it in the task after the scheduler. |
| SS-D10 | IMU and magnetometer converted to mg / mdps / mgauss in SensorService | The drivers return raw LSB (GPB-D4). Units match the existing CloudPublisher JSON. Accel 1000/16384 mg/LSB, gyro 875/100 mdps/LSB, mag 1000/6842 mgauss/LSB (sensitivities from the Group B LLD). |
| SS-D11 | `sensor_service_internal.h` wraps the four driver headers | Ceedling links a .c file for every header basename in the test TU; the wrapper's basename matches none, so the unit test sees driver types without linking real drivers. |
| SS-D12 | Gateway `init` takes the HealthMonitor handle | GW HealthMonitor is handle-based; the §2.2 `health_report_event()` was provisional. |
| SS-D13 | IIR filter seeded by the first accepted sample; rounding half away from zero | Avoids a ramp from zero (false low-pressure alarm at boot) and a one-LSB bias on negative axes. |
| SS-D14 | Health events are edge-triggered per channel: FAIL on first failure, RECOVERED on first good sample | v1.0 §5.1 "first failure this cycle" would push an event every 100 ms for a dead sensor. |
| SS-D15 | AlarmService evaluates only channels with `enabled` thresholds | Gateway IMU / magnetometer readings are valid; zero thresholds would alarm on any non-zero reading (SS-O3). |
| SS-D16 | Sources stay per board under `firmware/<board>/application/` | Matches every other module and the FD copy; closes SS-O5. |
| SS-D17 | `NOT_READY` from any of the four drivers keeps the previous reading and is not a fault | HTS221 / LPS22HB run at 1 Hz and the magnetometer at 10 Hz against a 100 ms cycle, so a cycle without a fresh sample is normal (found on the bench 2026-10-15: treating it as a fault failed TC-HW-SS-008). The sensor is still read every cycle so its latched DRDY / INT1 line is released (GPB-O6). |
| SS-D18 | The IIR state carries 4 fractional bits | With integer state a step below den / (2 * num) rounds to zero and the output sticks; the accelerometer held one value for 30 s on the bench (SS-O11). |
| SS-D19 | Group A (HTS221, LPS22HB) is read every 10th cycle, and every cycle while a channel has no valid reading | Those sensors run at 1 Hz (GPA-D2). Reading at 10 Hz made nine reads in ten hit the driver's 2 ms not-ready retry sleep, pushing the measured cycle to 5081 us against the 5 ms budget. |

---

## §16 Principles applied

- **P1 (Strict directional layering).** SensorService depends on driver
  interfaces (IBarometer, IHumidityTemp, IMagnetometer, IImu) and
  middleware (ITimeProvider, IConfigProvider); AlarmService depends on
  ISensorService.  Neither crosses a layer boundary upward.

- **P2 (Dependency Inversion).** Both services expose vtable interfaces
  (`isensor_service_t`, `ialarm_service_t`).  Consumers depend on the
  abstract interface, not the concrete implementation.

- **P3 (Interface Segregation).** SensorService and AlarmService have
  separate interfaces.  A consumer that only needs alarm state does not
  depend on the sensor acquisition API.

- **P4 (Cross-cutting concern exception).** Logger and HealthMonitor
  (`IHealthReport`) are referenced concretely per the cross-cutting
  exception.

- **P5 (Bounded resources, no dynamic allocation post-init).** All buffers
  and subscriber tables are statically allocated with compile-time sizing.
  No heap usage.

- **P6 (Responsibility traces to requirements).** Acquisition pipeline
  traces to REQ-SA-000–171; alarm evaluation traces to REQ-AM-000–040.
  §3 provides the full traceability matrix.

- **P7 (Pull-based downstream consumption).** `get_snapshot()` and
  `get_state()` / `get_all_states()` provide pull-based access.  The
  subscriber callback is the push complement for low-latency consumers
  that need immediate notification.

- **P10 (Naming conventions).** Prefixes `sensor_service_` /
  `alarm_service_`; interfaces `ISensorService` -> `isensor_service_t`,
  `IAlarmService` -> `ialarm_service_t`; errors `SENSOR_SERVICE_ERR_*` /
  `ALARM_SERVICE_ERR_*`.

**Error handling.** All public functions return typed `_err_t` enums.
Sensor read failures update the health report via `IHealthReport` and set
`reading.valid = false` rather than silently substituting stale data.

**BARR-C:2018.** Fixed-width integers (`uint8_t`, `uint32_t`), `const`
correctness on output pointer parameters, braces on all control
structures, module-prefixed public symbols, Doxygen on all public APIs.
Float values (`sensor_reading_t.value`, `alarm_threshold_t`) are used in
the application layer where the Cortex-M4F FPU provides hardware
acceleration (SS-D2); fixed-point representation is used at the Modbus
register boundary.

---

## §17 References

| Ref | Document |
|-----|----------|
| [1] | `components.md` — SensorService, AlarmService component descriptions |
| [2] | `hld.md` §5.3 — Data flow: sensor -> processing -> alarm -> consumers |
| [3] | `sequence-diagrams.md` SD-01 — Sensor acquisition cycle |
| [4] | `task-breakdown.md` §4.2 (FD), §5.2 (GW) — SensorTask specification |
| [5] | `SRS.md` REQ-SA-000–171, REQ-AM-000–040 |
| [6] | `modbus-register-map.md` — Holding register layout for sensor data |
| [7] | GW Sensor Drivers Group A LLD — IBarometer, IHumidityTemp vtables |
| [8] | GW Sensor Drivers Group B LLD — IMagnetometer, IImu vtables |
| [9] | I2cDriver LLD — LLD-D10 vtable singleton pattern |
| [10] | `architecture-principles.md` — P1–P10 |
| [11] | Vision §5.1.1 — FD simulated sensor backend |

---

## Phase H checklist

| # | Criterion | Status |
|---|-----------|--------|
| H-01 | Every public function has a Doxygen block with `@brief`, `@param`, `@return`, `@pre`/`@post` where applicable, and threading notes | Done — §4.2, §4.3 |
| H-02 | All consumed interface signatures listed with source companion reference | Done — §2.2 |
| H-03 | Traceability matrix maps requirements to API functions and test cases | Done — §3 |
| H-04 | Internal state structures defined with field-level comments | Done — §5.2, §5.4 |
| H-05 | Synchronisation model documented per operation | Done — §6 |
| H-06 | Init ordering specified with numbered steps | Done — §7.1 |
| H-07 | Sequence diagram trace table maps SD-NN to functions | Done — §8 |
| H-08 | Error table with cause, local behaviour, retry, observability | Done — §9 |
| H-09 | Memory and sizing budget with per-structure breakdown | Done — §11 |
| H-10 | File layout with directory structure and file descriptions | Done — §12 |
| H-11 | Unit-test plan with TC-NNN identifiers, inputs, expected results | Done — §13 |
| H-12 | `_reset_for_test()` under `#ifdef TEST` for both services | Done — §4.2, §4.3 |
| H-13 | Open items table with ID, resolution path, status | Done — §14 |
| H-14 | Decisions log with ID, decision, rationale | Done — §15 |
