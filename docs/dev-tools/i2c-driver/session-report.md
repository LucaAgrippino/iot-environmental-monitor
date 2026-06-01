# Session Report — I2cDriver

**Date:** 2026-06-01
**Branch:** `feature/phase-4-i2c-driver`
**Companion:** `docs/lld/drivers/i2c-driver.md`

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| `firmware/field-device/drivers/i2c/i2c_driver.h` | 160 | Shared header; both .c files include it |
| `firmware/field-device/drivers/i2c/i2c_driver_f4.c` | ~370 | STM32F469 I2C v1 implementation |
| `firmware/gateway/drivers/i2c/i2c_driver_l4.c` | ~310 | STM32L475 I2C v2 implementation |
| `tests/field-device/drivers/i2c/test_i2c_driver_f4.c` | ~420 | 13 F469 unit tests |
| `tests/gateway/drivers/i2c/test_i2c_driver_l4.c` | ~340 | 11 L475 unit tests |
| `tests/mocks/stm32l475xx.h` | ~150 | New L475 CMSIS mock header |
| `tests/mocks/stm32l475_cmsis_mock.h` | ~20 | New L475 mock entry point |
| `tests/mocks/stm32l475_cmsis_mock.c` | ~60 | New L475 mock storage + reset |
| `tests/mocks/stm32f469xx.h` | +80 | Extended with §I2C1 section |
| `tests/mocks/stm32_cmsis_mock.c` | +12 | Extended with I2C1 storage + reset |
| `tests/support/i2c_driver_f4.h` | 3 | Ceedling auto-link trigger stub |
| `tests/support/i2c_driver_l4.h` | 3 | Ceedling auto-link trigger stub |
| `tests/project.yml` | +6 | Added test_i2c_driver_f4 and _l4 defines |
| `firmware/field-device/integration-tests/i2c/test_i2c_driver_main.c` | ~210 | FD integration test |
| `docs/lld/drivers/i2c-driver.md` | Updated | Phase H complete, vtable added |

---

## Pre-code companion updates (Phase H)

The companion was in Draft status with several gaps fixed before coding:

1. **Status** updated to `Final (Phase H complete 2026-06-01)`.
2. **§2.6 Test-only hooks** added (`i2c_reset_for_test()`).
3. **§2.7 Singleton vtable** (`ii2c_t`) added — was referenced in §3.7 but never defined.
4. **§3.1 state duplication** fixed — `static bool s_initialised` removed; `s_i2c.initialised` in §3.0 struct is the sole state.
5. **§3.2.1 Singleton vtable instance** added — shows how both .c files populate it.
6. **§3.7 P5 wrong** — "RTOS mutex created in i2c_init()" corrected to "no RTOS primitives; caller serialises".
7. **§7 Unit-test plan** replaced — wrong file paths, no mock strategy, thin test descriptions all fixed.

---

## Unit test results

### F469 (I2C v1) — `test_i2c_driver_f4.c`

| Test ID | Description | Result |
|---------|-------------|--------|
| TC-I2C-F4-001 | i2c_init happy path | PASS |
| TC-I2C-F4-002 | i2c_init idempotent | PASS |
| TC-I2C-F4-003 | i2c_write happy path (2 bytes) | PASS |
| TC-I2C-F4-004 | i2c_write NACK on address | PASS |
| TC-I2C-F4-005 | i2c_write SB timeout | PASS |
| TC-I2C-F4-006 | i2c_write TXE timeout | PASS |
| TC-I2C-F4-007 | i2c_write BTF timeout | PASS |
| TC-I2C-F4-008 | i2c_write bus busy | PASS |
| TC-I2C-F4-009 | i2c_write_read happy path (1+2 bytes) | PASS |
| TC-I2C-F4-010 | i2c_write_read single-byte rx (ACK errata) | PASS |
| TC-I2C-F4-011 | i2c_write_read NACK in write phase | PASS |
| TC-I2C-F4-012 | i2c_read happy path (6 bytes) | PASS |
| TC-I2C-F4-013 | Bus recovery on timeout | PASS |

**Total:** 13 pass, 0 ignored.

### STM32L475 (I2C v2) — `test_i2c_driver_l4.c`

| Test ID | Description | Result |
|---------|-------------|--------|
| TC-I2C-L4-001 | i2c_init happy path | PASS |
| TC-I2C-L4-002 | i2c_init idempotent | PASS |
| TC-I2C-L4-003 | i2c_write happy path (2 bytes) | PASS |
| TC-I2C-L4-004 | i2c_write NACK (NACKF) | PASS |
| TC-I2C-L4-005 | i2c_write TXIS timeout | PASS |
| TC-I2C-L4-006 | i2c_write bus busy | PASS |
| TC-I2C-L4-007 | i2c_write_read happy path (1+2 bytes) | PASS |
| TC-I2C-L4-008 | i2c_write_read NACK in write phase | PASS |
| TC-I2C-L4-009 | i2c_write_read TC timeout | PASS |
| TC-I2C-L4-010 | i2c_read happy path (6 bytes) | PASS |
| TC-I2C-L4-011 | Bus recovery on timeout | PASS |

