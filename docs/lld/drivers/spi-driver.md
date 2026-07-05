# SpiDriver — LLD Companion

**Document:** `docs/lld/drivers/spi-driver.md`
**Version:** 0.2 (Phase H complete — ready for implementation)
**Board scope:** Gateway (B-L475E-IOT01A) only
**Layer:** Driver
**Status:** Implementation-ready
**Date:** July 2026

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

The public header (`spi_driver.h`) includes only `stm32l475xx.h` (via board wrapper) and `<stdint.h>` / `<stdbool.h>`. No FreeRTOS headers, no `gpio_driver.h`. NSS is managed by the caller via `GpioDriver`; SpiDriver configures only SCK, MOSI, MISO as alternate-function outputs. Confirmed clean.

### 2.2 ADT pattern

Gateway modules use the ADT pattern by default. SpiDriver follows this convention: an opaque handle (`spi_handle_t`) is returned by `spi_create()` from a static internal pool. The internal struct is hidden in the `.c` file. Pool size is 1 (only SPI3 is used on this board), but the pattern is applied consistently for API uniformity and to demonstrate the technique.

### 2.3 P3 consideration

Single consumer (`WifiDriver`), single use case (AT command exchange). The interface is trivially narrow — one create function and one transfer function. No ISP split warranted.

### 2.4 Transaction model

SPI is inherently full-duplex: MOSI and MISO are always active simultaneously. A single `spi_transceive` call covers all three usage patterns:

- **Write-only** (send AT command, discard MISO): pass `rx_buf = NULL`.
- **Read-only** (send dummy bytes, capture MISO): pass `tx_buf = NULL`; driver transmits `0x0000` dummy words.
- **Full-duplex** (both buffers provided): both are used word-for-word.

Separating into `spi_write` and `spi_read` would not simplify the caller — `WifiDriver` always knows which pattern it needs — and would obscure the hardware reality that the SPI peripheral always shifts in both directions.

### 2.5 Data types

```c
/**
 * @brief Opaque handle to an SPI driver instance.
 */
typedef struct spi_inst *spi_handle_t;

/**
 * @brief Error codes returned by all SpiDriver operations.
 */
typedef enum {
    SPI_ERR_OK          = 0, /**< Operation succeeded.                         */
    SPI_ERR_TIMEOUT     = 1, /**< TXE, RXNE, or BSY flag did not assert/clear
                                  within the timeout window.                   */
    SPI_ERR_NULL_PTR    = 2, /**< A required pointer argument was NULL.        */
    SPI_ERR_NO_RESOURCE = 3, /**< Static instance pool exhausted.              */
} spi_err_t;

/**
 * @brief SPI instance configuration.
 */
typedef struct {
    SPI_TypeDef *instance; /**< SPI peripheral (e.g. SPI3).                    */
} spi_config_t;
```

### 2.6 Public API (`spi_driver.h`)

