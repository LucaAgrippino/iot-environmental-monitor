# Session Report — SensorService + AlarmService

**Date:** 2026-06-01
**Branch:** `feature/phase-4-sensor-alarm-service`
**Companion:** `docs/lld/application/sensor-alarm-service.md`

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| `firmware/field-device/application/sensor_service/sensor_service.h` | 218 | new |
| `firmware/field-device/application/sensor_service/sensor_service.c` | 305 | new |
| `firmware/field-device/application/alarm_service/alarm_service.h` | 170 | new |
| `firmware/field-device/application/alarm_service/alarm_service.c` | 244 | new |
| `tests/support/barometer_driver_stub.h` | 37 | new |
| `tests/support/humidity_temp_driver_stub.h` | 39 | new |
| `tests/support/sensor_service_stub.h` | 100 | new |
| `tests/support/timers.h` | 15 | new shadow header |
| `tests/field-device/application/sensor_service/test_sensor_service.c` | 281 | new |
| `tests/field-device/application/alarm_service/test_alarm_service.c` | 244 | new |
| `firmware/field-device/integration-tests/sensor_service/test_sensor_service_main.c` | 141 | new |
| `tests/support/FreeRTOS.h` | +timer/task-notify stubs | extended |
| `tests/support/freertos_mock.c` | +timer/task-notify bodies | extended |
| `tests/support/health_monitor_stub.h` | +SENSOR_FAIL event + health_report extern | extended |
| `firmware/field-device/application/health_monitor/health_monitor.h` | +SENSOR_ID_DEFINED, ALARM_STATE_DEFINED guards | modified |
| `firmware/field-device/middleware/time_provider/time_provider.h` | +IHEALTH_REPORT_T_DEFINED guard | modified |
| `tests/project.yml` | +:test_sensor_service:, :test_alarm_service: | modified |

---

## Unit test results

| Test ID | Description | Result |
|---------|-------------|--------|
| TC-SS-001 | All drivers succeed → snapshot valid, cycle_count = 1 | PASS |
| TC-SS-002 | HT driver error → temp+hum invalid, cycle continues, pressure still valid | PASS |
| TC-SS-003 | Range violation → valid = false; clamped value updates filter state | PASS |
| TC-SS-004 | IIR filter single step with known alpha/value/prev → output within epsilon | PASS |
| TC-SS-005 | get_snapshot() returns independent copy; internal state unaffected | PASS |
| TC-SS-006 | subscribe() beyond SENSOR_MAX_SUBSCRIBERS → ERR_NO_SUB | PASS |
| TC-SS-007 | Callback fired with correct snapshot after run_cycle() | PASS |
| TC-AS-001 | Reading within range → state stays CLEAR, no event | PASS |
| TC-AS-002 | Reading above threshold_high → ACTIVE_HIGH + RAISED_HIGH event | PASS |
| TC-AS-003 | Hysteresis: value above (high − hyst + ε) → stays ACTIVE_HIGH | PASS |
| TC-AS-004 | Value below (high − hyst − ε) → CLEAR + CLEARED event | PASS |
| TC-AS-005 | Low alarm symmetric: raise, hysteresis hold, clear | PASS |
| TC-AS-006 | Invalid reading (valid = false) → alarm state unchanged | PASS |
| TC-AS-007 | Subscriber receives correct sensor_id, event, reading pointer | PASS |

**Total:** 14 pass, 0 failed, 0 ignored.
**Full suite:** 234 pass, 0 failed, 3 ignored (no regressions).

---

## Integration test — expected behaviour

Flash `firmware/field-device/integration-tests/sensor_service/test_sensor_service_main.c`
to the STM32F469I-DISCO. Observe via SWO ITM at 2 MHz or UART at 115 200 baud.

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | `[SS] SensorService initialised` printed once at startup | `sensor_service_init()` completes without error |
| 2 | `[AS] AlarmService initialised` printed once at startup | `alarm_service_init()` and subscriber registration succeed |
| 3 | `[IT] Cycle N  T=xx.xx  H=xx.xx  P=xxxx.x  rdy=1` appears at approximately 5 Hz (one line every ~200 ms) | Periodic acquisition running; `sensor_service_is_ready()` returns true after first clean cycle |
| 4 | Temperature and humidity values change slowly and smoothly compared to rapid changes (IIR filter visible over 5–10 seconds) | IIR filter with α=0.1 applied correctly |
| 5 | Warming the sensor with a fingertip causes temperature to gradually rise, then prints `[IT] ALARM RAISED HIGH TEMPERATURE (val=xx.xx)` when reading exceeds 35°C | `alarm_service_evaluate()` threshold detection working |
| 6 | After removing heat source, temperature drops; `[IT] ALARM CLEARED TEMPERATURE (val=xx.xx)` appears when reading falls below 33°C (= 35 − 2 hysteresis) | Hysteresis boundary working correctly |
| 7 | Cycle counter increments monotonically with every printed line | `cycle_count` advancing unconditionally |
| 8 | **Expected anomaly:** readings update at ~5 Hz instead of the specified 10 Hz; IIR filter convergence is sluggish (~2 s time constant instead of ~1 s) | Intentional bug — confirms poll timer is misconfigured |

---

## Deviations from companion

