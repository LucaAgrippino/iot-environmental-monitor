# SdramDriver — LLD Companion

**Document:** `docs/lld/companions/sdram-driver.md`
**Version:** 1.0
**Board scope:** Field Device (STM32F469) only
**Layer:** Driver
**Status:** Pass H — Implementation ready
**Date:** June 2026

**HLD anchor:** SdramDriver in `components.md` (FD driver layer)

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Initialises the FMC controller and exposes external SDRAM as memory-mapped space for the LCD framebuffer | `components.md` |
| PROVIDES (upward) | `ISdram` | `components.md` |
| USES (downward) | CMSIS | `components.md` |
| Root requirement | REQ-LD-010 | `components.md` |
| Hardware | FMC peripheral + 128 Mbit (16 MB) external SDRAM (ISSI IS42S32400F-6BL) | UM1932 §4.9 |

**Consumer:** `LcdDriver` only. Called once at boot (BringingUpLCD Init
sub-step) to obtain the framebuffer base address.

**No sequence diagram surface.** SdramDriver is infrastructure for
LcdDriver; it has no HLD-level lifeline.

---

## 2. Public API

### 2.1 Dependency-conformance check

`sdram_driver.h` includes only `stm32f469xx.h` and `stdint.h`. No
FreeRTOS. Confirmed clean.

### 2.2 Data types

```c
typedef enum {
    SDRAM_ERR_OK      = 0,
    SDRAM_ERR_TIMEOUT = 1, /**< FMC BUSY flag did not clear within timeout. */
} sdram_err_t;
```

### 2.3 Public API (`sdram_driver.h`)

```c
/**
 * @brief Initialise the FMC controller for SDRAM.
 *
 * Configures FMC SDRAM Bank 1 (SDNE0), sets SDCR and SDTR timing
 * registers, issues the initialisation command sequence (clock enable,
 * precharge all, two auto-refresh cycles, mode register set), and
 * configures the auto-refresh timer (SDRTR).
 *
 * After this call, the SDRAM address space at 0xC000_0000 is accessible
 * as memory-mapped RAM. The caller (LcdDriver) may then obtain the
 * framebuffer base address via sdram_get_base_addr().
 *
 * Must be called once from main() before lcd_init(). No FreeRTOS.
 *
 * @return SDRAM_ERR_OK on success; SDRAM_ERR_TIMEOUT if the FMC
 *         status register BUSY flag does not clear.
 * @note   Threading: task-context only, non-blocking. Must be called
 *         before the scheduler starts.
 */
sdram_err_t sdram_init(void);

/**
 * @brief Return the memory-mapped base address of the SDRAM.
 *
 * Returns the constant base address 0xC000_0000 (FMC SDRAM Bank 1,
 * selected by SDNE0). LcdDriver uses this to locate the framebuffer.
 *
 * Must only be called after a successful sdram_init().
 *
 * @return 0xC000_0000.
 * @note   Threading: task-context only, non-blocking. Not ISR-safe.
 */
uint32_t sdram_get_base_addr(void);
```

---

## 3. Internal design

### 3.1 Module-level state

```c
static bool s_initialised = false;
```

No dynamic allocation. The SDRAM base address is a compile-time constant.

### 3.2 FMC SDRAM init sequence

The STM32F469 FMC SDRAM initialisation follows a mandatory hardware
sequence (RM0386 §37.7.3):

```
1. Enable FMC clock:  RCC->AHB3ENR |= RCC_AHB3ENR_FMCEN.
2. Program SDCR1 (column/row bits, bus width, CAS latency, internal
   banks, SDCLK period, read-pipe delay, burst read).
3. Program SDTR1 (TMRD, TXSR, TRAS, TRC, TWR, TRP, TRCD timing values).
4. Issue CLK_ENABLE command via FMC_SDCMR. Wait BUSY clear.
5. Wait ≥ 100 µs (SDRAM power-on delay).
6. Issue PALL (Precharge All) command. Wait BUSY clear.
7. Issue two AUTO_REFRESH commands. Wait BUSY clear between each.
8. Issue LOAD_MODE_REGISTER command (CAS latency = 3, burst length = 1,
   burst type = sequential, write-burst mode = single). Wait BUSY clear.
9. Program FMC_SDRTR auto-refresh period (COUNT field).
```

### 3.3 Register values (HCLK = 180 MHz, SDCLK = HCLK/2 = 90 MHz)

Derived from ST's reference BSP for the F469 Discovery
(`stm32469i_discovery_sdram.c`). Timing values are valid for the
IS42S32400F-6BL at 90 MHz SDCLK; validated on hardware at integration.

**SDCR1:**

