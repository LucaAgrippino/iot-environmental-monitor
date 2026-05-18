# LLD Companion — SensorService · AlarmService

**Layer:** Application  
**Boards:** Field Device (FD) · Gateway (GW)  
**SensorService provides:** `ISensorService`  
**AlarmService provides:** `IAlarmService`  
**SensorService consumes (FD):** `IBarometer`, `IHumidityTemp`, `ITimeProvider`, `IConfigProvider`, `IHealthReport`, `ILogger`  
**SensorService consumes (GW):** `IBarometer`, `IHumidityTemp`, `IMagnetometer`, `IImu`, `ITimeProvider`, `IConfigProvider`, `IHealthReport`, `ILogger`  
**AlarmService consumes:** `ISensorService`, `IConfigProvider`, `ILogger`  
**SRS traces:** REQ-SA-000–SA-171; REQ-AM-000–AM-040  
**HLD ref:** `components.md` §Application — SensorService, AlarmService; `hld.md` §5.3; `sequence-diagrams.md` SD-01; `task-breakdown.md` §4.2 (FD), §5.2 (GW)
**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** SensorService + AlarmService in `components.md` (FD + GW application layer)

---

## 1. Sources

AlarmService is co-hosted in `SensorTask`. Its evaluation runs as the
new-reading callback registered with SensorService — when `SensorService`
completes a cycle and notifies subscribers, AlarmService's callback fires
in the same task context. A separate companion would split the description
of one execution flow across two documents.

---

## 2. Sensor inventory by board

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

## 3. Data types

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

typedef struct {
    float                value;      /* engineering units (°C, %RH, hPa, m/s², dps, µT) */
    bool                 valid;      /* false if driver returned error (REQ-SA-0E1) */
    time_provider_ts_t   timestamp;  /* from TimeProvider (REQ-SA-100) */
} sensor_reading_t;

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

---

## 2. Public API — provided interface `ISensorService`

```c
/**
 * @brief  Initialise SensorService.
 *
 * Reads polling interval and filter parameters from IConfigProvider
 * (falls back to defaults if config absent — REQ-SA-010, SA-050).
 * Attempts to initialise each driver; logs failures and continues
 * (REQ-SA-040, SA-060). Marks permanently failed sensors as invalid
 * so every subsequent cycle skips the failing driver.
 */
sensor_service_err_t sensor_service_init(void);

/**
 * @brief  Run one full acquisition cycle.
 *
 * Called from SensorTask on every 100 ms periodic tick.
 * Reads all active drivers, applies the full processing pipeline
 * (§6), updates the internal snapshot, then calls all registered
 * new-reading callbacks in registration order.
 *
 * Never blocks longer than the WCET budget (~3 ms at 80 MHz —
 * task-breakdown.md §8.1). All driver calls are synchronous.
 */
sensor_service_err_t sensor_service_run_cycle(void);

/**
 * @brief  Get the latest sensor snapshot (pull model).
 *
 * Called by LcdUi (FD), ModbusRegisterMap (FD), and CloudPublisher (GW)
 * to read current data. Thread-safe — snapshot is copied under a
 * critical section.
 *
 * @param[out] snap  Filled with the latest snapshot.
 */
sensor_service_err_t sensor_service_get_snapshot(sensor_snapshot_t *snap);

/**
 * @brief  Register a new-reading callback (push model).
 *
 * Callback fires in SensorTask context immediately after each cycle
 * completes. Keep callback execution time minimal — it delays the
 * next cycle.
 *
 * Maximum subscribers: SENSOR_MAX_SUBSCRIBERS (compile-time constant,
 * default 4). Returns ERR_NO_SUB if table is full.
 *
 * @param  cb  Function pointer; must remain valid for the system lifetime.
 */
sensor_service_err_t sensor_service_subscribe(
    void (*cb)(const sensor_snapshot_t *snap));

/**
 * @brief  Request an on-demand sensor read (REQ-SA-170).
 *
 * Triggers one additional acquisition cycle outside the periodic timer.
 * The result appears in the snapshot and is pushed to subscribers.
 * Called by ModbusRegisterMap (FD) on FC04 remote read request.
 *
 * Executes synchronously in the calling task's context — caller must
 * tolerate ~3 ms blocking.
 */
sensor_service_err_t sensor_service_read_on_demand(void);

/**
 * @brief  Return true if all non-failed sensors produced a valid reading
 *         in the last cycle. Used by LifecycleController self-check.
 */
bool sensor_service_is_ready(void);
```

