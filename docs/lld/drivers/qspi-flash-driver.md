# QspiFlashDriver — LLD Companion

**Document:** `docs/lld/drivers/qspi-flash-driver.md`
**Version:** 0.1 (draft — pending Luca review)
**Board scope:** Field Device (STM32F469, 16 MB) and Gateway (B-L475E-IOT01A, 8 MB MX25R6435F)
**Layer:** Driver
**Status:** Draft
**Date:** May 2026

**HLD anchor:** QspiFlashDriver in `components.md` (FD + GW driver layer)

---

## 1. Sources

| Attribute | Field Device | Gateway |
|---|---|---|
| Responsibility | Reads, writes, and erases sectors of the external QSPI flash | Reads, writes, and erases sectors of the external QSPI flash |
| PROVIDES (upward) | `IQspiFlash` | `IQspiFlash` |
| USES (downward) | CMSIS | CMSIS |
| Root requirements | REQ-NF-405, REQ-DM-074 | REQ-NF-402, REQ-DM-074 |
| Flash device | MX25L51245G or equivalent, 16 MB | MX25R6435F, 8 MB |
| Sector size | 4 KB | 4 KB |
| Page size | 256 B | 256 B |
| Memory-mapped base | `0x9000_0000` | `0x9000_0000` |
| Endurance (CON-009) | 100 000 cycles/sector | 100 000 cycles/sector |

**Consumers — Field Device:** `ConfigStore` (middleware) only.

**Consumers — Gateway:** `ConfigStore`, `CircularFlashLog`, `FirmwareStore` (all middleware).

**Partition surface accessed by each consumer (from `flash-partition-layout.md`):**

| Consumer | Partition | Address range | Size |
|---|---|---|---|
| ConfigStore (FD) | ConfigStore | `0x9000_0000` – `0x9000_FFFF` | 64 KB |
| ConfigStore (GW) | ConfigStore | `0x9000_0000` – `0x9000_FFFF` | 64 KB |
| CircularFlashLog (GW) | CircularFlashLog | `0x9002_0000` – `0x9011_FFFF` | 1 MB |
| FirmwareStore (GW) | OTA staging | `0x9012_0000` – `0x9051_FFFF` | 4 MB |

**Critical concurrency finding:** On the Gateway, three independent middleware components call `QspiFlashDriver` from different tasks protected by different mutexes (`config_store_mutex`, `logger_mutex`). These mutexes are not aware of each other and cannot collectively prevent simultaneous QUADSPI peripheral access. This is a peripheral-level concurrency gap that must be resolved before implementation. Tracked as **QSPID-O1** (§8).

**Sequence diagram appearances:** `QspiFlashDriver` appears in SD-06b (OTA staging write), SD-06c (boot indicator write), and SD-06d (rollback flag write/revert). All three are synchronous calls from `FirmwareStore`.

---

## 2. Public API

### 2.1 Dependency-conformance check

`qspi_flash_driver.h` includes only CMSIS device headers and `stdint.h`. No FreeRTOS headers. Confirmed clean.

### 2.2 P3 consideration

Three consumers on the Gateway but a single interface is correct: all three perform the same primitive operations (read, write page, erase sector). No consumer requires a read-only or erase-only subset; the full interface is always needed. No ISP split warranted.

### 2.3 Page write constraint

NOR flash page programming (Page Program command, 0x02) can only write within a single 256-byte page. If `addr + len` crosses a 256-byte page boundary, the write wraps back to the start of the page — a hardware behaviour that corrupts data silently. The driver enforces the boundary constraint and returns `QSPI_FLASH_ERR_LEN` on violation. Callers (middleware) are responsible for splitting multi-page writes into aligned calls.

### 2.4 Data types

```c
/**
 * @brief Error codes returned by all QspiFlashDriver operations.
 *
 * Naming follows the cross-cutting convention in lld.md §3.2.
 */
typedef enum {
    QSPI_FLASH_ERR_OK      = 0, /**< Operation succeeded. */
    QSPI_FLASH_ERR_BUSY    = 1, /**< QUADSPI peripheral busy or flash WIP set. */
    QSPI_FLASH_ERR_TIMEOUT = 2, /**< WIP polling exceeded timeout (erase/write). */
    QSPI_FLASH_ERR_ADDR    = 3, /**< Address exceeds device capacity. */
    QSPI_FLASH_ERR_LEN     = 4, /**< len == 0, or write crosses a page boundary. */
    QSPI_FLASH_ERR_DEVICE  = 5, /**< RDID response does not match expected ID. */
} qspi_flash_err_t;
```

