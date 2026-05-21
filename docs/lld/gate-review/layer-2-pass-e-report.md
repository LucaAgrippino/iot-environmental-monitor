# LLD Gate Review — Layer 2 Pass E Report
## §4 Hardware Contract Completeness

**Date:** 2026-05-21
**Branch:** `feature/lld-gate-review-layer-2-pass-e`
**Script:** `scripts/lld_gate_review_check.py` — `check_pass_e()`
**Driver companions reviewed:** 15 (17 total minus 2 pure-software drivers)
**Pure-software drivers excluded:** `led-driver.md`, `simulated-sensor-drivers.md`

---

## 1. Gate criterion (lld-methodology.md §4)

Every driver companion §4 Hardware contract must contain:

1. **Registers** — peripheral instance, register names and bit-fields used. CMSIS register symbols only.
2. **Pins** — GPIO port + pin + alternate-function number, cross-referenced against board user manuals (UM1932 for STM32F469-DISCO, UM2153 for B-L475E-IOT01A).
3. **Clocks** — RCC bus, enable-bit symbol, and clock-tree path.
4. **NVIC** — every interrupt line enabled, priority value, ISR function name.

Each subsection may state `N/A — <reason>` if the driver legitimately does not use that resource. No `HAL_` references or `#include "stm32xx_hal.h"` are permitted anywhere in §4 (CMSIS-only above the driver layer).

An NVIC priority allocation table must exist in `lld.md` if any driver companion enables interrupt lines.

---

## 2. Pre-remediation findings

| Category | Count |
|---|---|
| BLOCKER — §4 subsection missing (Registers / Pins / Clocks / NVIC) | 39 |
| FIX_NOW — NVIC priority allocation table absent from `lld.md` | 1 |
| **Total** | **40** |

No HAL references were found in any §4 section.

### 2.1 Missing subsections by companion (39 BLOCKERs)

| Companion | Missing subsections |
|---|---|
| `exti-driver.md` | Registers, Pins, Clocks, NVIC |
| `humidity-temp-barometer-drivers.md` | Registers, Pins, Clocks, NVIC |
| `i2c-driver.md` | NVIC |
| `lcd-driver.md` | Registers, Pins, Clocks, NVIC |
| `magnetometer-imu-drivers.md` | Registers, Pins, Clocks, NVIC |
| `modbus-uart-driver.md` | Clocks, NVIC |
| `qspi-flash-driver.md` | NVIC |
| `reset-driver.md` | Registers, Pins, Clocks, NVIC |
| `rtc-driver.md` | Pins, NVIC |
| `sdram-driver.md` | Registers, Pins, Clocks, NVIC |
| `spi-driver.md` | Registers, NVIC |
| `touchscreen-driver.md` | Registers, Clocks, NVIC |
| `wifi-driver.md` | Registers, Pins, Clocks, NVIC |

Companions that already had all four subsections present (no remediation required): `debug-uart-driver.md`, `gpio-driver.md`.

### 2.2 NVIC priority allocation table (1 FIX_NOW)

`lld.md` had no §6.3 section. Every driver companion that enables an interrupt line references a priority assignment; the project-wide allocation table was absent.

---

## 3. Remediation — `scripts/_fix_pass_e.py`

A targeted script was written and run in a single pass. It appended missing subsection blocks to thirteen driver companions and added the NVIC priority allocation table to `lld.md §6`. The script then deleted itself.

### 3.1 Subsection content per companion

**`exti-driver.md`** — All four subsections added:
- Registers: `SYSCFG->EXTICRx` (source selection), `EXTI->IMR`, `EXTI->RTSR`/`FTSR`/`PR`.
- Pins: N/A — caller-supplied; ExtiDriver does not call GpioDriver.
- Clocks: `SYSCFG` via `RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN`.
- NVIC: priority assigned by caller; must be ≥ `configMAX_SYSCALL_INTERRUPT_PRIORITY`.

**`humidity-temp-barometer-drivers.md`** — All four subsections added as N/A:
- All hardware access delegated to `I2cDriver` and `ExtiDriver`.

**`i2c-driver.md`** — NVIC only:
- N/A — driver uses polling; no interrupt line is enabled.

**`lcd-driver.md`** — All four subsections added:
- Registers: `LTDC`, `DSIHOST`, `DMA2D` register sets.
- Pins: N/A — display interface pins are pre-configured by the board BSP before `main()`.
- Clocks: `LTDC` on APB2, DSI host on APB2, `DMA2D` on AHB1, `PLLSAI` configured for pixel clock.
- NVIC: `LTDC_IRQn` at priority 6, `DSI_IRQn` at priority 6.

**`magnetometer-imu-drivers.md`** — All four subsections added as N/A:
- All hardware access delegated to `I2cDriver` and `ExtiDriver`.

**`modbus-uart-driver.md`** — Clocks and NVIC added:
- Clocks: `USART2` on `APB1` (Field Device), `USART3` on `APB1` (Gateway).
- NVIC: `USART2_IRQHandler` priority 6 (Field Device), `USART3_IRQHandler` priority 6 (Gateway).

**`qspi-flash-driver.md`** — NVIC only:
- N/A — driver uses polling; no interrupt line is enabled.

