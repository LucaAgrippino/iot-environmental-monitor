# LLD Companion — CloudPublisher

**Board:** Gateway only.
**Layer:** Application.

Serialises and publishes telemetry, alarm, and health payloads to AWS IoT
Core via `MqttClient`. Routes inbound MQTT commands to the appropriate
handler. Polls `IMqttStats` and reports connectivity metrics via
`IHealthReport`. Integrates with `StoreAndForward` when the cloud
connection is unavailable.

---

## 1. Sources

| Field | Value |
|---|---|
| **Provides** | *(none — top of the stack)* |
| **Uses** | `IMqttClient`, `IMqttStats`, `ISensorService`, `IAlarmService`, `IModbusPoller`, `IStoreAndForward`, `IHealthSnapshot`, `IHealthReport`, `IConfigManager` *(inbound config commands)*, `ILogger` |
| **Hosted in task** | `CloudPublisherTask` priority 3, 768 words / 3 KB |
| **Activation** | Periodic timers + alarm-event queue + inbound-command queue |

**Note — F-03 (UpdateService link).** The sequence diagrams identify that
`CloudPublisher` routes OTA commands to `UpdateService`. The
`components.md` `USES` list does not yet include `UpdateService`. Tracked
as **F-03** for a follow-up `components.md` correction; accounted for in
this companion.

---

## 2. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| Telemetry publish (60 s, QoS 0) | REQ-CC-000, CC-030, NF-111, NF-206, NF-106 | UC-05 |
| Health publish (600 s, QoS 0) | REQ-CC-010, CC-040, NF-112, NF-216, CC-090 | UC-06 |
| Alarm publish (event, QoS 1) | REQ-CC-020, NF-207, NF-113 | UC-09 |
| JSON + schema version | REQ-CC-070, CC-071 | — |
| Separate topics | REQ-CC-080 | — |
| Store-and-forward when offline | REQ-BF-000, BF-010, BF-020, NF-200 | UC-10, UC-11, UC-12 |
| Inbound command routing | REQ-DM-000, DM-002 | UC-15 |
| MQTT stats → IHealthReport | REQ-CC-010 (buffer occupancy, reconnect count, MQTT fail count) | UC-06 |
| TLS / auto-reconnect | REQ-CC-050, CC-060, NF-301 | — |

---

## 3. Activation model

`CloudPublisherTask` blocks on a FreeRTOS task notification (32-bit
bitmask) and a pair of queues:

| Bit / Queue | Source | Period / event |
|---|---|---|
| Bit 0 — `TELEMETRY_TICK` | FreeRTOS software timer | 60 s (REQ-NF-111) |
| Bit 1 — `HEALTH_TICK` | FreeRTOS software timer | 600 s (REQ-NF-112) |
| Bit 2 — `STATS_TICK` | FreeRTOS software timer | 1 Hz |
| Bit 3 — `ALARM_PENDING` | `alarm_queue` non-empty (set by alarm subscriber cb) | Event-driven |
| Bit 4 — `COMMAND_PENDING` | `command_queue` non-empty (set by MqttClient callback) | Event-driven |

Task loop:

```
CloudPublisherTask loop:
    xTaskNotifyWait(...)
    if TELEMETRY_TICK:  publish_telemetry()
    if HEALTH_TICK:     publish_health()
    if STATS_TICK:      poll_stats()
    if ALARM_PENDING:   drain_alarm_queue()
    if COMMAND_PENDING: drain_command_queue()
```

All five timers are created at `cloud_publisher_init()` and start
immediately. They are never stopped during Operational; if the connection
is down, publish calls route to `StoreAndForward` instead of `MqttClient`.

---

## 2. Public API

### 4.1 Telemetry (REQ-CC-000, NF-111, NF-206)

```
publish_telemetry():
    reading = sensor_service->get_latest(sensor_service)
    n = serialise_telemetry(scratch_buf, sizeof scratch_buf, &reading)
    enqueue_or_publish(TOPIC_TELEMETRY, scratch_buf, n, QOS_0)
```

