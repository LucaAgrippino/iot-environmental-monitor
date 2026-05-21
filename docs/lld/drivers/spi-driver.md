# SpiDriver — LLD Companion

**Document:** `docs/lld/drivers/spi-driver.md`
**Version:** 0.1 (draft — pending Luca review)
**Board scope:** Gateway (B-L475E-IOT01A) only
**Layer:** Driver
**Status:** Draft
**Date:** May 2026

**HLD anchor:** SpiDriver in `components.md` (GW driver layer)

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Transfers data between the MCU and SPI-connected peripherals | `components.md` |
| PROVIDES (upward) | `ISpi` | `components.md` |
| USES (downward) | CMSIS | `components.md` |
| Root requirement | CON-001 | `SRS.md` §4 |
| Board | Gateway only | `components.md` — absent from Field Device driver list |
| Hardware peripheral | SPI3 of STM32L475VG | UM2153 §7.11.3 |

**Consumer:** `WifiDriver` only. `WifiDriver USES SpiDriver, GpioDriver` — the two USES entries are independent: SpiDriver moves bytes over MOSI/MISO/SCK; GpioDriver manages the NSS chip-select line. SpiDriver never touches NSS.

**Task context:** `WifiDriver` is hosted exclusively in `WifiTask` (priority 3, task-breakdown.md §5.2, D29). `WifiTask` is the sole accessor of SPI3 — no concurrent access is possible by construction. No mutex is required, consistent with the "caller serialises" convention.

**ISR clarification:** the `SPI_wifi_IRQHandler` in the task-breakdown.md ISR inventory (§6.1) is triggered by the ISM43362 DATARDY GPIO line (an EXTI interrupt signalling that the WiFi module has a response ready), not by the SPI3 peripheral's own RX interrupt. This ISR belongs to `WifiDriver`, not to `SpiDriver`. `SpiDriver` has no ISR.

**CON-001 text (SRS.md §4):** *"The gateway WiFi module (ISM43362-M3G-L44) communicates with the host MCU via SPI using AT commands. All TCP/IP and TLS operations are handled by the module's internal stack, not by the application firmware."*

---

## 2. Public API

### 2.1 Dependency-conformance check

The public header (`spi_driver.h`) includes only `stm32l475xx.h` (via board wrapper) and `stdint.h`. No FreeRTOS headers, no `gpio_driver.h`. NSS is managed by the caller via `GpioDriver`; SpiDriver configures only SCK, MOSI, MISO as alternate-function outputs. Confirmed clean.

### 2.2 P3 consideration

Single consumer (`WifiDriver`), single use case (AT command exchange). The interface is trivially narrow — one init function and one transfer function. No ISP split warranted.

### 2.3 Transaction model

SPI is inherently full-duplex: MOSI and MISO are always active simultaneously. A single `spi_transceive` call covers all three usage patterns:

- **Write-only** (send AT command, discard MISO): pass `rx_buf = NULL`.
- **Read-only** (send dummy bytes, capture MISO): pass `tx_buf = NULL`; driver transmits `0x00` dummy bytes.
- **Full-duplex** (both buffers provided): both are used byte-for-byte.

Separating into `spi_write` and `spi_read` would not simplify the caller — `WifiDriver` always knows which pattern it needs — and would obscure the hardware reality that the SPI peripheral always shifts in both directions.

### 2.4 Data types

```c
/**
 * @brief Error codes returned by all SpiDriver operations.
 *
 * Naming follows the cross-cutting convention established in lld.md §3.2.
 */
typedef enum {
    SPI_ERR_OK      = 0, /**< Operation succeeded. */
    SPI_ERR_TIMEOUT = 1, /**< TXE or RXNE flag did not assert within timeout. */
} spi_err_t;
```

### 2.5 Public API (`spi_driver.h`)