### 2.5 Public API (`qspi_flash_driver.h`)

```c
/**
 * @brief Initialise the QUADSPI peripheral and verify the flash device.
 *
 * Configures the QUADSPI peripheral (prescaler, flash size, CS high time).
 * Issues a Read ID (RDID, 0x9F) command and verifies the 3-byte response
 * against the expected manufacturer + device type + capacity identifier.
 * Returns QSPI_FLASH_ERR_DEVICE if the response does not match — catches
 * wrong device population or open-circuit flash at boot.
 *
 * Must be called once from main() before the FreeRTOS scheduler starts.
 * Operates in indirect mode (1-1-1 SPI). Does not activate quad mode.
 *
 * @return QSPI_FLASH_ERR_OK on success; QSPI_FLASH_ERR_DEVICE on ID
 *         mismatch; QSPI_FLASH_ERR_TIMEOUT if the peripheral does not
 *         respond.
 * @note Threading: task-context only, non-blocking. Must be called before the scheduler starts.
 */
qspi_flash_err_t qspi_flash_init(void);

/**
 * @brief Read bytes from the flash device.
 *
 * Issues a Read Data command (0x03) in indirect mode. Reads any number
 * of bytes starting at addr; wraps at the device boundary are not
 * supported (QSPI_FLASH_ERR_ADDR if addr + len exceeds device capacity).
 *
 * Caller serialises concurrent calls — see §3.3 and QSPID-O1 (§8).
 *
 * @param addr  Byte address within the flash (0 .. device_size - 1).
 * @param buf   Destination buffer (must not be NULL; must be ≥ len bytes).
 * @param len   Number of bytes to read (must be ≥ 1).
 * @return QSPI_FLASH_ERR_OK on success; QSPI_FLASH_ERR_ADDR or
 *         QSPI_FLASH_ERR_LEN on constraint violation;
 *         QSPI_FLASH_ERR_BUSY or QSPI_FLASH_ERR_TIMEOUT on hardware error.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
qspi_flash_err_t qspi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

/**
 * @brief Program up to 256 bytes within a single flash page.
 *
 * Issues Write Enable (0x06) then Page Program (0x02). Polls WIP until
 * the device completes the write (typically < 1 ms; max 5 ms per
 * MX25R6435F datasheet).
 *
 * Constraints enforced by the driver:
 *   - len must be ≥ 1 and ≤ 256.
 *   - addr and addr + len - 1 must lie within the same 256-byte page
 *     (i.e. (addr & ~0xFF) == ((addr + len - 1) & ~0xFF)).
 *     Returns QSPI_FLASH_ERR_LEN if violated.
 *   - addr must not exceed device capacity.
 *
 * NOR flash can only change 1 → 0. Bytes that already contain the target
 * value are written harmlessly; bits that need 0 → 1 require a prior
 * sector erase. This is a hardware constraint — the driver does not
 * verify or enforce it.
 *
 * @param addr  Byte address of the first byte to program.
 * @param data  Pointer to data to write (must not be NULL).
 * @param len   Number of bytes to program (1 .. 256, page-aligned).
 * @return QSPI_FLASH_ERR_OK on success; error code on failure.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
qspi_flash_err_t qspi_flash_write_page(uint32_t addr,
                                        const uint8_t *data,
                                        uint16_t len);

/**
 * @brief Erase the 4 KB sector containing the given address.
 *
 * Issues Write Enable (0x06) then Sector Erase (0x20). Polls WIP until
 * the erase completes (typically 120 ms; max 300 ms per MX25R6435F).
 *
 * After erase, all bytes in the sector read as 0xFF. The address may be
 * any byte within the 4 KB sector — the driver aligns to the sector
 * boundary internally.
 *
 * @param addr  Any byte address within the target 4 KB sector.
 * @return QSPI_FLASH_ERR_OK on success; QSPI_FLASH_ERR_TIMEOUT if WIP
 *         does not clear within 500 ms; QSPI_FLASH_ERR_ADDR if addr
 *         exceeds device capacity.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
qspi_flash_err_t qspi_flash_erase_sector(uint32_t addr);
```

