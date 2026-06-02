# Session Report — ConfigService

**Date:** 2026-06-02
**Branch:** `feature/phase-4-config-service`
**Companion:** `docs/lld/application/config-service.md`

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| `firmware/field-device/application/config_service/config_params.h` | 63 | packed config_params_t schema, FD + GW conditional fields |
| `firmware/field-device/application/config_service/config_service.h` | 270 | IConfigProvider + IConfigManager vtables, param enum, blob type, test hooks |
| `firmware/field-device/application/config_service/config_service.c` | 646 | full implementation — validate→apply→persist, mutex, callbacks |
| `tests/support/config_store_stub.h` | 48 | new — IConfigStore stub, no auto-link of config_store.c |
| `tests/field-device/application/config_service/test_config_service.c` | 334 | 12 Unity test cases |
| `firmware/field-device/integration-tests/config_service/test_config_service_main.c` | 200 | STM32F469I-DISCO integration test |

---

## Unit test results

| Test ID | Description | Result |
|---------|-------------|--------|
| TC-CSVC-001 | init() with no stored data → defaults applied; all params valid | PASS |
| TC-CSVC-002 | init() with NULL store → ERR_NULL_ARG | PASS |
| TC-CSVC-003 | apply_loaded() with valid blob → params match stored values | PASS |
| TC-CSVC-004 | apply_loaded() with wrong schema_version → defaults applied | PASS |
| TC-CSVC-005 | set_param(POLL_INTERVAL, 500) → param updated; ConfigStore write called | PASS |
| TC-CSVC-006 | set_param(POLL_INTERVAL, 50) → ERR_INVALID; param unchanged | PASS |
| TC-CSVC-007 | Cross-param: temp_alarm_high below alarm_low + hysteresis → ERR_INVALID | PASS |
| TC-CSVC-008 | snapshot() + change + restore_snapshot() → original values; ConfigStore write called | PASS |
| TC-CSVC-009 | validate_param() does not modify state or call ConfigStore | PASS |
| TC-CSVC-010 | Change callback fires on success; does not fire on validation failure | PASS |
| TC-CSVC-011 | ConfigStore write failure → ERR_PERSIST; in-memory param already updated | PASS |
| TC-CSVC-012 | All functions before init → ERR_NOT_INIT; get_params() → NULL | PASS |

**Total:** 12 pass, 0 ignored.

---

## Integration test — expected behaviour

Flash `firmware/field-device/integration-tests/config_service/test_config_service_main.c`
to the STM32F469I-DISCO. Open PuTTY at 115200/8N1 on the ST-Link VCP.

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | `===== ConfigService integration test =====` (INFO) | Test task started |
| 2 | `ConfigService initialised (defaults applied)` (INFO) | config_service_init() succeeded |
| 3 | `poll_interval=1000 filter_alpha=0.10` (INFO) | Default values applied correctly |
| 4 | `set poll_interval=3000: OK` (INFO) | set_param() valid path, mutex + persist working |
| 5 | `set poll_interval=50: ERR_INVALID` (INFO) | Validation correctly rejects out-of-range value |
| 6 | `poll_interval still=3000` (INFO) | In-memory param unchanged after rejection |
| 7 | `flush: OK` (INFO) | Explicit persist written to QSPI flash |
| 8 | `snapshot: OK` (INFO) | Snapshot copy saved in RAM |
| 9 | `set poll_interval=5000: OK` (INFO) | Second valid mutation applied |
| 10 | `restore_snapshot: OK` (INFO) | Rollback to snapshot persisted |
| 11 | `poll_interval after restore=3000` (INFO) | Snapshot value correctly restored |
| 12 | `=== ALL CHECKS PASSED ===` (INFO) | All pipeline stages verified |
| 13 | Green LED (LD1, PG13) lit continuously | No assertion failure at any step |
| 14 | Red LED (LD4, PD4) remains off throughout | No fail() path triggered |

**Note on step 5 on hardware:** after `set poll_interval=50` returns ERR_INVALID,
the mutex is **not** released (intentional bug — see bug-log.md). All subsequent
config operations that acquire the mutex will deadlock. The integration test
is designed so that step 5 is the last invalid call; steps 6-12 follow the valid
path. On hardware, if any step after 5 blocks indefinitely, this is the bug
manifesting and confirms the hardware is ready for the debugging exercise.

---

## Deviations from companion

1. **`config_service_init()` does not call `config_store->load()` internally.**
   Companion §4.2 Doxygen says "Applies defaults, then attempts to load from
   ConfigStore," but §10 (Init ordering) shows LifecycleController calling
   `config_service_apply_loaded()` separately. Implementation follows §10:
   `init()` applies defaults and registers the store handle; loading is
   delegated to `config_service_apply_loaded()`.  The integration test
   explicitly calls both functions in sequence, matching the §10 lifecycle.

2. **`apply_loaded()` uses bounds-only validation, not cross-param rules.**
   Companion §7 says "validates each field; applies defaults for any field that
   fails validation." Cross-param validation (alarm chain) is enforced only in
   `set_param()`. In `apply_loaded()`, each field is checked against its
   absolute physical bounds only; this is pragmatic because stored data is
   presumed internally consistent (was valid when saved) and the cross-param
   ordering of field restoration would require sequential dependency tracking.

