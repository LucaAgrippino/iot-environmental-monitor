# CloudPublisher Reconciliation — Pass H decisions and implementation specification (WP1)

**Document:** `docs/lld/gate-review/cloud-publisher-reconciliation.md`
**Version:** 1.0 (supersedes the session 1 analysis, same path)
**Status:** Implemented 2026-10-28 in PR-0 (#111), PR-A (#112), PR-B (#113), PR-C (#114) and PR-D; hardware cases TC-HW-CP-014..017 to be run
**Board:** Gateway (B-L475E-IOT01A)
**Date:** 2026-10-28 · **Repo state reviewed:** `main` @ 4efc3aa
**Closes when implemented:** LC-O13, SAF-O3, TS-O6, CP-O2, CP-O4, MBP-O3, F-03 (CloudPublisher part). **Partly:** CP-O5 (disconnect callback), CFG-O8 (CloudPublisher as a consumer).

The session 1 analysis (findings, symbol table, type table) was checked
against the code and is correct. It is kept in Appendix A. This version
records the **decisions** (§2), three **extra findings** from the review
(§1), and the **implementation specification**, split into five PRs (§3–§8).

---

## 1. Extra findings from the review

| # | Finding | Evidence | Consequence |
|---|---|---|---|
| R-1 | **`/config` and `/control` are never subscribed.** Only the OTA topics are (`prv_subscribe_ota`). Remote configuration (UC-15) and remote restart (UC-17) cannot reach the device, whatever the routing does | `cloud_publisher.c:579–598`; `mqtt_topic_config.h` has no `/control` suffix | Subscribe to all command topics after every connect (§5.9) |
| R-2 | **The health payload carries 4 of the ~15 metrics REQ-CC-010 requires.** The real `health_snapshot_t` has most of the rest. REQ-NF-503 (firmware version in health) is not met, and UpdateService relies on it to reconcile reports after a reboot (US-D9) | `cp_serialise_health()`; `health_monitor.h:175–223` | Health payload 1.1 (§6.3). Metrics missing from the snapshot become an open item |
| R-3 | **No payload has a real timestamp.** All four use `xTaskGetTickCount()` (ms since boot), unflagged. REQ-TS-040 / REQ-NF-212 require uptime-based time to be **flagged** unsynchronised, and wall-clock time when synchronised | `cloud_publisher_json.c:149,168,192,211` | `time_provider_get()` on every payload, with `sync_state` (§6.1) |

---

## 2. Decisions (Pass H)

| ID | Question | Decision | Rationale |
|---|---|---|---|
| **CP-D18** (D-1) | Who decodes and keeps the Field Device readings? | **ModbusPoller** decodes 0x0010–0x0012 in `forward_response_to_consumers()` into a per-slave cache and exports `modbus_poller_get_fd_reading()` (§4). Values keep the **register units** (centi-°C, centi-%RH, deci-hPa, the same as SensorService and `config_params_t`), with **per-channel validity** from the sentinels. "Online" is the existing link hysteresis (REQ-NF-215). CloudPublisher converts to wire units | Option (a). The poller owns the profile, the register span and the link state; a separate data-store module would have one reader (P8). Pull access (P7). One unit system inside the Gateway |
| **CP-D19** (D-2) | Health events | Drop `ALARM_PUBLISHED`, `ALARM_BUFFERED` and `SAF_FULL`; nothing in the snapshot consumes them. CloudPublisher **pushes `HEALTH_EVENT_CLOUD_CONNECTED` / `_DISCONNECTED`** on every transition. The REQ-NF-113 observation for TC-HW-CP-011 moves to a CloudPublisher-local seam: `LOG_DEBUG` with the outcome plus a TEST-only counter | Option (b). An event no consumer reads is not a metric (P5) |
| **CP-D20** (D-2, CP-O5) | `disconnect_cb` | CloudPublisher exports a context-free `cloud_publisher_on_disconnect(void)`, the twin of `cloud_publisher_on_message()`. It runs in CloudPublisherTask (inside `mqtt_client_process()`), so it records the transition directly | MqttClient requires the callback; CloudPublisher already owns the connection state |
| **CP-D21** (D-3) | `/config` | **CloudPublisher parses** (it owns deserialisation, `components.md`) a **flat JSON object of integer values**. Keys are the `config_params_t` field names, in their native units. Only the **Tier 2** keys are accepted (§5.6; REQ-NF-306). It applies them **all or nothing** with `set_param_deferred` (≈20 µs each, never blocks the MQTT task), reverting in reverse order on the first failure. The result goes on `/config/result` (§6.4). **LifecycleController persists** through `persist_pending()` on its idle tick and before every reset (§7) | Option (a), but with the blocking write moved out of CloudPublisherTask (CFG-O7/O9). No string parameter is Tier 2, so the parser needs integers only: small, deterministic, no library. Reverting through the same states again keeps every intermediate state valid |
| **CP-D22** (D-4, CP-O4) | Cloud schema | **Schema 1.1** for all four payloads: `timestamp` = `time_provider_get().epoch` (epoch seconds when synchronised, uptime seconds otherwise) plus `sync_state` everywhere; an invalid channel is **JSON `null`** (no `invalid_channels` list); the Field Device is a nested object with `online` and the same channel names and units as the Gateway channels (§6.2) | A self-describing schema: a real 0 can no longer be confused with an invalid 0, and one convention covers both boards. There is no cloud consumer yet, so the change is free now. **Luca may veto before session 2**; if so, only §6 changes |
| **CP-D23** (D-5) | TimeService hook | Extract `itime_service.h` (vtable only). CloudPublisher calls `trigger_retry()` on **every** transition to connected, including the first after boot. `get_sync_state` is not used: TimeProvider's timestamp already carries the state | REQ-TS-0E1. The boot NTP attempt usually runs before WiFi is up (TS-D3). Interface-segregated headers are the project default |
| **CP-D24** (D-6) | Adjacent placeholders (SAF, CFL health calls) | Fix them in **this work package, as their own `fix:` PR** (§3, PR-B) | They are silent no-op weak functions, i.e. silent failures (BARR-C error handling); they belong to the same "link a real image" goal |
| **CP-D25** (D-7) | Config structure | §5.1: one `health` handle (the read and write sides are the same instance); the two ConfigService vtables; `time`; `lifecycle` as `const ilifecycle_t *` (vtable only, for `get_state`); `fd_slave_addr`; a `control_handler` function pointer replacing `lifecycle_handle_t`; `fw_version`. No placeholder type is left | Removes the reversed include (`lifecycle_wiring.h` → `cloud_publisher.h` just for a typedef) and the harness symbol collision |
| **CP-D26** (R-1) | Subscriptions | After every connect, subscribe to `/config` (QoS 1), `/control` (QoS 1) and, when OTA is wired, `/ota` (QoS 1) and `/ota/data` (QoS 0). Retried each stats tick until all hold | Without them UC-15 and UC-17 cannot work |
| **CP-D27** | Remote config while a local edit is open | Reject with `busy` unless `lifecycle->get_state()` is OPERATIONAL | A local cancel restores LifecycleController's snapshot, which would silently undo an acknowledged remote change (REQ-LI-120) |
| **CP-D28** | Payload buffer | `CP_JSON_BUF_SIZE` 4096 → **2048**, with `_Static_assert(CP_JSON_BUF_SIZE <= SAF_PAYLOAD_MAX)` | Every payload fits (health 1.1 is about 900 B); the real SAF limit is 4010 B; saves 2 KB of RAM |
| **CP-D29** | `CP_ERR_SAF_FULL` | Kept as a code, with a new meaning: "StoreAndForward refused the entry (flash error or too large); the message is lost". LOG_ERROR, no health event | The real SAF drops the oldest entry and succeeds when full (REQ-BF-020); the overflow is already counted through CircularFlashLog |
| **CP-D30** | Results topics | One rule: `dt/iotmonitor/<id>/<command>/result`. `/results` is replaced by `/config/result`; `/ota/result` is unchanged. `/control` results are **not** in this WP (new open item CP-O8, §9) | Consistent naming; no cloud consumer exists yet |
| **CP-D31** | Interval changes | CloudPublisher registers one ConfigService change callback. On `TELEMETRY_INTERVAL` or `HEALTH_INTERVAL` it notifies its own task (new bit), which calls `xTimerChangePeriod()` | Covers changes from any source (cloud or console); CloudPublisher's part of CFG-O8 |

---

## 3. Work split (five PRs, in this order)

| PR | Branch | Type | Content | Depends on |
|---|---|---|---|---|
| **PR-0** | `docs/hld-cp-reconciliation` | `docs:` | HLD amendments (§10). Model first | — |
| **PR-A** | `feature/phase-4-gw-modbus_poller_fd_reading` | `feat:` | ModbusPoller FD cache and `modbus_poller_get_fd_reading()` (§4); `modbus-poller-lld.md` v1.1 | PR-0 |
| **PR-B** | `fix/gw-saf-cfl-health-calls` | `fix:` | Replace the two weak health placeholders in StoreAndForward and CircularFlashLog with the real HealthMonitor calls (§8) | — |
| **PR-C** | `feature/phase-4-gw-lc_config_persist` | `feat:` | `itime_service.h` extraction; LifecycleController `persist_pending` on idle and before reset (§7) | — |
| **PR-D** | `feature/phase-4-gw-cloud_publisher_reconcile` | `feat:` | CloudPublisher (§5, §6), harness, LLD v2.0 | PR-0, A, B, C |

PR-A, PR-B and PR-C are independent of each other. PR-D is the one that
closes LC-O13.

---

## 4. PR-A — ModbusPoller Field Device reading (closes MBP-O3, CP-O2)

### 4.1 API (append to `modbus_poller.h`)

```c
/** @brief Latest decoded Field Device sensor registers (0x0010-0x0012), register units. */
typedef struct
{
    int16_t  temperature_centi_c;   /**< x0.01 degC; meaningful only if temperature_valid.  */
    uint16_t humidity_centi_pct;    /**< x0.01 %RH.                                          */
    uint16_t pressure_deci_hpa;     /**< x0.1 hPa.                                           */
    bool     temperature_valid;     /**< false: sentinel 0x8000, or never received.          */
    bool     humidity_valid;        /**< false: sentinel 0xFFFF, or never received.          */
    bool     pressure_valid;        /**< false: sentinel 0xFFFF, or never received.          */
    bool     link_online;           /**< Hysteresis state (REQ-NF-103/-104/-215).            */
    bool     has_data;              /**< At least one response decoded since boot.           */
    uint32_t updated_tick;          /**< xTaskGetTickCount() of the last decode.             */
} modbus_fd_reading_t;

/**
 * @brief Copy the latest decoded readings of one slave.
 * @param[in]  slave_addr  Provisioned slave address.
 * @param[out] out         Receives the copy.
 * @return MODBUS_POLLER_ERR_OK; _NOT_INIT; _NULL_ARG; _REJECTED if the slave is not provisioned.
 *         A provisioned slave with no response yet returns OK with has_data = false and all
 *         channels invalid.
 * @note Threading: any task. A short critical section guards the copy (about 20 bytes).
 */
modbus_poller_err_t modbus_poller_get_fd_reading(uint8_t slave_addr, modbus_fd_reading_t *out);
```

### 4.2 Behaviour

- The cache lives in the existing per-slave table (`modbus_slave_state_t` gains the reading, or a parallel array with the same index), so there is still no heap and nothing new to size.
- `forward_response_to_consumers(profile, response)`:
  1. Requires `response->data_len == 1 + 2 * profile->input_reg_count` (FC04 byte count plus data), else LOG_WARN and no update.
  2. For each of 0x0010, 0x0011 and 0x0012 that lies inside `[input_reg_start, input_reg_start + input_reg_count)`, decode the big-endian word at its offset. Valid = not the sentinel (`0x8000` for int16, `0xFFFF` for uint16, register map §5.4). A register outside the span is invalid.
  3. Write the values, the flags, `has_data = true` and `updated_tick` inside `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()`.
- `link_online` is copied from the slave state at read time; it is not stored twice.
- A poll failure does not clear the values. The consumer must use `link_online` (REQ-SA-160 semantics stay in CloudPublisher).

### 4.3 Tests (host)

| TC | Behaviour |
|---|---|
| TC-MBP-FD-01 | Valid response → three values, all valid, `has_data` true |
| TC-MBP-FD-02 | Each sentinel alone → only that channel invalid |
| TC-MBP-FD-03 | Wrong `data_len` → cache unchanged, WARN |
| TC-MBP-FD-04 | Profile span 0x0010 count 2 → pressure invalid |
| TC-MBP-FD-05 | Never polled → OK, `has_data` false, all invalid |
| TC-MBP-FD-06 | Unprovisioned slave → `REJECTED`; NULL out → `NULL_ARG`; before init → `NOT_INIT` |
| TC-MBP-FD-07 | Three failed polls → `link_online` false, last values kept |

---

## 5. PR-D — CloudPublisher specification

### 5.1 Configuration (replaces the current struct; no placeholder type remains)

```c
typedef void (*cp_control_handler_t)(const uint8_t *payload, uint32_t len);

typedef struct
{
    mqtt_client_handle_t        mqtt;
    saf_handle_t                saf;              /* store_and_forward.h                    */
    health_monitor_handle_t     health;           /* HealthMonitor (snapshot + report)      */
    const iconfig_provider_t   *cfg_read;         /* config_service_get_provider()          */
    const iconfig_manager_t    *cfg_write;        /* config_service_get_manager()           */
    const itime_service_t      *time;             /* NULL = no NTP retry on reconnect       */
    const ilifecycle_t         *lifecycle;        /* get_state only (CP-D27)                */
    cp_control_handler_t        control_handler;  /* NULL = /control not subscribed         */
    uint8_t                     fd_slave_addr;    /* 0 = no Field Device in telemetry       */
    uint32_t                    fw_version;       /* major<<24 | minor<<16 | patch          */
    const iupdate_service_t    *update_ops;       /* unchanged (#106)                       */
    update_service_t           *update;           /* unchanged (#106)                       */
    mqtt_connect_cfg_t          mqtt_connect_cfg; /* unchanged                              */
} cloud_publisher_config_t;
```

`cloud_publisher_create()` returns `CP_ERR_NULL_PTR` if `mqtt`, `saf`,
`health`, `cfg_read`, `cfg_write` or `lifecycle` is NULL. `time`,
`control_handler`, `update_ops`/`update` (both or neither) and
`fd_slave_addr` are optional.

`lifecycle_wiring` supplies `lifecycle_handle_remote` with the new
signature `void lifecycle_handle_remote(const uint8_t *payload, uint32_t len)`
(the unused handle is dropped). `lifecycle_wiring.h` no longer needs
`lifecycle_handle_t`. The CloudPublisher harness passes its own handler
through the config, so its global `lifecycle_handle_remote` goes and the
symbol collision with it.

### 5.2 Header hygiene

- Delete the whole "Placeholder dependency types" block of `cloud_publisher.h` except CloudPublisher's own wire model (`cp_alarm_*`, `cp_alarm_event_t`), which stays.
- Include the real headers: `store_and_forward.h`, `health_monitor.h`, `iconfig_provider.h`, `iconfig_manager.h`, `itime_service.h` (PR-C), `ilifecycle.h`, `modbus_poller.h`, `time_provider.h`.
- `cp_alarm_event_t` gains `time_provider_ts_t timestamp`, filled in the AlarmService callback from the reading's timestamp (REQ-AM-040, time of detection).

### 5.3 Call mapping (final)

| Placeholder | Replacement |
|---|---|
| `config_provider_get_telemetry_interval_s` / `_health_interval_s` | `cfg_read->get_params(cfg_read->ctx)->telemetry_interval_ms` / `health_interval_ms`, read once per use, converted with `pdMS_TO_TICKS()` |
| `config_manager_apply` | §5.6 |
| `health_snapshot_get` | `health_monitor_poll(health)` then `health_monitor_get_snapshot(health, &s_health_snap)`. `s_health_snap` is a file-scope static (pool size 1), not on the stack |
| `health_report_update_mqtt` | `health_monitor_update_mqtt_stats(health, &stats)`. Remove `last_mqtt_stats` |
| `health_report_push_event` | Removed (CP-D19). New calls are `CLOUD_CONNECTED` / `_DISCONNECTED` only (§5.8) |
| `modbus_poller_get_latest_fd` | `modbus_poller_get_fd_reading(fd_slave_addr, &fd)` if `fd_slave_addr != 0` |
| `store_and_forward_enqueue` | `saf_enqueue(saf, topic, buf, (uint16_t) len, (uint8_t) qos)`. `len` cannot exceed 2048 (CP-D28). `SAF_ERR_OK` → `CP_ERR_BUFFERED`; any other code → `CP_ERR_SAF_FULL` + LOG_ERROR with the code (CP-D29) |
| `store_and_forward_dequeue` | `saf_dequeue(saf, &entry)` with `const saf_entry_t *entry`. The entry is used **before** any other SAF call; `SAF_ERR_EMPTY` ends the drain |
| `store_and_forward_confirm` | `saf_confirm(saf)` after a successful publish. On `SAF_ERR_FLASH`: LOG_WARN and stop the drain for this tick (the entry is re-sent later; a duplicate is acceptable at QoS 1) |
| `lifecycle_handle_remote` | `control_handler(payload, len)` if not NULL |

### 5.4 Notification bits

Bit 8 is used by CPC-5. Add **bit 9 `CP_NOTIFY_CONFIG_CHANGED`** (CP-D31)
and include it in `CP_NOTIFY_ALL_BITS`.

### 5.5 Topics (`mqtt_topic_config.h`)

Add `MQTT_TOPIC_SUFFIX_CONTROL "/control"` and
`MQTT_TOPIC_SUFFIX_CONFIG_RESULT "/config/result"`. Build
`topic_config_result` from them and remove `topic_results` and the `/results`
literal (CP-D30). Routing stays exact-suffix (CPC-2).

### 5.6 Remote configuration (`/config`, CP-D21, CP-D27)

**Payload** (≤ `CP_CMD_PAYLOAD_MAX_LEN` = 128 B):

```json
{"req":17,"telemetry_interval_ms":30000,"temp_alarm_high":4500}
```

`req` (optional, uint32) is echoed in the result for correlation. Other
members are parameters. At most `CP_CFG_MAX_KEYS` = 8 parameters.

**Remotely settable keys (Tier 2, Vision §10; REQ-NF-306):**

| Key | `config_param_id_t` | C type |
|---|---|---|
| `polling_interval_ms` | `CONFIG_PARAM_POLL_INTERVAL` | `uint32_t` |
| `modbus_poll_period_ms` | `CONFIG_PARAM_MODBUS_POLL_PERIOD` | `uint32_t` |
| `telemetry_interval_ms` | `CONFIG_PARAM_TELEMETRY_INTERVAL` | `uint32_t` |
| `health_interval_ms` | `CONFIG_PARAM_HEALTH_INTERVAL` | `uint32_t` |
| `temp_alarm_high`, `temp_alarm_low`, `temp_hysteresis` | `CONFIG_PARAM_TEMP_*` | `int16_t` |
| `humidity_alarm_high`, `humidity_alarm_low`, `humidity_hysteresis` | `CONFIG_PARAM_HUMIDITY_*` | `uint16_t` |
| `pressure_alarm_high`, `pressure_alarm_low`, `pressure_hysteresis` | `CONFIG_PARAM_PRESSURE_*` | `uint16_t` |

Every other `config_params_t` field name is recognised and **rejected
with `not_remote`**: `filter_alpha`, the six `*_range_*`,
`modbus_slave_addr`, `mqtt_broker`, `mqtt_port`, `ntp_servers`,
`ntp_server_count`. Sensor ranges and filter alpha are not Tier 2
examples in the Vision, so they stay local (conservative choice).

**Algorithm (CloudPublisherTask; no blocking call):**

```
if lifecycle->get_state() != OPERATIONAL      → reject "busy"           (CP-D27)
parse into pairs[≤8] of (id, int64 value)     → "bad_json" | "unknown_key" | "not_remote" | "too_many_keys"
for each pair: value fits the C type?         → else "invalid_value" + key
for i in 0..n-1:
    old[i] = current value of pairs[i].id (from get_params)
    rc = cfg_write->set_param_deferred(ctx, id, &typed_value)
    rc != OK → for j = i-1 down to 0: set_param_deferred(old[j]) ; reject "invalid_value" + key
publish result "applied" (QoS 1, /config/result, through the SAF gate)
```

Nothing is written to flash here. LifecycleController persists within one
idle tick (§7). The result therefore means "validated and applied in
RAM; persisted within about 1 s" (REQ-DM-002 confirmation, REQ-DM-090
persistence). Keys are applied **in message order**; cross-field rules
(low < high, hysteresis) are checked by ConfigService against the current
values. The cloud tool must order keys so each intermediate state is
valid (for example raise `*_high` before `*_low` when moving a band up).
This is documented in the wire contract (§6.4).