---

## 3. Internal design

### 3.1 Module-level state

```c
static uint32_t s_device_size = 0U; /* populated at init; used for bounds checks */
static bool     s_initialised = false;
```

The flash device capacity differs per board (16 MB FD, 8 MB GW). `s_device_size` is set at `qspi_flash_init()` based on the RDID response and used by all operations for bounds checking.

### 3.2 QUADSPI indirect mode — all operations

The driver uses QUADSPI **indirect mode** (FMODE = 00b for write/erase, FMODE = 01b for read) for all flash commands. Memory-mapped mode (FMODE = 11b) is not used — switching between indirect and memory-mapped requires aborting the controller, adding latency and complexity. Indirect mode provides the same byte-level access pattern that all middleware consumers need.

### 3.3 Caller serialises

The driver has no internal synchronisation mechanism. Concurrent calls from different tasks result in QUADSPI register corruption. This is not an oversight — it is the established project convention (lld-methodology.md v1.1). The resolution for the Gateway multi-consumer case is a shared `qspi_flash_mutex` at the middleware caller layer. See **QSPID-O1** (§8).

### 3.4 Standard SPI (1-1-1) for all commands

All commands use single-wire instruction, single-wire address, and single-wire data (1-1-1 mode in QUADSPI CCR). Quad mode (1-1-4 or 4-4-4) is not activated. Rationale: the basic operations (0x03 read, 0x02 page program, 0x20 sector erase) are available in 1-1-1 mode on all NOR flash devices without any device initialisation sequence. Throughput is acceptable — at the QUADSPI clock rate used (QSPID-O2, §8), 1-1-1 read throughput far exceeds the middleware access patterns.

### 3.5 Command sequences

**Write Enable (issued before every write and erase):**
```
1. Set QUADSPI_CCR: INSTRUCTION=0x06, IMODE=01b (single), ADMODE=00b, DMODE=00b, FMODE=00b
2. Poll QUADSPI_SR.TCF until set
3. Clear TCF (write 1 to QUADSPI_FCR.CTCF)
```

**Read Data (0x03):**
```
1. Set QUADSPI_DLR = len - 1
2. Set QUADSPI_CCR: INSTRUCTION=0x03, IMODE=01b, ADMODE=01b, ADSIZE=10b (3-byte addr),
                    DMODE=01b, FMODE=01b (indirect read)
3. Set QUADSPI_AR = addr
4. Read QUADSPI_DR len times (1 byte per read, polling QUADSPI_SR.FTF/TCF)
5. Clear TCF
```

**Page Program (0x02):**
```
1. Issue Write Enable
2. Set QUADSPI_DLR = len - 1
3. Set QUADSPI_CCR: INSTRUCTION=0x02, IMODE=01b, ADMODE=01b, ADSIZE=10b,
                    DMODE=01b, FMODE=00b (indirect write)
4. Set QUADSPI_AR = addr
5. Write len bytes to QUADSPI_DR
6. Poll TCF until set; clear TCF
7. Poll WIP (Read Status Register 0x05, bit 0) until 0 (QSPID-D5)
```

**Sector Erase (0x20):**
```
1. Issue Write Enable
2. Set QUADSPI_CCR: INSTRUCTION=0x20, IMODE=01b, ADMODE=01b, ADSIZE=10b,
                    DMODE=00b, FMODE=00b
3. Set QUADSPI_AR = addr & ~0xFFFU   (align to 4 KB sector boundary)
4. Poll TCF until set; clear TCF
5. Poll WIP until 0 (timeout 500 ms — max erase time + margin)
```

**Read Status Register (0x05) for WIP polling:**
```
1. Set QUADSPI_DLR = 0 (read 1 byte)
2. Set QUADSPI_CCR: INSTRUCTION=0x05, IMODE=01b, ADMODE=00b, DMODE=01b, FMODE=01b
3. Read 1 byte from QUADSPI_DR → check bit 0 (WIP)
4. Clear TCF
5. Repeat until WIP = 0 or timeout
```

### 3.6 Board-specific constants