Period: 60 s (configurable via `IConfigProvider.get_telemetry_interval()`
— interval read at each tick, not cached, so remote changes take effect
within one cycle without restart).

### 4.2 Health (REQ-CC-010, NF-112, NF-216, CC-090)

```
publish_health():
    snap = health_snapshot->get(health_snapshot)
    n = serialise_health(scratch_buf, sizeof scratch_buf, &snap)
    enqueue_or_publish(TOPIC_HEALTH, scratch_buf, n, QOS_0)
```

Period: 600 s (10 min, REQ-NF-112). Configurable via
`IConfigProvider.get_health_interval()`.

### 4.3 Alarm (REQ-CC-020, NF-207, NF-113)

`AlarmService` notifies `CloudPublisher` via an alarm-event subscriber
callback registered at init. The callback runs in `SensorTask` context
(where `AlarmService` runs on GW), so it must not block. It enqueues the
alarm payload into the statically allocated `alarm_queue` and sets
`ALARM_PENDING` on `CloudPublisherTask`.

```
/* Runs in SensorTask context */
alarm_event_cb(alarm_event_t *ev):
    xQueueSendFromTask(alarm_queue, ev, 0)   /* non-blocking */
    xTaskNotify(cloud_publisher_task, ALARM_PENDING, eSetBits)

/* Runs in CloudPublisherTask context */
drain_alarm_queue():
    while xQueueReceive(alarm_queue, &ev, 0) == pdTRUE:
        n = serialise_alarm(scratch_buf, sizeof scratch_buf, &ev)
        enqueue_or_publish(TOPIC_ALARMS, scratch_buf, n, QOS_1)
        /* REQ-NF-113: alarm queued for publish ≤ 500 ms from detection.
         * The callback → queue → task-notify path adds < 1 ms on the
         * current task scheduling with no higher-priority contention.  */
```

`alarm_queue` capacity: 8 entries (static, `sizeof(alarm_event_t) × 8`).
If full, the callback drops the oldest entry and logs a warning — aligned
with the store-and-forward model where cloud ordering is best-effort.

### 4.4 `enqueue_or_publish` — connectivity gate

```
enqueue_or_publish(topic, buf, len, qos):
    if mqtt_client->is_connected(mqtt_client):
        rc = mqtt_client->publish(mqtt_client, topic, buf, len, qos)
        if rc != MQTT_OK:
            log_warn("publish failed — buffering")
            store_and_forward->enqueue(saf, topic, buf, len, qos)
    else:
        store_and_forward->enqueue(saf, topic, buf, len, qos)
        /* REQ-BF-000: buffer while offline */
```

`StoreAndForward.enqueue()` is responsible for dropping oldest when full
(REQ-BF-020) per the `store-and-forward.md` companion.

---

## 5. Store-and-forward drain

`CloudPublisher` is notified of reconnection via an `MqttClient`
state-change callback registered at init:

```
/* Runs in CloudPublisherTask context (posted via queue) */
on_mqtt_connected():
    while store_and_forward->dequeue(saf, &entry) == SAF_OK:
        rc = mqtt_client->publish(mqtt_client,
                                  entry.topic, entry.buf,
                                  entry.len, entry.qos)
        if rc == MQTT_OK:
            store_and_forward->confirm(saf)   /* removes entry */
        else:
            break   /* stop drain; re-attempt on next connect event */
    /* REQ-BF-010: chronological order guaranteed by StoreAndForward */
```

Live telemetry/health/alarm publishes arriving during a drain go through
`enqueue_or_publish`, which calls `mqtt_client->publish()` directly
(connection is up). They do not jump the drain queue because
`store_and_forward->enqueue()` is not called when `is_connected()` is true
and the publish succeeds. Chronological order of buffered entries is
preserved (REQ-BF-010).

---

## 6. Inbound command routing

`MqttClient` delivers inbound MQTT messages via a receive callback
registered at init. The callback runs in `MqttClientTask` context (or
equivalent), so it must be non-blocking. It enqueues the command into the
static `command_queue` and sets `COMMAND_PENDING`.

