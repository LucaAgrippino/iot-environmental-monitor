# LLD Companion — CloudPublisher

**Document:** `docs/lld/application/cloud-publisher.md`
**Version:** 0.2 (Phase H complete — ready for implementation)
**Board:** Gateway (B-L475E-IOT01A) only
**Layer:** Application
**Status:** Implementation-ready
**Date:** July 2026

**HLD anchor:** CloudPublisher in `components.md` (GW application layer)

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Serialises and publishes telemetry, alarm, and health payloads to AWS IoT Core via MqttClient. Routes inbound MQTT commands to the appropriate handler. Polls IMqttStats and reports connectivity metrics via IHealthReport. | `components.md` |
| PROVIDES (upward) | *(none — top of the stack)* | `components.md` |
| USES (downward) | MqttClient, IMqttStats, ISensorService, IAlarmService, IModbusPoller, StoreAndForward, IHealthSnapshot, IHealthReport, ILogger | `components.md` |
| Additional USES (command routing) | IConfigManager, IUpdateService, ILifecycle, IConfigProvider | Companion §7; tracked as F-03 for `components.md` update |
| Hosted in task | CloudPublisherTask, priority 3, 768 words / 3 KB | `task-breakdown.md` |
| Root requirements | REQ-CC-000–090 (cloud connectivity), REQ-BF-000–020 (buffering), REQ-DM-000/002 (commands) | `SRS.md` |

**F-03 (components.md gap):** The `components.md` USES list does not yet
include `IConfigManager`, `IConfigProvider`, `IUpdateService`, or
`ILifecycle`. These are used for inbound command routing and configurable
publish intervals. Tracked for a follow-up correction.

---

## 2. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| Telemetry publish (60 s, QoS 0) | REQ-CC-000, CC-030, NF-111, NF-206, NF-106 | UC-05 |
| Health publish (600 s, QoS 0) | REQ-CC-010, CC-040, NF-112, NF-216, CC-090 | UC-06 |
| Alarm publish (event, QoS 1) | REQ-CC-020, NF-207, NF-113 | UC-09 |
| JSON + schema version | REQ-CC-070, CC-071 | — |
| Separate topics | REQ-CC-080 | — |
| Store-and-forward when offline | REQ-BF-000, BF-010, BF-020, NF-200 | UC-10–12 |
| Inbound command routing | REQ-DM-000, DM-002 | UC-15 |
| MQTT stats → IHealthReport | REQ-CC-010 | UC-06 |
| TLS / auto-reconnect | REQ-CC-050, CC-060, NF-301 | — |

---

## 3. Public API

### 3.1 ADT pattern

CloudPublisher follows the Gateway ADT default: an opaque handle
(`cloud_publisher_handle_t`) returned by `cloud_publisher_create()` from
a static pool of 1. The 13-parameter init from v0.1 is collapsed into a
config struct with named fields.

### 3.2 Data types

```c
/* cloud_publisher.h */

#ifndef CLOUD_PUBLISHER_H
#define CLOUD_PUBLISHER_H

#include <stdint.h>
#include <stdbool.h>

/** @brief Opaque handle to a CloudPublisher instance. */
typedef struct cloud_publisher_inst *cloud_publisher_handle_t;

typedef enum {
    CP_ERR_OK             = 0,
    CP_ERR_NOT_INIT       = 1,
    CP_ERR_NULL_PTR       = 2,
    CP_ERR_NO_RESOURCE    = 3,
    CP_ERR_SERIALISE      = 4,  /**< JSON payload exceeded buffer.       */
    CP_ERR_PUBLISH_FAILED = 5,  /**< MQTT publish failed; routed to SAF. */
    CP_ERR_SAF_FULL       = 6,  /**< Store-and-forward buffer full.      */
} cloud_publisher_err_t;
```

### 3.3 Configuration struct