| Field | Value | Meaning |
|---|---|---|
| `NC` (column bits)  | `0b00` | 8 columns *(IS42S32400F has 256 = 8 bit columns)* |
| `NR` (row bits)     | `0b01` | 12 rows |
| `MWID` (data width) | `0b10` | 32-bit |
| `NB` (banks)        | `1`    | 4 internal banks |
| `CAS` (CAS latency) | `0b11` | 3 cycles |
| `WP` (write prot.)  | `0`    | disabled |
| `SDCLK`             | `0b10` | HCLK / 2 = 90 MHz |
| `RBURST`            | `1`    | burst read enabled |
| `RPIPE`             | `0b00` | no read pipe delay |

**SDTR1 (timing — 90 MHz, t<sub>CK</sub> ≈ 11.1 ns):**

| Field | Value (cycles - 1) | Spec |
|---|---|---|
| `TMRD` | `1` | 2 cycles |
| `TXSR` | `6` | 7 cycles (≥ 67 ns) |
| `TRAS` | `3` | 4 cycles (≥ 42 ns) |
| `TRC`  | `5` | 6 cycles (≥ 60 ns) |
| `TWR`  | `1` | 2 cycles |
| `TRP`  | `1` | 2 cycles (≥ 18 ns) |
| `TRCD` | `1` | 2 cycles (≥ 18 ns) |

**SDRTR (refresh rate):**
`COUNT = (SDRAM refresh period × SDCLK) / rows - 20`
= `(64 ms × 90 MHz) / 4096 - 20`
≈ `1386`

Concrete values are declared as `#define`s in `sdram_driver.c` so they
can be reviewed without reading the SDCR/SDTR bit layouts.

### 3.4 No ISR, no DMA

SdramDriver is purely a hardware initialisation driver. Once init is
complete, the SDRAM appears as memory-mapped RAM — no driver
involvement in reads or writes.

### 3.5 Principles applied

- **P1 (Strict directional layering).** Depends only on CMSIS FMC
  peripheral headers; no RTOS, no middleware.
- **P5 (Bounded resources, no dynamic allocation post-init).** After
  `sdram_init()`, the SDRAM region is available as flat memory; no
  internal state struct is maintained post-init.
- **P6 (Responsibility traces to requirements).** `sdram_init()` traces
  to REQ-LD-010 (provide framebuffer storage for the LCD).
- **P8 (Total error propagation, no silent failures).** Returns
  `sdram_err_t`; initialisation timeout triggers an error return.
- **P9 (BARR-C coding standard).** Register values expressed as
  `uint32_t` constants; no floating-point.
- **P10 (Naming conventions).** Prefix `sdram_`; errors `SDRAM_ERR_*`.

### 3.6 Synchronisation

Caller serialises. The driver holds no FreeRTOS synchronisation
primitives. Called from `main()` before the scheduler starts.

### 3.7 Per-function notes

**`sdram_init`**
Pre-condition: not previously called. Configures FMC, runs the init
sequence in §3.2, sets `s_initialised = true` on success. Each FMC
command waits the BUSY flag clear with a bounded loop; if the loop
exhausts its iteration count, the function returns `SDRAM_ERR_TIMEOUT`
and leaves `s_initialised` false. The FMC peripheral may be left in an
indeterminate state on timeout — the caller treats this as a
non-recoverable boot fault.

**`sdram_get_base_addr`**
Returns the constant `0xC000_0000UL`. No state read, no error path.

---

## 4. Hardware contract

### 4.1 SDRAM device

128 Mbit = 16 MB external SDRAM, **ISSI IS42S32400F-6BL** (confirmed via
UM1932 §4.9 and BOM). Organisation: 4 banks × 4096 rows × 256 columns ×
32 bits. CAS latency 3 at 90 MHz SDCLK. 32-bit data bus.

### 4.2 FMC bank assignment

FMC SDRAM **Bank 1** (SDNE0). Base address `0xC000_0000`, size 16 MB
(`0xC000_0000`–`0xC0FF_FFFF`). Confirmed via UM1932 §4.9.

### 4.3 Timing parameters

Derived from ST's reference BSP for the F469 Discovery and the
IS42S32400F datasheet at HCLK = 180 MHz, SDCLK = HCLK/2 = 90 MHz.
Concrete SDCR1 / SDTR1 / SDRTR values listed in §3.3 above. Validated
at integration on hardware (TC-SDRAM-005).

### 4.4 Framebuffer size check

At 800 × 480 pixels (F469 Discovery LCD) with RGB565 (2 B/px):
800 × 480 × 2 = 768 000 B ≈ 750 KB. The 16 MB SDRAM has ample
headroom. Double-buffering (two framebuffers for tear-free display)
needs 1.5 MB — still well within 16 MB. This decision belongs to
LcdDriver.

### 4.5 Registers

| Peripheral | Key registers | Purpose |
|---|---|---|
| `FMC` (AHB3) | `FMC_SDCR1` | SDRAM control: column/row bits, bus width, CAS latency, clock period. |
| `FMC` | `FMC_SDTR1` | SDRAM timing: TMRD, TXSR, TRAS, TRC, TWR, TRP, TRCD. |
| `FMC` | `FMC_SDCMR` | Command mode register — issues CLK_ENABLE, PALL, AUTO_REFRESH, LOAD_MODE. |
| `FMC` | `FMC_SDSR` | Status register — BUSY flag polled during init. |
| `FMC` | `FMC_SDRTR` | Auto-refresh timer period. |