```
/* Runs in MqttClientTask context */
command_received_cb(topic, payload, len):
    cmd_entry_t entry = { .len = len }
    strncpy(entry.topic, topic, sizeof entry.topic)
    memcpy(entry.payload, payload, MIN(len, CMD_PAYLOAD_MAX))
    xQueueSendFromTask(command_queue, &entry, 0)
    xTaskNotify(cloud_publisher_task, COMMAND_PENDING, eSetBits)

/* Runs in CloudPublisherTask context */
drain_command_queue():
    while xQueueReceive(command_queue, &entry, 0) == pdTRUE:
        route_command(&entry)

route_command(entry):
    if topic_matches(entry.topic, "devices/+/commands/config"):
        config_manager->apply_remote_change(cfg_write, entry.payload)
        publish_result(DM-002 ack/rej)
    elif topic_matches(entry.topic, "devices/+/commands/update"):
        update_service->handle_command(update_svc, entry.payload)
    elif topic_matches(entry.topic, "devices/+/commands/control"):
        lifecycle_controller->handle_remote_command(lc, entry.payload)
    else:
        log_warn("unknown command topic: %s", entry.topic)
```

`command_queue` capacity: 4 entries. Commands are rare; overflow implies
a malformed client sending a burst — logged and dropped.

---

## 7. MQTT stats polling — Metric Producer Pattern

```
poll_stats():
    mqtt_stats_t stats;
    mqtt_stats->snapshot(mqtt_stats, &stats)
    health_report->update_mqtt(health_write, &stats, &last_mqtt_stats)
    last_mqtt_stats = stats
```

1 Hz. `health_report->update_mqtt()` is one of `HealthMonitor`'s typed
update functions per the `HealthMonitor` LLD. Delta computation (to
avoid monotonically growing counter noise) is inside `update_mqtt`.
`last_mqtt_stats` is held in the `cloud_publisher_t` context — single-task
access, no mutex.

---

## 8. JSON serialisation

### 8.1 Scratch buffer

```c
#define CP_JSON_BUF_SIZE  4096U   /* matches MQTT_PKT_BUF_SIZE (MQTT-O3) */

typedef struct {
    /* ... provider handles ... */
    char scratch_buf[CP_JSON_BUF_SIZE];   /* static, single-use at a time */
    mqtt_stats_t last_mqtt_stats;
    /* queues, timer handles */
} cloud_publisher_t;
```

All three serialisers write into the same `scratch_buf`. This is safe
because `CloudPublisherTask` is the only thread that serialises, and all
three publish paths are sequential (no concurrency inside the task).

### 8.2 Topic scheme

| Message type | Topic | QoS |
|---|---|---|
| Telemetry | `devices/{serial}/telemetry` | 0 |
| Health | `devices/{serial}/health` | 0 |
| Alarms | `devices/{serial}/alarms` | 1 |
| Command results | `devices/{serial}/results` | 1 |
| Commands (subscribe) | `devices/{serial}/commands/#` | — |

`{serial}` is the MCU UID hex string, read once at init from the
hardware and stored in the `cloud_publisher_t` context.

### 8.3 Telemetry payload (REQ-CC-000, CC-070, CC-071)

```json
{
  "schema_version": "1.0",
  "device_id": "aabbccddeeff112233445566",
  "timestamp": 1705312983,
  "sync_state": "synchronised",
  "temperature_deci_c": 234,
  "humidity_pct": 61,
  "pressure_hpa": 1013,
  "accel_x_mg": 12,
  "accel_y_mg": -5,
  "accel_z_mg": 998,
  "gyro_x_mdps": 0,
  "gyro_y_mdps": 0,
  "gyro_z_mdps": 0,
  "mag_x_mgauss": 120,
  "mag_y_mgauss": -45,
  "mag_z_mgauss": 380,
  "field_device_temp_deci_c": 215,
  "field_device_humidity_pct": 58,
  "field_device_pressure_hpa": 1011,
  "field_device_valid": true
}
```

`field_device_*` fields are the latest values polled from the field
device via `IModbusPoller.get_latest_fd_readings()`. If the field device
is offline, `field_device_valid` is `false` and the other `field_device_*`
fields are omitted (REQ-SA-160 — publish with error flag).