```c
/**
 * @brief Create and initialise an SPI driver instance.
 *
 * Enables the SPI peripheral clock, configures SCK, MOSI, MISO as
 * alternate-function outputs (AF6 for SPI3 on the L475).
 * Mode 0 (CPOL=0, CPHA=0), 16-bit data frame (DS=1111), MSB first.
 * Sets FRXTH=0 in CR2 so RXNE fires after a full 16-bit word is
 * received (see §3.3).
 * Clock speed: 10 MHz (BR=010, PCLK1/8; see §4.2).
 * Does NOT configure or assert NSS; NSS is managed by WifiDriver
 * via GpioDriver.
 *
 * Must be called once from main() before any spi_transceive call.
 *
 * @param[in]  config  SPI peripheral to use.
 * @param[out] handle  Receives the created handle on success.
 * @return SPI_ERR_OK on success; SPI_ERR_NULL_PTR if config or handle
 *         is NULL; SPI_ERR_NO_RESOURCE if the static pool is exhausted.
 * @note Threading: task-context only. Must be called before the
 *       scheduler starts.
 */
spi_err_t spi_create(const spi_config_t *config, spi_handle_t *handle);

/**
 * @brief Exchange 16-bit words over SPI.
 *
 * Full-duplex transfer: for each word, one 16-bit word is shifted out
 * on MOSI and one is shifted in on MISO simultaneously.
 *
 * If tx_buf is NULL, dummy words (0x0000) are transmitted.
 * If rx_buf is NULL, received words are discarded.
 * Both tx_buf and rx_buf NULL is a caller error and returns
 * SPI_ERR_NULL_PTR.
 *
 * NSS must be asserted by the caller (WifiDriver via GpioDriver)
 * before calling this function, and de-asserted after it returns.
 * SpiDriver never touches NSS.
 *
 * @param[in]  handle  Handle from spi_create().
 * @param[in]  tx_buf  Pointer to words to transmit, or NULL for dummy.
 * @param[out] rx_buf  Pointer to receive buffer, or NULL to discard.
 * @param[in]  len     Number of 16-bit words to exchange.
 * @return SPI_ERR_OK on success; SPI_ERR_NULL_PTR if handle is NULL or
 *         both buffers are NULL; SPI_ERR_TIMEOUT if a flag does not
 *         assert/clear within the timeout window.
 * @note Threading: task-context only, blocking. Not ISR-safe.
 */
spi_err_t spi_transceive(spi_handle_t handle,
                          const uint16_t *tx_buf,
                          uint16_t *rx_buf,
                          uint16_t len);
```

---

## 3. Internal design

### 3.1 Private struct and static pool

```c
/* spi_driver.c — internal, not visible to consumers */

#define SPI_MAX_INSTANCES  1u

struct spi_inst {
    SPI_TypeDef *periph;     /**< Pointer to SPI register block.  */
    bool         in_use;     /**< Slot allocated by spi_create(). */
};

static struct spi_inst g_pool[SPI_MAX_INSTANCES];
static uint8_t         g_count;
```

One SPI3 peripheral, one consumer, one task. Pool size of 1 matches the hardware. The ADT pattern is applied for API consistency across the Gateway driver layer, not because multiple instances are expected.

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

Step 5 (BSY check) ensures the last word has fully clocked out before returning. Without it, de-asserting NSS while BSY is still set truncates the last bit on some SPI devices. WifiDriver de-asserts NSS immediately after `spi_transceive` returns, so the BSY wait is mandatory.

### 3.3 FRXTH — critical L4-specific configuration

On STM32L4 SPI, CR2.FRXTH controls the FIFO RX threshold that asserts RXNE. The ISM43362 requires 16-bit SPI frames (DS[3:0] = 1111 in CR2). For 16-bit frames, **FRXTH must be 0** (RXNE asserts when FIFO contains ≥ 16 bits). Setting FRXTH=1 with 16-bit frames causes RXNE to fire after the first 8 bits only, producing corrupted reads. Verified against RM0351 §40.4.7.

### 3.4 No ISR, no DMA, no callbacks

Consistent with the driver design pattern across all prior companions. The transfer is synchronous and runs entirely within `WifiTask`'s execution context.

### 3.5 Test reset hook

```c
#ifdef TEST
void spi_reset_for_test(void)
{
    memset(g_pool, 0, sizeof(g_pool));
    g_count = 0u;
}
#endif
```

---

## 4. Hardware contract

### 4.1 SPI mode and frame format

| Parameter | Value | Source |
|---|---|---|
| Mode | 0 (CPOL=0, CPHA=0) | ISM43362-M3G-L44 SPI protocol |
| Data size | 16-bit (DS[3:0] = 1111 in SPI_CR2) | ISM43362 SPI protocol requirement |
| FRXTH | 0 (RXNE on ≥ 16 bits in FIFO) | Required for 16-bit frames; see §3.3 |
| Bit order | MSB first | ISM43362 SPI protocol |
| Full-duplex | Yes | SPI3 master mode |

Mode 0 means data is sampled on the rising edge of SCK. CPOL=0 sets SCK idle low; CPHA=0 samples on the first (rising) edge.

### 4.2 Clock speed — RESOLVED

