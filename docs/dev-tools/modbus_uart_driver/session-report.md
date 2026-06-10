# Session Report — ModbusUartDriver

**Date:** 2026-06-10
**Branch:** `feature/phase-4-modbus-uart-driver`
**Companion:** `docs/lld/drivers/modbus-uart-driver.md` (v1.0)

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| `firmware/field-device/drivers/modbus_uart_driver/modbus_uart_driver.h` | 213 | New — public API, types, test-reset hook |
| `firmware/field-device/drivers/modbus_uart_driver/modbus_uart_driver.c` | 376 | New — dual-board (F469/L475), board abstraction macros, ISR, TX/RX API |
| `tests/field-device/drivers/modbus_uart_driver/test_modbus_uart_driver_fd.c` | 333 | New — FD suite, compiled with -DSTM32F469xx |
| `tests/gateway/drivers/modbus_uart_driver/test_modbus_uart_driver_gw.c` | 306 | New — GW suite, compiled with -DSTM32L475xx |
| `firmware/field-device/integration-tests/modbus_uart_driver/test_modbus_uart_driver_main.c` | 232 | New — loopback integration test for STM32F469I-DISCO |
| `tests/mocks/stm32f469xx.h` | — | Extended: USART6, APB2ENR, SR bits (TC, IDLE), USART6_IRQn |
| `tests/mocks/stm32_cmsis_mock.c` | — | Extended: g_mock_usart6, APB2ENR reset |
| `tests/mocks/stm32l475xx.h` | — | Extended: USART_L4_TypeDef (ISR/ICR/RDR/TDR), UART4, NVIC helpers |
| `tests/mocks/stm32l475_cmsis_mock.c` | — | Extended: g_mock_uart4, NVIC counters, NVIC_EnableIRQ/DisableIRQ |
| `tests/project.yml` | — | Added :test_modbus_uart_driver_fd: and :test_modbus_uart_driver_gw: blocks |
| `docs/lld/drivers/modbus-uart-driver.md` | — | Updated: COMPLETE status, §2.5 tick source API, Clocks/NVIC fixed, test paths updated, MBUART-O3 closed |

---

## Unit test results

### FD suite (STM32F469xx — USART6)

| Test ID | Description | Result |
|---------|-------------|--------|
| T-MBUART-01 | `modbus_uart_init`: GPIOG/USART6 clocks, CR1 (UE+TE), CR3 (DEM) | IGNORE (BRR deferred — MBUART-O2) |
| T-MBUART-02 | `modbus_uart_attach_rx`: CR1 RE+RXNEIE+IDLEIE, NVIC enable | PASS |
| T-MBUART-03 | `modbus_uart_transmit` happy path — 8-byte frame, DR holds last byte | PASS |
| T-MBUART-04 | `modbus_uart_transmit` TXE timeout | PASS |
| T-MBUART-05 | `modbus_uart_transmit` TC timeout (TXE set, TC never asserts) | PASS |
| T-MBUART-06 | ISR RXNE only — 4 bytes, no callback | PASS |
| T-MBUART-07 | ISR IDLE after 4 bytes — RX_DONE callback, correct frame data | PASS |
| T-MBUART-08 | ISR ORE flag — RX_ERROR callback, rx_len reset to 0 | PASS |
| T-MBUART-09 | ISR buffer overrun (257 bytes before IDLE) — RX_ERROR on IDLE | PASS |
| T-MBUART-10 | `modbus_uart_get_rx_frame` after RX_DONE — correct bytes and length | PASS |
| T-MBUART-11 | Concurrent BUSY guard | IGNORE (requires two callers — deferred to integration test) |

**FD total: 9 PASS, 2 IGNORED, 0 FAILED.**

### GW suite (STM32L475xx — UART4)

| Test ID | Description | Result |
|---------|-------------|--------|
| T-MBUART-01 (GW) | `modbus_uart_init`: GPIOA/UART4 clocks, CR1 (UE+TE), CR3 (DEM) | IGNORE (BRR deferred — MBUART-O2) |
| T-MBUART-02 (GW) | `modbus_uart_attach_rx`: CR1 RE+RXNEIE+IDLEIE, NVIC enable | PASS |
| T-MBUART-03 (GW) | `modbus_uart_transmit` happy path — 8-byte frame, TDR holds last byte | PASS |
| T-MBUART-04 (GW) | `modbus_uart_transmit` TXE timeout | PASS |
| T-MBUART-05 (GW) | `modbus_uart_transmit` TC timeout | PASS |
| T-MBUART-06 (GW) | ISR RXNE only — 4 bytes via RDR, no callback | PASS |
| T-MBUART-07 (GW) | ISR IDLE after 4 bytes — RX_DONE, ICR.IDLECF written | PASS |
| T-MBUART-08 (GW) | ISR ORE flag — RX_ERROR, ICR.ORECF written, rx_len reset | PASS |
| T-MBUART-09 (GW) | ISR buffer overrun (257 bytes) — RX_ERROR on IDLE | PASS |
| T-MBUART-10 (GW) | `modbus_uart_get_rx_frame` after RX_DONE | PASS |
| T-MBUART-11 (GW) | Concurrent BUSY guard | IGNORE (same reason as FD) |

