# SdramDriver — LLD Companion

**Document:** `docs/lld/drivers/sdram-driver.md`
**Version:** 0.1 (draft — pending Luca review)
**Board scope:** Field Device (STM32F469) only
**Layer:** Driver
**Status:** Draft

---

## 1. Source summary

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Initialises the FMC controller and exposes external SDRAM as memory-mapped space for the LCD framebuffer | `components.md` |
| PROVIDES (upward) | `ISdram` | `components.md` |
| USES (downward) | CMSIS | `components.md` |
| Root requirement | REQ-LD-010 | `components.md` |
| Hardware | FMC peripheral + 128 Mbit (16 MB) external SDRAM | UM1932 |

**Consumer:** `LcdDriver` only. Called once at boot (BringingUpLCD Init sub-step) to obtain the framebuffer base address.

**No sequence diagram surface.** SdramDriver is infrastructure for LcdDriver; it has no HLD-level lifeline.

---

## 2. API

### 2.1 Dependency-conformance check

`sdram_driver.h` includes only `stm32f469xx.h` and `stdint.h`. No FreeRTOS. Confirmed clean.

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
 * Configures FMC bank 5 or 6 (board-specific — SDRD-O1), sets SDCR and
 * SDTR timing registers, issues the initialisation command sequence
 * (clock enable, precharge all, two auto-refresh cycles, mode register
 * set), and configures the auto-refresh timer (SDRTR).
 *
 * After this call, the SDRAM address space is accessible as memory-mapped
 * RAM. The caller (LcdDriver) may then obtain the framebuffer base address
 * via sdram_get_base_addr().
 *
 * Must be called once from main() before lcd_init(). No FreeRTOS.
 *
 * @return SDRAM_ERR_OK on success; SDRAM_ERR_TIMEOUT if the FMC
 *         status register BUSY flag does not clear.
 */
sdram_err_t sdram_init(void);

/**
 * @brief Return the memory-mapped base address of the SDRAM.
 *
 * Returns the constant base address of the FMC bank mapped to the
 * SDRAM device. LcdDriver uses this to locate the framebuffer.
 *
 * Must only be called after a successful sdram_init().
 *
 * @return Base address of the SDRAM (e.g. 0xC000_0000 for FMC bank 6).
 */
uint32_t sdram_get_base_addr(void);
```

---

## 3. Internal design

### 3.1 Module-level state

```c
static bool s_initialised = false;
```

No dynamic allocation. The SDRAM base address is a compile-time constant derived from the FMC bank assignment (SDRD-O1).

### 3.2 FMC SDRAM init sequence

The STM32F469 FMC SDRAM initialisation follows a mandatory hardware sequence (RM0386 §37.7.3):

```
1. Program SDCR (column/row bits, bus width, CAS latency, internal banks,
   SDCLK period, read-pipe delay, burst read).
2. Program SDTR (TMRD, TXSR, TRAS, TRC, TWR, TRP, TRCD timing values).
3. Issue CLK_ENABLE command via FMC_SDCMR.
4. Wait ≥ 100 µs (SDRAM power-on delay).
5. Issue PALL (Precharge All) command.
6. Issue two AUTO_REFRESH commands.
7. Issue LOAD_MODE_REGISTER command (CAS latency, burst length).
8. Program FMC_SDRTR auto-refresh period.
```

Timing values depend on the SDRAM device (IS42S16400J or equivalent) and the FMC clock. Tracked as **SDRD-O1** and **SDRD-O2** (§8).

### 3.3 No ISR, no DMA

SdramDriver is purely a hardware initialisation driver. Once init is complete, the SDRAM appears as memory-mapped RAM — no driver involvement in reads or writes.

---

## 4. Hardware contract

### 4.1 SDRAM device

128 Mbit = 16 MB external SDRAM (UM1932 hardware sweep). Device likely IS42S16400J or equivalent (verify against the UM1932 BOM). Key parameters needed for FMC timing: CAS latency, row/column address bits, tRCD, tRP, tRC, tRAS, tXSR, tMRD, tWR.

### 4.2 FMC bank assignment (open item — SDRD-O1)

The FMC on STM32F469 maps SDRAM to bank 5 (`0xC000_0000`) or bank 6 (`0xD000_0000`). Verify against UM1932 schematic which bank the SDRAM is connected to.

### 4.3 Timing parameters (open item — SDRD-O2)

SDCR and SDTR register values are derived from the SDRAM device datasheet timing parameters and the FMC clock (`HCLK / 2` or `HCLK / 3` as configured in SDCR.SDCLK). Cannot be finalised until the system clock tree is fixed. Shares the same root dependency as DUART-O2.

### 4.4 Framebuffer size check

At 800×480 pixels with RGB565 (2 bytes/pixel), the framebuffer is 800 × 480 × 2 = 768,000 bytes = 750 KB. The 16 MB SDRAM has ample headroom. Double-buffering (two framebuffers for tear-free display) would require 1.5 MB — still well within 16 MB. This decision is deferred to LcdDriver.

---

## 5. Sequence integration

None. SdramDriver is not a sequence diagram participant.

---

## 6. Error handling

On `SDRAM_ERR_TIMEOUT`, the BringingUpLCD Init sub-state fails and the FD enters Faulted (`state-machines.md` §3.1, failure handling column). The LCD is essential (REQ-LD-000); SDRAM initialisation failure is non-recoverable at boot.

---

## 7. Test plan

Host-platform testing is limited — the FMC peripheral cannot be realistically mocked for a full timing sequence. The primary test strategy is integration testing on hardware.

| ID | Test case | Expected result |
|---|---|---|
| T-SDRAM-01 | `sdram_init` with mocked FMC BUSY clearing normally | Returns SDRAM_ERR_OK; s_initialised = true |
| T-SDRAM-02 | `sdram_init` with BUSY stuck | Returns SDRAM_ERR_TIMEOUT |
| T-SDRAM-03 | `sdram_get_base_addr` returns expected constant | Returns the correct FMC bank base address (SDRD-O1) |
| T-SDRAM-04 | Integration: write pattern to SDRAM, read back | Data integrity verified on hardware |

Test file: `tests/drivers/test_sdram_driver.c`.

---

## 8. Open items

| ID | Item | Owner | Resolution path |
|---|---|---|---|
| SDRD-O1 | FMC bank assignment (bank 5 or 6). Verify UM1932 schematic — which SDRAM signals connect to which FMC bank pins. | Luca | Check UM1932 schematic at implementation |
| SDRD-O2 | SDCR and SDTR timing register values. Derive from SDRAM device datasheet × FMC clock frequency. Depends on clock-config.md. | Luca | Resolve when `clock-config.md` lands |
| SDRD-O3 | SDRAM device identity. Confirm part number from UM1932 BOM; download datasheet for timing parameters. | Luca | Check UM1932 BOM |

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| SDRD-D1 | `sdram_get_base_addr()` returns a constant, not a pointer to allocated memory | The SDRAM is memory-mapped hardware; there is nothing to allocate. LcdDriver uses the base address to place its framebuffer |
| SDRD-D2 | Singleton — no handle | One FMC, one SDRAM bank; consistent with all prior companions |
| SDRD-D3 | Driver exposes only init + base address | P1: the driver's job is hardware initialisation. Framebuffer management (offset, size, double-buffering) belongs in LcdDriver |