```c
/**
 * @brief CloudPublisher creation configuration.
 *
 * All dependencies injected via opaque handles.
 */
typedef struct {
    mqtt_client_handle_t   mqtt;
    sensor_service_handle_t sensors;       /**< ISensorService — latest readings.    */
    alarm_service_handle_t  alarms;        /**< IAlarmService — alarm subscription.  */
    modbus_poller_handle_t  poller;        /**< IModbusPoller — FD readings.         */
    store_and_forward_handle_t saf;        /**< IStoreAndForward — offline buffer.   */
    health_monitor_handle_t health_read;   /**< IHealthSnapshot — health payload.    */
    health_monitor_handle_t health_write;  /**< IHealthReport — MQTT stats push.     */
    config_service_handle_t cfg_read;      /**< IConfigProvider — publish intervals. */
    config_service_handle_t cfg_write;     /**< IConfigManager — remote config cmds. */
    /* command routing targets */
    update_service_handle_t update_svc;    /**< May be NULL until UpdateService LLD; see CP-O1. */
    lifecycle_handle_t      lifecycle;     /**< ILifecycle — restart command.        */
} cloud_publisher_config_t;
```

### 3.4 Public API

```c
/**
 * @brief Create and initialise a CloudPublisher instance.
 *
 * Stores all dependency handles, creates the alarm queue (8 entries),
 * command queue (4 entries), FreeRTOS software timers (telemetry 60 s,
 * health 600 s, stats 1 Hz), registers alarm and MQTT callbacks, and
 * creates CloudPublisherTask.
 *
 * @param[in]  config  Injected dependencies.
 * @param[out] handle  Receives the created handle on success.
 * @return CP_ERR_OK on success; CP_ERR_NULL_PTR if config or handle
 *         is NULL; CP_ERR_NO_RESOURCE if pool exhausted.
 * @note Threading: call before scheduler starts.
 */
cloud_publisher_err_t cloud_publisher_create(
    const cloud_publisher_config_t *config,
    cloud_publisher_handle_t *handle);

#endif /* CLOUD_PUBLISHER_H */
```

CloudPublisher PROVIDES nothing upward — it is the top of the stack.
All behaviour is internal to CloudPublisherTask. The public API is
`cloud_publisher_create()` only; all other operations are driven by
timers, queues, and callbacks within the task.

---

## 4. Activation model

CloudPublisherTask blocks on a FreeRTOS task notification (32-bit
bitmask) and two queues:

| Bit / Queue | Source | Period / event |
|---|---|---|
| Bit 0 — `TELEMETRY_TICK` | Software timer | 60 s (REQ-NF-111, configurable) |
| Bit 1 — `HEALTH_TICK` | Software timer | 600 s (REQ-NF-112, configurable) |
| Bit 2 — `STATS_TICK` | Software timer | 1 Hz |
| Bit 3 — `ALARM_PENDING` | alarm_queue non-empty | Event-driven |
| Bit 4 — `COMMAND_PENDING` | command_queue non-empty | Event-driven |

```
CloudPublisherTask loop:
    xTaskNotifyWait(...)
    if TELEMETRY_TICK:  publish_telemetry()
    if HEALTH_TICK:     publish_health()
    if STATS_TICK:      poll_stats()
    if ALARM_PENDING:   drain_alarm_queue()
    if COMMAND_PENDING: drain_command_queue()
    mqtt_client_process(mqtt)   /* service keep-alive + inbound */
```

Timers start at create. If the connection is down, publish calls route
to StoreAndForward.

---

## 5. Internal design

### 5.1 Private struct and static pool