**Total:** 11 pass, 0 ignored.

**Combined:** 24 pass, 0 ignored.

---

## New mock infrastructure

The I2cDriver is the first module to target both boards with separate
implementation files. This required new L475-side mock infrastructure:

- `tests/mocks/stm32l475xx.h` — mirrors `stm32f469xx.h` for the L475; defines
  `I2C_TypeDef` (v2 layout), `GPIO_TypeDef`, `RCC_TypeDef` (L4 field names),
  and all bit constants used by `i2c_driver_l4.c`.
- `tests/mocks/stm32l475_cmsis_mock.{h,c}` — mirrors `stm32_cmsis_mock.{h,c}`;
  provides storage instances and `stm32l475_cmsis_mock_reset()`.
- Two Ceedling auto-link trigger stubs (`i2c_driver_f4.h`, `i2c_driver_l4.h`)
  in `tests/support/` — empty headers whose basename matches the `.c` file
  Ceedling should link. Including them in the test TU triggers auto-link without
  duplicating declarations (the real API is in `i2c_driver.h`).

This pattern is reusable for any future module that has separate F4/L4
implementation files (e.g., SpiDriver when implemented).

---

## Mock limitation: shared DR register (F469 I2C v1)

In I2C v1, `DR` is a single register serving both TX and RX roles. The mock
uses a `volatile uint32_t DR` field. When the driver writes the address+R byte
to `DR` at the start of the read phase, it overwrites any pre-loaded test value.
Subsequent `DR` reads return the address byte, not the expected receive data.

Impact: tests TC-I2C-F4-009, TC-I2C-F4-010, and TC-I2C-F4-012 cannot verify
received data content on the host. The assertions verify control flow (START,
STOP, ACK bits) instead. Receive-data correctness is validated at integration.

The I2C v2 mock (`RXDR` and `TXDR` are separate registers) does not have this
limitation.

---

## Integration test — expected behaviour

Flash `firmware/field-device/integration-tests/i2c/test_i2c_driver_main.c`
to the STM32F469I-DISCO. **First resolve I2CD-O3** (confirm I2C1 pin
assignment against UM1932 schematic; PB8/PB9 assumed).

FT6206 touchscreen controller I2C address: 0x38. Chip ID register: 0xA3,
expected value 0x06.

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | UART: `[I2C] ===== I2cDriver integration test =====` | Logger and UART working |
| 2 | UART: `[I2C] i2c_init OK` | Peripheral enabled, pins configured |
| 3 | UART: `[I2C] Phase 2: i2c_write OK` | Write transaction to FT6206 succeeds |
| 4 | UART: `[I2C] Phase 3: chip_id=0x06 (expected 0x06) PASS` | write-read returns correct data |
| 5 | UART: `[I2C] Phase 4: fw_ver=0x<non-zero>` | read-only transaction works |
| 6 | UART: `[I2C] Phase 6: vtable write_read OK, chip_id=0x06` | vtable dispatch works |
| 7 | UART: `[I2C-task] tick N` at 1 Hz | Scheduler running, task alive |
| 8 | Green LED toggles once per second | Led driver integration intact |
| 9 | No UART freeze for 60 seconds | No deadlock or watchdog reset |
| 10 | Logic analyser on SCL (Phase 5 uncommented): 9 SCL pulses after forced timeout | Bus recovery sequence executed |

If Phase 2 returns `NACK` or `TIMEOUT`: verify I2CD-O3 (pin assignment) and
confirm FT6206 is powered. If Phase 3 returns wrong chip ID: check 7-bit
address (0x38 is confirmed for FT6206 in most Discovery configurations).

---

## Deviations from companion

1. **Ceedling auto-link trigger stubs** — `tests/support/i2c_driver_f4.h` and
   `i2c_driver_l4.h` were added. The companion describes `#include` of the
   real implementation header to link the SUT, but with split implementation
   files (no `i2c_driver.c`), Ceedling needs an explicit stub trigger. The stub
   files are empty and add no declarations.

2. **I2C v1 mock DR limitation** — receive data values cannot be verified in
   TC-I2C-F4-009/-010/-012 due to the shared TX/RX register. Noted in companion
   §7.3 coverage notes as "validated at integration."

3. **F469 i2c_read SB timeout** — not listed in the companion §7.3 (was merged
   into TC-I2C-F4-005 covering all SB timeout cases). Companion §7.3 has been
   updated to reflect this.

