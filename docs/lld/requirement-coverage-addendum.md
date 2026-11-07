# Requirement coverage addendum

**Version:** 1.0 · **Date:** November 2026 · **Purpose:** closes the implementation phase. Records, for every SRS
requirement that the generated matrix (`docs/test/requirements-traceability.md`) could not link to an LLD or to
code by its ID, where the requirement is implemented and what verifies it. The matrix finds evidence by text search
for the ID, so a requirement that is implemented but never named shows up as "No reference".

Status words: **Implemented** (code exists and has host tests), **By construction** (a property of the build or the
design, checked by a tool or a measurement), **Verify** (implemented; the check belongs to the verification phase).

## 1. Sensor acquisition and history (Field Device LcdUi, `ui_history.[ch]`)

| ID | Where | Evidence | Status |
|---|---|---|---|
| REQ-SA-175 | `sensor_reading_t.valid` is false when the sensor read fails; SensorService marks the channel failed | `fd-gw-sensor-alarm-service-lld.md`; host tests of SensorService | Implemented |
| REQ-SA-210 | 60 entries per channel (`UI_HISTORY_LEN`) | TC-UIH tests, `test_ui_history.c` | Implemented |
| REQ-SA-220 | Each entry: value, validity flag and acquisition timestamp (`ui_history_get_time`) | `test_ui_history.c` (timestamp tests), TC-LCDUI-050 | Implemented (added at closure) |
| REQ-SA-230 | The ring is static RAM, cleared by `ui_history_reset()` in `lcd_ui_init()`; nothing is written to flash | `ui_history.c` | Implemented |
| REQ-SA-240 | A sample is recorded invalid when the latest reading is older than the 5 s history interval | TC-LCDUI-051 | Implemented (added at closure) |

## 2. Local display (LcdUi, LLD v0.4)

| ID | Where | Evidence | Status |
|---|---|---|---|
| REQ-LD-020 | Header clock shows the timestamp of the most recent reading | `lcd_ui.c` (`clock_text`), screenshots in `docs/lcd-ui-design/screenshots` | Implemented |
| REQ-LD-120 | Apply opens the "Apply Configuration?" modal listing the changes | TC-LCDUI config tests, `confirm_modal` | Implemented |
| REQ-LD-130 | CONFIRM writes the parameters (`apply_block_to_config`) | TC-LCDUI config tests | Implemented |
| REQ-LD-150 | `config_service_set_param` saves the blob through ConfigStore (`store->save`) | `config_service.c`; persistence across reset is a verification step | Implemented, Verify |
| REQ-LD-155 | The modal's 30 s timeout and the CANCEL bar discard all pending input | TC-LCDUI confirm timeout test | Implemented |
| REQ-LD-170 | Trend charts span the 60 samples, 5 minutes | `trend_panel`, simulator `trend-full-gap` | Implemented |
| REQ-LD-180 | The Trend screen redraws when `hist_gen` changes (a new sample) | `lcd_ui.c`, TC-LCDUI trend tests | Implemented |
| REQ-LD-260 | HealthMonitor drives the green LED: on in Operational | `health_monitor.c`, TC-HM-003..005 | Implemented, Verify on the board |
| REQ-LD-270 | The green LED is off in a fault (Error) state | `health_monitor.c`, TC-HM-004 | Implemented, Verify on the board |
| REQ-LD-280 | The red LED is on while an alarm is active (Field Device only) | `health_monitor.c`, TC-HM-003, TC-HM-005 | Implemented, Verify on the board |

## 3. Local interface (ConsoleService)

| ID | Where | Evidence | Status |
|---|---|---|---|
| REQ-LI-020 | Diagnostic commands (`selftest`, `status`, `modbus status`) print their result | `console_service.c` command table | Implemented |
| REQ-LI-170 | A failing diagnostic command prints an error line | `console-service-lld.md` error table, host tests | Implemented |
| REQ-LI-180 | A rejected value prints the problem and changes nothing | `console-service-lld.md` error table, host tests | Implemented |
| REQ-LI-190 | A staged change is discarded when the confirmation times out (`read_confirm_with_timeout`) | `console_service.c` | Implemented |
| REQ-NF-303 | The console exists only on the debug UART (ST-LINK virtual COM port); there is no network listener | Gateway and Field Device `main.c` | By construction |
| REQ-NF-307 | The remote restart needs a `restart_confirm` within a timeout (`CP_CTRL_RESTART`, `CP_CTRL_RESTART_CONFIRM`) | `cloud_publisher_json.h`, CloudPublisher LLD | Implemented, Verify |

## 4. Time and alarms

