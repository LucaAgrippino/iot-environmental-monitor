# CloudPublisher Reconciliation — Analysis for Pass H (WP1, session 1)

**Document:** `docs/lld/gate-review/cloud-publisher-reconciliation.md`
**Status:** Draft — analysis only, no code changed. Awaiting Pass H.
**Board:** Gateway (B-L475E-IOT01A)
**Date:** 2026-10-28 · **Repo state:** `main` @ 4efc3aa
**Closes (when implemented):** LC-O13, SAF-O3, TS-O6. **Touches:** CP-O2, CP-O4, CP-O5.

**Sources read:** `firmware/gateway/application/cloud_publisher/cloud_publisher.{h,c}` and `cloud_publisher_json.c`; the real headers of StoreAndForward, HealthMonitor, ConfigService (`iconfig_provider.h`, `iconfig_manager.h`, `config_params.h`, `config_service_types.h`), ModbusPoller, TimeService, MqttClient, `lifecycle_wiring.{h,c}`, CircularFlashLog; `cloud-publisher-lld.md`, `modbus-poller-lld.md`, `time-service-lld.md`, `docs/hld/modbus-register-map.md`, `docs/hld/sequence-diagrams.md` (SD-07), `docs/SRS.md`.

---

## 1. Headline findings

1. **Ten symbols have no definition anywhere** (confirmed by searching `application/`, `middleware/` and `drivers/`). Seven of them have a real module that offers an equivalent under another name, so they are an adaptation. **Three need something that does not exist yet:** the Field Device reading (`modbus_poller_get_latest_fd`), a payload-level config apply (`config_manager_apply`), and two health events with no real counterpart (`ALARM_PUBLISHED`, `ALARM_BUFFERED`).
2. **CP-O2 has a negative answer.** ModbusPoller has no `get_latest_fd`, and no cache at all: `forward_response_to_consumers()` is an empty stub (`modbus_poller.c`, MBP-O3 open). The Field Device's readings are polled over RS-485 and then discarded. Nothing in the Gateway can publish them today.
3. **The `/config` command has no owner and no schema.** The HLD (SD-07) has CloudPublisher send one "apply config change" message to ConfigService, which validates. The as-built ConfigService has only per-parameter `set_param` / `validate_param` by `config_param_id_t`. Nothing parses a JSON payload, and no document defines the payload.
4. **Nobody tells HealthMonitor the cloud is connected.** `HEALTH_EVENT_CLOUD_CONNECTED` / `_DISCONNECTED` have no caller outside HealthMonitor itself, so `health_snapshot_t.cloud_connected` would stay `false` forever once the placeholders are replaced.
5. **The "guarded placeholder" convention does not work for most types.** Only `HEALTH_MONITOR_HANDLE_T_DEFINED` is honoured by the real headers. Including `store_and_forward.h` together with `cloud_publisher.h` is a redefinition error (`saf_err_t`, `saf_entry_t`), not a graceful supersede. Each placeholder block has to be removed, not guarded.
6. **Units differ at three boundaries:** config intervals (seconds in the placeholder, milliseconds in `config_params_t`), Field Device registers (×0.01 °C, ×0.01 %RH, ×0.1 hPa with sentinels, against deci-°C, whole %RH and whole hPa in the placeholder), and the SAF length (`uint32_t`, 512-byte entries against `uint16_t`, 4 010-byte payloads).
7. **Adjacent placeholders found outside CloudPublisher:** CircularFlashLog and StoreAndForward each carry a weak no-op health function that the real HealthMonitor does not provide under that name (section 2.2).

---

## 2. Placeholder symbols

### 2.1 Symbols called by CloudPublisher

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

### 2.2 Adjacent placeholders (not called by CloudPublisher)

| Where | Placeholder | Real API | Note |
|---|---|---|---|
| `store_and_forward.h/.c` | weak `health_update_buffer_occupancy(handle, count, capacity, percent)` | `health_monitor_update_buffer_occupancy(handle, entry_count)` | Different name and a narrower signature; the weak default is a no-op, so `buffer_entry_count` never reaches the snapshot from SAF. `capacity` and `percent` have no place in the real snapshot |
| `circular_flash_log.h/.c` | weak `health_report_push_buffer_event(handle, cfl_health_event_t)` | `health_monitor_push_event(handle, HEALTH_EVENT_BUFFER_OVERFLOW, 0)` | The weak default is a no-op, so overflows never reach the snapshot |