---

## 5. Processing pipeline (SD-01, per sensor per cycle)

Executed by `sensor_service_run_cycle()` for each enabled sensor:

```
1. Call driver read function.
   On error: mark reading.valid = false; log (REQ-SA-080, SA-0E1);
             push HEALTH_EVENT_SENSOR_FAIL if first failure this cycle;
             skip steps 2–5 for this sensor.

2. Stamp: reading.timestamp = time_provider_get()   [REQ-SA-100]

3. Range validate: if value < range_min || value > range_max:
       reading.valid = false  [REQ-SA-120]
       (clamped copy still passes through filter — see step 4)

4. Clamp: value = clamp(value, range_min, range_max)  [REQ-SA-130]

5. Low-pass IIR filter:                               [REQ-SA-140]
       filtered = alpha * value + (1.0f - alpha) * prev_filtered[sensor_id]
       prev_filtered[sensor_id] = filtered
       reading.value = filtered
```

The filter is applied to the clamped value, not the raw value. This
prevents an out-of-range spike from corrupting the filter state with an
extreme value.

**Alpha parameter (REQ-SA-140 — [TBD]):** provisionally 0.1 (heavy
smoothing — time constant ≈ 1 s at 10 Hz, ≈ 10 s at 1 Hz). Loaded from
`IConfigProvider`; falls back to this default if absent (REQ-SA-050).
See SS-O1.

After all sensors processed, update the internal snapshot under a
`taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` guard (brief — just a
`memcpy`), then call all subscribers.

---

## 6. AlarmService — provided interface `IAlarmService`

```c
/**
 * @brief  Initialise AlarmService.
 *
 * Reads alarm thresholds and hysteresis values from IConfigProvider
 * for each sensor. Initialises all alarm states to CLEAR.
 * Registers alarm_service_evaluate() as a SensorService subscriber.
 */
alarm_service_err_t alarm_service_init(void);

/**
 * @brief  Get the current alarm state for one sensor.
 *
 * Thread-safe — alarm state is a simple enum, read atomically on
 * Cortex-M4.
 */
alarm_service_err_t alarm_service_get_state(sensor_id_t   sensor,
                                             alarm_state_t *state_out);

/**
 * @brief  Get alarm states for all sensors at once.
 *
 * @param[out] states  Array of SENSOR_ID_COUNT alarm_state_t values.
 */
alarm_service_err_t alarm_service_get_all_states(
    alarm_state_t states[SENSOR_ID_COUNT]);

/**
 * @brief  Register an alarm event callback.
 *
 * Fired in SensorTask context when an alarm is raised or cleared.
 * Consumers: LcdUi (FD), ModbusRegisterMap (FD — exposes alarm
 * register bits), CloudPublisher (GW — publishes alarm events).
 *
 * Maximum subscribers: ALARM_MAX_SUBSCRIBERS (default 4).
 */
alarm_service_err_t alarm_service_subscribe(
    void (*cb)(sensor_id_t       sensor,
               alarm_event_t     event,
               const sensor_reading_t *reading));
```

---

## 7. Alarm evaluation logic

`alarm_service_evaluate()` is the SensorService subscriber callback.
It runs in SensorTask context, called after each acquisition cycle.

