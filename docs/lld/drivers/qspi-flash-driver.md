# QspiFlashDriver — LLD Companion

**Document:** `docs/lld/drivers/qspi-flash-driver.md`
**Version:** 0.3 (Pass H complete — Gateway implemented)
**Board scope:** Field Device (STM32F469, 16 MB) and Gateway (B-L475E-IOT01A, 8 MB MX25R6435F)
**Layer:** Driver
**Status:** Pass H complete — FD v1.0 implemented (2026-06), GW v1.0 implemented (2026-09-15)
**Date:** May 2026 (Pass H September 2026)

**HLD anchor:** QspiFlashDriver in `components.md` (FD + GW driver layer)

---

## 1. Sources

| Attribute | Field Device | Gateway |
|---|---|---|
| Responsibility | Reads, writes, and erases sectors of the external QSPI flash | Reads, writes, and erases sectors of the external QSPI flash |
| PROVIDES (upward) | `IQspiFlash` | `IQspiFlash` |
| USES (downward) | CMSIS | CMSIS |
| Root requirements | REQ-NF-405, REQ-DM-074 | REQ-NF-402, REQ-DM-074 |
| Flash device | MT25QL128ABA, 16 MB | MX25R6435F, 8 MB |
| Sector size | 4 KB | 4 KB |
| Page size | 256 B | 256 B |
| Memory-mapped base | `0x9000_0000` | `0x9000_0000` |
| Endurance (CON-009) | 100 000 cycles/sector | 100 000 cycles/sector |

**Consumers — Field Device:** `ConfigStore` (middleware) only.

**Consumers — Gateway:** `ConfigStore`, `CircularFlashLog`, `FirmwareStore` (all middleware).

**Gateway ADT exception:** the Gateway default is the ADT pattern (opaque handle + static pool). This driver is a documented exception — a singleton with module-prefixed free functions (QSPID-D6): there is exactly one QUADSPI peripheral and one flash device per board, and the same shape is used by RtcDriver on the Gateway. Dependency inversion is still honoured through the `iqspi_flash_t` vtable (§2.5).

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
    QSPI_FLASH_OK      = 0, /**< Operation succeeded. */
    QSPI_FLASH_ERR_BUSY    = 1, /**< QUADSPI peripheral busy or flash WIP set. */
    QSPI_FLASH_ERR_TIMEOUT = 2, /**< WIP polling exceeded timeout (erase/write). */
    QSPI_FLASH_ERR_ADDR    = 3, /**< Address exceeds device capacity. */
    QSPI_FLASH_ERR_LEN     = 4, /**< len == 0, or write crosses a page boundary. */
    QSPI_FLASH_ERR_DEVICE  = 5, /**< RDID response does not match expected ID. */
    QSPI_FLASH_ERR_NOT_INITIALISED = 6, /**< qspi_flash_init() has not succeeded yet (QSPID-O7). */
    QSPI_FLASH_ERR_NULL_POINTER    = 7, /**< A required pointer argument was NULL (QSPID-O7). */
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
 * @return QSPI_FLASH_OK on success; QSPI_FLASH_ERR_DEVICE on ID
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
 * @return QSPI_FLASH_OK on success; QSPI_FLASH_ERR_ADDR or
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
 * @return QSPI_FLASH_OK on success; error code on failure.
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
 * @return QSPI_FLASH_OK on success; QSPI_FLASH_ERR_TIMEOUT if WIP
 *         does not clear within 500 ms; QSPI_FLASH_ERR_ADDR if addr
 *         exceeds device capacity.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
qspi_flash_err_t qspi_flash_erase_sector(uint32_t addr);
```

---

## 3. Internal design

### 3.0 Private struct

```c
typedef struct {
    uint32_t device_size; /**< Flash capacity in bytes; read from RDID at init. */
    bool     initialised; /**< Set by qspi_flash_init(). */
} qspi_flash_driver_t;

static qspi_flash_driver_t s_qspi;
```


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
#if defined(STM32F469xx)
  #define QSPI_DEVICE_SIZE_BYTES  (16UL * 1024UL * 1024UL)   /* 16 MB */
  #define QSPI_DCR_FSIZE          (23U)                        /* 2^24 bytes */
  #define QSPI_EXPECTED_RDID      (0x20BA18U)                  /* MT25QL128ABA — QSPID-O3 */
#elif defined(STM32L475xx)
  #define QSPI_DEVICE_SIZE_BYTES  (8UL * 1024UL * 1024UL)    /* 8 MB */
  #define QSPI_DCR_FSIZE          (22U)                        /* 2^23 bytes */
  #define QSPI_EXPECTED_RDID      (0xC22817U)                  /* MX25R6435F — QSPID-O3 */
#endif
```