```c
/* Per-board compile-time constants */
#if defined(BOARD_FIELD_DEVICE)
  #define QSPI_DEVICE_SIZE_BYTES  (16UL * 1024UL * 1024UL)   /* 16 MB */
  #define QSPI_DCR_FSIZE          (23U)                        /* 2^24 bytes */
  #define QSPI_EXPECTED_RDID      (0xC22018U)                  /* MX25L example */
#elif defined(BOARD_GATEWAY)
  #define QSPI_DEVICE_SIZE_BYTES  (8UL * 1024UL * 1024UL)    /* 8 MB */
  #define QSPI_DCR_FSIZE          (22U)                        /* 2^23 bytes */
  #define QSPI_EXPECTED_RDID      (0xC22817U)                  /* MX25R6435F */
#endif
```

The `QSPI_EXPECTED_RDID` values must be verified against the actual device datasheets at implementation. Tracked as **QSPID-O3** (§8).

### 3.7 WIP polling — no RTOS blocking

WIP polling uses a busy-wait loop with a software counter timeout. FreeRTOS is not present in the driver. The erase timeout (500 ms) means the calling task is blocked for up to 500 ms during an erase — this is acceptable because:
- `ConfigStore` erases are rare (config changes only).
- `CircularFlashLog` erases one sector per revolution (~193 per year per flash-partition-layout.md §6.2 analysis).
- `FirmwareStore` erases the OTA staging region during OTA — `UpdateServiceTask` (priority 1) holds no resources other tasks urgently need.

If profiling reveals this is a problem, replacing the busy-wait with a `vTaskDelay(1)` loop would free the CPU at the cost of importing `task.h` — which violates the driver convention. Defer until integration measurements confirm it is needed. Tracked as **QSPID-O4**.

---

### 3.8 Principles applied

- **P1 (Strict directional layering).** Depends only on CMSIS QUADSPI peripheral headers; no RTOS, no middleware.
- **P2 (Dependency Inversion).** Exposes `iqspi_flash_t` vtable; CircularFlashLog, ConfigStore, and FirmwareStore all depend on `IQspiFlash`.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static `QspiFlashState`; command buffers allocated on the stack (bounded by maximum command size); no heap.
- **P6 (Responsibility traces to requirements).** Read / program / erase operations trace to REQ-BF-* and REQ-NF-304 persistent-store requirements.
- **P8 (Total error propagation, no silent failures).** `qspi_flash_err_t` on all operations; WIP-poll timeout returns error rather than blocking indefinitely.
- **P9 (BARR-C coding standard).** Addresses and lengths `uint32_t`; no implicit widening.
- **P10 (Naming conventions).** Prefix `qspi_flash_`; interface `IQspiFlash` -> `iqspi_flash_t`; errors `QSPI_FLASH_ERR_*`.


## 4. Hardware contract

### 4.1 QUADSPI peripheral — both boards

Both STM32F469 and STM32L475 include the same QUADSPI peripheral IP. Register layout is identical: `QUADSPI_CR` (control), `QUADSPI_DCR` (device config — flash size, CS high time, clock mode), `QUADSPI_SR` (status — BUSY, TCF, FTF, FLEVEL), `QUADSPI_FCR` (flag clear), `QUADSPI_DLR` (data length), `QUADSPI_CCR` (communication config), `QUADSPI_AR` (address), `QUADSPI_DR` (data). A single `.c` file with board-specific constants (§3.6) is sufficient.

### 4.2 Clock prescaler (open item — QSPID-O2)

The QUADSPI clock = `HCLK / (prescaler + 1)`. The maximum QSPI clock for the MX25R6435F is 80 MHz (SDR mode). The prescaler value depends on HCLK, which is unresolved pending `clock-config.md`. Tracked as **QSPID-O2**.

### 4.3 CS high time (CSHT in DCR)

Between consecutive commands, the NCS pin must remain high for a minimum time. For MX25R6435F: t_SHSL2 ≥ 10 ns. At any practical QSPI clock ≥ 8 MHz, one clock cycle exceeds this. `CSHT = 0` (1 clock cycle) is safe. Verify against the FD device datasheet at implementation.

### 4.4 Pin assignment (open item — QSPID-O5)

QUADSPI pin assignments (CLK, NCS, IO0–IO3) vary per board. Verify against UM1932 (F469) and UM2153 (L475) schematics at implementation.

---