```c
static void alarm_service_evaluate(const sensor_snapshot_t *snap)
{
    for (int i = 0; i < SENSOR_ID_COUNT; i++) {
        const sensor_reading_t *r = &snap->readings[i];

        if (!r->valid) { continue; }   /* skip invalid readings */

        float threshold_high = s_as.thresholds[i].high;
        float threshold_low  = s_as.thresholds[i].low;
        float hysteresis     = s_as.thresholds[i].hysteresis;

        switch (s_as.alarm_state[i]) {
        case ALARM_STATE_CLEAR:
            if (r->value > threshold_high) {
                set_alarm(i, ALARM_STATE_ACTIVE_HIGH, ALARM_EVENT_RAISED_HIGH, r);
            } else if (r->value < threshold_low) {
                set_alarm(i, ALARM_STATE_ACTIVE_LOW, ALARM_EVENT_RAISED_LOW, r);
            }
            break;
        case ALARM_STATE_ACTIVE_HIGH:
            if (r->value < (threshold_high - hysteresis)) {
                set_alarm(i, ALARM_STATE_CLEAR, ALARM_EVENT_CLEARED, r);
            }
            break;
        case ALARM_STATE_ACTIVE_LOW:
            if (r->value > (threshold_low + hysteresis)) {
                set_alarm(i, ALARM_STATE_CLEAR, ALARM_EVENT_CLEARED, r);
            }
            break;
        }
    }
}
```

`set_alarm()` updates `s_as.alarm_state[i]` and calls all registered
alarm subscribers with the event and the triggering reading. The alarm
notification includes all fields required by REQ-AM-040: sensor ID,
alarm type, measured value, threshold value, timestamp, device ID.

**Hysteresis must be positive and smaller than (threshold_high −
threshold_low) / 2.** Validate at `alarm_service_init()` when loading
from ConfigProvider; reject and use defaults on violation. This prevents
a misconfigured hysteresis from making alarms impossible to clear.

---

## 3. Internal design

```c
/* sensor_service.c */

#define SENSOR_MAX_SUBSCRIBERS  4U

typedef struct {
    bool                initialised;
    sensor_snapshot_t   snapshot;     /* protected by taskENTER_CRITICAL */
    float               prev_filtered[SENSOR_ID_COUNT];
    float               alpha;        /* IIR filter coefficient */
    bool                driver_failed[SENSOR_ID_COUNT];  /* permanent fail flag */
    void (*subscribers[SENSOR_MAX_SUBSCRIBERS])(const sensor_snapshot_t *);
    uint8_t             subscriber_count;
    TimerHandle_t       poll_timer;   /* 100 ms periodic tick */
} SensorServiceState;

static SensorServiceState s_ss;

/* alarm_service.c */

typedef struct {
    float high;
    float low;
    float hysteresis;
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

## 9. Task and timer design

SensorTask blocks on a FreeRTOS direct-to-task notification. A 100 ms
software timer fires and sends the notification; SensorTask wakes and
calls `sensor_service_run_cycle()`.

```c
void vSensorTask(void *pvParameters)
{
    (void)pvParameters;
    /* Wait for LifecycleController start-gate */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    sensor_service_init();
    alarm_service_init();

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  /* 100 ms tick */
        sensor_service_run_cycle();               /* pipeline + alarm eval */
    }
}
```

The poll timer callback is:
```c
static void poll_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    xTaskNotifyGive(s_sensor_task_handle);
}
```

---

## 10. Board differences

| Aspect | FD | GW |
|--------|----|----|
| Sensor count | 3 (temp, humidity, pressure) | 12 readings across 6 sensor types |
| Drivers called | `HumidityTempDriver`, `BarometerDriver` | + `ImuDriver`, `MagnetometerDriver` |
| IMU axis evaluation | — | 3 readings each for accel, gyro |
| Alarm-evaluated sensors | Temp, humidity, pressure | Same 3 scalar sensors; IMU alarms deferred (SS-O3) |
| WCET budget | ~3 ms | ~5 ms (more drivers) |

The FD alarm evaluation is identical to the GW for the three scalar
sensors. IMU/magnetometer alarm evaluation is a GW-only extension
deferred to SS-O3 — only magnitude-threshold alarms or per-axis
alarms make physical sense, and the threshold semantics are non-trivial.

---

## 11. Thread safety summary

| Operation | Caller task(s) | Protection |
|-----------|---------------|------------|
| `sensor_service_run_cycle()` | SensorTask only | No protection needed |
| Snapshot write (inside run_cycle) | SensorTask only | `taskENTER_CRITICAL` for the `memcpy` |
| `sensor_service_get_snapshot()` | Any task (LcdUi, Modbus, Cloud) | `taskENTER_CRITICAL` for the `memcpy` |
| `alarm_service_get_state()` | Any task | Single-word read — atomic on Cortex-M4 |
| Subscriber callbacks | SensorTask only (fired from run_cycle) | No protection needed |

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

## 6. Error and fault behaviour

Error codes and propagation policy are defined in the Public API section above. All public functions return an error code; callers must not ignore non-OK returns.

## 7. Unit-test plan

```c
#ifdef UNIT_TEST
/* SensorService stubs */
#define humidity_temp_driver_read(t, h)   stub_ht_read(t, h)
#define barometer_driver_read(p)          stub_baro_read(p)
#define imu_driver_read_accel(x,y,z)      stub_imu_accel(x,y,z)
#define time_provider_get(ts)             stub_time_get(ts)

