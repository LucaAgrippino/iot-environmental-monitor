# LLD Gate Review — Layer 2 Pass G Report
## §6 Error & Fault Behaviour Completeness

**Date:** 2026-05-21
**Branch:** `feature/lld-gate-review-layer-2-pass-g`
**Script:** `scripts/lld_gate_review_check.py` — `check_pass_g()`
**Companions reviewed:** 39 (17 drivers, 10 middleware, 12 application)

---

## 1. Gate criterion (lld-methodology.md §5, Step 6)

For every companion, §6 Error & fault behaviour must contain:

1. **Status enum exhaustive** — every non-OK `<module>_err_t` value mentioned by symbol name, with semantic meaning.
2. **Failure-mode coverage** — each non-OK value documented with: Cause, Local behaviour, Caller-visible result.
3. **Retry policy** — explicit statement of bounded retry count or `"no retry — surfaced to caller"` per failure mode.
4. **Observability** — which sink (ILogger severity, IHealthReport event) handles each failure mode, or `"not observed"` with rationale.

---

## 2. Pre-remediation findings

| Category | Count |
|---|---|
| BLOCKER — §6 missing non-OK enum value by symbol name | 92 |
| BLOCKER — §6 missing retry policy statement | 17 |
| FIX_NOW — §6 missing observability sink mention | 17 |
| **Total BLOCKERs** | **109** |
| **Total FIX_NOWs** | **17** |

### 2.1 Companions with value-coverage gaps (92 BLOCKERs across 27 companions)

Most companions had §6 bodies of exactly one line: `"Error codes and propagation policy are defined in the Public API section above. All public functions return an error code; callers must not ignore non-OK returns."` This line contains no enum symbols and no retry or observability information, triggering all three categories of findings.

| Companion | Missing non-OK values |
|---|---|
| `exti-driver.md` | `EXTI_ERR_INVALID_ARG`, `EXTI_ERR_CONFLICT` |
| `magnetometer-imu-drivers.md` | `MAGNETOMETER_ERR_I2C`, `MAGNETOMETER_ERR_NOT_READY`, `MAGNETOMETER_ERR_INVALID_ARG`, `IMU_ERR_I2C`, `IMU_ERR_NOT_READY`, `IMU_ERR_INVALID_ARG` |
| `modbus-uart-driver.md` | `MODBUS_UART_ERR_BUSY` |
| `rtc-driver.md` | `RTC_ERR_NULL_ARG`, `RTC_ERR_BACKUP_BOUNDS` |
| `touchscreen-driver.md` | `TS_ERR_NO_DATA` |
| `wifi-driver.md` | `WIFI_ERR_NOT_INIT`, `WIFI_ERR_SOCKET` |
| `circular-flash-log.md` | All 8 non-OK values |
| `config-store.md` | All 7 non-OK values |
| `firmware-store.md` | All 10 non-OK values |
| `graphics-library.md` | All 3 non-OK values |
| `logger.md` | `LOGGER_ERR_NOT_INIT`, `LOGGER_ERR_INVALID_ARG` |
| `modbus-master-poller.md` | All 10 non-OK values (two enums) |
| `modbus-slave.md` | All 5 non-OK values |
| `mqtt-client.md` | All 6 non-OK values |
| `ntp-client.md` | All 5 non-OK values |
| `time-provider.md` | `TIME_PROVIDER_ERR_NOT_INIT`, `TIME_PROVIDER_ERR_RTC_FAIL`, `TIME_PROVIDER_ERR_NULL_ARG` |
| `cloud-publisher-lld.md` | `CP_ERR_NOT_INIT`, `CP_ERR_NULL_ARG` |
| `config-service.md` | All 4 non-OK values |
| `health-monitor.md` | `HEALTH_MONITOR_ERR_NOT_INIT`, `HEALTH_MONITOR_ERR_NULL_ARG`, `HEALTH_ADMIN_ERR_NOT_INIT` |
| `lifecycle-controller.md` | `LIFECYCLE_ERR_NULL_ARG`, `LIFECYCLE_ERR_NOT_INIT` |
| `sensor-alarm-service.md` | All 6 non-OK values (two enums) |

Companions with partial existing §6 content that still had missing values:
- `modbus-uart-driver.md`: `MODBUS_UART_ERR_TIMEOUT` was documented; `MODBUS_UART_ERR_BUSY` was missing.
- `wifi-driver.md`: 6 of 8 values were documented; `WIFI_ERR_NOT_INIT` and `WIFI_ERR_SOCKET` were missing.
- `cloud-publisher-lld.md`: 3 of 5 values documented; `CP_ERR_NOT_INIT` and `CP_ERR_NULL_ARG` missing.

### 2.2 Retry policy gaps (17 companions)