SPI3 is clocked from PCLK1 (APB1) on the STM32L475. CPU module companion establishes PCLK1 = 80 MHz (no APB1 prescaler).

**Selected divisor:** BR[2:0] = 010 → f_SCK = PCLK1 / 8 = **10 MHz**.

The ISM43362 supports SPI clock up to 20 MHz. 10 MHz provides a 2× safety margin while still being fast enough for AT command exchange throughput. Higher speeds (BR=001 → 20 MHz) are at the module's absolute limit and risk signal integrity issues on the Discovery board's PCB traces.

### 4.3 Pin assignment (Gateway, SPI3) — CONFIRMED

Per UM2153 I/O assignment table (Table 11, pins 78–80) and board schematic (Figure 23):

| Signal | MCU pin | AF | Direction | UM2153 label |
|---|---|---|---|---|
| SCK | PC10 | AF6 | Output | INTERNAL-SPI3_SCK |
| MISO | PC11 | AF6 | Input | INTERNAL-SPI3_MISO |
| MOSI | PC12 | AF6 | Output | INTERNAL-SPI3_MOSI |

NSS (ISM43362 chip-select) is PE0, managed by `WifiDriver` via `GpioDriver` — not by SpiDriver.

DATARDY (ISM43362 data-ready interrupt) is PE1, managed by `WifiDriver` — not by SpiDriver.

### 4.4 Registers

| Register | Access | Purpose |
|---|---|---|
| `SPI3->CR1` | R/W | Master mode (MSTR=1), CPOL=0, CPHA=0, BR[2:0]=010, SSM=1, SSI=1, SPE=1. |
| `SPI3->CR2` | R/W | DS[3:0]=1111 (16-bit frame), FRXTH=0 (RXNE on ≥ 16 bits). |
| `SPI3->SR` | R | TXE, RXNE, BSY flags polled in the transfer loop. |
| `SPI3->DR` | R/W | Data register — 16-bit write transmits; 16-bit read receives. |

Register access via the CMSIS `SPI3` macro (`SPI_TypeDef *` at the fixed peripheral base address). No HAL.

### 4.5 NVIC

N/A — the driver uses a polling model (busy-wait on TXE / RXNE / BSY). The SPI interrupt (`SPI3_IRQn`) is not enabled. Polling is acceptable for the ISM43362 transfer sizes used by WifiDriver.

---

## 5. Sequence integration

`SpiDriver` has no HLD-level sequence diagram surface. All WiFi-related sequences (SD-03, SD-04, SD-09) have `WifiDriver` as the lowest visible lifeline; `SpiDriver` is an internal implementation detail of `WifiDriver`. No changes to `sequence-diagrams.md` are required.

---

## 6. Error and fault behaviour

All public functions return `spi_err_t`; callers must not ignore non-OK returns.
No retry is performed by the driver — callers apply retry and logging policy.

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `SPI_ERR_TIMEOUT` | TXE, RXNE, or BSY flag did not assert/clear within `SPI_TIMEOUT_MS` | Return error; NSS left in its current state (caller responsible for de-assert on error) | Non-OK return | No — WifiDriver may retry at the AT command protocol level; SpiDriver itself does not | Caller logs at WARN via ILogger; WifiDriver returns `WIFI_ERR_SPI` to consumers |
| `SPI_ERR_NULL_PTR` | Handle is NULL, or both tx_buf and rx_buf are NULL | Return error immediately; no hardware access | Non-OK return | No | Indicates a programming error — caller logs at ERROR |
| `SPI_ERR_NO_RESOURCE` | `spi_create()` called when pool is exhausted | Return error; no hardware configured | Non-OK return | No | Indicates a system design error — should never occur in correct initialisation |

---

## 7. Principles applied