```c
/* cloud_publisher.c */

#define CP_MAX_INSTANCES     1u
#define CP_JSON_BUF_SIZE  4096u   /**< Matches MQTT_PKT_BUF_SIZE; see MQTT-O3. */
#define CP_ALARM_QUEUE_LEN   8u
#define CP_CMD_QUEUE_LEN     4u

struct cloud_publisher_inst {
    /* Injected dependencies (all opaque handles) */
    mqtt_client_handle_t       mqtt;
    sensor_service_handle_t    sensors;
    alarm_service_handle_t     alarms;
    modbus_poller_handle_t     poller;
    store_and_forward_handle_t saf;
    health_monitor_handle_t    health_read;
    health_monitor_handle_t    health_write;
    config_service_handle_t    cfg_read;
    config_service_handle_t    cfg_write;
    update_service_handle_t    update_svc;
    lifecycle_handle_t         lifecycle;

    /* Working state */
    char               scratch_buf[CP_JSON_BUF_SIZE];
    mqtt_stats_t       last_mqtt_stats;
    char               device_serial[25]; /**< MCU UID hex string. */

    /* FreeRTOS objects */
    QueueHandle_t      alarm_queue;
    QueueHandle_t      command_queue;
    TimerHandle_t      telemetry_timer;
    TimerHandle_t      health_timer;
    TimerHandle_t      stats_timer;
    TaskHandle_t       task_handle;

    bool               in_use;
};

static struct cloud_publisher_inst g_pool[CP_MAX_INSTANCES];
static uint8_t                     g_count;
```

### 5.2 Publish paths

**Telemetry (REQ-CC-000, NF-111):**

```
publish_telemetry():
    reading = sensor_service_get_latest(inst->sensors)
    fd = modbus_poller_get_latest_fd(inst->poller)
    n = serialise_telemetry(inst->scratch_buf, CP_JSON_BUF_SIZE,
                            &reading, &fd)
    enqueue_or_publish(inst, TOPIC_TELEMETRY, inst->scratch_buf, n, QOS_0)
```

Period configurable via `config_provider_get_telemetry_interval()` —
read at each tick, not cached.

**Health (REQ-CC-010, NF-112):**

```
publish_health():
    snap = health_snapshot_get(inst->health_read)
    n = serialise_health(inst->scratch_buf, CP_JSON_BUF_SIZE, &snap,
                         inst->device_serial)
    enqueue_or_publish(inst, TOPIC_HEALTH, inst->scratch_buf, n, QOS_0)
```

**Alarm (REQ-CC-020, NF-113):**

AlarmService notifies via an event-subscriber callback registered at
create. The callback runs in SensorTask context — it must not block:

```c
/* Runs in SensorTask context (push from AlarmService) */
static void prv_alarm_event_cb(alarm_event_t *ev, void *ctx)
{
    struct cloud_publisher_inst *inst = ctx;
    xQueueSend(inst->alarm_queue, ev, 0u); /* non-blocking */
    xTaskNotify(inst->task_handle, ALARM_PENDING, eSetBits);
}
```

CloudPublisherTask drains the queue and publishes at QoS 1:

```
drain_alarm_queue():
    while xQueueReceive(alarm_queue, &ev, 0) == pdTRUE:
        n = serialise_alarm(scratch_buf, CP_JSON_BUF_SIZE, &ev)
        enqueue_or_publish(inst, TOPIC_ALARMS, scratch_buf, n, QOS_1)
```

### 5.3 Connectivity gate

```
enqueue_or_publish(inst, topic, buf, len, qos):
    if mqtt_client_is_connected(inst->mqtt):
        rc = mqtt_client_publish(inst->mqtt, topic, buf, len, qos)
        if rc != OK:
            store_and_forward_enqueue(inst->saf, topic, buf, len, qos)
    else:
        store_and_forward_enqueue(inst->saf, topic, buf, len, qos)
```

### 5.4 Store-and-forward drain

On reconnection (MqttClient state-change callback):

```
on_mqtt_connected():
    while store_and_forward_dequeue(inst->saf, &entry) == SAF_OK:
        rc = mqtt_client_publish(inst->mqtt, entry.topic,
                                 entry.buf, entry.len, entry.qos)
        if rc == OK:
            store_and_forward_confirm(inst->saf)
        else:
            break   /* stop drain; re-attempt on next connect */
```

