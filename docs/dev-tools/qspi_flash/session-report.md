# Session Report — QspiFlashDriver (Gateway)

**Date:** 2026-09-15 (implementation) / 2026-09-16 (integration test + bench)
**Branch:** `feature/phase-4-gw-qspi-flash` → merged to `main` as `1cafc84`
**Companion:** `docs/lld/drivers/qspi-flash-driver.md` (v0.3, Pass H PASS)

> Note: this is the **Gateway** port (MX25R6435F, 8 MB, STM32L475).
> `docs/dev-tools/qspi_flash_driver/` is the separate **Field Device**
> driver (MT25QL128ABA, 16 MB, STM32F469) from 2026-06-02.

> Process note: the module was built across two sessions. Steps 0–5 and
> 7–11 were completed on 2026-09-15; **Step 6 (integration test main) was
> missed**, and the PR was briefly marked ready for review without it. The
> gap was caught by the user on 2026-09-16, the Step 6 deliverable was
> written, the PR returned to draft, run on hardware, and only then merged.
> Root cause: an abridged copy of this prompt (325 lines, in `~/Downloads`)
> was used as the process authority instead of this 805-line canonical
> document, and that copy has no Step 6.

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| `firmware/gateway/drivers/qspi_flash/qspi_flash.h` | 192 | New |
| `firmware/gateway/drivers/qspi_flash/qspi_flash.c` | 491 | New |
| `firmware/gateway/drivers/qspi_flash/qspi_flash_hw.h` | 51 | New — register-access indirection for testability (QSPID-D8) |
| `tests/gateway/drivers/qspi_flash/test_qspi_flash_gw.c` | 504 | New — 27 TCs |
| `firmware/gateway/integration-tests/qspi_flash/main_test_qspi_flash.c` | 522 | New — 10 hardware TCs (Step 6) |
| `tests/mocks/stm32l475_cmsis_mock.c` | +93 | Extended (additive) |
| `tests/mocks/stm32l475xx.h` | +107 | Extended (additive) |
| `tests/project.yml` | +3 | Extended (additive) |
| `docs/lld/drivers/qspi-flash-driver.md` | +198 / −29 | Companion: Pass H table, §7.6, QSPID-O9 |

---

## Reused infrastructure

| File | Status | Symbols added (if extended) |
|------|--------|-----------------------------|
| `tests/mocks/stm32l475xx.h` | extended | `§QUADSPI` register block (`QUADSPI_TypeDef`, `QUADSPI` base), `RCC_AHB3ENR_QSPIEN`, `QUADSPI_SR_BUSY` / `_TCF` / `_FTF`, `QUADSPI_CCR_*`, `QUADSPI_CR_*` |
| `tests/mocks/stm32l475_cmsis_mock.c` | extended | QUADSPI storage + reset; `qspi_hw_*` instrumented stubs behind `qspi_flash_hw.h` — CCR command log with per-command DLR snapshot, RX byte FIFO with underflow flag, TX byte capture |
| `tests/project.yml` | extended | `:test_qspi_flash_gw:` defines |
| `firmware/gateway/drivers/cpu/` (`cpu_init`, `cpu_delay_ms`, `cpu_get_sysclk_hz`) | reused | none — consumed as-is by the integration main |
| `firmware/gateway/drivers/{gpio,debug_uart}/` | reused | none — reporting channel for the integration main |

**Additive-only verified:** `git diff --numstat e17a0f9 1cafc84 -- tests/support/ tests/mocks/`
→ `93 0`, `107 0`. Zero deletions, zero modifications. No parallel mock files created.

---

## Unit test results

Host suite, `tests/gateway/drivers/qspi_flash/test_qspi_flash_gw.c`:

| Test ID | Description | Result |
|---------|-------------|--------|
| TC-QSPI-001 | `qspi_flash_init()` happy path — RCC, pins, RDID match | PASS |
| TC-QSPI-002 | `init()` idempotent on second call | PASS |
| TC-QSPI-003 | `init()` timeout on a peripheral that never clears BUSY | PASS |
| TC-QSPI-004 | `init()` returns `ERR_DEVICE` on RDID mismatch | PASS |
| TC-QSPI-005 | PE10–PE15 configured AF10 via CMSIS (pins QSPID-D10) | PASS |
| TC-QSPI-010 | `read()` issues command 0x03 with correct address | PASS |
| TC-QSPI-011 | `read()` returns the bytes the device supplied | PASS |
| TC-QSPI-012 | `read()` multi-byte sequencing | PASS |
| TC-QSPI-013 | `read()` before `init()` → `ERR_NOT_INITIALISED` | PASS |
| TC-QSPI-014 | `read(NULL)` → `ERR_NULL_POINTER` | PASS |
| TC-QSPI-015 | `read()` past device end → `ERR_ADDR` | PASS |
| TC-QSPI-020 | `write_page()` issues WREN before PP (command ordering) | PASS |
| TC-QSPI-021 | `write_page()` sends the payload bytes | PASS |
| TC-QSPI-022 | `write_page()` polls WIP until clear | PASS |
| TC-QSPI-023 | `write_page()` full 256-byte page accepted | PASS |
| TC-QSPI-024 | `write_page()` len 0 → `ERR_LEN` | PASS |
| TC-QSPI-025 | `write_page()` len > 256 → `ERR_LEN` | PASS |
| TC-QSPI-026 | `write_page()` before `init()` → `ERR_NOT_INITIALISED` | PASS |
| TC-QSPI-027 | `write_page(NULL)` → `ERR_NULL_POINTER` | PASS |
| TC-QSPI-028 | `write_page()` crossing a page boundary → `ERR_LEN` | PASS |
| TC-QSPI-030 | `erase_sector()` issues WREN before 0x20 | PASS |
| TC-QSPI-031 | `erase_sector()` auto-aligns to the 4 KB sector base | PASS |
| TC-QSPI-032 | `erase_sector()` polls WIP until clear | PASS |
| TC-QSPI-033 | `erase_sector()` timeout → `ERR_TIMEOUT` | PASS |
| TC-QSPI-034 | `erase_sector()` before `init()` → `ERR_NOT_INITIALISED` | PASS |
| TC-QSPI-035 | `erase_sector()` past device end → `ERR_ADDR` | PASS |
| (vtable) | `qspi_flash_driver` vtable binds the three operations | PASS |

**Total:** 27 pass, 0 ignored.

Ignored tests (with reason): none. Every TC ID in companion §7.3 is implemented.

Full host suite on `main` after merge: **721 pass, 0 fail, 7 ignored** (728 tested).
The 7 ignores belong to other modules.

---

## Integration test — expected behaviour

`firmware/gateway/integration-tests/qspi_flash/main_test_qspi_flash.c`.
Bare-metal, reports on USART1/PB6 @ 115 200 8N1. **Destructive** on the 4 KB
sector at byte offset `0x00520000` — the *(reserved)* region of
`flash-partition-layout.md` §5.2, chosen so no partition is harmed. The FD
counterpart erases sector 0, which on the Gateway is the live `ConfigStore`;
deliberately not copied.

| # | What to observe | Verifies |
|---|-----------------|----------|
| 1 | `TC-HW-QSPI-001 init OK - RDID 0xC22817 confirmed` | The Macronix part is populated and the QSPI lines carry data — closes QSPID-O3 |
| 2 | `TC-HW-QSPI-002 init is idempotent` | Second `init()` returns OK without re-touching hardware |
| 3 | `TC-HW-QSPI-003 sector erased - 0xFF at offsets 0, 0x800, 0xFFF` | Erase reached the **memory array**, across the whole sector — a register mock cannot fail this |
| 4 | `TC-HW-QSPI-004 256-byte page programmed and verified` | Full-page program then byte-identical read-back |
| 5 | `TC-HW-QSPI-005 adjacent page written; page A intact (no wrap)` | The page-boundary guard (QSPID-D3) holds on real silicon |
| 6 | `TC-HW-QSPI-007 512-byte read spans both pages contiguously` | Reads are not page-limited; only writes are |
| 7 | `TC-HW-QSPI-006 program-without-erase yields old AND new` | NOR 1→0 semantics — device physics, untestable on host |
| 8 | `TC-HW-QSPI-008 re-erase restored 0xFF across the sector` | Erase is repeatable |
| 9 | `TC-HW-QSPI-009 validation cascade returned the documented error codes` | §6 error contract holds against the real peripheral |
| 10 | `TC-HW-QSPI-010 erase <N> ms, page program <N> us` | Timing inside datasheet windows and the bounded WIP poll |

Execution order note: TC-007 runs **before** TC-006 by design — the cross-page
read must observe the clean A/B patterns before the AND test corrupts page A.

**Bench result — 2026-09-16, B-L475E-IOT01A: 10/10 PASS.**
Measured: sector erase **81 ms** (datasheet typ 40 / max 240), page program
**3181 µs** per 256 B (max 10 000). Both inside the datasheet windows and well
inside the driver's ~500 ms bounded WIP poll.

To run it: swap the `.cproject` `integration-tests` exclusion list so
`qspi_flash` is active and `cloud_publisher` is excluded, **and** repoint the
explicit `<entry ... name="integration-tests/cloud_publisher"/>` sourcePath.
Both edits are bench-local — revert with
`git checkout -- firmware/gateway/.cproject` afterwards, or the Gateway
firmware CI job starts building the bring-up test as the firmware `main()`.

---

## Deviations from companion