```c
/**
 * @brief Initialise SPI3 for communication with the ISM43362 WiFi module.
 *
 * Configures SCK, MOSI, MISO as alternate-function outputs.
 * Mode 0 (CPOL=0, CPHA=0), 8-bit data, MSB first.
 * Clock speed: see §4.2 (SPID-O1 — baud rate divisor pending clock config).
 * Sets FRXTH=0 in CR2 to trigger RXNE after every byte (required for
 * correct 16-bit operation on L4 SPI — see §3.3).
 * Does NOT configure or assert NSS; NSS is managed by WifiDriver
 * via GpioDriver.
 *
 * Must be called once from main() before any spi_transceive call.
 *
 * @return SPI_ERR_OK on success.
 * @note Threading: task-context only, non-blocking. Must be called before the scheduler starts.
 */
spi_err_t spi_init(void);

/**
 * @brief Exchange bytes over SPI3.
 *
 * Full-duplex transfer: for each byte, one byte is shifted out on MOSI
 * and one is shifted in on MISO simultaneously.
 *
 * If tx_buf is NULL, dummy bytes (0x00) are transmitted.
 * If rx_buf is NULL, received bytes are discarded.
 * Both tx_buf and rx_buf NULL is a caller error; behaviour is undefined.
 *
 * NSS must be asserted by the caller (WifiDriver via GpioDriver) before
 * calling this function, and de-asserted after it returns. SpiDriver
 * never touches NSS.
 *
 * Caller serialises all calls (established convention; WifiTask is the
 * sole caller — no concurrency concern in practice).
 *
 * @param tx_buf  Pointer to bytes to transmit, or NULL for dummy bytes.
 * @param rx_buf  Pointer to receive buffer, or NULL to discard.
 * @param len     Number of 16-bit words to exchange (not bytes).
 * @return SPI_ERR_OK on success; SPI_ERR_TIMEOUT if a flag does not
 *         assert within the timeout window.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
spi_err_t spi_transceive(const uint16_t *tx_buf, uint16_t *rx_buf, uint16_t len);
```

---

## 3. Internal design

### 3.0 Private struct

```c
typedef struct {
    bool initialised; /**< Set by spi_init(). */
} spi_driver_t;

static spi_driver_t s_spi;
```


### 3.1 Module-level state

```c
static bool s_initialised = false;
```

One SPI3 peripheral, one consumer, one task. No further state required. Consistent with the singleton pattern established across all prior driver companions.

### 3.2 Transfer loop

The STM32L475 SPI peripheral (register-level, no HAL) follows this polling sequence per word:

```
For each word in [0, len):
  1. Wait for TXE = 1 in SR     → SPI_ERR_TIMEOUT if expired
  2. Write tx word to DR         (or 0x0000 if tx_buf is NULL)
  3. Wait for RXNE = 1 in SR    → SPI_ERR_TIMEOUT if expired
  4. Read rx word from DR        (discard if rx_buf is NULL)
After all words:
  5. Wait for BSY = 0 in SR     → SPI_ERR_TIMEOUT if expired
```

Step 5 (BSY check) ensures the last byte has fully clocked out before returning. Without it, de-asserting NSS while BSY is still set truncates the last bit on some SPI devices. WifiDriver de-asserts NSS immediately after `spi_transceive` returns, so the BSY wait is mandatory.

### 3.3 FRXTH — critical L4-specific configuration

On STM32L4 SPI, CR2.FRXTH controls the FIFO RX threshold that asserts RXNE. The ISM43362 requires 16-bit SPI frames (DS[3:0] = 1111 in CR2). For 16-bit frames, FRXTH must be 0 (RXNE asserts when FIFO contains ≥ 16 bits). Setting FRXTH=1 with 16-bit frames causes RXNE to fire after the first 8 bits only, producing corrupted reads. Verified against RM0351 §40.4.7. Note: the FRXTH=1 / 8-bit assumption recorded in an earlier draft of this companion was incorrect for the ISM43362 use case; corrected per WIFI-O2.

### 3.4 No ISR, no DMA, no callbacks

Consistent with the driver design pattern across all prior companions. The transfer is synchronous and runs entirely within `WifiTask`'s execution context.

---

### 3.5 Principles applied

- **P1 (Strict directional layering).** Depends only on CMSIS SPI peripheral headers (L4-specific FRXTH flag handled internally); no middleware.
- **P2 (Dependency Inversion).** Exposes `ispi_t` vtable; MagnetometerDriver and ImuDriver depend on `ISpi`.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static `SpiState`; transfer buffer on caller's stack; no heap.
- **P6 (Responsibility traces to requirements).** Synchronous transfer function traces to GW sensor Group B sampling requirements.
- **P8 (Total error propagation, no silent failures).** `spi_err_t` on all transfers; timeout returns error rather than spinning indefinitely.
- **P9 (BARR-C coding standard).** `uint8_t*` for data; `uint16_t` for transfer length; FRXTH bit set per datasheet requirement.
- **P10 (Naming conventions).** Prefix `spi_`; interface `ISpi` -> `ispi_t`; errors `SPI_ERR_*`.