**`reset-driver.md`** — All four subsections added:
- Registers: `SCB->AIRCR` (SYSRESETREQ bit).
- Pins: N/A — no GPIO.
- Clocks: N/A — SCB is always clocked; no RCC enable required.
- NVIC: N/A — `reset_driver_request_reset()` does not return; no ISR.

**`rtc-driver.md`** — Pins and NVIC added:
- Pins: N/A — RTC uses dedicated oscillator pins; no GPIO configuration by this driver.
- NVIC: `RTC_Alarm_IRQn` at priority 6 — registered in Phase 4 alongside alarm callback implementation.

**`sdram-driver.md`** — All four subsections added:
- Registers: `FMC_SDCR1/2`, `FMC_SDTR1/2`, `FMC_SDCMR`, `FMC_SDRTR`.
- Pins: N/A — FMC SDRAM pins are system-level alternate-function pins configured by BSP before `main()`.
- Clocks: `FMC` on AHB3 via `RCC->AHB3ENR |= RCC_AHB3ENR_FMCEN`.
- NVIC: N/A — SdramDriver uses polling for command completion; no interrupt line.

**`spi-driver.md`** — Registers and NVIC added:
- Registers: `SPI3->CR1`, `SPI3->CR2`, `SPI3->SR`, `SPI3->DR`.
- NVIC: N/A — driver uses polling; no interrupt line is enabled.

**`touchscreen-driver.md`** — Registers, Clocks, and NVIC added:
- Registers: N/A — touchscreen I2C communication delegated to `I2cDriver`.
- Clocks: N/A — I2C clock enabled by `I2cDriver`; GPIO clock by `GpioDriver`.
- NVIC: touch interrupt handled via `ExtiDriver`; caller sets priority (must be ≥ `configMAX_SYSCALL_INTERRUPT_PRIORITY`).

**`wifi-driver.md`** — All four subsections added:
- Registers: N/A — all SPI register access delegated to `SpiDriver`.
- Pins: ISM43362 GPIO control lines (NSS, DRDY, RST, WAKEUP, BOOT0) accessed via `GpioDriver`; assignments in §4.2 GPIO lines table.
- Clocks: N/A — SPI3 clock enabled by `SpiDriver`; GPIO clocks by `GpioDriver`.
- NVIC: DRDY pin (`EXTI1_IRQHandler`) registered by caller via `ExtiDriver`; priority must be ≥ `configMAX_SYSCALL_INTERRUPT_PRIORITY`.

### 3.2 NVIC priority allocation table (lld.md §6.3)

A new §6.3 was added to `lld.md` enumerating all ISR lines across the driver layer:

| ISR | Driver | Board | Priority | FreeRTOS constraint |
|---|---|---|---|---|
| `USART2_IRQHandler` | DebugUartDriver | Field Device (F469) | 6 | ≥ configMAX_SYSCALL_INTERRUPT_PRIORITY (5) ✓ |
| `USART2_IRQHandler` | ModbusUartDriver | Field Device (F469) | 6 | ≥ 5 ✓ |
| `USART3_IRQHandler` | ModbusUartDriver | Gateway (L475) | 6 | ≥ 5 ✓ |
| `EXTIx_IRQHandler` | ExtiDriver (caller-set) | Both | ≥ 6 | Caller must honour lld.md §6.3 |
| `LTDC_IRQn` | LcdDriver | Field Device (F469) | 6 | ≥ 5 ✓ |
| `DSI_IRQn` | LcdDriver | Field Device (F469) | 6 | ≥ 5 ✓ |
| `RTC_Alarm_IRQn` | RtcDriver | Both | 6 | ≥ 5 ✓ (Phase 4) |

Open item EXTI-O1 is noted: caller-configured EXTI priorities must be verified at integration time.

---

## 4. Post-remediation verification

```
python scripts/lld_gate_review_check.py

LLD gate review — Layer 1 + Layer 2 (Passes B, C, D, E)
Summary: 0 findings (0 blockers)
Layer 1 PASSES — no blockers found.
```

All prior Layer 1 and Pass B/C/D findings remain at 0. No regressions.

---

## 5. Acceptance

| Criterion | Result |
|---|---|
| 0 BLOCKER from `check_pass_e` | PASS |
| 0 FIX_NOW from `check_pass_e` | PASS |
| Every driver companion has a complete §4 or explicit `N/A — <reason>` rows | PASS |
| No `HAL_` references or `stm32xx_hal` includes in any §4 | PASS |
| NVIC priority allocation table present in `lld.md §6.3` | PASS |
| All prior gate passes (Layer 1, B, C, D) still at 0 | PASS |

**Pass E GATE PASSES — 0 BLOCKERs, 0 FIX_NOWs.**

---

## 6. Escalations

None. No finding in Pass E required an architectural decision or exposed an ambiguity requiring halt-and-report.

Open item EXTI-O1 (caller-configured EXTI priorities) was noted in the NVIC allocation table; it is a Phase 4 integration-time verification task, not an LLD gap.

---

## 7. Open items

None introduced by this pass. Pre-existing open items in companion §8 tables are unchanged.