The `QSPI_EXPECTED_RDID` values must be verified against the actual device datasheets at implementation. Tracked as **QSPID-O3** (§8) — Gateway value confirmed by implementation (2026-09-15).

**Gateway file layout.** The Gateway implementation lives in `firmware/gateway/drivers/qspi_flash/` (`qspi_flash.h`, `qspi_flash.c`, `qspi_flash_hw.h`) — the unsuffixed name follows the Gateway `spi/`, `rtc/` convention and avoids a Ceedling basename collision with the Field Device `qspi_flash_driver.c`. `qspi_flash_hw.h` is the same mockable-indirection pattern as `cpu_hw.h`: in firmware builds its macros are direct register accesses (CCR write; byte-width DR read/write via a `volatile uint8_t *` so the QUADSPI FIFO advances one byte per access); in host tests they route to instrumented stubs in `stm32l475_cmsis_mock.c` (QSPID-D8).

### 3.7 WIP polling — no RTOS blocking

WIP polling uses a busy-wait loop with a software counter timeout. FreeRTOS is not present in the driver. The erase timeout (500 ms) means the calling task is blocked for up to 500 ms during an erase — this is acceptable because:
- `ConfigStore` erases are rare (config changes only).
- `CircularFlashLog` erases one sector per revolution (~193 per year per flash-partition-layout.md §6.2 analysis).
- `FirmwareStore` erases the OTA staging region during OTA — `UpdateServiceTask` (priority 1) holds no resources other tasks urgently need.

If profiling reveals this is a problem, replacing the busy-wait with a `vTaskDelay(1)` loop would free the CPU at the cost of importing `task.h` — which violates the driver convention. Defer until integration measurements confirm it is needed. Tracked as **QSPID-O4**.

**Every wait is bounded (QSPID-D9).** The Gateway implementation has no unbounded `while` loop: TCF, FTF, FLEVEL and BUSY waits use a 10 000-iteration counter; the WIP poll uses 400 000 RDSR round trips (~1.2 µs each at 26.67 MHz ≈ 500 ms, comfortably above the MX25R6435F worst-case sector erase of 240 ms). Any expiry returns `QSPI_FLASH_ERR_TIMEOUT` (P8).

---

### 3.8 Principles applied

- **P1 (Strict directional layering).** Depends only on CMSIS QUADSPI peripheral headers; no RTOS, no middleware.
- **P2 (Dependency Inversion).** Exposes `iqspi_flash_t` vtable; CircularFlashLog, ConfigStore, and FirmwareStore all depend on `IQspiFlash`.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static `QspiFlashState`; command buffers allocated on the stack (bounded by maximum command size); no heap.
- **P6 (Responsibility traces to requirements).** Read / program / erase operations trace to REQ-BF-* and REQ-NF-304 persistent-store requirements.
- **P8 (Total error propagation, no silent failures).** `qspi_flash_err_t` on all operations; WIP-poll timeout returns error rather than blocking indefinitely.
- **P9 (BARR-C coding standard).** Addresses and lengths `uint32_t`; no implicit widening.
- **P10 (Naming conventions).** Prefix `qspi_flash_`; interface `IQspiFlash` -> `iqspi_flash_t`; errors `QSPI_FLASH_ERR_*`.

### qspi_flash_init

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### qspi_flash_read

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### qspi_flash_erase_sector

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).


## 4. Hardware contract

### 4.1 QUADSPI peripheral — both boards

Both STM32F469 and STM32L475 include the same QUADSPI peripheral IP. Register layout is identical: `QUADSPI_CR` (control), `QUADSPI_DCR` (device config — flash size, CS high time, clock mode), `QUADSPI_SR` (status — BUSY, TCF, FTF, FLEVEL), `QUADSPI_FCR` (flag clear), `QUADSPI_DLR` (data length), `QUADSPI_CCR` (communication config), `QUADSPI_AR` (address), `QUADSPI_DR` (data). A single `.c` file with board-specific constants (§3.6) is sufficient.

### 4.2 Clock prescaler (QSPID-O2 — resolved for Gateway)

The QUADSPI clock = `HCLK / (prescaler + 1)`.

| Board | HCLK | Prescaler | QSPI clock | Limit that applies |
|---|---|---|---|---|
| FD | 180 MHz | 2 | 60 MHz | MT25QL128ABA: 133 MHz (SDR) |
| GW | 80 MHz (CpuDriver PLL) | 2 | 26.67 MHz | MX25R6435F **READ 03h: 33 MHz** |

The 80 MHz figure quoted for the MX25R6435F applies to `FAST_READ (0x0B)` only; the plain `READ (0x03)` opcode this driver uses (QSPID-D2) is rated to 33 MHz, so prescaler 1 (40 MHz) would be out of specification. Prescaler 2 is the fastest in-spec setting.