---

## Open items

| ID | Item |
|----|------|
| I2CD-O1 | L475 `TIMINGR` value — placeholder `0x00303D5BU` used; finalise when `clock-config.md` lands |
| I2CD-O2 | F469 `CCR = 35`, `TRISE = 13` — assumes PCLK1 = 42 MHz; finalise when `clock-config.md` lands |
| I2CD-O3 | F469 I2C1 pin assignment (PB8/PB9 assumed) — **must confirm against UM1932 before flashing** |
| I2CD-O4 | Bus recovery 9-clock toggle count — validated by logic analyser at integration (Phase 5 in integration test) |

---

## Commit messages

### Commit 1
```
feat: add I2cDriver — F469 I2C v1 and L475 I2C v2 implementations

Implements the II2c interface for both boards: write, read, and atomic
write-read transactions with bus recovery, per the LLD companion.

- Shared header: firmware/field-device/drivers/i2c/i2c_driver.h
- F469 impl:    firmware/field-device/drivers/i2c/i2c_driver_f4.c
- L475 impl:    firmware/gateway/drivers/i2c/i2c_driver_l4.c
- F469 tests:   tests/field-device/drivers/i2c/test_i2c_driver_f4.c (13 pass)
- L475 tests:   tests/gateway/drivers/i2c/test_i2c_driver_l4.c (11 pass)
- New mocks:    stm32l475xx.h, stm32l475_cmsis_mock.{h,c}
- F469 mock extended with I2C1 section
- Companion doc updated to Final: Phase H (unit-test plan) complete

Open items carried forward: I2CD-O1 (L475 TIMINGR), I2CD-O2 (F469
CCR/TRISE), I2CD-O3 (F469 pin assignment), I2CD-O4 (recovery validation).
```

---

## PR description

Title: `feat: I2cDriver — F469 I2C v1 and L475 I2C v2 polling implementations`

Body:
```markdown
## Summary

- Implements `II2c` for both boards: `i2c_write`, `i2c_read`, `i2c_write_read`
  (atomic repeated-START), and bus recovery (9 SCL clock pulses per §3.5).
- Two separate `.c` files share one `.h`; build system selects per target.
- 24 unit tests (13 F469 + 11 L475), 0 ignored.
- New L475 mock infrastructure reusable for SpiDriver and future GW drivers.

## What is in this PR

| Files | Description |
|-------|-------------|
| `firmware/field-device/drivers/i2c/i2c_driver.h` | Shared header, vtable, error codes |
| `firmware/field-device/drivers/i2c/i2c_driver_f4.c` | F469 I2C v1 implementation |
| `firmware/gateway/drivers/i2c/i2c_driver_l4.c` | L475 I2C v2 implementation |
| `tests/field-device/drivers/i2c/test_i2c_driver_f4.c` | 13 F469 unit tests |
| `tests/gateway/drivers/i2c/test_i2c_driver_l4.c` | 11 L475 unit tests |
| `tests/mocks/stm32l475*.{h,c}` | New L475 CMSIS mock infrastructure |
| `tests/mocks/stm32f469xx.h` | Extended with §I2C1 (v1 registers + bit constants) |
| `docs/lld/drivers/i2c-driver.md` | Phase H complete: vtable, test plan, state consolidation |

## Architecture decisions applied

- **I2CD-D1**: Two `.c` files, no `#ifdef` gating.
- **I2CD-D2**: Polling only; no ISR, no DMA.
- **I2CD-D3**: `i2c_write_read` is a single atomic call (repeated START is
  internal).
- **I2CD-D4**: Caller supplies 7-bit address; driver shifts internally.
- **I2CD-D5**: Singleton module — no handle.
- **I2CD-D6**: Bus recovery on timeout before returning error (REQ-NF-205).

## Test evidence

```
OVERALL TEST SUMMARY
TESTED:  24
PASSED:  24
FAILED:   0
IGNORED:  0
```

cppcheck: 0 findings (unusedStructMember on vtable members suppressed with
inline comment — false positive from cross-file designated initialiser use).

## Open items

- I2CD-O1/O2: Timing register placeholders; resolve when clock-config.md lands.
- I2CD-O3: F469 I2C1 pin assignment assumed PB8/PB9 — **confirm before flashing**.
- I2CD-O4: Bus recovery 9-pulse count deferred to hardware integration.

## Requirement traceability

| Requirement | Satisfied by |
|-------------|--------------|
| REQ-LD-050 | `i2c_driver_f4.c` — serialised I2C bus access for FD peripherals |
| REQ-SA-031 | `i2c_driver_l4.c` — serialised I2C bus access for GW sensor drivers |
| REQ-NF-205 | `i2c_bus_recovery()` — peripheral left in usable state after timeout |
```