## 5. Sequence integration

`QspiFlashDriver` appears as an explicit lifeline in three sequence diagrams, all on the Gateway:

| SD | Message | Driver call |
|---|---|---|
| SD-06b (OTA download) | FirmwareStore → QspiFlashDriver: write chunk to OTA staging | `qspi_flash_write_page(addr, data, len)` repeated |
| SD-06c (bank swap) | FirmwareStore → QspiFlashDriver: write boot indicator | `qspi_flash_write_page()` to metadata sector (on-chip — **see QSPID-O6**) |
| SD-06d (rollback) | FirmwareStore → QspiFlashDriver: write rollback flag, revert boot indicator | `qspi_flash_write_page()` |

**QSPID-O6 (§8):** SD-06c step 2 and SD-06d step 10' show `FirmwareStore → QspiFlashDriver` writes to the boot indicator and rollback flag. The flash-partition-layout.md §5.1 places the metadata partition at `0x0800_4000` — which is **on-chip flash**, not QSPI. This is a sequence-diagram inconsistency: `QspiFlashDriver` cannot write to on-chip flash. The bootloader metadata writes should be performed by the Bootloader itself or by a separate `OnChipFlashDriver`. This must be resolved before LLD for `FirmwareStore`. Flagged here as it is discovered from the driver's perspective.

No new sequence diagrams are required for this driver.

---

## 6. Error and fault behaviour

| Error | Consumer response |
|---|---|
| `QSPI_FLASH_ERR_BUSY` | Retry after acquiring `qspi_flash_mutex` (QSPID-O1 resolution); if peripheral is stuck, log and report via `IHealthReport` |
| `QSPI_FLASH_ERR_TIMEOUT` (erase/write) | Flash device failure. Log, report via `IHealthReport`, do not retry the same sector. Middleware may mark the sector bad (out of scope for driver) |
| `QSPI_FLASH_ERR_ADDR` | Caller bug — partition constant misconfigured. Log and halt in DEBUG builds (ASSERT macro) |
| `QSPI_FLASH_ERR_LEN` | Caller bug — page boundary alignment error. Log and halt in DEBUG builds |
| `QSPI_FLASH_ERR_DEVICE` | Flash device absent or wrong. Log and halt — system cannot operate without flash |

---

## 7. Unit-test plan

Host-platform tests (Unity framework). The QUADSPI peripheral is mocked via `#define QUADSPI (&mock_quadspi)`. Command sequences are verified by inspecting the mock's register state after each call.

| ID | Test case | Expected result |
|---|---|---|
| T-QSPI-01 | `qspi_flash_init` happy path: RDID returns expected ID | Returns OK; s_device_size populated; DCR.FSIZE correct |
| T-QSPI-02 | `qspi_flash_init` wrong RDID | Returns QSPI_FLASH_ERR_DEVICE |
| T-QSPI-03 | `qspi_flash_read` 256 bytes at addr 0 | CCR.INSTRUCTION=0x03; AR=0; DLR=255; 256 DR reads; returns OK |
| T-QSPI-04 | `qspi_flash_read` addr + len exceeds device size | Returns QSPI_FLASH_ERR_ADDR |
| T-QSPI-05 | `qspi_flash_read` len = 0 | Returns QSPI_FLASH_ERR_LEN |
| T-QSPI-06 | `qspi_flash_write_page` 128 bytes, page-aligned | WREN issued; CCR.INSTRUCTION=0x02; 128 DR writes; WIP polled until clear; returns OK |
| T-QSPI-07 | `qspi_flash_write_page` crosses page boundary | Returns QSPI_FLASH_ERR_LEN; no WREN issued |
| T-QSPI-08 | `qspi_flash_write_page` WIP timeout | Returns QSPI_FLASH_ERR_TIMEOUT |
| T-QSPI-09 | `qspi_flash_erase_sector` addr within sector | WREN issued; CCR.INSTRUCTION=0x20; AR=addr aligned to 4 KB; WIP polled; returns OK |
| T-QSPI-10 | `qspi_flash_erase_sector` addr not aligned | AR = addr & ~0xFFF (sector-aligned automatically); correct sector erased |
| T-QSPI-11 | `qspi_flash_erase_sector` WIP timeout | Returns QSPI_FLASH_ERR_TIMEOUT |
| T-QSPI-12 | `qspi_flash_erase_sector` addr exceeds device size | Returns QSPI_FLASH_ERR_ADDR |
| T-QSPI-13 | QUADSPI BUSY at entry to any operation | Returns QSPI_FLASH_ERR_BUSY without issuing command |