### 4.3 CS high time (CSHT in DCR)

Between consecutive commands, the NCS pin must remain high for a minimum time. For MX25R6435F (GW): t_SHSL2 ≥ 10 ns. For MT25QL128ABA (FD): t_SHSL ≥ 10 ns. At any practical QSPI clock ≥ 8 MHz, one clock cycle exceeds this. `CSHT = 0` (1 clock cycle) is safe for both devices. Verify at implementation.

### 4.4 Pin assignment (QSPID-O5 — resolved)

| Signal | FD (UM1932) | GW (UM2153) |
|---|---|---|
| CLK | PF10 AF9 | PE10 AF10 |
| NCS | PB6 AF10 | PE11 AF10 |
| IO0 | PF8 AF10 | PE12 AF10 |
| IO1 | PF9 AF10 | PE13 AF10 |
| IO2 | PF7 AF9 | PE14 AF10 |
| IO3 | PF6 AF9 | PE15 AF10 |

On the Gateway the driver configures PE10–PE15 itself through CMSIS (`RCC->AHB2ENR`, `GPIOE->MODER/OSPEEDR/OTYPER/PUPDR/AFR[1]`) so that USES stays exactly `CMSIS` as `components.md` states (QSPID-D10). The Field Device implementation calls `gpio_configure_pin()` from GpioDriver instead — a USES widening not reflected in `components.md`; see QSPID-O8.

---

### NVIC

N/A — the driver uses indirect-mode polling (busy-wait on `QUADSPI_SR.TCF`). The QUADSPI interrupt (`QUADSPI_IRQn`) is not enabled. This avoids a FreeRTOS import at the driver layer and matches the access pattern of the two middleware callers (single-task, bounded operation time).


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
| `QSPI_FLASH_ERR_NOT_INITIALISED` | Caller bug — boot ordering error (driver used before `qspi_flash_init()`). Log and halt in DEBUG builds |
| `QSPI_FLASH_ERR_NULL_POINTER` | Caller bug. Log and halt in DEBUG builds |

---

## 7. Unit-test plan

### 7.1 Framework and location

- **Framework:** Unity (ThrowTheSwitch.org).
- **Files:** `tests/field-device/drivers/qspi_flash_driver/test_qspi_flash_driver.c` (FD) and `tests/gateway/drivers/qspi_flash/test_qspi_flash_gw.c` (GW).
- **Build target:** host (PC). Same approach as GPIO and DebugUart testing.
- **Board selection:** Following the RtcDriver / DebugUartDriver split, each board has its own implementation file in its own CubeIDE project (`firmware/field-device/drivers/qspi_flash_driver/` and `firmware/gateway/drivers/qspi_flash/`); the GW test is compiled with `STM32L475xx` (see `tests/project.yml` `:test_qspi_flash_gw:`).

### 7.2 Mock strategy

The QUADSPI peripheral is a single-instance register block. The mock uses the same `stm32_cmsis_mock.h` infrastructure established for GPIO and DebugUart:

- **Mock peripheral.** A static `QUADSPI_TypeDef g_mock_quadspi` with the registers the driver touches (`CR`, `DCR`, `SR`, `FCR`, `DLR`, `CCR`, `AR`, `ABR`, `DR`). The driver source compiles unchanged — `#define QUADSPI (&g_mock_quadspi)` redirects the CMSIS pointer (GW: `tests/mocks/stm32l475xx.h` §QUADSPI).
- **Command-sequence instrumentation (GW).** A plain register struct only retains the *last* value written, so CCR writes and byte-width DR accesses go through `qspi_flash_hw.h` (QSPID-D8). In test builds the stubs log every CCR write together with the DLR value latched at that moment (`g_mock_quadspi_ccr_log[]`, `g_mock_quadspi_dlr_log[]`, `g_mock_quadspi_ccr_count`) — the DLR snapshot matters because the WIP poll legitimately reprograms `DLR = 0` after a Page Program.
- **RCC clock-enable registers.** Extended with the QUADSPI clock-enable bit (`QSPIEN` in `RCC->AHB3ENR` on both boards) and the GPIO port clock bits for the QUADSPI pins.
- **Status register injection.** Tests pre-set `mock_quadspi.SR` fields (`TCF`, `BUSY`, `FTF`, `FLEVEL`) before each driver call. The default `setUp` state: `BUSY = 0`, `TCF = 1` (transfer-complete immediately), `FTF = 1` (FIFO threshold — data ready). This makes happy-path tests pass without timing simulation.
- **Data register (DR) — read FIFO mock.** For operations that read DR multiple times (RDID reads 3 bytes, `qspi_flash_read` reads N bytes), a mock FIFO is provided: `mock_quadspi_push_dr(uint8_t val)` enqueues a value; each DR read pops the next value. Tests pre-load the FIFO before calling the driver. A read from an empty FIFO returns 0xFF and sets a test-visible underflow flag.
- **Data register (DR) — write capture.** For `qspi_flash_write_page`, each DR write is captured into a test-visible array (`mock_quadspi_written_data[]`, `mock_quadspi_written_count`). Tests verify the written bytes match the input data.
- **CCR command capture.** Each write to `CCR` is recorded in a circular log (`mock_quadspi_ccr_log[]`, `mock_quadspi_ccr_count`). Tests verify the command sequence: e.g., WREN (instruction 0x06) issued before Page Program (0x02).
- **WIP polling mock.** The Read Status Register command (0x05) returns 1 byte from DR. For WIP-clear tests, the FIFO is pre-loaded with `0x00` (WIP = 0). For timeout tests, WIP is set permanently: a mock counter limits how many WIP polls are allowed before the test infrastructure forces the driver loop to exit, preventing an infinite host-test hang. The driver's internal loop counter provides the bounded exit.
- **Test-isolation hook.** `qspi_flash_reset_for_test(void)` under `#ifdef TEST`. Clears `s_initialised`, `s_device_size`. Called from `setUp()` alongside `stm32_cmsis_mock_reset()`.
- **Tick source.** The QSPI driver uses a software counter for WIP polling timeout (§3.7), not an injected tick source. The counter is deterministic on the host — no tick injection needed (unlike DebugUartDriver's `debug_uart_set_tick_source`). Timeout tests set WIP permanently and verify the function returns `QSPI_FLASH_ERR_TIMEOUT` after the counter expires.

### 7.3 Test cases

Each test calls `qspi_flash_reset_for_test()` and `stm32_cmsis_mock_reset()` in `setUp()`. Unless stated otherwise, precondition "driver initialised" means `qspi_flash_init()` has returned `QSPI_FLASH_OK` with the mock DR FIFO pre-loaded with the expected RDID bytes.

**Pass H finding — error enum gaps:** The current `qspi_flash_err_t` (§2.4) lacks `QSPI_FLASH_ERR_NOT_INITIALISED` and `QSPI_FLASH_ERR_NULL_POINTER`. Every other driver in the project validates these guards. Test cases TC-QSPI-014, -027, -034, -015, -026 assume these codes will be added before implementation. Tracked as **QSPID-O7** (§8).

**`qspi_flash_init`**

| TC ID | Test function | Precondition | Action | Expected result |
|---|---|---|---|---|
| TC-QSPI-001 | `test_qspi_flash_init_happy_path` | Driver not initialised. Mock DR FIFO pre-loaded with 3 bytes of `QSPI_EXPECTED_RDID` (MSB-first). `SR.TCF = 1`. | Call `qspi_flash_init()`. | Returns `QSPI_FLASH_OK`. CCR log shows instruction 0x9F (RDID) issued. `DCR.FSIZE` set to board-expected value (23 for FD, 22 for GW). Subsequent operations do not return `ERR_NOT_INITIALISED`. |
| TC-QSPI-002 | `test_qspi_flash_init_wrong_rdid` | DR FIFO pre-loaded with `{0xFF, 0xFF, 0xFF}` (wrong ID). | Call `qspi_flash_init()`. | Returns `QSPI_FLASH_ERR_DEVICE`. Driver remains not-initialised. |
| TC-QSPI-003 | `test_qspi_flash_init_timeout` | `SR.TCF` permanently clear (RDID command never completes). | Call `qspi_flash_init()`. | Returns `QSPI_FLASH_ERR_TIMEOUT`. Driver remains not-initialised. |
| TC-QSPI-004 | `test_qspi_flash_init_idempotent` | Driver already initialised. | Call `qspi_flash_init()` again. | Returns `QSPI_FLASH_OK`. CCR log shows no new RDID command issued (short-circuits on `s_initialised`). |
| TC-QSPI-005 | `test_qspi_flash_init_enables_clocks_and_configures_pins` | Mock RCC registers zeroed. | Call `qspi_flash_init()`. | `RCC->AHB3ENR` has `QSPIEN` set. GPIO port clocks for QUADSPI pins enabled. QUADSPI pin `MODER` set to alternate function. `CR` prescaler set. |

**`qspi_flash_read`**

| TC ID | Test function | Precondition | Action | Expected result |
|---|---|---|---|---|
| TC-QSPI-010 | `test_qspi_flash_read_happy_path` | Driver initialised. DR FIFO pre-loaded with 256 bytes of known pattern. | Call `qspi_flash_read(0x0000, buf, 256)`. | Returns `QSPI_FLASH_OK`. `buf` contains the pre-loaded pattern. CCR log shows instruction 0x03, ADMODE single-wire, ADSIZE 3-byte. `AR == 0`. `DLR == 255`. |
| TC-QSPI-011 | `test_qspi_flash_read_addr_plus_len_exceeds_device` | Driver initialised. | Call `qspi_flash_read(QSPI_DEVICE_SIZE_BYTES - 10, buf, 20)`. | Returns `QSPI_FLASH_ERR_ADDR`. No command issued. |
| TC-QSPI-012 | `test_qspi_flash_read_len_zero` | Driver initialised. | Call `qspi_flash_read(0, buf, 0)`. | Returns `QSPI_FLASH_ERR_LEN`. No command issued. |
| TC-QSPI-013 | `test_qspi_flash_read_null_buf` | Driver initialised. | Call `qspi_flash_read(0, NULL, 16)`. | Returns `QSPI_FLASH_ERR_NULL_POINTER` (see QSPID-O7). |
| TC-QSPI-014 | `test_qspi_flash_read_not_initialised` | Driver not initialised. | Call `qspi_flash_read(0, buf, 16)`. | Returns `QSPI_FLASH_ERR_NOT_INITIALISED` (see QSPID-O7). |
| TC-QSPI-015 | `test_qspi_flash_read_peripheral_busy` | Driver initialised. `SR.BUSY = 1`. | Call `qspi_flash_read(0, buf, 16)`. | Returns `QSPI_FLASH_ERR_BUSY`. No command issued. |

**`qspi_flash_write_page`**

| TC ID | Test function | Precondition | Action | Expected result |
|---|---|---|---|---|
| TC-QSPI-020 | `test_qspi_flash_write_page_happy_path` | Driver initialised. WIP clears immediately (DR FIFO: status byte `0x00`). | Call `qspi_flash_write_page(0x0000, data, 128)` with 128-byte known pattern. | Returns `QSPI_FLASH_OK`. CCR log shows WREN (0x06) then Page Program (0x02) in that order. `AR == 0x0000`. `DLR == 127`. Written data captured in mock matches input. WIP polled at least once. |
| TC-QSPI-021 | `test_qspi_flash_write_page_max_page_size` | Driver initialised. WIP clears immediately. | Call `qspi_flash_write_page(0x0100, data, 256)` — full page at page-aligned address. | Returns `QSPI_FLASH_OK`. `DLR == 255`. 256 bytes written to DR. |
| TC-QSPI-022 | `test_qspi_flash_write_page_crosses_page_boundary` | Driver initialised. | Call `qspi_flash_write_page(0x00F0, data, 32)` — addr 0xF0 + 32 = 0x110, crosses into next page. | Returns `QSPI_FLASH_ERR_LEN`. No WREN issued. No DR writes. |
| TC-QSPI-023 | `test_qspi_flash_write_page_wip_timeout` | Driver initialised. WIP permanently set (DR FIFO: repeated `0x01`). | Call `qspi_flash_write_page(0x0000, data, 1)`. | Returns `QSPI_FLASH_ERR_TIMEOUT`. WREN and Page Program commands were issued (write committed to device but completion not confirmed). |
| TC-QSPI-024 | `test_qspi_flash_write_page_len_zero` | Driver initialised. | Call `qspi_flash_write_page(0x0000, data, 0)`. | Returns `QSPI_FLASH_ERR_LEN`. |
| TC-QSPI-025 | `test_qspi_flash_write_page_addr_exceeds_device` | Driver initialised. | Call `qspi_flash_write_page(QSPI_DEVICE_SIZE_BYTES, data, 1)`. | Returns `QSPI_FLASH_ERR_ADDR`. |
| TC-QSPI-026 | `test_qspi_flash_write_page_null_data` | Driver initialised. | Call `qspi_flash_write_page(0x0000, NULL, 16)`. | Returns `QSPI_FLASH_ERR_NULL_POINTER` (see QSPID-O7). |
| TC-QSPI-027 | `test_qspi_flash_write_page_not_initialised` | Driver not initialised. | Call `qspi_flash_write_page(0x0000, data, 1)`. | Returns `QSPI_FLASH_ERR_NOT_INITIALISED` (see QSPID-O7). |
| TC-QSPI-028 | `test_qspi_flash_write_page_peripheral_busy` | Driver initialised. `SR.BUSY = 1`. | Call `qspi_flash_write_page(0x0000, data, 1)`. | Returns `QSPI_FLASH_ERR_BUSY`. No WREN issued. |

**`qspi_flash_erase_sector`**

| TC ID | Test function | Precondition | Action | Expected result |
|---|---|---|---|---|
| TC-QSPI-030 | `test_qspi_flash_erase_sector_happy_path` | Driver initialised. WIP clears immediately. | Call `qspi_flash_erase_sector(0x1000)` — sector-aligned address. | Returns `QSPI_FLASH_OK`. CCR log shows WREN (0x06) then Sector Erase (0x20). `AR == 0x1000`. WIP polled at least once. |
| TC-QSPI-031 | `test_qspi_flash_erase_sector_auto_aligns` | Driver initialised. WIP clears immediately. | Call `qspi_flash_erase_sector(0x1234)` — mid-sector address. | Returns `QSPI_FLASH_OK`. `AR == 0x1000` (aligned to 4 KB boundary via `addr & ~0xFFFU`). |
| TC-QSPI-032 | `test_qspi_flash_erase_sector_wip_timeout` | Driver initialised. WIP permanently set. | Call `qspi_flash_erase_sector(0x0000)`. | Returns `QSPI_FLASH_ERR_TIMEOUT`. |
| TC-QSPI-033 | `test_qspi_flash_erase_sector_addr_exceeds_device` | Driver initialised. | Call `qspi_flash_erase_sector(QSPI_DEVICE_SIZE_BYTES)`. | Returns `QSPI_FLASH_ERR_ADDR`. |
| TC-QSPI-034 | `test_qspi_flash_erase_sector_not_initialised` | Driver not initialised. | Call `qspi_flash_erase_sector(0x0000)`. | Returns `QSPI_FLASH_ERR_NOT_INITIALISED` (see QSPID-O7). |
| TC-QSPI-035 | `test_qspi_flash_erase_sector_peripheral_busy` | Driver initialised. `SR.BUSY = 1`. | Call `qspi_flash_erase_sector(0x0000)`. | Returns `QSPI_FLASH_ERR_BUSY`. |

**Error-code coverage cross-check**

| Error code | Produced by TC(s) |
|---|---|
| `QSPI_FLASH_OK` | TC-QSPI-001, -004, -005, -010, -020, -021, -030, -031 |
| `QSPI_FLASH_ERR_BUSY` | TC-QSPI-015, -028, -035 |
| `QSPI_FLASH_ERR_TIMEOUT` | TC-QSPI-003, -023, -032 |
| `QSPI_FLASH_ERR_ADDR` | TC-QSPI-011, -025, -033 |
| `QSPI_FLASH_ERR_LEN` | TC-QSPI-012, -022, -024 |
| `QSPI_FLASH_ERR_DEVICE` | TC-QSPI-002 |
| `QSPI_FLASH_ERR_NOT_INITIALISED` *(pending QSPID-O7)* | TC-QSPI-014, -027, -034 |
| `QSPI_FLASH_ERR_NULL_POINTER` *(pending QSPID-O7)* | TC-QSPI-013, -026 |

All existing values of `qspi_flash_err_t` are exercised. Two proposed additions (QSPID-O7) are also covered.

### 7.4 Coverage target

- 100% of public API functions exercised.
- 100% of error codes in `qspi_flash_err_t` produced by at least one test case.
- ≥ 90% statement coverage in `qspi_flash_driver.c`.

### 7.5 Cannot be host-tested

- Actual SPI timing on the wire (prescaler accuracy, CS high time, read/write clock cycles).
- Flash device electrical behaviour (erase times, page-program times, endurance degradation).
- WIP polling duration in real time — host tests verify the loop exits correctly, not the elapsed milliseconds.
- Concurrent access from multiple RTOS tasks (QSPID-O1 mutex validation) — integration-phase only.
- Memory-mapped mode (not used by the driver, but if ever added).

These are integration-phase concerns.

### 7.6 Hardware bring-up test (Gateway)

File: `firmware/gateway/integration-tests/qspi_flash/main_test_qspi_flash.c`
— the vehicle for the §7.5 concerns. Bare-metal (no FreeRTOS), reports over
USART1/PB6 at 115 200 8N1, following the Gateway bring-up convention. Not
compiled by default; activation steps are in the file header.

**Destructive.** It erases and programs the 4 KB sector at byte offset
`0x0052_0000` — the start of the *(reserved)* region in
`flash-partition-layout.md` §5.2, so no partition is touched. It must not be
repointed at offset 0, which is the live `ConfigStore` partition.

| TC | Case | Closes |
|---|---|---|
| TC-HW-QSPI-001 | `qspi_flash_init()` returns `QSPI_FLASH_OK` on the physical part — positive confirmation of RDID `0xC22817` | **QSPID-O3** |
| TC-HW-QSPI-002 | Second `qspi_flash_init()` is idempotent | §2.5 contract |
| TC-HW-QSPI-003 | Erase, then `0xFF` at sector offsets 0, `0x800`, `0xFFF` | §7.5 erase reaches the array |
| TC-HW-QSPI-004 | 256-byte page program, verified read-back | §7.5 |
| TC-HW-QSPI-005 | Adjacent page written; page A re-verified intact (no page-wrap spill) | QSPID-D3 on silicon |
| TC-HW-QSPI-006 | Program without erase yields old `AND` new (NOR 1→0 only) | Device semantics — untestable on host |
| TC-HW-QSPI-007 | 512-byte read spans both pages contiguously | Read is not page-bound |
| TC-HW-QSPI-008 | Re-erase restores `0xFF` across the sector | §7.5 |
| TC-HW-QSPI-009 | Validation cascade returns the documented error codes | §6 |
| TC-HW-QSPI-010 | DWT-timed erase and page-program against datasheet windows (erase max 240 ms, program max 10 ms) and the ~500 ms bounded WIP poll | **QSPID-O4** measurement |

Not covered, and still deferred: QSPID-O1 multi-task mutex validation (needs a
second consumer to exist) and wire-level timing at 26.67 MHz (needs a logic
analyser on PE10–PE15).

**Diagnostic gap (QSPID-O9).** `qspi_flash_init()` verifies RDID internally but
never exposes the value it read, so a `QSPI_FLASH_ERR_DEVICE` result tells the
bench operator that the ID mismatched without saying what was actually on the
bus — the first thing needed to diagnose a wrong or unpopulated part.

---

## 8. Open items

| ID | Item | Owner | Resolution path |
|---|---|---|---|
| QSPID-O1 | **Peripheral-level concurrency gap (Gateway).** Three middleware consumers (`ConfigStore`, `CircularFlashLog`, `FirmwareStore`) access `QspiFlashDriver` from different tasks under independent mutexes. A shared `qspi_flash_mutex` must be acquired by all callers before calling any driver function. This mutex is a cross-cutting resource (not owned by any single middleware component). Resolution: add `qspi_flash_mutex` to the shared-resource locking table in `task-breakdown.md` §7 and initialise it in `main()`. All three consumers must acquire it before calling the driver. This is an HLD escalation — update `task-breakdown.md` and `lld.md` §5 (cross-cutting) accordingly. | Luca | Update `task-breakdown.md` §7 before implementing any of the three GW consumers |
| QSPID-O2 | ~~QUADSPI clock prescaler.~~ **Resolved 2026-09-15** — GW HCLK is 80 MHz (CpuDriver); prescaler 2 → 26.67 MHz, bounded by the 33 MHz limit of the `READ 03h` opcode, not the 80 MHz `FAST_READ` figure (§4.2). FD: prescaler 2 → 60 MHz. | — | Closed |
| QSPID-O3 | ~~Verify `QSPI_EXPECTED_RDID`.~~ **Resolved** — FD 0x20BA18 validated on hardware (v1.0, 2026-06); GW 0xC22817 (Macronix 0xC2, MX25R 0x28, 64 Mbit 0x17) per datasheet, implemented; hardware confirmation pending first GW bench run. | Luca | Run TC-HW-QSPI-001 in `main_test_qspi_flash.c` (§7.6) on the GW bench |
| QSPID-O4 | WIP polling busy-waits up to 500 ms during erase. If integration profiling reveals this degrades system responsiveness, replace with `vTaskDelay(1)` loop — but note this imports FreeRTOS into the driver, violating the driver convention. Evaluate trade-off at integration. | Luca | Defer until integration measurements |
| QSPID-O5 | ~~QUADSPI pin assignments.~~ **Resolved** — table in §4.4; GW PE10–PE15 AF10 per UM2153, implemented and pinned by TC-QSPI-005. | — | Closed |
| QSPID-O6 | **SD-06c/06d sequence diagram inconsistency.** The SDs show `FirmwareStore → QspiFlashDriver` writing the boot indicator and rollback flag, but these fields are in the on-chip metadata partition (`0x0800_4000`) per `flash-partition-layout.md` §5.1. `QspiFlashDriver` cannot write to on-chip flash. The SDs must be corrected and a separate on-chip flash write mechanism (Bootloader scope, or a `FlashDriver` for internal flash) identified. Escalate to HLD before `FirmwareStore` LLD companion is drafted. | Luca | Raise HLD gap; correct SD-06c and SD-06d; decide ownership of on-chip metadata writes |
| QSPID-O7 | ~~Error enum lacks `ERR_NOT_INITIALISED` and `ERR_NULL_POINTER`.~~ **Resolved for GW** — codes 6 and 7 added to §2.4 and §6, guards implemented and pinned by TC-QSPI-013/-014/-026/-027/-034. **FD drift:** the FD v1.0 implementation carries `QSPI_FLASH_ERR_NOT_INIT = 6` (different spelling) and has no NULL-pointer guard — align when the FD driver is next touched. | Luca | FD: rename to `_NOT_INITIALISED`, add code 7 + guard |
| QSPID-O8 | **FD USES drift (found during GW implementation).** The FD implementation `#include`s `gpio/gpio_driver.h` and calls `gpio_configure_pin()`, so its real USES is `CMSIS + GpioDriver`, whereas `components.md` says `CMSIS`. The GW implementation configures its pins through CMSIS directly (QSPID-D10) and matches the HLD. Either update `components.md` (FD row) or refactor the FD driver. | Luca | HLD decision when FD driver is next touched |
| QSPID-O9 | **No way to read back the device ID.** `qspi_flash_init()` checks RDID against `QSPI_EXPECTED_RDID` internally and returns `QSPI_FLASH_ERR_DEVICE` on mismatch without reporting the observed value, leaving a bench operator with no diagnostic. Found while writing the bring-up test (§7.6). Options: a `qspi_flash_get_device_id(uint32_t *id)` accessor, or cache the read ID in a `#ifdef TEST`-free module-scope variable with a getter. Not urgent — affects diagnosis, not correctness. | Luca | Decide when the driver is next touched; low priority |

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
| QSPID-D8 | GW: `qspi_flash_hw.h` mockable indirection for CCR writes and byte-width DR access | Same pattern as `cpu_hw.h`. Lets host tests observe command *sequences* (WREN before PP) and drive multi-byte responses, which a last-value register mock cannot; zero cost in firmware builds (macros expand to direct accesses) |
| QSPID-D9 | GW: every hardware wait is a bounded counter returning `ERR_TIMEOUT` | P8 — the FD v1.0 implementation has unbounded `while` loops on BUSY/FTF/TCF; a stuck peripheral would hang the calling task. Also makes TC-QSPI-003 (init timeout) testable |
| QSPID-D10 | GW: driver configures its own pins via CMSIS, not via GpioDriver | Keeps USES exactly `CMSIS` as in `components.md`; pinned by TC-QSPI-005. (SpiDriver GW chose the opposite — pins are main()'s job — which is also HLD-conformant; QSPI has a fixed single pin set so owning it in-driver is simpler) |

---

## Phase H — Readiness review

| # | Check | Status |
|---|---|---|
| H1 | PROVIDES / USES match `components.md` exactly | PASS (GW) — PROVIDES IQspiFlash, USES CMSIS only. FD implementation drifts (uses GpioDriver) — QSPID-O8 |
| H2 | Root SRS requirements cited and quoted | PASS — REQ-NF-405 / REQ-NF-402, REQ-DM-074, CON-009 |
| H3 | All public API functions have complete Doxygen | PASS — brief, param, return, note on every function |
| H4 | ADT pattern applied (or exception documented) | PASS — singleton exception documented in §1 (QSPID-D6) |
| H5 | Error enum covers all failure modes | PASS — 8 codes after QSPID-O7; every code produced by ≥ 1 TC |
| H6 | Hardware contract specifies all pins, AF numbers, clock config | PASS — §4.2 prescaler, §4.3 CSHT, §4.4 pin table (both boards) |
| H7 | All critical open items resolved or have named owner | PASS — O2, O5 closed; O3 hardware-confirm pending; O1, O4, O6, O7 (FD), O8 owned by Luca with paths |
| H8 | Unit-test plan covers happy path + error cases | PASS — 27 TCs across 4 functions + vtable; all TC IDs implemented in `test_qspi_flash_gw.c` (27/27 green, 2026-09-15) |
| H9 | Test file path follows Gateway folder convention | PASS — `tests/gateway/drivers/qspi_flash/test_qspi_flash_gw.c` |
| H10 | P1–P10 compliance reviewed | PASS — §3.8 |
| H11 | No FreeRTOS dependency | PASS — polling only, no ISR, no RTOS headers (§3.7, §4 NVIC) |
| H12 | Thread safety documented | PASS — §3.3 caller serialises; GW shared mutex is QSPID-O1 (middleware/HLD scope) |
| H13 | `reset_for_test` hook specified | PASS — §7.2, implemented under `#ifdef TEST` |
| H14 | Decisions log complete | PASS — 10 decisions |
| H15 | Sequence integration traces to HLD SDs | PASS — §5 (SD-06b/c/d), with QSPID-O6 inconsistency flagged upward |

**Verdict: PASS — Gateway implemented.** Remaining open items (QSPID-O1 shared mutex, O6 on-chip metadata SDs) are middleware/HLD scope and block the *consumers'* LLDs (CircularFlashLog, ConfigStore, FirmwareStore), not this driver.