Live publishes during drain go through `enqueue_or_publish` directly —
they do not jump the drain queue. Chronological order of buffered entries
preserved (REQ-BF-010).

### 5.5 Inbound command routing

MqttClient delivers inbound messages via the `msg_cb` registered at
MqttClient create. The callback runs in CloudPublisherTask context
(inside `mqtt_client_process()`), so it enqueues into `command_queue`
and sets `COMMAND_PENDING`.

```
drain_command_queue():
    while xQueueReceive(command_queue, &entry, 0) == pdTRUE:
        route_command(inst, &entry)

route_command(inst, entry):
    if topic matches ".../config":
        config_manager_apply(inst->cfg_write, entry->payload)
        publish_result(inst, ack_or_reject)
    elif topic matches ".../ota":
        update_service_handle_command(inst->update_svc, entry->payload)
    elif topic matches ".../control":
        lifecycle_handle_remote(inst->lifecycle, entry->payload)
    else:
        log_warn("unknown command topic")
```

### 5.6 MQTT stats polling — Metric Producer Pattern

```
poll_stats():    /* 1 Hz */
    mqtt_client_get_stats(inst->mqtt, &stats)
    health_report_update_mqtt(inst->health_write, &stats,
                              &inst->last_mqtt_stats)
    inst->last_mqtt_stats = stats
```

Delta computation in `health_report_update_mqtt` avoids monotonically
growing counter noise.

### 5.7 Test reset hook

```c
#ifdef TEST
void cloud_publisher_reset_for_test(void)
{
    memset(g_pool, 0, sizeof(g_pool));
    g_count = 0u;
}
#endif
```

---

## 6. MQTT topic map

Topics assembled from client ID (MCU UID hex) and per-message suffix.
Convention aligned with MqttClient companion §6.

| Message type | Topic | QoS |
|---|---|---|
| Telemetry | `dt/iotmonitor/<device_id>/telemetry` | 0 |
| Health | `dt/iotmonitor/<device_id>/health` | 0 |
| Alarms | `dt/iotmonitor/<device_id>/alarms` | 1 |
| Command results | `dt/iotmonitor/<device_id>/results` | 1 |
| Commands (subscribe) | `cmd/iotmonitor/<device_id>/config` | — |
| OTA (subscribe) | `cmd/iotmonitor/<device_id>/ota` | — |

`dt/` = device-to-cloud; `cmd/` = cloud-to-device (AWS IoT Core
convention). `<device_id>` is the MCU UID hex string read at init.

---

## 7. JSON payload schemas

### 7.1 Telemetry (REQ-CC-000, CC-070, CC-071)

```json
{
  "schema_version": "1.0",
  "device_id": "aabbccddeeff112233445566",
  "timestamp": 1705312983,
  "sync_state": "synchronised",
  "temperature_deci_c": 234,
  "humidity_pct": 61,
  "pressure_hpa": 1013,
  "accel_x_mg": 12, "accel_y_mg": -5, "accel_z_mg": 998,
  "gyro_x_mdps": 0, "gyro_y_mdps": 0, "gyro_z_mdps": 0,
  "mag_x_mgauss": 120, "mag_y_mgauss": -45, "mag_z_mgauss": 380,
  "field_device_temp_deci_c": 215,
  "field_device_humidity_pct": 58,
  "field_device_pressure_hpa": 1011,
  "field_device_valid": true
}
```

If field device is offline, `field_device_valid` = `false` and other
`field_device_*` fields omitted (REQ-SA-160).

### 7.2 Health (REQ-CC-010, CC-090)

All fields from IHealthSnapshot + `device_id` + `schema_version`.

### 7.3 Alarm (REQ-AM-040, CC-070, CC-071)

```json
{
  "schema_version": "1.0",
  "device_id": "...",
  "timestamp": 1705313010,
  "sensor_id": "temperature",
  "alarm_type": "HIGH",
  "measured_value_raw": 412,
  "threshold_value_raw": 400,
  "source": "gateway"
}
```