/* AlarmService stubs */
/* Inject sensor_snapshot_t directly into alarm_service_evaluate() */
#endif
```

Minimum test cases — SensorService:
- All drivers succeed → snapshot.readings all valid, cycle_count++.
- Driver returns error → reading.valid = false; cycle continues; no abort.
- Range validation: value > range_max → reading.valid = false; clamped value feeds filter.
- IIR filter: single step with known alpha, value, prev → expected output within float epsilon.
- `get_snapshot()` returns copy of latest snapshot.
- `sensor_service_subscribe()` beyond SENSOR_MAX_SUBSCRIBERS → ERR_NO_SUB.
- Callback fired with correct snapshot after run_cycle().

Minimum test cases — AlarmService:
- Reading within range → state stays CLEAR.
- Reading above threshold_high → ALARM_STATE_ACTIVE_HIGH; ALARM_EVENT_RAISED_HIGH fired.
- Hysteresis: reading falls to (threshold_high − hysteresis + ε) → stays ACTIVE_HIGH.
- Reading falls below (threshold_high − hysteresis) → ALARM_STATE_CLEAR; ALARM_EVENT_CLEARED.
- Low alarm symmetric to high alarm.
- Invalid reading (valid = false) → alarm state unchanged.
- Subscriber receives correct sensor_id, event, and reading pointer.

---

## 8. Open items

| ID | Item | Resolution path | Status |
|--------|------|-----------------|--------|
| SS-O1 | IIR filter alpha value — [TBD] in REQ-SA-140. Provisional 0.1. Confirm with sensor characterisation data or customer spec. Must be validated to be in range (0, 1) exclusive at init. | Confirm IIR alpha with sensor characterisation data or customer spec before coding | Open |
| SS-O2 | REQ-SA-090 "most recent [TBD] readings per sensor" — TBD not yet resolved. Decision: store only the latest reading (N=1) unless a specific requirement for historical access emerges. If N > 1 is required, add a static ring buffer per sensor inside SensorService. | Implement N=1 (latest reading only); revisit if historical access is required | Open |
| SS-O3 | IMU and magnetometer alarm evaluation (GW) — deferred. Per-axis thresholds and magnitude thresholds have different physical interpretations. Design at coding time when customer threshold requirements are confirmed. | Design per-axis/magnitude thresholds at coding time once customer spec confirmed | Open |
| SS-O4 | WCET measurement — the 3 ms (FD) / 5 ms (GW) budget must be verified by timing `sensor_service_run_cycle()` under debugger at target speed before sign-off. Record in `task-breakdown.md` §8 when measured. | Time sensor_service_run_cycle() on target under debugger; record in task-breakdown.md §8 | Open |