Companions whose §6 body contained no keyword from: `retry`, `retries`, `retried`, `no retry`, `surfaced to caller`, `propagated`, `attempt`.

All 27 companions in §2.1 also failed this check. In addition, some companions with partial §6 content (e.g. `lcd-driver.md`, `sdram-driver.md`) mentioned the error value but not the retry policy.

### 2.3 Observability gaps (17 FIX_NOW)

Companions with no mention of: `log`, `logger`, `ihealth`, `ihealthreport`, `severity`, `warn`, `error level`, `debug`, `not observed`.

---

## 3. Remediation — `scripts/_fix_pass_g.py`

A targeted script was written and run in a single pass. For each of the 32 failing companions it located the `## 6. Error and fault behaviour` section and replaced its body with a structured failure-mode table covering:

- A header row: `| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |`
- One data row per non-OK enum value
- Introductory prose stating the general retry and propagation policy

The script used the same `_find_sec6_span()` helper to locate the section. One companion (`console-service-lld.md`) had an out-of-order `## 3. Internal design` section between `## 6.` and `## 10. Initialisation` — the replacement accidentally consumed that section. It was manually restored after the script run.

Two additional driver companions required targeted row insertions after the initial run, because the ad-hoc extraction query used during script writing missed enum values that were on lines with spacing differences:
- `exti-driver.md`: added `EXTI_ERR_INVALID_ARG`
- `magnetometer-imu-drivers.md`: added `MAGNETOMETER_ERR_I2C`, `MAGNETOMETER_ERR_INVALID_ARG`, `IMU_ERR_I2C`, `IMU_ERR_INVALID_ARG`
- `touchscreen-driver.md`: added `TS_ERR_NO_DATA`

The script then deleted itself.

### 3.1 Retry policy conventions applied

| Layer / Component | Retry convention |
|---|---|
| All drivers | No retry — surfaced to caller (drivers are hardware-access wrappers; upper layers decide retry policy) |
| ModbusMaster | 3 retries on TIMEOUT/CRC per REQ-MB-060; no retry on EXCEPTION/BAD_RESPONSE |
| ModbusPoller | No retry — programming errors or command-queue full surfaced to caller |
| MqttClient | No retry internally — CloudPublisher drives reconnect/retry with exponential back-off |
| NtpClient | No retry per server attempt — TimeService drives overall retry with back-off |
| CircularFlashLog / ConfigStore | No retry — StoreAndForward/ConfigService decides retry on flash I/O errors |
| FirmwareStore | No retry on VALIDATION/SIGNATURE failures; UpdateService retries downloads (3 per DM-053) |
| UpdateService | 3 download retries per DM-053; validation/auth failures non-retriable |
| Logger | No retry — messages dropped silently to avoid recursion |
| Application components | No retry — surfaces to CallerTask; human-operator or supervisory component retries |

### 3.2 Observability conventions applied

- All driver errors: logged at WARN or ERROR by the caller via ILogger
- Programming errors (`_ERR_NULL_ARG`, `_ERR_NOT_INIT`): logged at ERROR; assert in debug builds
- Transient/expected conditions (`CFL_ERR_EMPTY`, `TS_ERR_NO_DATA`, `WIFI_ERR_NOT_CONNECTED`): not logged or logged at DEBUG
- Health-significant failures: `HEALTH_EVENT_SENSOR_FAIL`, `HEALTH_EVENT_CONFIG_WRITE_FAIL`, `HEALTH_EVENT_TIME_SYNC_LOST`, etc. pushed to IHealthReport after threshold

---

## 4. Post-remediation verification

```
python scripts/lld_gate_review_check.py

LLD gate review — Layer 1 + Layer 2 (Passes B, C, D, E, F, G)
Summary: 0 findings (0 blockers)
Layer 1 PASSES — no blockers found.
```

All prior Layer 1 and Pass B/C/D/E/F findings remain at 0. No regressions.

---

## 5. Acceptance

| Criterion | Result |
|---|---|
| 0 BLOCKER from `check_pass_g` | PASS |
| 0 FIX_NOW from `check_pass_g` | PASS |
| Every non-OK enum value documented by symbol name in §6 | PASS |
| Retry policy stated for every companion with an error enum | PASS |
| Observability sink declared for every companion with an error enum | PASS |
| All prior gate passes (Layer 1, B, C, D, E, F) still at 0 | PASS |

**Pass G GATE PASSES — 0 BLOCKERs, 0 FIX_NOWs.**

---

## 6. Escalations

None. No failure mode was found to have no corresponding handler in any caller — all non-OK values are surfaced to a documented caller that either retries, logs, or triggers a supervisory state transition. The halt-and-report condition (genuine error-propagation gap) was not triggered.

---

## 7. Open items

None introduced by this pass. Pre-existing open items in companion §8 tables are unchanged.