`source`: `"gateway"` for GW sensors, `"field_device"` for FD alarms
forwarded via Modbus.

---

## 8. Memory and sizing

| Item | Size |
|---|---|
| `cloud_publisher_inst` context | ~120 B |
| `scratch_buf` (JSON) | 4 KB |
| `alarm_queue` (8 × ~64 B) | ~512 B |
| `command_queue` (4 × ~260 B) | ~1 KB |
| Timer handles (×3) | ~48 B |
| **Total RAM** | **~5.7 KB** |

Stack: 768 words / 3 KB. Peak during `serialise_health()` (~20 fields
via snprintf). Estimated peak frame < 512 B.

---

## 9. Synchronisation

| State | Access context | Protection |
|---|---|---|
| `scratch_buf` | CloudPublisherTask only | None |
| `last_mqtt_stats` | CloudPublisherTask only | None |
| `alarm_queue` | Write: SensorTask (callback); Read: CloudPublisherTask | FreeRTOS queue (inherently safe) |
| `command_queue` | Write: CloudPublisherTask (`mqtt_client_process`); Read: CloudPublisherTask | FreeRTOS queue |
| Dependency handles | Set at create, immutable | None |

SensorTask → `alarm_queue` is the only cross-task write path. It uses
`xQueueSend` non-blocking to avoid blocking SensorTask.

---

## 10. Sequence integration

| SD | Role | Key functions |
|---|---|---|
| SD-03 | Telemetry (60 s) and health (600 s) MQTT publish | `publish_telemetry()`, `publish_health()` |
| SD-04 | Cloud disconnect → SAF enqueue; reconnect → drain | `on_disconnect()`, `on_mqtt_connected()` |
| SD-05 | Alarm → MQTT publish at QoS 1 | `drain_alarm_queue()` |
| SD-06 | OTA command receive → route to UpdateService; progress/completion publish | `route_command()` |
| SD-07 | Remote config receive → route to ConfigService | `route_command()` |
| SD-08 | Remote restart receive → route to LifecycleController | `route_command()` |

---

## 11. Error and fault behaviour

| Error | Cause | Behaviour |
|---|---|---|
| `CP_ERR_NOT_INIT` | Called before `cloud_publisher_create()` | Return immediately |
| `CP_ERR_NULL_PTR` | Required pointer is NULL | Return immediately |
| `CP_ERR_NO_RESOURCE` | Pool exhausted | Return immediately |
| `CP_ERR_SERIALISE` | JSON exceeded `CP_JSON_BUF_SIZE` | Message dropped, logged at WARN |
| `CP_ERR_PUBLISH_FAILED` | MQTT publish failed while connected | Routed to StoreAndForward |
| `CP_ERR_SAF_FULL` | StoreAndForward rejected entry | Message dropped; `HEALTH_EVENT_SAF_FULL` pushed to IHealthReport |

---

## 12. Principles applied

- **P1 (Strict directional layering).** Depends on middleware interfaces (IMqttClient, IStoreAndForward) and application peers; no layer skipped.
- **P2 (DIP).** Consumes all dependencies via opaque ADT handles; never includes concrete implementation headers.
- **P4 (Cross-cutting exception).** Logger and IHealthReport referenced per convention.
- **P5 (Bounded resources).** All buffers, queues, timers statically allocated. No heap post-init.
- **P6 (Traces to requirements).** §2 traces every concern to specific REQ and UC.
- **P7 (Pull-based access).** Telemetry and health are pull-based: CloudPublisher polls ISensorService and IHealthSnapshot on its timer cadence. **Exception:** Alarms are event-driven push via AlarmService subscriber callback — this is the correct pattern for latency-sensitive events (REQ-NF-113: ≤ 500 ms from detection to publish queuing).
- **P8 (Total error propagation).** 6 distinct error codes; publish failures routed to SAF rather than silently discarded.
- **P9 (BARR-C).** Fixed-width types; `const` on read-only pointers.
- **P10 (Naming).** Prefix `cloud_publisher_`; handle `cloud_publisher_handle_t`; errors `CP_ERR_*`. No `ICloudPublisher` interface — this component PROVIDES nothing upward (top of stack).