1. **ADT pattern not applied** — documented exception. The Gateway default is
   opaque handle + static pool; this driver is a singleton with module-prefixed
   free functions (QSPID-D6): one QUADSPI peripheral and one flash device per
   board. Dependency inversion is preserved through the `iqspi_flash_t` vtable.
   Stated in companion §1 and permitted by the prompt's "ADT exceptions" clause.
2. **Driver owns its own pins** (QSPID-D10) — configures PE10–PE15 via CMSIS
   rather than through `GpioDriver`, so USES stays exactly `CMSIS` as
   `components.md` requires. SpiDriver GW made the opposite choice; both are
   HLD-conformant, and QSPI has a fixed single pin set, so owning it in-driver
   is simpler.

---

## Open items

| ID | Item | Status |
|---|---|---|
| QSPID-O1 | Shared `qspi_flash_mutex` — three GW middleware consumers under independent mutexes cannot serialise peripheral access. HLD escalation to `task-breakdown.md` §7. | **Open — blocks ConfigStore / CircularFlashLog / FirmwareStore GW LLDs** |
| QSPID-O3 | GW RDID `0xC22817` hardware confirmation | **Closed 2026-09-16** on the bench |
| QSPID-O4 | WIP busy-wait during erase. Measured 81 ms — no timeout risk, but it is a *spin* blocking the calling task and everything at or below its priority. CircularFlashLog backs StoreAndForward, so erases land during cloud outages while alarms buffer against REQ-NF-113's 500 ms (81 ms ≈ 16%). | Open — re-evaluate when CircularFlashLog lands |
| QSPID-O6 | SD-06c/06d show `FirmwareStore → QspiFlashDriver` writing on-chip metadata, which this driver cannot do | **Open — blocks FirmwareStore LLD** |
| QSPID-O7 | FD drift: `ERR_NOT_INIT` spelling, no NULL guard | Open (FD scope) |
| QSPID-O8 | FD USES drift: FD driver uses `GpioDriver`, `components.md` says CMSIS only | Open (HLD decision) |
| QSPID-O9 | `init()` verifies RDID but never exposes the value read, so `ERR_DEVICE` gives a bench operator no diagnostic | Open — low priority, diagnosis only |

---

## PR title

feat: implement QspiFlashDriver for Gateway

---

## PR description

## What this PR contains

- `firmware/gateway/drivers/qspi_flash/qspi_flash.h` — `IQspiFlash` public API, geometry constants, 8-code error enum, vtable
- `firmware/gateway/drivers/qspi_flash/qspi_flash.c` — MX25R6435F over QUADSPI indirect 1-1-1: RSTEN/RST + JEDEC ID check at init, read (0x03), page program (0x02) with page-boundary enforcement, 4 KB sector erase (0x20) with auto-alignment
- `firmware/gateway/drivers/qspi_flash/qspi_flash_hw.h` — mockable register indirection (same pattern as `cpu_hw.h`)
- `tests/gateway/drivers/qspi_flash/test_qspi_flash_gw.c` — 27 unit tests
- `firmware/gateway/integration-tests/qspi_flash/main_test_qspi_flash.c` — 10 hardware TCs
- `docs/lld/drivers/qspi-flash-driver.md` — companion: Pass H table (PASS), §7.6 bring-up plan, QSPID-O9
- Extended additively: `tests/mocks/stm32l475xx.h`, `tests/mocks/stm32l475_cmsis_mock.c`, `tests/project.yml`

## Design decisions

- **QSPID-D6 singleton, not ADT** — one peripheral and one device per board; DI preserved via the vtable.
- **QSPID-D10 driver owns PE10–PE15 via CMSIS** — keeps USES exactly `CMSIS` per `components.md`.
- **Prescaler 2 → 26.67 MHz** — bounded by the 33 MHz limit of the `READ 03h` opcode, not the 80 MHz `FAST_READ` figure (QSPID-O2).
- **QSPID-D9 every hardware wait is a bounded counter** returning `ERR_TIMEOUT` — the FD v1.0 driver has unbounded `while` loops on BUSY/FTF/TCF; a stuck peripheral would hang the calling task.
- **QSPID-D8 `qspi_flash_hw.h` indirection** — lets host tests assert command *sequences* (WREN before PP), which a last-value register mock cannot.

## Test evidence

All 6 CI checks green.
Unity host tests: 27 pass, 0 fail, 0 ignore for this module; 721 pass / 7 ignore suite-wide.
Integration test validated on L475 hardware: 10/10 PASS, erase 81 ms, page program 3181 µs.

## Open items carried forward

- QSPID-O1 shared mutex — must be settled in `task-breakdown.md` §7 before any of the three GW consumers is implemented.
- QSPID-O4 erase busy-wait — decide spin vs `vTaskDelay(1)` vs low-priority erase task when CircularFlashLog lands.
- QSPID-O6 on-chip metadata SDs — blocks the FirmwareStore LLD.
- QSPID-O9 no device-ID accessor — diagnosis only.