### Synchronisation

Caller serialises. The driver holds no FreeRTOS synchronisation primitives. All entry points are intended to be called from a single task context or from `main()` before the scheduler starts. Concurrent access from multiple tasks is not safe unless the caller provides a mutex.

### spi_init

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### spi_transceive

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).


## 4. Hardware contract

### 4.1 SPI mode and frame format

| Parameter | Value | Source |
|---|---|---|
| Mode | 0 (CPOL=0, CPHA=0) | ISM43362-M3G-L44 SPI protocol |
| Data size | 16-bit (DS[3:0] = 1111 in SPI_CR2) | ISM43362 SPI protocol requirement |
| FRXTH     | 0 (RXNE on ≥ 16-bit in FIFO)        | Required for 16-bit frames; see §3.3 |
| Bit order | MSB first | ISM43362 SPI protocol |
| Full-duplex | Yes | SPI3 master mode |

Mode 0 means data is sampled on the rising edge of SCK. CPOL=0 sets SCK idle low; CPHA=0 samples on the first (rising) edge.

### 4.2 Clock speed (open item)

SPI3 is clocked from PCLK1 (APB1) on the STM32L475. The baud rate divisor (`BR[2:0]` in SPI_CR1) is set relative to PCLK1: `f_SCK = PCLK1 / 2^(BR+1)`. The ISM43362 supports SPI clock up to 20 MHz. The actual divisor cannot be determined until the system clock tree is fixed. Tracked as **SPID-O1** (§8).

### 4.3 Pin assignment (Gateway, SPI3)

Per UM2153 board schematic and the hardware bottom-up sweep in `components.md`:

| Signal | MCU pin | Direction |
|---|---|---|
| SCK | PC10 (SPI3_SCK) | Output |
| MOSI | PC12 (SPI3_MOSI) | Output |
| MISO | PC11 (SPI3_MISO) | Input |

NSS is not managed by SpiDriver. The ISM43362 chip-select line is a GPIO output controlled by `WifiDriver` via `GpioDriver`.

**Action required (Luca at implementation):** confirm SPI3 alternate function numbers for PC10/PC11/PC12 against RM0351 §3 (pin multiplexing table) and verify against the UM2153 schematic. The pin assignments above are derived from the UM2153 hardware block diagram; verify before coding.

### 4.4 DUART-O2 interaction

SPID-O1 shares the same root dependency (APB1/PCLK1 clock configuration) as DUART-O2 and I2CD-O1/O2. All three resolve when `clock-config.md` lands.

---

### Registers

| Register | Access | Purpose |
|---|---|---|
| `SPI3->CR1` | R/W | Master mode, CPOL/CPHA, software NSS, data-frame format select. |
| `SPI3->CR2` | R/W | DS[3:0] = 1111 (16-bit frame); FRXTH = 0 (RXNE on ≥ 16 bits). |
| `SPI3->SR` | R | TXE, RXNE, BSY flags polled in the transfer loop. |
| `SPI3->DR` | R/W | Data register — 16-bit write transmits; 16-bit read receives. |

Register access via the CMSIS `SPI3` macro (`SPI_TypeDef *` at the fixed peripheral base address). No HAL.

### NVIC

N/A — the driver uses a polling model (busy-wait on `SPI_SR.TXE` / `SPI_SR.RXNE` / `SPI_SR.BSY`). The SPI interrupt (`SPI3_IRQn`) is not enabled. Polling is acceptable for the ISM43362 transfer sizes used by WifiDriver.


## 5. Sequence integration

`SpiDriver` has no HLD-level sequence diagram surface. All WiFi-related sequences (SD-03, SD-04, SD-09) have `WifiDriver` as the lowest visible lifeline; `SpiDriver` is an internal implementation detail of `WifiDriver`. No changes to `sequence-diagrams.md` are required.

---

## 6. Error and fault behaviour