### 8.4 Health payload (REQ-CC-010, CC-090)

All fields from `IHealthSnapshot` mapped to JSON keys. `device_id`
(device serial number) is included per REQ-CC-090. Schema version per
REQ-CC-071.

### 8.5 Alarm payload (REQ-AM-040, CC-070, CC-071)

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

`source` distinguishes gateway-local alarms from field-device alarms
forwarded via Modbus. Set to `"field_device"` for alarms originating on
the FD; `"gateway"` for alarms raised on the GW's own sensors.

---

## 3. Internal design

| State | Access context | Mutex |
|---|---|---|
| `scratch_buf` | `CloudPublisherTask` only | None |
| `last_mqtt_stats` | `CloudPublisherTask` only | None |
| `alarm_queue` | Write: `SensorTask` (callback); Read: `CloudPublisherTask` | FreeRTOS queue |
| `command_queue` | Write: `MqttClientTask`; Read: `CloudPublisherTask` | FreeRTOS queue |
| Provider handles | Set at init, immutable | None |

`SensorTask` → `alarm_queue` is the only cross-task write path in this
component. It uses `xQueueSendFromTask` (non-blocking) to avoid
blocking `SensorTask`.

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

## 6. Error and fault behaviour

```c
typedef enum {
    CP_OK = 0,
    CP_ERR_NULL_ARG,
    CP_ERR_NOT_INIT,
    CP_ERR_SERIALISE,        /* snprintf produced truncated output */
    CP_ERR_PUBLISH_FAILED,   /* MqttClient returned error         */
    CP_ERR_SAF_FULL,         /* StoreAndForward queue full        */
} cloud_publisher_err_t;
```

- `CP_ERR_SERIALISE` — JSON exceeded `CP_JSON_BUF_SIZE`. Logged; message
  dropped. Not fatal. (In practice, health payload is the largest at ~800
  bytes — well within 4 KB.)
- `CP_ERR_PUBLISH_FAILED` — MQTT publish error when `is_connected()` was
  true. Route to `StoreAndForward` as fallback.
- `CP_ERR_SAF_FULL` — `StoreAndForward.enqueue()` rejected the entry.
  Log at warn; message dropped. `HealthReport` counter incremented.

---

## 11. Initialisation

```c
cloud_publisher_err_t
cloud_publisher_init(cloud_publisher_t      *self,
                     IMqttClient            *mqtt,
                     const IMqttStats       *mqtt_stats,
                     const ISensorService   *sensors,
                     const IAlarmService    *alarms,
                     const IModbusPoller    *poller,
                     IStoreAndForward       *saf,
                     const IHealthSnapshot  *health_read,
                     IHealthReport          *health_write,
                     IConfigManager         *cfg_write,
                     IUpdateService         *update_svc,
                     ILifecycle             *lifecycle,
                     ILogger                *log);
```

Steps:
1. Store all handles; read device serial from hardware.
2. Create `alarm_queue` (8 × `alarm_event_t`) and `command_queue`
   (4 × `cmd_entry_t`).
3. Create the three FreeRTOS software timers (telemetry, health, stats).
4. Register `alarm_event_cb` with `AlarmService`.
5. Register `command_received_cb` and `on_mqtt_connected` with `MqttClient`.
6. Create `CloudPublisherTask`.

---

## 12. Memory and sizing

| Item | Size (estimate) |
|---|---|
| `cloud_publisher_t` context | ~120 B |
| `scratch_buf` (JSON) | 4 KB |
| `alarm_queue` (8 × ~64 B) | ~512 B |
| `command_queue` (4 × ~260 B) | ~1 KB |
| Software timer handles (×3) | ~48 B |
| **Total RAM** | **~5.7 KB** |

Stack: 768 words / 3 KB. Peak usage occurs during `serialise_health()`,
which formats ~20 fields into `scratch_buf` via `snprintf`. Estimated
peak stack frame < 512 B.

---

## 7. Unit-test plan