### 4.6 Pins

N/A — FMC SDRAM pins (address, data, control lines) are
alternate-function pins across multiple GPIO ports configured by the
board support initialisation before `main()`. SdramDriver does not call
GpioDriver; these are system-level pins outside the firmware driver
scope.

### 4.7 Clocks

`FMC` clock: `RCC->AHB3ENR |= RCC_AHB3ENR_FMCEN`. Enabled in
`sdram_init()`. FMC runs on AHB3 (same as core HCLK at 180 MHz on
F469). SDCLK = HCLK/2 = 90 MHz per SDCR1.SDCLK = 0b10.

### 4.8 NVIC

N/A — the FMC SDRAM interrupt (`FMC_IRQn`) is not enabled. SDRAM
appears as flat memory after init.

---

## 5. Sequence integration

None. SdramDriver is not a sequence diagram participant.

---

## 6. Error and fault behaviour

All public functions return `sdram_err_t`; callers must not ignore
non-OK returns. No retry is performed — `LifecycleController` treats
SDRAM init failure as a non-recoverable boot fault.

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `SDRAM_ERR_TIMEOUT` | FMC BUSY flag did not clear within the bounded wait | Return error; FMC peripheral left in an indeterminate state | Non-OK return | No retry — BringingUpLCD Init sub-state fails and the device enters Faulted (LCD and SDRAM are essential per REQ-LD-000) | LifecycleController logs at ERROR via ILogger; system cannot reach Operational |

---

## 7. Unit-test plan

Host-platform testing is limited — the FMC peripheral cannot be
realistically mocked for a full timing sequence. Primary verification
is integration on hardware.

Test file: `tests/field-device/drivers/sdram_driver/test_sdram_driver.c`.

| Test ID | Description | Verifies |
|---|---|---|
| TC-SDRAM-001 | `sdram_init()` with FMC BUSY clearing immediately (mocked) → `SDRAM_ERR_OK`; `s_initialised == true` | Init happy path |
| TC-SDRAM-002 | `sdram_init()` with FMC BUSY stuck high → `SDRAM_ERR_TIMEOUT`; `s_initialised == false` | Timeout path |
| TC-SDRAM-003 | `sdram_get_base_addr()` returns `0xC000_0000` | Constant return |
| TC-SDRAM-004 | `sdram_init()` called twice in a row → second call still returns `SDRAM_ERR_OK` (idempotent re-init) **or** `SDRAM_ERR_OK` and skips re-init (implementer's choice — document the chosen behaviour) | Re-init semantics |

### 7.1 Integration tests — on target

| Test ID | Description | Verification |
|---|---|---|
| TC-SDRAM-005 | March test: write `i ^ 0xA5A5A5A5` to every 4-byte word in the 16 MB region, then read back. Repeat with `i ^ 0x5A5A5A5A`. | Full address-line and data-line coverage; passes only if FMC timing and bank selection are correct |
| TC-SDRAM-006 | Aged-data test: write fixed pattern across 1 MB, sleep 200 ms, read back. | Verifies auto-refresh is running (SDRTR programmed correctly) |
| TC-SDRAM-007 | Random spot-check at 1024 random addresses, write-read-verify with varying bit patterns. | Detects stuck-at faults and addressing aliases |

---

## 8. Open items

| ID | Item | Resolution | Status |
|---|---|---|---|
| SDRD-O1 | FMC bank assignment. | FMC SDRAM Bank 1 (SDNE0), base `0xC000_0000`, size 16 MB. Confirmed via UM1932 §4.9. | Resolved |
| SDRD-O2 | SDCR / SDTR / SDRTR register values. | Derived from ST F469 Discovery BSP at HCLK 180 MHz / SDCLK 90 MHz. Concrete values in §3.3. Validated at integration via TC-SDRAM-005..007. | Resolved (post-code validation via integration tests) |
| SDRD-O3 | SDRAM device identity. | ISSI IS42S32400F-6BL (UM1932 §4.9 and BOM). | Resolved |

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| SDRD-D1 | `sdram_get_base_addr()` returns a constant, not a pointer to allocated memory | The SDRAM is memory-mapped hardware; there is nothing to allocate. LcdDriver uses the base address to place its framebuffer. |
| SDRD-D2 | Singleton — no handle | One FMC, one SDRAM bank; consistent with all prior driver companions. |
| SDRD-D3 | Driver exposes only init + base address | P1: the driver's job is hardware initialisation. Framebuffer management (offset, size, double-buffering) belongs in LcdDriver. |
| SDRD-D4 | Use ST BSP reference timing values rather than compute from datasheet | The BSP is field-proven on this exact board and SDRAM device. Recomputing from the datasheet adds risk for no gain at this clock. If the system clock changes, recompute. |