Test file: `tests/drivers/test_qspi_flash_driver.c` (one file; board-specific constants exercised via `BOARD_FIELD_DEVICE` / `BOARD_GATEWAY` compile flags).

---

## 8. Open items

| ID | Item | Owner | Resolution path |
|---|---|---|---|
| QSPID-O1 | **Peripheral-level concurrency gap (Gateway).** Three middleware consumers (`ConfigStore`, `CircularFlashLog`, `FirmwareStore`) access `QspiFlashDriver` from different tasks under independent mutexes. A shared `qspi_flash_mutex` must be acquired by all callers before calling any driver function. This mutex is a cross-cutting resource (not owned by any single middleware component). Resolution: add `qspi_flash_mutex` to the shared-resource locking table in `task-breakdown.md` §7 and initialise it in `main()`. All three consumers must acquire it before calling the driver. This is an HLD escalation — update `task-breakdown.md` and `lld.md` §5 (cross-cutting) accordingly. | Luca | Update `task-breakdown.md` §7 before implementing any of the three GW consumers |
| QSPID-O2 | QUADSPI clock prescaler. Depends on HCLK, unresolved pending `clock-config.md`. Max QSPI clock: 80 MHz (MX25R6435F). | Luca | Resolve when `clock-config.md` lands |
| QSPID-O3 | Verify `QSPI_EXPECTED_RDID` values against actual device datasheets. Manufacturer ID 0xC2 (Macronix) is confirmed; device type and capacity bytes must be verified. | Luca | Check MX25R6435F datasheet (GW) and FD device datasheet at implementation |
| QSPID-O4 | WIP polling busy-waits up to 500 ms during erase. If integration profiling reveals this degrades system responsiveness, replace with `vTaskDelay(1)` loop — but note this imports FreeRTOS into the driver, violating the driver convention. Evaluate trade-off at integration. | Luca | Defer until integration measurements |
| QSPID-O5 | QUADSPI pin assignments (CLK, NCS, IO0–IO3). Verify against UM1932 and UM2153 schematics. | Luca | Check at implementation |
| QSPID-O6 | **SD-06c/06d sequence diagram inconsistency.** The SDs show `FirmwareStore → QspiFlashDriver` writing the boot indicator and rollback flag, but these fields are in the on-chip metadata partition (`0x0800_4000`) per `flash-partition-layout.md` §5.1. `QspiFlashDriver` cannot write to on-chip flash. The SDs must be corrected and a separate on-chip flash write mechanism (Bootloader scope, or a `FlashDriver` for internal flash) identified. Escalate to HLD before `FirmwareStore` LLD companion is drafted. | Luca | Raise HLD gap; correct SD-06c and SD-06d; decide ownership of on-chip metadata writes |

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| QSPID-D1 | Indirect mode for all operations | Avoids mode-switching overhead between memory-mapped and indirect; all middleware access patterns are random-address; no XIP requirement from the driver |
| QSPID-D2 | Standard SPI (1-1-1) for all commands | No quad mode init sequence required; cross-device safe; throughput sufficient for all middleware access patterns at reasonable QSPI clock |
| QSPID-D3 | Page write boundary enforced in driver | Silent data corruption from page-wrap is a hard defect; enforcing in the driver means callers never need to implement the check themselves |
| QSPID-D4 | Sector address auto-aligned in `qspi_flash_erase_sector` | Callers (middleware) logically think in partition offsets, not aligned sector addresses; the alignment is a hardware concern that belongs in the driver |
| QSPID-D5 | WIP polling after every write and erase | Mandatory: the flash device is internally busy after these commands; issuing a new command while WIP is set corrupts the operation |
| QSPID-D6 | Singleton module (no handle) | One QSPI flash device per board; consistent with all prior driver companions |
| QSPID-D7 | `QSPI_FLASH_ERR_DEVICE` on RDID mismatch | Catches wrong device at boot — board population error or open-circuit flash — before any data is written; fail-fast is the correct embedded behaviour |