- **P1 (Strict directional layering).** Depends only on CMSIS SPI peripheral headers; no middleware, no application dependencies.
- **P3 (ISP).** Single consumer (`WifiDriver`), trivially narrow interface — no split warranted.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static pool of `struct spi_inst`; transfer buffers on caller's stack; no heap.
- **P6 (Responsibility traces to requirements).** Synchronous transfer function traces directly to CON-001 (WiFi module SPI communication).
- **P8 (Total error propagation, no silent failures).** `spi_err_t` on all functions; timeout returns error rather than spinning indefinitely; NULL pointer validation on all entry points.
- **P9 (BARR-C coding standard).** `uint16_t *` for data buffers; `uint16_t` for transfer length; fixed-width types throughout.
- **P10 (Naming conventions).** Prefix `spi_`; handle type `spi_handle_t`; errors `SPI_ERR_*`; config struct `spi_config_t`.

**P2 (DIP) — not applicable.** SpiDriver has a single consumer (`WifiDriver`) at the same driver layer. The `ISpi` interface named in `components.md` serves as the contract, but no cross-layer inversion is involved.

---

## 8. Synchronisation

Caller serialises. The driver holds no FreeRTOS synchronisation primitives. `spi_create()` is called once from `main()` before the scheduler starts. `spi_transceive()` is called exclusively from `WifiTask`. No concurrent access is possible by construction.

---

## 9. Unit-test plan

Host-platform tests (Unity + CMock). The CMSIS `SPI3` macro is redirected via `#define SPI3 (&mock_spi)` in the test build, substituting a statically allocated `SPI_TypeDef` instance.

Test file: `tests/gateway/drivers/spi_driver/test_spi_driver.c`

| ID | Test case | Expected result |
|---|---|---|
| T-SPI-01 | `spi_create` happy path: verify SPI3 enabled, Mode 0, 16-bit, FRXTH=0 | CR1: CPOL=0, CPHA=0, MSTR=1, BR=010, SSM=1, SSI=1, SPE=1; CR2: DS=1111, FRXTH=0. Handle non-NULL. Returns `SPI_ERR_OK`. |
| T-SPI-02 | `spi_create` with NULL config | Returns `SPI_ERR_NULL_PTR`; no hardware access |
| T-SPI-03 | `spi_create` with NULL handle pointer | Returns `SPI_ERR_NULL_PTR`; no hardware access |
| T-SPI-04 | `spi_create` called twice (pool exhaustion) | First call returns `SPI_ERR_OK`; second returns `SPI_ERR_NO_RESOURCE` |
| T-SPI-05 | `spi_transceive` full-duplex: 4 words | All 4 tx words written to DR in order; all 4 rx words captured from DR. Returns `SPI_ERR_OK`. |
| T-SPI-06 | `spi_transceive` with tx_buf=NULL: 2 words | 0x0000 dummy words written to DR; rx words captured |
| T-SPI-07 | `spi_transceive` with rx_buf=NULL: 2 words | tx words written normally; DR read and discarded without writing rx_buf |
| T-SPI-08 | `spi_transceive` both buffers NULL | Returns `SPI_ERR_NULL_PTR`; no hardware access |
| T-SPI-09 | `spi_transceive` with NULL handle | Returns `SPI_ERR_NULL_PTR` |
| T-SPI-10 | `spi_transceive` single word | tx written, rx captured, BSY wait issued. Returns `SPI_ERR_OK`. |
| T-SPI-11 | TXE timeout: TXE never asserts | Returns `SPI_ERR_TIMEOUT`; no words transmitted |
| T-SPI-12 | RXNE timeout: TXE ok, RXNE never asserts | Returns `SPI_ERR_TIMEOUT` |
| T-SPI-13 | BSY timeout: all words transferred, BSY stuck | Returns `SPI_ERR_TIMEOUT` |
| T-SPI-14 | `spi_create` idempotency after `spi_reset_for_test` | After reset, `spi_create` succeeds again with a fresh instance |

---

## 10. Open items

All open items are resolved.

| ID | Item | Status | Resolution |
|---|---|---|---|
| SPID-O1 | SPI3 baud rate divisor | **Resolved** | PCLK1 = 80 MHz (CPU module companion). BR[2:0] = 010 → 80/8 = 10 MHz. Under 20 MHz ISM43362 limit with 2× margin. |
| SPID-O2 | SPI3 AF numbers and pin assignment | **Resolved** | UM2153 Table 11 confirms PC10=SCK, PC11=MISO, PC12=MOSI, all AF6. |