---

## 13. Unit-test plan

Test file: `tests/gateway/application/cloud_publisher/test_cloud_publisher.c`

| ID | Scenario | Expected |
|---|---|---|
| CP-T01 | `cloud_publisher_create` happy path | Handle returned, queues created, callbacks registered |
| CP-T02 | `cloud_publisher_create` NULL config | Returns `CP_ERR_NULL_PTR` |
| CP-T03 | `cloud_publisher_create` pool exhaustion | Returns `CP_ERR_NO_RESOURCE` |
| CP-T04 | Telemetry — connected | Calls `sensor_service_get_latest`, serialises, calls `mqtt_client_publish` with telemetry topic, QoS 0 |
| CP-T05 | Telemetry — disconnected | `is_connected` returns false → `store_and_forward_enqueue` called; `mqtt_client_publish` NOT called |
| CP-T06 | Health — connected | QoS 0, health topic, includes device serial |
| CP-T07 | Alarm — connected | Alarm enqueued via callback → drain publishes at QoS 1 |
| CP-T08 | Alarm — disconnected | Alarm enqueued to StoreAndForward at QoS 1 |
| CP-T09 | SAF drain on reconnect | `on_mqtt_connected` → dequeue/publish/confirm loop |
| CP-T10 | Drain stops on publish failure | Mid-drain error → drain stops; entry not confirmed |
| CP-T11 | Inbound config command | Config topic → `config_manager_apply` called; result published |
| CP-T12 | Inbound unknown topic | Logged; no crash |
| CP-T13 | Stats polling | `poll_stats` → `mqtt_client_get_stats` + `health_report_update_mqtt` |
| CP-T14 | JSON truncation | Reduced buffer → `CP_ERR_SERIALISE` returned; no overrun |
| CP-T15 | Alarm queue full | Overflow → log warn; oldest dropped; component stable |
| CP-T16 | Configurable intervals | `get_telemetry_interval` changed → new period used on next tick |

---

## 14. Open items

| ID | Item | Status | Resolution |
|---|---|---|---|
| CP-O1 | `IUpdateService` interface not yet defined. `update_svc` in config may be NULL until UpdateService LLD. | **Open** | Define at UpdateService LLD. |
| CP-O2 | `IModbusPoller.get_latest_fd_readings()` — confirm method exists. | **Open** | Confirm at ModbusPoller LLD. |
| CP-O3 | Largest telemetry payload must fit within `CP_JSON_BUF_SIZE` (4096). | **Open** | Validate at integration with full sensor set. |
| CP-O4 | `field_device_valid = false` — confirm omitting vs null for cloud consumer. | **Open** | Confirm with cloud schema design. |
| CP-O5 | MqttClient's `msg_cb`/`disconnect_cb` are registered at `mqtt_client_create()` time, upstream of CloudPublisher's config injection — inbound command delivery (§5.5) has no real wiring yet. | **Open** | Whichever module ends up owning `mqtt_client_create()` (likely LifecycleController per boot order) must plumb the callbacks through, or CloudPublisher's config must carry a `wifi_handle_t` and call `mqtt_client_create()` itself. |
| F-03 | `components.md` USES list needs: IConfigManager, IConfigProvider, IUpdateService, ILifecycle. | **Open** | Update `components.md`. |

---