**GW total: 9 PASS, 2 IGNORED, 0 FAILED.**

---

## Integration test — expected behaviour

Flash `firmware/field-device/integration-tests/modbus_uart_driver/test_modbus_uart_driver_main.c`
to the STM32F469I-DISCO. Wire PG14 (TX) → PG9 (RX) via RS-485 transceiver or direct wire.
Open PuTTY on ST-Link VCP at 115200 / 8N1.

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | `[MBUART] ===== ModbusUartDriver integration test =====` | Pre-scheduler init complete |
| 2 | `[MBUART] modbus_uart_init OK` | Phase 1 init success |
| 3 | `[MBUART] Phase 1: transmit OK (8 bytes)` | `modbus_uart_transmit` returns OK |
| 4 | `[MBUART] Phase 1: RX_DONE received` | IDLE ISR fires and invokes callback |
| 5 | `[MBUART] Phase 1: frame matches [PASS]` | Loopback frame content correct |
| 6 | `[MBUART] Phase 2: RX_ERROR injection requires hardware break...` | Phase 2 manual-step notice |
| 7 | `[MBUART-task] tick N` at 1 Hz (N = 1, 2, 3, …) | Periodic tick; no freeze |

If loopback wire not fitted, step 4/5 replaced by: `[MBUART] Phase 1: RX timeout [SKIP]`

---

## Deviations from companion

| # | Companion says | Implementation does | Reason |
|---|----------------|---------------------|--------|
| 1 | §2.5 does not list `modbus_uart_set_tick_source()` | Added to public API | Tick-source injection required for testable TX timeouts; follows DebugUartDriver pattern. LLD companion updated. |
| 2 | §3.5 TC wait uses `MODBUS_UART_TC_TIMEOUT_MS` (10 ms) | TC wait uses `MODBUS_UART_TXE_TIMEOUT_MS` (5 ms) — intentional bug | See bug-log.md (MBUART-BUG-01). DEVIATION comment in source at line 295. |

---

## Open items carried forward

| ID | Item |
|----|------|
| MBUART-O2 | BRR register value for 9600 bps pending `clock-config.md`. T-MBUART-01 deferred with TEST_IGNORE_MESSAGE. |
| MBUART-BUG-01 | Intentional TC timeout constant bug — see bug-log.md. |

---

## PR title

`feat: add ModbusUartDriver v1.0 — RS-485 byte-stream transport (FD + GW)`

---

## PR description

### What this PR contains

- `firmware/field-device/drivers/modbus_uart_driver/modbus_uart_driver.h` — public API, event/error enums, rx callback type
- `firmware/field-device/drivers/modbus_uart_driver/modbus_uart_driver.c` — dual-board implementation (USART6/UART4), IDLE ISR, polling TX
- `tests/field-device/drivers/modbus_uart_driver/test_modbus_uart_driver_fd.c` — 11 Unity tests for F469 variant
- `tests/gateway/drivers/modbus_uart_driver/test_modbus_uart_driver_gw.c` — 11 Unity tests for L475 variant
- `firmware/field-device/integration-tests/modbus_uart_driver/test_modbus_uart_driver_main.c` — hardware loopback test
- `docs/lld/drivers/modbus-uart-driver.md` — companion updated to COMPLETE

### Design decisions

- Board abstraction via `MBUART_*` macros in the `.c` file: F4 uses SR/DR (two-step IDLE clear), L4 uses ISR/ICR/RDR/TDR (atomic IDLE clear via ICR.IDLECF). Both boards share the same ISR body and public API.
- `modbus_uart_set_tick_source()` added (not in original companion §2.5): tick injection follows the DebugUartDriver precedent and is required for testable TX timeouts on host.
- GW tests placed in `tests/gateway/drivers/modbus_uart_driver/` following the I2cDriver dual-board pattern.

### Test evidence

```
FD: TESTED 11  PASSED 9  IGNORED 2  FAILED 0
GW: TESTED 11  PASSED 9  IGNORED 2  FAILED 0
cppcheck: PASS   clang-format: PASS (auto-fixed)
```

### Open items carried forward

- MBUART-O2: BRR value deferred pending `clock-config.md` (shared with DUART-O2, I2CD-O1/O2, SPID-O1)
