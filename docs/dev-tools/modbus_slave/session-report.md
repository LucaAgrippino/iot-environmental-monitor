# Session Report — ModbusSlave

**Date:** 2026-06-10
**Branch:** `feature/phase-4-modbus-slave`
**Companion:** `docs/lld/middleware/modbus_slave/modbus-slave.md` (v1.0)

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| `firmware/field-device/middleware/modbus_slave/modbus_crc.h` | 10 | New — CRC-16/IBM declaration |
| `firmware/field-device/middleware/modbus_slave/modbus_crc.c` | 40 | New — 256-entry reflected table (poly 0xA001), verified against {01 03 00 00 00 02} → 0x0BC4 |
| `firmware/field-device/middleware/modbus_slave/modbus_slave.h` | 60 | New — public API, IModbusRegisterMap vtable, modbus_slave_stats_t, modbus_slave_err_t |
| `firmware/field-device/middleware/modbus_slave/modbus_slave.c` | 406 | New — reactive singleton, FC03/04/06/16, ISR callback, stats with critical sections |
| `tests/field-device/middleware/modbus_slave/test_modbus_slave.c` | 430 | New — 23 Unity tests TC-MBS-001..023 |
| `firmware/field-device/integration-tests/modbus_slave/test_modbus_slave_main.c` | 239 | New — two-task integration test harness (ModbusTask + StatsTask) |
| `tests/support/modbus_uart_driver_stub.h` | 44 | New — minimal driver declarations swapped in under TEST |
| `tests/support/FreeRTOS.h` | +12 | Extended — eNotifyAction enum, xTaskNotifyFromISR declaration |
| `tests/support/freertos_mock.c` | +18 | Extended — xTaskNotifyFromISR mock implementation + reset |
| `tests/project.yml` | +4 | Added :test_modbus_slave: block (STM32F469xx, TEST, LOG_LEVEL_MIN=-1) |
| `docs/lld/middleware/modbus-slave.md` | +140/-132 | Updated — COMPLETE status, v1.0, June 2026 |

---

## Unit test results

**Suite:** `test_modbus_slave` (FD — STM32F469xx)

| # | Test ID | Description | Result |
|---|---------|-------------|--------|
| 1 | TC-MBS-001 | init(NULL reg_map) → ERR_NULL_ARG | PASS |
| 2 | TC-MBS-002 | init(addr=0) → ERR_INVALID_ADDR | PASS |
| 3 | TC-MBS-003 | init(addr=248) → ERR_INVALID_ADDR | PASS |
| 4 | TC-MBS-004 | address mismatch → silent drop; mismatches==1 | PASS |
| 5 | TC-MBS-005 | wrong CRC → silent drop; crc_errors==1 | PASS |
| 6 | TC-MBS-006 | FC04 read input register → 7-byte response; successful==1 | PASS |
| 7 | TC-MBS-007 | FC04 invalid addr (ERR_INVALID_ADDR) → exception 0x02 | PASS |
| 8 | TC-MBS-008 | FC03 read holding register → correct 7-byte response | PASS |
| 9 | TC-MBS-009 | FC06 write single → write_holding called; 8-byte echo ACK | PASS |
| 10 | TC-MBS-010 | FC06 invalid value (ERR_INVALID_VALUE) → exception 0x03 | PASS |
| 11 | TC-MBS-011 | FC06 CMD_SOFT_RESTART 0xA5A5 → accepted ACK | PASS |
| 12 | TC-MBS-012 | FC06 CMD_SOFT_RESTART wrong value → exception 0x03 | PASS |
| 13 | TC-MBS-013 | FC16 byte_count ≠ 2×qty → exception 0x03 | PASS |
| 14 | TC-MBS-014 | FC16 two registers → both writes in order; 8-byte ACK | PASS |
| 15 | TC-MBS-015 | unsupported FC (0x01) → exception 0x01; unsupported_fc==1 | PASS |
| 16 | TC-MBS-016 | response CRC low-byte-first | PASS |
| 17 | TC-MBS-017 | get_stats(NULL) → ERR_NULL_ARG | PASS |
| 18 | TC-MBS-018 | get_stats after mismatch+CRC err+valid → snapshot correct | PASS |
| 19 | TC-MBS-019 | reset_stats → all counters zero | PASS |
| 20 | TC-MBS-020 | set_address(0) → ERR_INVALID_ADDR | PASS |
| 21 | TC-MBS-021 | set_address(7) → addr 7 processed, addr 1 dropped | PASS |
| 22 | TC-MBS-022 | read_input ERR_DEVICE_FAIL → exception 0x04 | PASS |
| 23 | TC-MBS-023 | process/set_address before init → ERR_NOT_INIT | PASS |

**Total: 23 PASS, 0 FAIL, 0 IGNORED**

---

## Architecture decisions

### D1 — modbus_uart_driver swap-in via `#ifndef TEST`

In production builds, `modbus_slave.c` includes the real
`modbus_uart_driver/modbus_uart_driver.h`. Under `TEST`, it includes
`modbus_uart_driver_stub.h` — a minimal header whose basename
(`modbus_uart_driver_stub`) does not match any real `.c` file, so
Ceedling never auto-links the real driver. Stub implementations
(attach\_rx, get\_rx\_frame, transmit) are defined inline in the test TU.

### D2 — Two-phase init pattern

`ModbusTask` is created first (before the scheduler) so that
`xTaskGetCurrentTaskHandle()` returns a valid handle inside the task body.
`modbus_slave_init()` is then called at the top of the task function,
capturing the handle for subsequent `xTaskNotifyFromISR()` calls from the
ISR callback.

### D3 — Logger suppression

`LOG_LEVEL_MIN=-1` collapses all `LOG_*` macros to `((void)0)`.
Additionally, `#ifndef TEST` guards in `modbus_slave.c` skip the logger
include entirely — no `logger.c` auto-link in the test build.

### D4 — Stats critical sections

`modbus_slave_get_stats()` and `modbus_slave_reset_stats()` wrap their
`memcpy`/`memset` in `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()`.
These are no-ops in the host build but protect the stats snapshot on
target against partial reads by the `ModbusTask` (which increments
counters without a lock).

---

## Commits on `feature/phase-4-modbus-slave`

| Hash | Message |
|------|---------|
| dc2148f | feat: add ModbusSlave v1.0 -- Modbus RTU slave middleware |
| 62b489d | test: add ModbusSlave Unity unit tests TC-MBS-001..023 |
| a21629f | test: add ModbusSlave hardware integration test harness |
| 6aa13a6 | docs: update ModbusSlave LLD companion to v1.0 COMPLETE |
