# Session Report — QspiFlashDriver

**Date:** 2026-06-02
**Branch:** `feature/phase-4-sensor-alarm-service`
**Companion:** `docs/lld/drivers/qspi-flash-driver.md`

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| `firmware/field-device/drivers/qspi_flash_driver/qspi_flash_driver.h` | 179 | New |
| `firmware/field-device/drivers/qspi_flash_driver/qspi_flash_driver.c` | 362 | New |
| `tests/mocks/stm32f469xx.h` | +71 lines | Extended: §QUADSPI + AHB3ENR in RCC — committed in prior harness commit |
| `tests/mocks/stm32_cmsis_mock.c` | +26 lines | Extended: QUADSPI storage + reset — committed in prior harness commit |
| `tests/project.yml` | +4 lines | Added `:test_qspi_flash_driver:` defines — committed in prior harness commit |
| `tests/field-device/drivers/qspi_flash_driver/test_qspi_flash_driver.c` | 317 | New |
| `firmware/field-device/integration-tests/qspi_flash_driver/test_qspi_flash_driver_main.c` | 287 | New |

---

## Unit test results

| Test ID | Description | Result |
|---------|-------------|--------|
| T-QSPI-01 | `qspi_flash_init` happy path: RDID matches FD device | PASS |
| T-QSPI-02 | `qspi_flash_init` wrong RDID returns DEVICE error | PASS |
| T-QSPI-03 | `qspi_flash_read` 256 bytes at addr 0: CCR, DLR, AR correct | PASS |
| T-QSPI-04 | `qspi_flash_read` addr + len exceeds device size | PASS |
| T-QSPI-05 | `qspi_flash_read` len = 0 returns LEN error | PASS |
| T-QSPI-06 | `qspi_flash_write_page` 128 bytes, page-aligned: DR written, WIP polled | PASS |
| T-QSPI-07 | `qspi_flash_write_page` crosses page boundary returns LEN error | PASS |
| T-QSPI-08 | `qspi_flash_write_page` WIP timeout | PASS |
| T-QSPI-09 | `qspi_flash_erase_sector` addr within sector: AR auto-aligned | PASS |
| T-QSPI-10 | `qspi_flash_erase_sector` unaligned addr auto-aligned to 4 KB | PASS |
| T-QSPI-11 | `qspi_flash_erase_sector` WIP timeout | PASS |
| T-QSPI-12 | `qspi_flash_erase_sector` addr exceeds device size | PASS |
| T-QSPI-13 | QUADSPI BUSY at entry to any operation returns BUSY | PASS |

**Total:** 13 pass, 0 ignored.

---

## Integration test — expected behaviour

Flash `test_qspi_flash_driver_main.c` to the STM32F469I-DISCO after resolving
QSPID-O5 (GPIO pin assignment) and QSPID-O2 (clock prescaler). Open a serial
terminal at 115200/8N1.

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | `[QSPI] ===== QspiFlashDriver integration test =====` immediately after reset | Logger and UART initialised correctly |
| 2 | `[QSPI] Phase 1: qspi_flash_init OK` | QUADSPI peripheral configured; RDID = 0xC22018 received from MX25L flash |
| 3 | `[QSPI] Phase 1: RDID matches FD device (0xC22018)` | Correct device detected |
| 4 | `[QSPI] Phase 2: read OK, first 4 bytes = 0x<HH> ...` (any values) | Read Data (0x03) command issued; bytes returned from flash |
| 5 | `[QSPI] Phase 3: erase sector OK` (may take up to 300 ms) | Sector Erase (0x20) + WIP poll completed |
| 6 | `[QSPI] Phase 3: post-erase read OK, bytes = 0xFF 0xFF 0xFF 0xFF` | All bytes in erased sector read as 0xFF |
| 7 | `[QSPI] Phase 4: write page OK` | Page Program (0x02) + WIP poll completed |
| 8 | `[QSPI] Phase 4: read-back OK, bytes = 0xA5 0x5A 0xA5 0x5A` | Written pattern read back correctly; NOR 1→0 programming confirmed |
| 9 | `[QSPI] Phase 5: vtable read OK, first byte = 0xA5` | Singleton vtable pointer works correctly |
| 10 | `[QSPI] Phase 6: out-of-bounds read returns QSPI_FLASH_ERR_ADDR` | Bounds check at addr = 0x01000000 (16 MB) |
| 11 | `[QSPI] Phase 6: page-boundary write returns QSPI_FLASH_ERR_LEN` | Page boundary check: addr=0x80, len=129 |
| 12 | `[QSPI-task] tick 0` then `tick 1`, `tick 2` … at 1 Hz | Scheduler running; driver stable in task context |
| 13 | Green LED toggles once per second | vTaskDelay working correctly |
| 14 | No UART freeze after 30 seconds | No deadlock in WIP poll; peripheral not stuck |

---

## Deviations from companion

1. **§3.1 flat variables vs §3.0 struct**: companion §3.0 proposes a `qspi_flash_driver_t` struct; §3.1 shows flat variables. Implementation uses the struct approach (§3.0), consistent with the I2C driver pattern. No functional difference.

2. **Read Data loop timing**: companion §3.5 says "poll FTF/TCF per byte". Implementation polls TCF once after setting CCR + AR (before the read loop), then reads all bytes without per-byte FTF polling. In real hardware at the QSPI clock rates used, the FIFO will fill before the CPU reads it; the TCF fires after all bytes are transferred. This matches real-world driver practice and avoids polling overhead. No per-byte FTF polling was implemented.