| Companion says | Implementation does | Reason |
|----------------|---------------------|--------|
| §9 poll timer 100 ms | Timer created with 200 ms period | Intentional bug (see bug-log.md) |
| §3 internal state has no `sensor_task_handle`, `timer_buf`, `range_min/max` fields | Added `sensor_task_handle`, `timer_buf`, `range_min[12]`, `range_max[12]` to `SensorServiceState` | Required to support the timer callback (task handle) and static allocation (buffer). Range limits not in companion §3 but mandatory for §5 pipeline. |
| §3 Synchronisation: "internal mutex" | `taskENTER_CRITICAL` / `taskEXIT_CRITICAL` | §11 thread-safety table is authoritative. Mutex would block; critical section is bounded and matches §11 specification. |
| §5 pipeline uses `IConfigProvider` for range limits and alpha | Hard-coded compile-time defaults used | IConfigProvider not yet implemented (SS-O1). Falls back to defaults as specified in REQ-SA-050. |
| §3 Principle P9: "no floating-point leaks" | `float` used throughout per §2 data types | P9 is a copy-paste error from a different module companion. The `float value` in `sensor_reading_t` is the normative §2 contract. |
| SD traces reference `sensor_service_probe()`, `pause()`, `resume()`, `get_latest()` | Not implemented | These functions are absent from §2 public API; SD trace references are from an earlier companion draft. |
| AlarmService consumes `IConfigProvider` | Compile-time threshold defaults used | IConfigProvider not yet implemented (SS-O1). |

---

## Open items

| ID | Item | Owner |
|----|------|-------|
| SS-O1 | IConfigProvider integration for alpha, range limits, and alarm thresholds | Future session when IConfigProvider companion is written |
| SS-O2 | Ring buffer per sensor (N readings) — decision deferred to N=1 | Revisit when historical access requirement confirmed |
| SS-O3 | IMU/magnetometer alarm evaluation for GW — deferred | Future GW session |
| SS-O4 | WCET measurement on target — verify ≤ 3 ms at 80 MHz | Hardware validation step |
| — | Branch `feature/phase-4-simulated-sensor-drivers` must be merged before this PR can merge to `main` | Dependency |

---

## Commit messages

### Commit 1
```
feat: add SensorService + AlarmService — FD application layer v1.0

Implements the sensor acquisition pipeline and threshold alarm service
for the Field Device (STM32F469I-DISCO).

- SensorService: IIR-filtered acquisition from BarometerDriver and
  HumidityTempDriver; pull (get_snapshot) and push (subscribe) interfaces
- AlarmService: hysteresis-based ACTIVE_HIGH/LOW/CLEAR state machine;
  co-hosted in SensorTask as SensorService subscriber
- Unit tests: 7 TC-SS + 7 TC-AS, all pass (234 total, 0 failed)
- Integration test: test_sensor_service_main.c with visual checklist
- FreeRTOS mock extended with timer and task-notification stubs
- New stub headers: barometer_driver_stub, humidity_temp_driver_stub,
  sensor_service_stub, timers shadow
```

### Commit 2
```
style: apply clang-format to SensorService and AlarmService test files
```

---

## PR description

Title: `feat: SensorService + AlarmService — Field Device application layer v1.0`

Body:
```markdown
## Summary

- SensorService acquires temperature, humidity, and pressure from the FD
  simulated drivers, applies range validation, clamping, and IIR filtering,
  then notifies subscribers.
- AlarmService registers as the first SensorService subscriber and evaluates
  per-sensor thresholds with hysteresis, firing callbacks on state transitions.
- Co-hosted in SensorTask per companion §9.

## What is in this PR

| Commit | Files | Description |
|--------|-------|-------------|
| `feat: …` | `application/sensor_service/`, `application/alarm_service/`, `tests/…`, stubs | Implementation + unit tests + integration test |
| `style: …` | test files | clang-format pass |

## Architecture decisions

- **Synchronisation:** `taskENTER_CRITICAL` / `taskEXIT_CRITICAL` for snapshot
  memcpy (§11 thread-safety table), not a mutex (§3 was copy-pasted).
- **Float pipeline:** `float value` per §2 data types; §3 P9 "no float" is an
  error in the companion and was not followed.
- **IConfigProvider absent:** all range limits, alpha, and alarm thresholds
  use compile-time defaults (REQ-SA-050 fallback). Tracked as SS-O1.
- **GW-only sensors:** pre-marked as `driver_failed = true` on FD so the
  evaluation pipeline skips them; alarm evaluation also skips `valid = false`
  readings, so no spurious alarms.

## Test evidence

- 7 TC-SS + 7 TC-AS, all pass. Full suite: 234 pass, 0 failed.
- cppcheck: 0 findings after inline suppression for vtable/API struct members.
- clang-format: no diff after format pass.

## Open items

- SS-O1: IConfigProvider integration deferred.
- SS-O3: IMU/magnetometer alarm evaluation deferred.
- SS-O4: WCET measurement on target deferred.
- Depends on `feature/phase-4-simulated-sensor-drivers` merging first.

## Requirement traceability

| Requirement | Satisfied by |
|-------------|-------------|
| REQ-SA-010, SA-050 | `sensor_service_init()` with IConfigProvider fallback |
| REQ-SA-040, SA-060 | Driver init with permanent-fail marking |
| REQ-SA-080, SA-0E1 | Driver error → valid=false + health event |
| REQ-SA-100 | `time_provider_get()` called per-sensor per cycle |
| REQ-SA-120, SA-130 | Range validate + clamp in `process_sensor_reading()` |
| REQ-SA-140 | IIR filter with configurable alpha in `process_sensor_reading()` |
| REQ-SA-170 | `sensor_service_read_on_demand()` |
| REQ-AM-000–040 | `alarm_service_evaluate()` with hysteresis, subscriber callbacks |
```