## 15. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| CP-D1 | ADT pattern (opaque handle, static pool of 1) | Gateway default. Collapses 13-parameter init into config struct. |
| CP-D2 | Topic scheme `dt/iotmonitor/<device_id>/...` and `cmd/iotmonitor/<device_id>/...` | Consistent with MqttClient companion §6; follows AWS IoT Core convention. |
| CP-D3 | Alarms are event-pushed (not pulled) | REQ-NF-113 requires ≤ 500 ms detection-to-publish. Timer-based polling at 60 s is too slow. Event-driven push via subscriber callback meets the latency requirement. |
| CP-D4 | `scratch_buf` = 4 KB, shared across all serialisers | All three publish paths run sequentially in CloudPublisherTask — no concurrency inside the task. |
| CP-D5 | No `ICloudPublisher` interface | Top of stack; no consumer above. Public API is `cloud_publisher_create()` only. |
| CP-D6 | Stats polling at 1 Hz, delta-computed | Avoids monotonically growing counter noise in health reports. Delta is computed inside `health_report_update_mqtt`. |
| CP-D7 (resolved) | Connectivity gate uses `mqtt_client_is_connected()` (§5.3, matches original pseudocode) | Originally collapsed into `mqtt_client_publish()`'s own `MQTT_CLIENT_ERR_NOT_CONNECTED` return, since `is_connected()` did not exist when this module was first implemented. Resolved by adding `mqtt_client_is_connected()` to MqttClient (additive, no behaviour change to existing MqttClient callers). |
| CP-D9 | CloudPublisher owns the MQTT connect/reconnect state machine | `mqtt_client.h` explicitly hands this responsibility to CloudPublisher ("does not own the Cloud Connectivity state machine or the reconnect timer") but no module previously called `mqtt_client_connect()` more than once — confirmed on real hardware (a stopped/restarted broker never reconnected). `config->mqtt` is now handed to CloudPublisher unconnected; the existing 1 Hz stats tick attempts the initial connect and every later reconnect via the same `prv_maybe_reconnect()` path, with a fixed backoff (`CP_RECONNECT_RETRY_PERIOD_S`, 30 s) between failed attempts. Deliberately does NOT give MqttClient its own task: MqttClient's header already documents "no thread of its own", and a new task would add RAM to an already-tight GW budget (worst case 90.4 % of 128 KB, MQTT-O1). |

---

## 16. File layout

```
firmware/gateway/application/cloud_publisher/
├── cloud_publisher.h          /* public API — handle, config, error enum */
├── cloud_publisher.c          /* implementation — task, timers, queues   */
└── cloud_publisher_json.c     /* JSON serialisation helpers             */

tests/gateway/application/cloud_publisher/
└── test_cloud_publisher.c     /* Unity + mock stubs                     */
```

---

## Phase H — Readiness review

| # | Check | Status |
|---|---|---|
| H1 | PROVIDES / USES match `components.md` (with F-03 gap documented) | PASS |
| H2 | Root SRS requirements cited | PASS — §2 traceability table |
| H3 | All public API functions have complete Doxygen | PASS |
| H4 | ADT pattern applied | PASS — CP-D1 |
| H5 | Error enum covers all failure modes | PASS — 6 error codes |
| H6 | Activation model documented (timers, queues, notification bits) | PASS — §4 |
| H7 | Open items have named owner and resolution path | PASS |
| H8 | Unit-test plan covers happy + error cases | PASS — 16 test cases |
| H9 | Test file path follows Gateway convention | PASS |
| H10 | P1–P10 compliance reviewed; P7 exception documented | PASS — §12, CP-D3 |
| H11 | Thread safety documented with cross-task path analysis | PASS — §9 |
| H12 | `reset_for_test` hook specified | PASS — §5.7 |
| H13 | JSON schemas documented with requirement traces | PASS — §7 |
| H14 | Topic scheme consistent with MqttClient companion | PASS — §6, CP-D2 |
| H15 | Decisions log complete | PASS — 6 decisions |

**Verdict: PASS — ready for implementation.**

Five open items remain — all are deferred to peer companion documents
(UpdateService, ModbusPoller) or integration-time validation. None
blocks CloudPublisher implementation.