All public functions return `spi_err_t`; callers must not ignore non-OK returns.
No retry is performed by the driver — callers apply retry and logging policy.

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `SPI_ERR_TIMEOUT` | TXE or RXNE flag did not assert within `SPI_TIMEOUT_MS` | Return error; NSS left in its current state (caller is responsible for deassert on error) | Non-OK return | No retry — WifiDriver and QspiFlashDriver may retry at the protocol level; SpiDriver itself does not | Caller logs at WARN via ILogger; WifiDriver returns `WIFI_ERR_SPI` to NtpClient/CloudPublisher |


## 7. Unit-test plan

Host-platform tests (Unity framework). The CMSIS `SPI3` macro is redirected via `#define SPI3 (&mock_spi)` in the test build, substituting a statically allocated `SPI_TypeDef` instance.

| ID | Test case | Expected result |
|---|---|---|
| T-SPI-01 | `spi_init`: verify SPI3 enabled, Mode 0, 8-bit, FRXTH=1 | CR1: CPOL=0, CPHA=0, SPE=1; CR2: DS=1111 (16-bit), FRXTH=0 |
| T-SPI-02 | `spi_transceive` happy path, full-duplex: 4 bytes | All 4 tx bytes written to DR in order; all 4 rx bytes captured from DR |
| T-SPI-03 | `spi_transceive` with tx_buf=NULL: 2 bytes | 0x00 dummy bytes written to DR; rx bytes captured |
| T-SPI-04 | `spi_transceive` with rx_buf=NULL: 2 bytes | tx bytes written normally; RXNE read and discarded without writing rx_buf |
| T-SPI-05 | `spi_transceive` single byte | Correct: tx written, rx captured, BSY wait issued |
| T-SPI-06 | TXE timeout: TXE never asserts | Returns `SPI_ERR_TIMEOUT`; no bytes transmitted |
| T-SPI-07 | RXNE timeout: TXE ok, RXNE never asserts | Returns `SPI_ERR_TIMEOUT` |
| T-SPI-08 | BSY timeout: all bytes transferred, BSY stuck | Returns `SPI_ERR_TIMEOUT` |
| T-SPI-09 | `spi_init` idempotency: call twice | Second call re-initialises without hang |

Test file: `tests/drivers/test_spi_driver.c`.

---

## 8. Open items

| ID | Item | Owner | Resolution path |
|---|---|---|---|
| SPID-O1 | SPI3 baud rate divisor (`BR[2:0]`). Depends on PCLK1 from the clock tree. Use RM0351 §40.4.1 formula once `clock-config.md` is settled. Ensure resulting SCK ≤ 20 MHz (ISM43362 limit). | Luca | Resolve when `clock-config.md` lands (shared root with DUART-O2, I2CD-O1/O2) |
| SPID-O2 | Confirm SPI3 AF numbers and pin assignment for PC10/PC11/PC12 against RM0351 pin multiplexing table. | Luca | Check at implementation |

**Inherited open items with no surface area in this companion:**
- O1 (WiFi SPI driver naming — resolved: `SpiDriver` per `components.md`). This was listed as open in `project_status.md`; it is now confirmed.
- O2 (worst-case stack measurements): not applicable.
- O3 (hardware watchdog): not applicable.

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| SPID-D1 | Single `spi_transceive` covering write-only, read-only, and full-duplex via NULL pointer convention | SPI is inherently full-duplex; separate write/read functions would obscure hardware reality and add no clarity for the single consumer |
| SPID-D2 | NSS not managed by SpiDriver | `WifiDriver USES SpiDriver, GpioDriver` independently per `components.md`; NSS timing is part of the AT command protocol (WifiDriver's concern), not the byte-transfer concern |
| SPID-D3 | Polling, not interrupt or DMA | `WifiTask` is dedicated to WiFi I/O; blocking the task during a transfer is the correct and simplest model. DMA adds complexity with no benefit for the transfer sizes involved in AT command exchanges |
| SPID-D4 | FRXTH=1 mandated in `spi_init` documentation | This is a required L4-specific configuration for correct 8-bit operation; documenting it explicitly prevents the most common defect in STM32L4 SPI drivers |
| SPID-D5 | BSY wait after last byte before returning | Ensures the caller (WifiDriver) can safely de-assert NSS immediately after `spi_transceive` returns without truncating the last clock cycle |
| SPID-D6 | Singleton module (no handle) | One SPI3 peripheral, one consumer; consistent with all prior driver companions |
| SPID-D7 | O1 (WiFi SPI driver naming) resolved | `SpiDriver` is the correct name per `components.md`. The open item in `project_status.md` is now closed |