Both link today only because of the weak defaults. Whether to fix them in this work package is question D-6.

---

## 3. Placeholder types against the real ones

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

## 4. TS-O6 — what CloudPublisher needs from `itime_service_t`

`itime_service_t` (in `time_service.h`) has two members: `get_sync_state(ctx)` and `trigger_retry(ctx)`. `trigger_retry` posts a task notification to TimeServiceTask (non-blocking) to run an NTP attempt (REQ-TS-0E1, "retry NTP on internet reconnect").

What is missing in CloudPublisher:

1. A `const itime_service_t *time` member in `cloud_publisher_config_t` (NULL means "no TimeService", the same convention as `update_ops`).
2. A "was connected" flag in the instance and a call to `time->trigger_retry(time->ctx)` on each transition from disconnected to connected. The natural place is `prv_maybe_reconnect()` (`cloud_publisher.c:502`), which already notices the connected state once per 1 Hz stats tick. MqttClient gives no connect callback, so detection by polling is the existing pattern.
3. Open point: also on the first connect after boot? TimeService makes its own boot sync as soon as the scheduler runs (TS-D3), possibly before WiFi is up, so a trigger on the first connect is cheap and probably wanted. This is for the reviewer (D-5).

Related observation, not a placeholder: the `timestamp` field in all four payloads is `xTaskGetTickCount()` (`cloud_publisher_json.c`), that is milliseconds since boot, not wall-clock time, although the sensor snapshot already carries a `TIME_SYNC_SYNCHRONISED` state. Whether the cloud schema expects epoch time is part of the schema decision (D-4); `get_sync_state` would be needed for that too.

Layering note: the interface is declared inside `time_service.h` together with the module's init API. Other consumers got their own interface headers (`iupdate_service.h`, `iota_transport.h`, `iconfig_provider.h`). Extracting `itime_service.h` would keep CloudPublisher from including the whole TimeService module header. This is a small, optional change (D-5).

---

## 5. CP-O4 — invalid-channel encoding (current state)

Implemented in `cloud_publisher_json.c`:

- **Gateway sensors:** a channel whose reading is not valid is sent as **`0`** and its name is added to the **`invalid_channels`** array (`"invalid_channels":["pressure","magnetometer_x"]`). This is SS-O12. A consumer cannot tell a real 0 from an invalid 0 without reading the array.
- **Field Device:** when the link is up the payload adds `field_device_temp_deci_c`, `field_device_humidity_pct`, `field_device_pressure_hpa` and `"field_device_valid":true`. When it is down the three fields are **omitted** and `"field_device_valid":false` is sent (REQ-SA-160).

The open question (CP-O4, restated): should an invalid Gateway channel and an offline Field Device be encoded as an **omitted field**, as **`null`**, or as today (**0 plus a list** / **omit plus a flag**), and should the two cases use the same convention? This is a cloud-schema decision owned by Luca. It interacts with finding 2: once the Field Device reading exists, a single failed channel arrives as a sentinel (`0x8000` / `0xFFFF`), and the placeholder's single `valid` flag cannot express a per-channel failure. The reading type should carry per-channel validity if the schema wants it (D-1, D-4). No code change is proposed before the schema is decided.

---

## 6. CP-O5 — inbound path after #106, and what `main()` must bind

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

---

## 7. Design questions for Pass H

No decision is taken here. Each question gives the options and a leaning where there is one.

**D-1 Field Device readings (CP-O2, MBP-O3).** Who decodes and keeps the readings?
- *(a)* ModbusPoller decodes registers 0x0010 to 0x0012 in `forward_response_to_consumers()` into a small cache (per slave, with a timestamp) and exports `modbus_poller_get_latest_fd(modbus_fd_reading_t *out)`. Closes MBP-O3. The type lives in `modbus_poller.h`. *Leaning:* this, because the poller already owns the profile, the link state and the register span.
- *(b)* A separate "shared data store" module, as MBP-O3 describes. More general (MBP-O3 names CloudPublisher and LcdDisplay as consumers), but on the Gateway CloudPublisher is the only reader today, so it adds a module for one reader.
- Either way decide: units on the type (keep raw register units and convert in CloudPublisher, or convert in the poller), per-channel validity, and what "offline" means (link state from the hysteresis counters, or reading age).