| ID | Where | Evidence | Status |
|---|---|---|---|
| REQ-TS-050 | `time_service_trigger_retry()` runs when the connection returns (CloudPublisher reconnect, TS-O6 closed, CP-D23) | `time_service.c`, `cloud-publisher-lld.md` | Implemented, Verify |
| REQ-TS-060 | The Gateway writes the time to the Field Device again on the next successful Modbus exchange (`TS_NOTIFY_BIT_RETRY`) | `time_service.c`; the two-board bench of 2026-11-02 (time push, PR #153) | Implemented |
| REQ-NF-102 | AlarmService evaluates every polling cycle and clears with hysteresis | `alarm_service.c`, TC-AS tests | Implemented |
| REQ-NF-114 | The polling interval is limited to 100 ms .. 10 s on both boards (config services, register map, LCD field) | config service tests (boundaries), TC-LCDUI config tests | Implemented (range corrected at closure: it was 100 ms .. 60 s) |

## 5. Resources and maintainability

| ID | Where | Evidence | Status |
|---|---|---|---|
| REQ-NF-400 | Gateway image: main RAM 97 080 of 98 304 B (98.75 %, **1 224 B spare**), SRAM2 11 440 of 32 768 B | `scripts/build-gateway-app.sh` (the link fails if the image does not fit), 2026-11-08 | By construction. The margin is thin: any new static buffer needs a budget check |
| REQ-NF-401 | Gateway image: flash 336 608 B of 1 MB (32 %) | same build | By construction |
| REQ-NF-406 | One 1 MB CircularFlashLog ring, under the 3 MB maximum | `flash-partition-layout.md`, SRS note | By construction |
| REQ-NF-502 | clang-format 17 check and cppcheck run on every pull request (CI) | `.github/workflows/ci.yml` | By construction. A full BARR-C review is a verification step |
| REQ-NF-505 | Public headers carry `@brief`/`@param` comments; no Doxygen run is part of CI | headers | Verify (run Doxygen and read the warnings) |
| REQ-NF-506 | Application and middleware depend on the driver headers and on `i*_t` interfaces, not on vendor headers (`scripts/check-hal-encapsulation.sh`) | the script, run in CI | By construction |

## 6. Requirements the matrix finds only in the HLD

| ID | Where | Evidence | Status |
|---|---|---|---|
| REQ-SA-110 | A remote `read` control command triggers an on-demand acquisition through `sensor_task_request_read`, the same SensorTask pipeline as the periodic read | PR #168, `cloud-publisher-lld.md` control commands | Implemented, Verify |
| REQ-SA-150 | The sensor snapshot feeds the Modbus register map and the LCD (`get_snapshot`) | `modbus_register_map.c`, `lcd_ui.c` | Implemented |
| REQ-AM-011 | Per-sensor hysteresis in the alarm thresholds (`thresholds sensor n: high, low, hyst` in the boot log) | `alarm_service.c`, TC-AS tests | Implemented |
| REQ-CC-030 | Telemetry and health intervals are configuration parameters (`telemetry_interval_ms`, `health_interval_ms`), settable from the console and by cloud command | `config_params.h`, `cloud-publisher-lld.md` | Implemented. The LCD offers only the polling interval and the alarm thresholds, so "locally via the LCD" is met for the polling rate, not for these two (see the closure report, deviation D-1) |
| REQ-CC-040 | Telemetry and health intervals are configuration parameters (`telemetry_interval_ms`, `health_interval_ms`), settable from the console and by cloud command | `config_params.h`, `cloud-publisher-lld.md` | Implemented. The LCD offers only the polling interval and the alarm thresholds, so "locally via the LCD" is met for the polling rate, not for these two (see the closure report, deviation D-1) |
| REQ-CC-071 | Every payload starts with `"schema_version":"1.1"` | `cloud_publisher_json.c`, TC-CP-T95 | Implemented |
| REQ-DM-021 | A restart that is not confirmed within the timeout is cancelled | LifecycleController confirm timer, `lifecycle-controller-lld.md` | Implemented, Verify |
| REQ-DM-062 | A failed image validation logs an error and rejects the update | `update_service_task.c` (`LOG_ERROR` on verify), UpdateService LLD | Implemented |
| REQ-NF-207 | Alarm notifications are published with QoS 1 | `cloud_publisher.c` (alarm publish with `MQTT_QOS_1`), `mqtt-client.md` | Implemented |
| REQ-NF-209 | Reconnect after 2 s, doubling to 60 s, retried indefinitely | `mqtt-client.md`, SRS note of PR #166 | Implemented |
| REQ-NF-404 | Field Device image: flash 490 772 B of 2 MB, RAM 209 168 B of 256 KB (GUI image) | `scripts/build-fd-app.sh`, 2026-11-08 | By construction |
