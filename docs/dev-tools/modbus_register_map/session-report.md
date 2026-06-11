# Session Report — ModbusRegisterMap

**Date:** 2026-06-11
**Branch:** `feature/phase-4-modbus-register-map`
**Companion:** `docs/lld/application/modbus-register-map-lld.md` (v1.0)

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| `firmware/field-device/application/modbus_register_map/modbus_register_map.h` | 234 | New — public API, DIP vtable, error codes, register constants, new singleton vtable types, test hooks |
| `firmware/field-device/application/modbus_register_map/modbus_register_map.c` | 1034 | New — 39-slot sorted register table, FC03/04/06/16 dispatch, slot handlers, intentional bug MRM-BUG-001 |
| `tests/support/mrm_deps_stub.h` | 316 | New — consolidated type-only stub for all 11 provider deps; `sensor_reading_t.value` is `int32_t` |
| `tests/field-device/application/modbus_register_map/test_modbus_register_map.c` | 540 | New — 27 Unity tests TC-MRM-001..027 |
| `firmware/field-device/integration-tests/modbus_register_map/test_modbus_register_map_main.c` | 535 | New — 5-phase hardware integration test, FreeRTOS task, stub vtables |
| `tests/project.yml` | +4 | Added `:test_modbus_register_map:` block (STM32F469xx, TEST, LOG_LEVEL_MIN=-1) |

---

## Unit test results

**Suite:** `test_modbus_register_map` (FD — STM32F469xx)

| Result | Count |
|--------|-------|
| PASSED | 26    |
| FAILED | 1 (intentional — TC-MRM-009, MRM-BUG-001) |
| IGNORED | 0   |
| TOTAL  | 27    |

TC-MRM-009 (`test_TC_MRM_009_fc04_pressure_bug_detected`) fails by design:
`read_pressure()` reads from `SENSOR_ID_HUMIDITY` instead of `SENSOR_ID_PRESSURE`,
so the register returns 5000 (humidity) when the expected value is 10132 (pressure).
See `docs/dev-tools/modbus_register_map/bug-log.md`.

---

## Key design decisions

### 1. vtable naming: `imodbus_register_map_t` (snake_case)

The existing `modbus_slave.h` defines `IModbusRegisterMap` as a per-register
function pointer typedef. The Application-layer bulk interface had to be named
differently to avoid a redefinition conflict. The new type `imodbus_register_map_t`
follows the project P10 snake_case convention. Both will be unified in a future
ModbusSlave refactor (documented as MRM-DEVIATION-001 in the header).

### 2. New singleton vtable types (no ctx/self pointer)

`imodbus_slave_stats_t`, `imodbus_slave_t`, and `ilifecycle_controller_t` are
new vtable types injected into MRM. They follow the existing project pattern
(singleton-style, no ctx pointer) rather than the LLD companion pseudo-code
which passed a self pointer.

### 3. MODBUS_SLAVE_ADDR register at 0x0150

The HLD data-spec lists 0x0150–0x01FF as reserved. The MODBUS_SLAVE_ADDR
register is placed at the first reserved address (0x0150) to support the
Mediator role (TC-MRM-022). The deviation is documented in the slot table
comment.

### 4. LCD registers (0x0140–0x0141) as placeholders

`config_params.h` has no LCD fields yet. The handlers return a constant
read value (80) and accept any value in range as a no-op write.

### 5. MODBUS_TIMEOUT registers (0x0038–0x0039) always return zero

`modbus_slave_stats_t` has no timeout counter. `read_zero()` is used as
a placeholder.

### 6. SENSOR_FAULT bit (bit 6 of alarm flags)

Iterates over indices 0–2 only (the three environmental sensors) to avoid
false positives from IMU/mag sensors which are Gateway-only and always
report invalid on FD.

### 7. Opaque type test pattern

Tests cannot `sizeof(modbus_register_map_t)` (opaque in public header).
`modbus_register_map_get_test_instance()` returns a pointer to the module's
internal `static modbus_register_map_t s_mrm`, eliminating the size dependency.

---

## Stub strategy

`mrm_deps_stub.h` replaces all five production headers under `TEST`:

- **Key difference from `sensor_service_stub.h`:** `sensor_reading_t.value` is
  `int32_t` (fixed-point). The existing stub incorrectly uses `float` and is
  incompatible with MRM tests.
- **`ihealth_report_t`**: defined with the same struct tag (`ihealth_report_s`)
  as `health_monitor.h` so the typedef is compatible.
- **`modbus_slave_stats_t`**: guarded by `MODBUS_SLAVE_STATS_DEFINED` to
  prevent redefinition if `modbus_slave.h` is ever included alongside.
- **`config_params.h`**: included directly (no transitive deps, safe to link).

---

## Integration test

The integration test (`test_modbus_register_map_main.c`) exercises ModbusRegisterMap
in a live FreeRTOS environment on the STM32F469I-DISCO using stub vtables:

| Phase | Coverage |
|-------|----------|
| 1 — Identity registers | FC04 read of 8 registers at 0x0000; verify MAP_VERSION=1, VENDOR_CODE=0x1A45 |
| 2 — Sensor registers | FC04 read of TEMPERATURE and HUMIDITY (stub values) |
| 3 — Config read/write | FC03 read sampling_period=1000; FC06 write 2000; set_param called |
| 4 — Mediator | FC06 write slave_addr=5; both set_param + set_slave_address confirmed |
| 5 — poll_stats | poll_stats updates cached snapshot; health_write->update called |
| 6 — Periodic tick | 1 Hz tick via vTaskDelay to confirm no freeze or watchdog reset |