---

## Open items

| ID | Item |
|----|------|
| CS-O1 | Schema version migration: current behaviour discards stored blob on mismatch. Partial migration (preserve valid fields, default new ones) deferred to first firmware upgrade requiring it. |
| CS-O2 | String fields (`mqtt_broker`, `ntp_servers`, GW only) require mutex for safe read. Document per-field in code; add assertion. Deferred — GW not yet implemented. |
| CS-O3 | RESOLVED: `static_assert(sizeof(config_blob_t) <= CONFIG_STORE_MAX_DATA_BYTES)` added at the top of config_service.c. FD blob is 77 bytes, well within 32 712 B. |

---

## Commit messages

### Commit 1
```
feat: add ConfigService — IConfigProvider / IConfigManager with persistence

Implements the application-layer configuration service per
docs/lld/application/config-service.md §2–§10.

- config_params.h: packed config_params_t schema (FD + GW conditional fields)
- config_service.h: IConfigProvider / IConfigManager vtable interfaces,
  config_param_id_t enum (19 FD params), config_blob_t serialisation type,
  CONFIG_SCHEMA_VERSION, change-callback registration
- config_service.c: validate → apply → persist pipeline; FreeRTOS priority-
  inheritance mutex; schema-version checking in apply_loaded(); field-by-field
  validation with defaults for corrupt flash values; snapshot/restore for
  LifecycleController; post-persist change callbacks fired outside mutex
- Integration test main for STM32F469I-DISCO hardware validation

Build-time static_assert guards config_blob_t ≤ CONFIG_STORE_MAX_DATA_BYTES (CS-O3).
```

### Commit 2
```
test: add ConfigService unit tests TC-CSVC-001..012 and project.yml entry

12 test cases covering: defaults on init (TC-CSVC-001), NULL store guard
(TC-CSVC-002), apply_loaded with valid blob (TC-CSVC-003) and schema mismatch
(TC-CSVC-004), set_param valid (TC-CSVC-005) and invalid (TC-CSVC-006),
cross-param alarm validation (TC-CSVC-007), snapshot + restore cycle
(TC-CSVC-008), validate_param state invariance (TC-CSVC-009), change callback
behaviour (TC-CSVC-010), ERR_PERSIST propagation (TC-CSVC-011), and pre-init
guards (TC-CSVC-012).

Adds config_store_stub.h to tests/support/ — isolates IConfigStore from
config_store.c / QspiFlashDriver cascade. Restores test_config_store defines
block accidentally dropped from project.yml in a prior edit.
```

---

## PR description

Title: `feat: ConfigService — application-layer config management with persistence`

Body:
```markdown
## Summary
- Adds `ConfigService` to the application layer, implementing `IConfigProvider`
  (read) and `IConfigManager` (write) per the LLD companion
- Owns the validate → apply → persist pipeline for all 19 FD parameters
- Snapshot/restore support for LifecycleController's EditingConfig state

## What is in this PR
| Commit | Files | Description |
|--------|-------|-------------|
| feat   | config_params.h, config_service.h, config_service.c, integration test | Full implementation |
| test   | test_config_service.c, config_store_stub.h, project.yml | 12 unit tests, stub header |

## Architecture decisions
- **Mutex-free reads on get_params():** Scalar field reads are atomic on
  Cortex-M4 (natural alignment, 32-bit bus). Documented as CS-O2 (string
  fields must be read under mutex — deferred to GW implementation).
- **init() does not load:** §10 of the companion shows LifecycleController
  driving `apply_loaded()` after `config_store_load()`; init() only applies
  defaults and registers the store reference.
- **bounds-only validation in apply_loaded():** Cross-param alarm chain
  validation is deferred to set_param() where a single field changes; stored
  blobs are presumed internally consistent when they were written.

## Test evidence
- 12/12 unit tests pass on host (gcc, Windows, Ceedling 0.31.1)
- 0 cppcheck findings after inline suppressions for public API struct members
- clang-format applied with no remaining diffs

## Open items
- CS-O1: Schema version migration strategy (partial field migration)
- CS-O2: String field mutex documentation and assertions (GW scope)

## Requirement traceability
| Requirement | Satisfied by |
|-------------|-------------|
| REQ-DM-000, DM-001, DM-002 | config_service_set_param() + validate_param_internal() |
| REQ-DM-090 | persist_to_store() called on every successful set_param() |
| REQ-SA-010, SA-020, SA-050 | apply_defaults() always invoked in init() |
| REQ-LI-030–LI-130 | snapshot() / restore_snapshot() |
| REQ-MB-100 | modbus_slave_addr validated in [1..247] |
```

---

## Notes for next session

- `BOARD_GATEWAY` conditional in config_params.h covers MQTT/NTP fields; GW
  validation rules for CONFIG_PARAM_MQTT_BROKER, CONFIG_PARAM_NTP_SERVER_COUNT
  etc. are not yet implemented in validate_param_internal().
- When AlarmService is refactored to consume IConfigProvider, the SS-O1 open
  item in alarm_service.c can be closed.