**D-2 Health events.** `ALARM_PUBLISHED` and `ALARM_BUFFERED` have no real event; `CLOUD_CONNECTED` / `_DISCONNECTED` exist but nobody pushes them.
- *(a)* Add two events to `health_event_t` (a HealthMonitor LLD change) and keep the TC-HW-CP-011 seam.
- *(b)* Keep REQ-NF-113 observation inside CloudPublisher (a counter or a log line) and drop the two events. *Leaning:* (b), since the snapshot has no field that consumes them.
- *Either way:* CloudPublisher pushes `CLOUD_CONNECTED` / `_DISCONNECTED` on the transitions it already detects, and the `disconnect_cb` question (section 6) is settled with it.

**D-3 `/config` handling.** Who parses the payload, and into what?
- *(a)* CloudPublisher parses the JSON and calls `validate_param` / `set_param_deferred` per key, then `persist_pending` once.
- *(b)* ConfigService gains an `apply_json(payload, len)` (closest to SD-07 as drawn: one sync message to ConfigService), owning schema, validation and persistence.
- Open in both: the payload schema (which of the 25 `config_param_id_t` values are remotely settable, units, name of each key), all-or-nothing against per-key partial apply, and the blocking time (0.77 s per `set_param`, CFG-O7) in a task that also runs the MQTT loop. The profile-update branch of SD-07 (DeviceProfileRegistry) is out of scope until WP8.

**D-4 Cloud schema (CP-O4 and the timestamp).** Encoding of invalid channels, of an offline Field Device, and whether `timestamp` is epoch seconds (needs `get_sync_state`) or ticks. Owned by Luca.

**D-5 TimeService hook.** Trigger on every connect including the first? Extract `itime_service.h`? Optional use of `get_sync_state` for timestamps.

**D-6 Adjacent placeholders (section 2.2).** Fix the SAF and CFL health calls in the same work package, or in their own `fix:` PR? Both are small; they change two other modules' LLD companions.

**D-7 Config structure.** `cloud_publisher_config_t` currently holds seven peer handles. Replacing `config_service_handle_t` by two vtable pointers, and `lifecycle_handle_t` by nothing, is a breaking change for the CloudPublisher harness and tests. Confirm the shape before session 2: `{ mqtt, fd_source?, saf, health, cfg_read (iconfig_provider_t*), cfg_write (iconfig_manager_t*), time (itime_service_t*), update_ops, update, mqtt_connect_cfg }` and no `lifecycle` member (the `/control` route would call `lifecycle_handle_remote` directly, or through a function pointer if the dependency on `lifecycle_wiring` is unwanted).

---

## 8. Proposed scope of session 2 (after Pass H)

1. Remove every placeholder block from `cloud_publisher.h`; include the real headers; adapt `cloud_publisher.c` (rows 1 to 11, section 4) and the serialiser.
2. Implement what D-1 to D-3 decide in the owning modules (ModbusPoller, HealthMonitor, ConfigService) with their own tests.
3. Update `cloud-publisher-lld.md`: close LC-O13 / SAF-O3 / TS-O6 / CP-O2 as resolved, new decisions `CP-D18+`, the test list; and the touched companions.
4. Host tests: existing CP tests unchanged in behaviour (CP-013 included), new ones for the `poll` call, the connect transitions, the SAF error mapping and the pointer-out drain.
5. Bring-up harness: `main_test_cloud_publisher.c` moves to the real modules.

**Done when** (from the plan): `firmware/gateway` links CloudPublisher with the real modules and no placeholder symbol is left, checked with `nm` on the harness ELF or the map file (the ten names in section 2.1 must not appear as undefined or as weak definitions); CloudPublisher host tests and CP-013 behave as before; the LLD is updated.

**Risks to flag now:** (1) D-1 is the largest item and adds a feature to ModbusPoller, whose LLD is at v1.0, implementation ready; (2) `set_param` blocking time in the MQTT task; (3) the real `saf_enqueue` changes the full-buffer behaviour, so TC-HW-CP cases that assumed `CP_ERR_SAF_FULL` need re-reading; (4) the CloudPublisher harness depends on the placeholders and on a local `lifecycle_handle_remote`.