### 13.1 Unit tests — `tests/application/test_cloud_publisher.c`

| Suite | Coverage |
|---|---|
| Init | Null-arg rejection; queues created; callbacks registered |
| Telemetry — connected | `publish_telemetry` calls `sensor_service->get_latest`, serialises, calls `mqtt_client->publish` with telemetry topic and QoS 0 |
| Telemetry — disconnected | `is_connected()` returns false → `store_and_forward->enqueue` called; `mqtt_client->publish` NOT called |
| Health — connected | Same pattern; QoS 0; health topic; includes serial number |
| Alarm — connected | Alarm enqueued via callback; `drain_alarm_queue` publishes at QoS 1 on alarm topic |
| Alarm — disconnected | Alarm enqueued to `StoreAndForward` at QoS 1 |
| Store-and-forward drain | `on_mqtt_connected` triggers drain loop; `dequeue → publish → confirm` for each entry |
| Drain stops on publish failure | Publish returns error mid-drain → drain stops; entry not confirmed |
| Inbound command — config | Command on config topic → `IConfigManager.apply_remote_change` called; result published |
| Inbound command — unknown topic | Unknown topic → logged; no crash |
| Stats polling | `poll_stats` calls `IMqttStats.snapshot` and `IHealthReport.update_mqtt` |
| JSON serialise truncation | `CP_JSON_BUF_SIZE` reduced to tiny value; `CP_ERR_SERIALISE` returned; no buffer overrun |
| Alarm queue full | Overflow triggers log warn; oldest entry dropped; component stable |
| Configurable intervals | `get_telemetry_interval()` changed between ticks; new period used on next tick |

### 13.2 Integration tests — on target

| Test | Setup |
|---|---|
| End-to-end telemetry | System running; verify AWS IoT Core receives telemetry message within 65 s; verify JSON schema and values |
| Alarm end-to-end | Drive GW sensor above threshold; verify alarm appears in IoT Core within 500 ms (REQ-NF-113) |
| Store-and-forward | Disconnect WiFi; verify telemetry is enqueued; reconnect; verify messages drain in order |
| Buffer full drop | Fill buffer; verify oldest entry is discarded; verify no crash |
| Remote config command | Publish config command to IoT Core; verify FW applies and acknowledges |

---

## 8. Open items

| ID | Item |
|---|---|
| **CP-O1** | `IUpdateService` interface not yet defined (UpdateService LLD pending). `cloud_publisher_init` accepts it as `void *update_svc` until UpdateService companion is done. |
| **CP-O2** | `IModbusPoller.get_latest_fd_readings()` — confirm this method exists in the ModbusPoller LLD companion when produced; the telemetry serialiser depends on it. |
| **CP-O3** | MQTT-O3 (MQTT_PKT_BUF_SIZE 4096) — verify the largest possible telemetry payload (all sensors + field device values + metadata) fits within 4096 bytes with room for MQTT overhead. |
| **CP-O4** | `field_device_valid = false` serialisation — confirm omitting `field_device_*` fields (vs including them as `null`) is acceptable to the cloud consumer. |
| **F-03** | `components.md` GW `CloudPublisher` USES list — add `UpdateService` and `ILifecycle`. |

---

## 15. References

- `docs/components.md` (GW CloudPublisher, AlarmService, StoreAndForward).
- `docs/sequence-diagrams.md` SD-03a (telemetry), SD-03b (health), SD-04
  (store-and-forward), SD-05 (alarm), SD-06a (OTA), SD-07 (remote config).
- `docs/state-machines.md` Machine 2 (Cloud Connectivity, GW).
- `docs/lld/mqtt-client.md` (defines `IMqttClient`, `IMqttStats`).
- `docs/lld/store-and-forward.md` (defines `IStoreAndForward`).
- `docs/lld/health-monitor.md` (defines `IHealthSnapshot`, `IHealthReport`).
- `docs/architecture-principles.md` P7 (pull-based), P8 (no dynamic alloc).

---

*Companion produced during the LLD Application Phase. Authored by Luca
Agrippino; reviewed against the V-Model gate criteria for LLD.*