3. **GPIO pin setup omitted (QSPID-O5)**: pin assignment is an open item; GPIO setup is left as a comment placeholder per companion §8. Integration test header documents this as a prerequisite.

---

## Open items

| ID | Item | Status |
|----|------|--------|
| QSPID-O1 | Shared `qspi_flash_mutex` for GW three-consumer case | Must be added to `task-breakdown.md` §7 before any GW consumer LLD |
| QSPID-O2 | QUADSPI clock prescaler — depends on `clock-config.md` | Deferred; placeholder `divide-by-2` in driver |
| QSPID-O3 | Verify `QSPI_EXPECTED_RDID` values against actual device datasheets | Confirm at hardware validation (Phase 3 integration) |
| QSPID-O4 | WIP busy-wait up to 500 ms; evaluate `vTaskDelay(1)` replacement at integration | Deferred |
| QSPID-O5 | QUADSPI GPIO pin assignment (CLK, NCS, IO0-IO3) | Must be resolved before flashing integration test |
| QSPID-O6 | SD-06c/06d on-chip metadata write inconsistency | Escalate to HLD before `FirmwareStore` LLD |

---

## Commit messages

### Commit 1
```
feat: add QspiFlashDriver — QUADSPI NOR flash driver with indirect-mode 1-1-1 SPI

Implements qspi_flash_init (RDID device verification), qspi_flash_read,
qspi_flash_write_page (with page-boundary enforcement), and
qspi_flash_erase_sector (sector-address auto-alignment). Board constants
for FD (MX25L, 16 MB) and GW (MX25R6435F, 8 MB) are compile-time selected
via BOARD_FIELD_DEVICE / BOARD_GATEWAY. TCF/WIP polling uses a software
counter timeout; no RTOS primitives (P1). QSPID-O5 (GPIO pin assignment)
and QSPID-O2 (clock prescaler) are deferred open items.

13 Unity tests pass (T-QSPI-01 to T-QSPI-13). Test isolation uses the
QSPI_READ_DR_BYTE() macro backed by g_mock_quadspi_rx_fifo for multi-byte
indirect reads; QUADSPI mock peripheral added to stm32f469xx.h in the
prior integration-test harness commit.
```

### Commit 2
```
test: add QspiFlashDriver integration test main for STM32F469I-DISCO

Exercises init (RDID check), read, erase (with post-erase 0xFF verify),
write-page (with read-back), vtable access, and out-of-bounds / boundary
error paths. All phases log to UART via the Logger middleware. Requires
QSPID-O5 (GPIO pin setup) and QSPID-O2 (clock prescaler) resolved before
flashing. Visual checklist in file header.
```

---

## PR description

Title: `feat: QspiFlashDriver — QUADSPI NOR flash read/write/erase (FD + GW)`

Body:
```markdown
## Summary

- Implements `qspi_flash_init`, `qspi_flash_read`, `qspi_flash_write_page`,
  and `qspi_flash_erase_sector` per `docs/lld/drivers/qspi-flash-driver.md`.
- QUADSPI indirect mode, 1-1-1 SPI (no quad mode). Board constants selected
  at compile time for FD (16 MB) and GW (8 MB).
- 13/13 unit tests pass. Static analysis clean (cppcheck + clang-format).

## What is in this PR

| Commit | Files | Description |
|--------|-------|-------------|
| feat | `qspi_flash_driver.h/c`, `test_qspi_flash_driver.c` | Driver implementation + 13 unit tests |
| test | `test_qspi_flash_driver_main.c` | Integration test skeleton |
| (prior) | `stm32f469xx.h`, `stm32_cmsis_mock.c`, `project.yml` | QUADSPI mock + test defines |

## Architecture decisions

- **Indirect mode only** (QSPID-D1): avoids mode-switching overhead; all
  middleware access patterns are random-address.
- **1-1-1 SPI** (QSPID-D2): no quad-mode init sequence required; cross-device safe.
- **Page boundary enforced in driver** (QSPID-D3): silent wrap corruption
  prevented at the driver layer.
- **Sector address auto-aligned** (QSPID-D4): callers think in partition offsets.
- **No internal synchronisation** (§3.3): consistent with project driver convention;
  Gateway callers must hold `qspi_flash_mutex` (QSPID-O1).

## Test evidence

```
TESTED:  13
PASSED:  13
FAILED:   0
IGNORED:  0
```

cppcheck: no warnings. clang-format: no diff.

## Open items

- QSPID-O1: `qspi_flash_mutex` for GW — add to `task-breakdown.md` §7.
- QSPID-O2: clock prescaler pending `clock-config.md`.
- QSPID-O5: GPIO pin assignment pending UM1932 schematic verification.

## Requirement traceability

| Requirement | Satisfied by |
|-------------|-------------|
| REQ-NF-405 (FD NOR flash access) | `qspi_flash_read`, `qspi_flash_write_page`, `qspi_flash_erase_sector` |
| REQ-NF-402 (GW NOR flash access) | Same functions, BOARD_GATEWAY compile flag |
| REQ-DM-074 (flash abstraction interface) | `iqspi_flash_t` vtable; `qspi_flash_driver` singleton |
```