---

## 11. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| SPID-D1 | Single `spi_transceive` covering write-only, read-only, and full-duplex via NULL pointer convention | SPI is inherently full-duplex; separate write/read functions would obscure hardware reality and add no clarity for the single consumer |
| SPID-D2 | NSS not managed by SpiDriver | `WifiDriver USES SpiDriver, GpioDriver` independently per `components.md`; NSS timing is part of the AT command protocol (WifiDriver's concern), not the byte-transfer concern |
| SPID-D3 | Polling, not interrupt or DMA | `WifiTask` is dedicated to WiFi I/O; blocking the task during a transfer is the correct and simplest model. DMA adds complexity with no benefit for the transfer sizes involved in AT command exchanges |
| SPID-D4 | FRXTH=0 mandated in `spi_create` for correct 16-bit operation | Required L4-specific configuration: with DS=1111 (16-bit frames), FRXTH must be 0 so RXNE fires after a full 16-bit word. FRXTH=1 with 16-bit frames causes premature RXNE after 8 bits, producing corrupted reads. |
| SPID-D5 | BSY wait after last word before returning | Ensures the caller (WifiDriver) can safely de-assert NSS immediately after `spi_transceive` returns without truncating the last clock cycle |
| SPID-D6 | ADT pattern (opaque handle, static pool of 1) | Gateway default. Replaces the singleton pattern from the v0.1 draft. Pool size 1 matches the hardware (single SPI3); the pattern is applied for API consistency and to demonstrate the technique. |
| SPID-D7 | 10 MHz SPI clock (BR=010, PCLK1/8) | ISM43362 supports up to 20 MHz; 10 MHz provides 2× safety margin against signal integrity issues on the Discovery board's PCB traces |
| SPID-D8 | P2 not applicable | Single consumer (WifiDriver) at the same driver layer. No cross-layer inversion involved. The ISpi interface in components.md is the contract, but DIP as an architectural pattern does not apply here. |

---

## 12. File layout

```
firmware/gateway/drivers/spi_driver/
├── spi_driver.h       /* public API — opaque handle, config, error enum */
└── spi_driver.c       /* implementation — internal struct, static pool  */

tests/gateway/drivers/spi_driver/
└── test_spi_driver.c  /* Unity + CMock host tests                       */
```

---

## Phase H — Readiness review

| # | Check | Status |
|---|---|---|
| H1 | PROVIDES / USES match `components.md` exactly | PASS — PROVIDES ISpi, USES CMSIS |
| H2 | Root SRS requirement (CON-001) cited and quoted | PASS |
| H3 | All public API functions have complete Doxygen (brief, param, return, note) | PASS |
| H4 | ADT pattern applied (or exception documented) | PASS — ADT with pool of 1; SPID-D6 documents rationale |
| H5 | Error enum covers all failure modes; no silent failures | PASS — timeout, null pointer, no resource |
| H6 | Hardware contract specifies all register fields, pin assignments, AF numbers | PASS — §4.1–4.5; all derived from UM2153 and RM0351 |
| H7 | All open items resolved or have a named owner and resolution path | PASS — SPID-O1 and SPID-O2 both resolved |
| H8 | Unit-test plan covers happy path + error cases for every public function | PASS — 14 test cases, covers create, transceive, timeout, null, pool exhaustion |
| H9 | Test file path follows Gateway folder convention | PASS — `tests/gateway/drivers/spi_driver/test_spi_driver.c` |
| H10 | P1–P10 compliance reviewed; inapplicable principles noted | PASS — §7 |
| H11 | No FreeRTOS dependency from driver layer (unless USES permits) | PASS — no FreeRTOS headers |
| H12 | Thread safety documented with task-context analysis | PASS — §8; caller serialises, single-task access by construction |
| H13 | `reset_for_test` hook specified | PASS — §3.5 |
| H14 | Decisions log complete with rationale for each choice | PASS — 8 decisions logged |

**Verdict: PASS — ready for implementation.**