**Parser:** a hand-written scanner for one flat object: string keys,
signed decimal integer values, optional whitespace. Anything else
(nested object, string value, float, duplicate key, trailing data) is
`bad_json`. No library, no recursion, bounded by the 128-byte input.

### 5.7 Change callback (CP-D31)

`cloud_publisher_create()` calls `config_service_register_change_cb(prv_config_changed)`.
The callback (runs in the caller's task) does only
`xTaskNotify(task, CP_NOTIFY_CONFIG_CHANGED, eSetBits)` for
`TELEMETRY_INTERVAL` and `HEALTH_INTERVAL`. The task then re-reads both
intervals and calls `xTimerChangePeriod(timer, pdMS_TO_TICKS(ms), 0)` for
each that changed. If registration fails, `create` returns
`CP_ERR_NO_RESOURCE` (ConfigService allows 4 callbacks).

### 5.8 Connection transitions (CP-D19, CP-D20, CP-D23)

A task-only `bool was_connected` in the instance.

| Transition (detected in `prv_maybe_reconnect()` or `cloud_publisher_on_disconnect()`) | Actions |
|---|---|
| disconnected → connected | `link_up = true`; `health_monitor_push_event(health, HEALTH_EVENT_CLOUD_CONNECTED, 0)`; `time->trigger_retry(time->ctx)` if `time`; subscriptions reset (§5.9) |
| connected → disconnected | `link_up = false`; `push_event(HEALTH_EVENT_CLOUD_DISCONNECTED, 0)`; subscriptions marked lost |

`cloud_publisher_on_disconnect(void)` reaches the instance through the
pool entry, like `cloud_publisher_on_message()`. It only records the
transition; reconnecting stays with the stats tick (CP-D9, CP-D13).

### 5.9 Subscriptions (CP-D26)

`prv_subscribe_ota()` becomes `prv_subscribe_commands()`. After each
connect it subscribes, in order: `/config` (QoS 1); `/control` (QoS 1)
if `control_handler`; `/ota` (QoS 1) and `/ota/data` (QoS 0) if
`update_ops`. One `subscribed` flag per topic, so a partial failure
retries only what is missing, on the next stats tick.

### 5.10 Unchanged

The alarm path and CP-D12 ordering, the OTA flow (#106, CPC-1..6),
reconnect and backoff (CP-D9, CP-D13), `is_ready`, `flush`, and the
non-blocking publish (MQTT-D13).

---

## 6. Wire contract — schema 1.1 (CP-D22)

Common to all four payloads:

```json
"schema_version":"1.1","device_id":"<serial>","timestamp":<uint32>,"sync_state":"synchronised"|"unsynchronised"
```

`timestamp` and `sync_state` come from one `time_provider_get()` call at
serialisation. Epoch seconds when synchronised; uptime seconds otherwise
(REQ-TS-040, REQ-NF-212). On a TimeProvider error the payload is still
sent with `"sync_state":"unsynchronised"` and `timestamp` = uptime from
the tick count, and a LOG_WARN.

### 6.1 Units

Unchanged Gateway wire units: `temperature_deci_c`, `humidity_pct`,
`pressure_hpa`, `accel_*_mg`, `gyro_*_mdps`, `mag_*_mgauss`, with the
existing rounding (`prv_div_round`) and clamping. Field Device values are
converted from register units with the same functions.

### 6.2 Telemetry

```json
{ <common>,
  "temperature_deci_c":231,"humidity_pct":45,"pressure_hpa":1013,
  "accel_x_mg":-12,"accel_y_mg":3,"accel_z_mg":1002,
  "gyro_x_mdps":0,"gyro_y_mdps":null,"gyro_z_mdps":0,
  "mag_x_mgauss":120,"mag_y_mgauss":-30,"mag_z_mgauss":410,
  "field_device":{"online":true,"temperature_deci_c":215,"humidity_pct":40,"pressure_hpa":null} }
```

- An invalid Gateway channel is `null` (REQ-SA-160, REQ-NF-208). `invalid_channels` is removed.
- `field_device` always has all four members. `online` = `link_online && has_data`. When `online` is false, the three values are `null`. When `fd_slave_addr == 0`, `"field_device":null`.

### 6.3 Health

```json
{ <common>,
  "fw_version":"1.2.0","uptime_s":3600,"lifecycle_state":"operational","last_reset_cause":"soft",
  "sensor_fail_count":0,"alarm_raise_count":2,"config_write_failed":false,
  "modbus_transactions_ok":3590,"modbus_timeouts":4,"modbus_link_online":true,
  "mqtt_publishes_sent":120,"mqtt_publishes_failed":0,"mqtt_reconnect_count":1,
  "wifi_rssi_dbm":-61,"cloud_connected":true,
  "buffer_entry_count":0,"buffer_overflow_count":0,
  "ntp_sync_fail_count":0,"last_ntp_sync_epoch":1793200000,
  "stack_free_words":[120,65,300,410,586,90,200,180] }
```

- Every field is a direct copy of `health_snapshot_t` (enums as lower-case strings).
- `stack_free_words` lists the registered tasks in registration order. Its length is the number registered, so it is not padded with zeros.
- REQ-CC-010 metrics **not in the snapshot** (free heap, CPU load, MCU temperature, Modbus CRC count) are not invented here; they become CP-O9 (§9). REQ-NF-503 is met by `fw_version`; REQ-CC-090 by `device_id`.

### 6.4 Config result — `dt/iotmonitor/<id>/config/result` (QoS 1)

```json
{ <common>,"event":"config","req":17,"result":"applied" }
{ <common>,"event":"config","req":17,"result":"rejected","reason":"invalid_value","key":"temp_alarm_high" }
```

Reasons: `busy`, `bad_json`, `unknown_key`, `not_remote`,
`too_many_keys`, `invalid_value`. `req` is omitted if absent in the
command; `key` only for `not_remote`, `unknown_key` and `invalid_value`.
Note for the cloud tool: keys are applied in message order (§5.6).

### 6.5 Alarm

Unchanged fields plus the common block. `timestamp` and `sync_state` come
from the reading that raised the alarm (§5.2), not from serialisation time.

### 6.6 OTA

`/ota/request` and `/ota/result` (#106) gain the common block and
`schema_version` 1.1. Field names are otherwise unchanged;
`scripts/ota_server.py` must accept the extra members.

All payloads must stay ≤ `CP_JSON_BUF_SIZE` (2048). A serialiser that
would exceed it returns 0, and the caller logs and drops, as today.

---

## 7. PR-C — LifecycleController persistence and `itime_service.h`

1. **`itime_service.h`:** move the `itime_service_t` typedef out of `time_service.h` into `application/time_service/itime_service.h` (vtable and `time_service_sync_t` only). `time_service.h` includes it, so no consumer breaks.
2. **`lc_config_manager_if_t`** gains `lc_dep_status_t (*persist_pending)(void *ctx)`. The `lifecycle_wiring` adapter maps it to `config_service_persist_pending()`.
3. **LifecycleController** calls `persist_pending` (all three below):
   - in the Operational loop pass after the event wait (no-op and cheap when nothing is pending);
   - before every `soft_reset` (restart confirmation, `OTA_RESET_REQUESTED`, rollback);
   - before `enter_faulted()`'s LED latch.
   
   A failure is LOG_WARN; the change stays pending and is retried on the next pass (ConfigService contract). It is never a fault.
4. Tests: persist called once per idle pass; called before each reset path; a failure does not change the state.
5. `lifecycle-controller-lld.md`: new decision LC-D23 (LifecycleController owns the deferred-config flush, CFG-D16 "callers own that call").

---

## 8. PR-B — StoreAndForward and CircularFlashLog health calls (CP-D24)

| Module | Remove | Replace with |
|---|---|---|
| StoreAndForward | weak `health_update_buffer_occupancy(handle, count, capacity, percent)` | `health_monitor_update_buffer_occupancy(handle, entry_count)` (capacity and percent are dropped: the snapshot has no field for them) |
| CircularFlashLog | weak `health_report_push_buffer_event(handle, cfl_health_event_t)` | `health_monitor_push_event(handle, HEALTH_EVENT_BUFFER_OVERFLOW, 0)` |

- Both modules include `health_monitor.h`. A NULL health handle stays allowed (a call is then skipped, as today), so the bring-up harnesses keep working.
- Update their host tests to expect the real calls (CMock), and their LLD companions (close SAF-O3's health part; CFL decision note).

---

## 9. Open items after this WP

| ID | Item | Status |
|---|---|---|
| LC-O13, SAF-O3, TS-O6, CP-O2, CP-O4, MBP-O3 | Closed by PR-A..PR-D | **Close** |
| F-03 | `components.md` CloudPublisher USES (PR-0) | **Close** |
| CP-O5 | Disconnect callback (CP-D20); `main()` binding remains WP4 | Close in CloudPublisher; binding tracked by LC-O15 |
| CFG-O8 | CloudPublisher part done (CP-D31). SensorService, AlarmService, TimeService and ModbusPoller still do not react to changes | Open (WP12) |
| **CP-O8** (new) | `/control` has no result topic: UC-17 step 3 ("acknowledge receipt and request confirmation") is not reported to the cloud | Open |
| **CP-O9** (new) | REQ-CC-010 metrics not in `health_snapshot_t`: free heap, CPU load, MCU temperature, Modbus CRC errors | Open (HealthMonitor) |
| **CP-O10** (new) | Remote config is applied in message order; the cloud tool must order band changes (§5.6) | Accepted, documented |

---

## 10. PR-0 — HLD amendments (model first)

| Artefact | Change |
|---|---|
| `components.md` CloudPublisher | USES: `IConfigProvider`, `IConfigManager`, `ITimeService`, `ILifecycle` (state query), ModbusPoller FD reading, TimeProvider, StoreAndForward, HealthMonitor; PROVIDES `IOtaTransport` (already B2). Closes F-03 |
| `components.md` ModbusPoller | PROVIDES the FD reading query (P7 pull) |
| `sequence-diagrams.md` SD-07 | CloudPublisher parses and applies per parameter (`set_param_deferred`); ConfigService validates each; persistence by LifecycleController on its idle tick; the result on `/config/result`. The profile-update branch is unchanged (WP8) |
| `sequence-diagrams.md` SD-03a/b, SD-05 | Payload notes: schema 1.1, timestamp and `sync_state` |
| `mqtt-client.md` §6 topic list | `/control`, `/config/result` |
| VP model | Same changes (Luca) |

---

## 11. Tests

### 11.1 Host (CloudPublisher; numbering continues after CP-T42)

| TC | Behaviour |
|---|---|
| CP-T50 | `create` rejects each mandatory NULL member; accepts NULL `time`, `control_handler`, `fd_slave_addr` 0 |
| CP-T51 | `create` registers the config change callback; a registration failure → `NO_RESOURCE` |
| CP-T52 | Telemetry interval read from `get_params()` in ms; change callback → `xTimerChangePeriod` with the new period; other parameters ignored |
| CP-T53 | Health tick: `health_monitor_poll` then `get_snapshot` in that order |
| CP-T54 | Stats tick: `health_monitor_update_mqtt_stats` with the absolute stats |
| CP-T55 | Disconnected → connected: `CLOUD_CONNECTED` once, `trigger_retry` once; no repeat while connected |
| CP-T56 | `cloud_publisher_on_disconnect` → `CLOUD_DISCONNECTED` once; `link_up` false |
| CP-T57 | `time == NULL` → no `trigger_retry` call, no crash |
| CP-T58 | Subscriptions after connect: `/config`, `/control`, `/ota`, `/ota/data`; `/control` skipped with a NULL handler; a failed one is retried alone next tick |
| CP-T59 | SAF enqueue OK → `CP_ERR_BUFFERED`; FLASH or TOO_LARGE → `CP_ERR_SAF_FULL` + ERROR log; no health event |
| CP-T60 | Drain: pointer entry published, then `saf_confirm`; `EMPTY` ends; `confirm` FLASH stops the drain this tick |
| CP-T61 | Telemetry with FD online → nested values converted (centi → deci-°C, centi → whole %, deci → whole hPa, rounding as Gateway) |
| CP-T62 | FD offline or `has_data` false → `online:false`, three `null`s; `fd_slave_addr` 0 → `"field_device":null` |
| CP-T63 | Invalid Gateway channel → `null`; no `invalid_channels` member |
| CP-T64 | Common block: `schema_version` 1.1, epoch and `synchronised` when TimeProvider is synchronised; uptime and `unsynchronised` otherwise; TimeProvider error → unsynchronised + WARN |
| CP-T65 | Health 1.1: every field present with snapshot values; `stack_free_words` length = registered tasks |
| CP-T66 | Alarm timestamp from the reading, not from serialisation time |
| CP-T67 | Config: applied → `set_param_deferred` per key in order, result `applied` with `req`; no `set_param` and no `persist_pending` call |
| CP-T68 | Config: second key invalid → first key reverted with its old value; result `rejected/invalid_value/key` |
| CP-T69 | Config: Tier 1 key → `not_remote`; unknown key → `unknown_key`; nothing applied |
| CP-T70 | Config: malformed inputs (string value, float, nested, duplicate, trailing data, > 8 keys, empty object) → the matching reason; nothing applied |
| CP-T71 | Config: value outside the C type (e.g. `temp_alarm_high` 40000) → `invalid_value` before any call to ConfigService |
| CP-T72 | Config while lifecycle state ≠ OPERATIONAL → `busy` |
| CP-T73 | Result topic is `/config/result`; no publish on `/results` |
| CP-T74 | `/control` → `control_handler` called with the payload; NULL handler → ignored with a WARN |
| CP-T75 | Every serialiser output ≤ 2048 at the largest values (min and max ints, all channels, 8 tasks) |
| CP-T76 | `nm`-style link test: the test build links without any of the 10 placeholder symbols (CMock mocks of the real headers only) |

Existing CP-T01..T42 keep their behaviour. Update only the ones that
asserted the removed health events, `invalid_channels`, the `/results`
topic or the tick timestamp.

### 11.2 Hardware (CloudPublisher bring-up harness on the real modules)

| TC | Pass criterion |
|---|---|
| TC-HW-CP-001..013 | Re-run. Same results as 2026-10-24, with schema 1.1 payloads at the broker |
| TC-HW-CP-014 | `{"req":1,"telemetry_interval_ms":10000}` published to `/config` → `applied` on `/config/result`; telemetry every 10 s; after a reset the interval is still 10 s (persisted by the LifecycleController stub or the harness idle loop) |
| TC-HW-CP-015 | A Tier 1 key → `not_remote`; a bad band → `invalid_value`, config unchanged |
| TC-HW-CP-016 | Broker stopped and restarted → health snapshot `cloud_connected` false then true; TimeService retry logged |
| TC-HW-CP-017 | Health payload checked field by field against the console snapshot |

Field Device values in telemetry are checked in WP7 (two boards). In this
WP the harness uses ModbusPoller with a scripted FD response.

---

## 12. Done when

1. The CloudPublisher harness ELF has **no undefined or weak definition** of: `config_provider_get_telemetry_interval_s`, `config_provider_get_health_interval_s`, `config_manager_apply`, `health_snapshot_get`, `health_report_update_mqtt`, `health_report_push_event`, `modbus_poller_get_latest_fd`, `store_and_forward_enqueue`, `store_and_forward_dequeue`, `store_and_forward_confirm`, `health_update_buffer_occupancy`, `health_report_push_buffer_event` (`arm-none-eabi-nm -A` on the ELF, result in the PR).
2. Host suites pass (counts reported); CP-T50..T76 pass; cppcheck and clang-format clean.
3. TC-HW-CP-001..017 pass on the board.
4. LLDs updated in the same PRs: `cloud-publisher-lld.md` → v2.0 (CP-D18..D31, CP-O8..O10), `modbus-poller-lld.md` v1.1, `lifecycle-controller-lld.md` (LC-D23), `time-service-lld.md` (header), `store-and-forward-lld.md`, `circular-flash-log.md`, and this document marked Implemented.
5. `module-implementation-status.md`: CloudPublisher row updated; LC-O13 removed from §6.

---

## Appendix A — Session 1 analysis (unchanged, for reference)

**Instruction for Claude Code:** when committing this version, copy
sections 1–6 of the session 1 analysis (headline findings, symbol table
§2.1 rows 1–12, adjacent placeholders §2.2, type table §3, TS-O6 §4,
CP-O4 §5, CP-O5 §6) **verbatim** below this paragraph, renumbered A.1–A.6.
They remain valid. The decisions above replace its §7 (questions) and §8
(proposed scope).

The binding list for `main()` (A.6) gains: `disconnect_cb = cloud_publisher_on_disconnect`,
`control_handler = lifecycle_handle_remote`,
`lifecycle = lifecycle_controller`, `fd_slave_addr` (1, from the
DeviceProfileRegistry placeholder), and `fw_version` (from
`firmware_boot_info_t.running_version`, or the build version for a
development image).

### A.1 Headline findings

1. **Ten symbols have no definition anywhere** (confirmed by searching `application/`, `middleware/` and `drivers/`). Seven of them have a real module that offers an equivalent under another name, so they are an adaptation. **Three need something that does not exist yet:** the Field Device reading (`modbus_poller_get_latest_fd`), a payload-level config apply (`config_manager_apply`), and two health events with no real counterpart (`ALARM_PUBLISHED`, `ALARM_BUFFERED`).
2. **CP-O2 has a negative answer.** ModbusPoller has no `get_latest_fd`, and no cache at all: `forward_response_to_consumers()` is an empty stub (`modbus_poller.c`, MBP-O3 open). The Field Device's readings are polled over RS-485 and then discarded. Nothing in the Gateway can publish them today.
3. **The `/config` command has no owner and no schema.** The HLD (SD-07) has CloudPublisher send one "apply config change" message to ConfigService, which validates. The as-built ConfigService has only per-parameter `set_param` / `validate_param` by `config_param_id_t`. Nothing parses a JSON payload, and no document defines the payload.
4. **Nobody tells HealthMonitor the cloud is connected.** `HEALTH_EVENT_CLOUD_CONNECTED` / `_DISCONNECTED` have no caller outside HealthMonitor itself, so `health_snapshot_t.cloud_connected` would stay `false` forever once the placeholders are replaced.
5. **The "guarded placeholder" convention does not work for most types.** Only `HEALTH_MONITOR_HANDLE_T_DEFINED` is honoured by the real headers. Including `store_and_forward.h` together with `cloud_publisher.h` is a redefinition error (`saf_err_t`, `saf_entry_t`), not a graceful supersede. Each placeholder block has to be removed, not guarded.
6. **Units differ at three boundaries:** config intervals (seconds in the placeholder, milliseconds in `config_params_t`), Field Device registers (×0.01 °C, ×0.01 %RH, ×0.1 hPa with sentinels, against deci-°C, whole %RH and whole hPa in the placeholder), and the SAF length (`uint32_t`, 512-byte entries against `uint16_t`, 4 010-byte payloads).
7. **Adjacent placeholders found outside CloudPublisher:** CircularFlashLog and StoreAndForward each carry a weak no-op health function that the real HealthMonitor does not provide under that name (section 2.2).

---

### A.2 Placeholder symbols

#### A.2.1 Symbols called by CloudPublisher

Call sites are in `cloud_publisher.c`. "Provides?" says whether the real module already offers the operation.

| # | Placeholder signature (`cloud_publisher.h`) | Real API | Semantic difference | Provides? |
|---|---|---|---|---|
| 1 | `uint32_t config_provider_get_telemetry_interval_s(config_service_handle_t)` (create, telemetry tick) | `iconfig_provider_t::get_params(void *ctx)` → `const config_params_t *`, field `telemetry_interval_ms` (`config_service/iconfig_provider.h`, `config_params.h`; default 60 000, range 1 000 to 3 600 000). Instance: `config_service_get_provider()` | Seconds → milliseconds. A handle → a vtable with `ctx`. Returns a pointer to ConfigService's live parameters, so the value must be read once per use (SS-D20 and `set_param` can change it from another task). The placeholder's "0 means use the default" can no longer occur (minimum is 1 000 ms) | **Yes**, no new operation |
| 2 | `uint32_t config_provider_get_health_interval_s(config_service_handle_t)` | same vtable, field `health_interval_ms` (default 600 000, range 10 000 to 86 400 000) | as row 1 | **Yes** |
| 3 | `bool config_manager_apply(config_service_handle_t, const uint8_t *payload, uint32_t len)` (`/config` route) | `iconfig_manager_t`: `validate_param`, `set_param`, `set_param_deferred`, `persist_pending`, all by `(config_param_id_t id, const void *value)`; errors are `config_service_err_t`. Instance: `config_service_get_manager()` | **No payload-level operation.** Someone must parse the JSON into `(id, value)` pairs, and the schema is undefined (question D-3). `set_param` persists before it returns (about 0.77 s, CFG-O7) and would block CloudPublisherTask, which also runs the MQTT loop; `set_param_deferred` needs a caller that runs `persist_pending()` from a context allowed to block. The result is an error code, not a `bool` | **No** (new operation or new parser) |
| 4 | `cp_health_snapshot_t health_snapshot_get(health_monitor_handle_t)` (health tick) | `health_monitor_get_snapshot(handle, health_snapshot_t *out)` → `health_monitor_err_t` (`health_monitor.h`) | By value → out-parameter. `health_snapshot_t` is much larger (per-sensor arrays, 8 stack watermarks, lifecycle state): it must live in the instance or a static buffer (no heap, no big stack frame). The four fields CloudPublisher uses exist under the same names (`uptime_s`, `cloud_connected`, `mqtt_reconnect_count`, `buffer_entry_count`). The header says CloudPublisher must call `health_monitor_poll()` first on each health tick; it does not | **Yes** (add the `poll` call) |
| 5 | `void health_report_update_mqtt(handle, const mqtt_stats_t *stats, const mqtt_stats_t *last_stats)` (1 Hz stats tick) | `health_monitor_update_mqtt_stats(handle, const mqtt_stats_t *stats)` | The real call copies the absolute counters (`publishes_sent`, `publish_failures`, `reconnect_count`). The `last_stats` parameter has no use, so `inst->last_mqtt_stats` can go. Returns an error code, ignored today | **Yes** |
| 6 | `void health_report_push_event(handle, cp_health_event_t)` with `SAF_FULL`, `ALARM_PUBLISHED`, `ALARM_BUFFERED` | `health_monitor_push_event(handle, health_event_t, uint32_t param)` | Mapping: `SAF_FULL` has no equivalent and cannot arise (the real `saf_enqueue` drops the oldest entry instead of failing, REQ-BF-020, and the overflow is already counted as `HEALTH_EVENT_BUFFER_OVERFLOW` by CircularFlashLog's own report). `ALARM_PUBLISHED` and `ALARM_BUFFERED` have **no counterpart** in `health_event_t` (`ALARM_RAISED` / `_CLEARED` belong to AlarmService and carry a sensor id). They are also the observation seam of TC-HW-CP-011 (REQ-NF-113 timing) | **Partly:** needs a decision (D-2) |
| 7 | `modbus_poller_fd_reading_t modbus_poller_get_latest_fd(modbus_poller_handle_t)` (telemetry tick) | **None.** `modbus_poller.h` offers `init`, `is_ready`, `send_command`, `run` and test hooks. ModbusPoller is a singleton, so there is no handle either | See finding 2. A new read-side operation and a place to keep the decoded values are needed (D-1). Register units and sentinels: temperature int16 ×0.01 °C (invalid `0x8000`), humidity uint16 ×0.01 %RH (invalid `0xFFFF`), pressure uint16 ×0.1 hPa (invalid `0xFFFF`), input registers 0x0010 to 0x0012 (`docs/hld/modbus-register-map.md` 6.2). The placeholder expects deci-°C, whole %RH and whole hPa and has one `valid` flag for all three | **No** |
| 8 | `saf_err_t store_and_forward_enqueue(handle, topic, const uint8_t *buf, uint32_t len, mqtt_qos_t qos)` | `saf_enqueue(saf_handle_t, const char *topic, const uint8_t *buf, uint16_t len, uint8_t qos)` | Name; `uint32_t` → `uint16_t` length (CloudPublisher's buffers are 4 096 B, SAF accepts 1 to 4 010 B, otherwise `SAF_ERR_TOO_LARGE`); `qos` type. Error set differs (no `FULL`; new `TOO_LARGE`, `NOT_INIT`, `FLASH`, `NO_RESOURCE`). **Behaviour change:** when the ring is full the real SAF drops the oldest entries and succeeds, so `CP_ERR_SAF_FULL` is reachable only on a flash error | **Yes** |
| 9 | `saf_err_t store_and_forward_dequeue(handle, saf_entry_t *entry_out)` (`prv_drain_saf`) | `saf_dequeue(saf_handle_t, const saf_entry_t **out)` | Copy-out → pointer-out: the entry is valid only until the next `dequeue` / `confirm` / a dropping `enqueue`. This removes a 584-byte `saf_entry_t` from the task stack. Fields: `payload_len` is `uint16_t` (placeholder `len` is `uint32_t`), `qos` is `uint8_t`. A repeated `dequeue` without `confirm` returns the same entry, which matches the drain loop's retry. Empty is `SAF_ERR_EMPTY` (value 5, not 2) | **Yes** |
| 10 | `saf_err_t store_and_forward_confirm(handle)` | `saf_confirm(saf_handle_t)` | Same meaning; idempotent with no pending entry; may return `SAF_ERR_FLASH`, in which case the entry stays pending and would be re-sent (duplicate on the wire) | **Yes** |
| 11 | `void lifecycle_handle_remote(lifecycle_handle_t, const uint8_t *payload, uint32_t len)` (`/control` route) | **Already real:** `lifecycle_wiring.c:510`, declared in `lifecycle_wiring.h:95`, same signature | None. But `lifecycle_handle_t` is defined only in `cloud_publisher.h` and `lifecycle_wiring.h` includes `cloud_publisher.h` to get it: the dependency points the wrong way. The CloudPublisher bring-up harness also defines its own `lifecycle_handle_remote` (`main_test_cloud_publisher.c:501`), which will collide in any image that links both | **Yes** (move the typedef) |
| 12 | `update_service_handle_command` | `iupdate_service_t::handle_command` (CPC-1..6) | Already replaced in #106 | **Closed** |

#### A.2.2 Adjacent placeholders (not called by CloudPublisher)

| Where | Placeholder | Real API | Note |
|---|---|---|---|
| `store_and_forward.h/.c` | weak `health_update_buffer_occupancy(handle, count, capacity, percent)` | `health_monitor_update_buffer_occupancy(handle, entry_count)` | Different name and a narrower signature; the weak default is a no-op, so `buffer_entry_count` never reaches the snapshot from SAF. `capacity` and `percent` have no place in the real snapshot |
| `circular_flash_log.h/.c` | weak `health_report_push_buffer_event(handle, cfl_health_event_t)` | `health_monitor_push_event(handle, HEALTH_EVENT_BUFFER_OVERFLOW, 0)` | The weak default is a no-op, so overflows never reach the snapshot |

Both link today only because of the weak defaults. Whether to fix them in this work package is question D-6.

---

### A.3 Placeholder types against the real ones

| Placeholder (in `cloud_publisher.h`) | Real | Verdict |
|---|---|---|
| `modbus_poller_handle_t`, `modbus_poller_fd_reading_t` | none (singleton; no reading type) | Depends on D-1 |
| `store_and_forward_handle_t` (`struct store_and_forward_inst *`) | `saf_handle_t` (`struct saf_inst *`) | Replace; the config field and `struct cloud_publisher_inst` change type |
| `saf_err_t` (`OK, FULL, EMPTY, NULL_PTR`) | `saf_err_t` (`OK, NULL_PTR=1, NO_RESOURCE, NOT_INIT, TOO_LARGE, EMPTY=5, FLASH`) | Same name, different values and members: redefinition error if both are included |
| `SAF_TOPIC_MAX_LEN` 64, `SAF_ENTRY_MAX_LEN` 512 | `SAF_TOPIC_MAX` 64, `SAF_PAYLOAD_MAX` = 4 078 − 68 = 4 010 | Rename; the 512-byte ceiling disappears |
| `saf_entry_t` (copies of topic and payload, `uint32_t len`, `mqtt_qos_t qos`) | `saf_entry_t` (pointer payload, `uint16_t payload_len`, `uint8_t qos`) | Replace, see row 9 |
| `health_monitor_handle_t` | `health_monitor_handle_t` (`health_monitor.h`, guard `HEALTH_MONITOR_HANDLE_T_DEFINED` honoured) | Already compatible |
| `cp_health_snapshot_t` | `health_snapshot_t` | Replace; the four used fields keep their names |
| `cp_health_event_t` | `health_event_t` | Three values, none of which maps one to one (row 6) |
| `config_service_handle_t` | none (ConfigService is a singleton with two vtables) | Replace by the two vtable pointers `const iconfig_provider_t *` and `const iconfig_manager_t *` |
| `lifecycle_handle_t` | none (LifecycleController is a singleton) | The handle is unused by `lifecycle_handle_remote`; see row 11 |
| `cp_alarm_*`, `cp_alarm_event_t` | CloudPublisher's own wire model (documented as such) | Stays |
| `mqtt_stats_t`, `mqtt_qos_t`, `mqtt_client_handle_t` | already the real `mqtt_client.h` types | Nothing to do |

---

### A.4 TS-O6 — what CloudPublisher needs from `itime_service_t`

`itime_service_t` (in `time_service.h`) has two members: `get_sync_state(ctx)` and `trigger_retry(ctx)`. `trigger_retry` posts a task notification to TimeServiceTask (non-blocking) to run an NTP attempt (REQ-TS-0E1, "retry NTP on internet reconnect").

What is missing in CloudPublisher:

1. A `const itime_service_t *time` member in `cloud_publisher_config_t` (NULL means "no TimeService", the same convention as `update_ops`).
2. A "was connected" flag in the instance and a call to `time->trigger_retry(time->ctx)` on each transition from disconnected to connected. The natural place is `prv_maybe_reconnect()` (`cloud_publisher.c:502`), which already notices the connected state once per 1 Hz stats tick. MqttClient gives no connect callback, so detection by polling is the existing pattern.
3. Open point: also on the first connect after boot? TimeService makes its own boot sync as soon as the scheduler runs (TS-D3), possibly before WiFi is up, so a trigger on the first connect is cheap and probably wanted. This is for the reviewer (D-5).

Related observation, not a placeholder: the `timestamp` field in all four payloads is `xTaskGetTickCount()` (`cloud_publisher_json.c`), that is milliseconds since boot, not wall-clock time, although the sensor snapshot already carries a `TIME_SYNC_SYNCHRONISED` state. Whether the cloud schema expects epoch time is part of the schema decision (D-4); `get_sync_state` would be needed for that too.

Layering note: the interface is declared inside `time_service.h` together with the module's init API. Other consumers got their own interface headers (`iupdate_service.h`, `iota_transport.h`, `iconfig_provider.h`). Extracting `itime_service.h` would keep CloudPublisher from including the whole TimeService module header. This is a small, optional change (D-5).

---

### A.5 CP-O4 — invalid-channel encoding (current state)

Implemented in `cloud_publisher_json.c`:

- **Gateway sensors:** a channel whose reading is not valid is sent as **`0`** and its name is added to the **`invalid_channels`** array (`"invalid_channels":["pressure","magnetometer_x"]`). This is SS-O12. A consumer cannot tell a real 0 from an invalid 0 without reading the array.
- **Field Device:** when the link is up the payload adds `field_device_temp_deci_c`, `field_device_humidity_pct`, `field_device_pressure_hpa` and `"field_device_valid":true`. When it is down the three fields are **omitted** and `"field_device_valid":false` is sent (REQ-SA-160).

The open question (CP-O4, restated): should an invalid Gateway channel and an offline Field Device be encoded as an **omitted field**, as **`null`**, or as today (**0 plus a list** / **omit plus a flag**), and should the two cases use the same convention? This is a cloud-schema decision owned by Luca. It interacts with finding 2: once the Field Device reading exists, a single failed channel arrives as a sentinel (`0x8000` / `0xFFFF`), and the placeholder's single `valid` flag cannot express a per-channel failure. The reading type should carry per-channel validity if the schema wants it (D-1, D-4). No code change is proposed before the schema is decided.

---

### A.6 CP-O5 — inbound path after #106, and what `main()` must bind

`cloud_publisher_on_message()` (CP-D17) is a context-free function that reaches the instance through the pool entry. Passed as `mqtt_client_config_t::msg_cb`, it covers the whole inbound path: the `/config`, `/ota` and `/control` routes through `prv_route_command()`, and the chunk fast path for the OTA data topic (a full 1 KB chunk goes straight to `on_chunk`, never queued).

Not covered:

- **`disconnect_cb` is mandatory.** `mqtt_client_create()` fails with a NULL `disconnect_cb`, and CloudPublisher exports no function for it. `main()` has to pass something, and it also decides who tells HealthMonitor about the disconnect (finding 4, D-2). This was already noted in CP-O5 ("the `disconnect_cb` is unchanged") but not turned into a binding item.

What `main()` must bind (extends the list in the status document, section 6):

| Item | Value |
|---|---|
| `mqtt_client_config_t::msg_cb` | `cloud_publisher_on_message` |
| `mqtt_client_config_t::disconnect_cb` | to be decided (above) |
| `cloud_publisher_config_t::update_ops`, `update` | `&update_service_ops`, the created `update_service_t` (both NULL means no OTA) |
| `update_service_config_t` transport | `cloud_publisher_get_ota_transport(handle, &transport)` |
| `lifecycle_wiring_config_t` OTA members | `update_ops`, `update`, `firmware_ops`, `firmware` (all four or none; partial is `NULL_ARG`) |
| `running_image_base` | `cpu_get_vector_table_base()` |
| after this work package | `cfg_read` / `cfg_write` as the two vtables, `saf` as `saf_handle_t`, `health` as the real handle, `time` as `itime_service_t`, and (D-1) the Field Device reading source |

Creation order matters: StoreAndForward needs CircularFlashLog, CircularFlashLog needs QspiFlashDriver, `cloud_publisher_create()` registers the alarm callback and creates its task, and the OTA transport needs the handle that `cloud_publisher_create()` returns before `update_service_create()`. That order belongs in WP4 (`gateway-main.md`).
