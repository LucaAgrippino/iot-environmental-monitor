# LLD Companion — ExtiDriver

**Board:** Both (Field Device + Gateway)  
**Branch:** `feature/lld-exti-driver`  
**Status:** Draft  
**Methodology:** lld-methodology.md v1.1, steps 1–8  

---

## 1. Scope and rationale

ExtiDriver is the sole owner of the EXTI and SYSCFG_EXTICRx peripheral
configuration on both boards. It was identified during the WifiDriver
companion (WIFI-O2 root) as a missing driver required to prevent a class
of defects.

**The concrete problem it solves:**

The `SYSCFG_EXTICRx` registers map GPIO ports to EXTI lines. Each register
is 32 bits wide and covers four EXTI lines (4 bits each). Without a central
owner, every driver that needs an EXTI interrupt would read-modify-write the
same shared register in isolation — an error-prone pattern that is hard to
audit. ExtiDriver provides a single, conflict-detecting entry point for all
EXTI line configuration on both boards.

**Consumers:**

| Board | Driver | EXTI line | Trigger |
|-------|--------|-----------|---------|
| GW | WifiDriver | EXTI1 (ISM43362 DRDY) | Rising |
| GW | MagnetometerDriver | EXTI8 (LIS3MDL DRDY) | Rising |
| GW | ImuDriver | EXTI11 (LSM6DSL INT1) | Rising |
| FD | TouchscreenDriver | EXTIx (FT6206 IRQ) | Falling |

**P6 traceability:** ExtiDriver has no dedicated SRS requirement. It traces
through its consumers: REQ-SA-031, REQ-SA-071, REQ-CC-050, REQ-LD-050.
Infrastructure drivers that serve only as a shared-resource guard are an
accepted exception to P6's "one component per use case" guidance, provided
the need is demonstrated (it is — see above).

---

## 2. Source references (Step 1)

| Source | Relevant section |
|--------|-----------------|
| `components.md` | GpioDriver entry (peer pattern); ExtiDriver added — see §9 |
| `wifi-driver.md` | WIFI-O2 (origin of this driver) |
| `magnetometer-imu-drivers.md` | §4.2 Phase 1 EXTI config; GPB-O3 |
| `humidity-temp-barometer-drivers.md` | §5.1/5.2 DRDY pins (not used at firmware level) |
| `touchscreen-driver.md` | Phase 1 EXTI GPIO config |
| RM0351 §13 (STM32L475) | SYSCFG_EXTICRx, EXTI registers |
| RM0386 §10 (STM32F469) | SYSCFG_EXTICRx, EXTI registers |
| `stm32l475xx.h`, `stm32f469xx.h` | EXTI_TypeDef, SYSCFG_TypeDef register definitions |

---

## 3. API — Step 2

```c
/* exti_driver.h */

#ifndef EXTI_DRIVER_H
#define EXTI_DRIVER_H

#include <stdint.h>

typedef enum {
    EXTI_ERR_OK          = 0,
    EXTI_ERR_INVALID_ARG = 1,   /* line > 15 or invalid port          */
    EXTI_ERR_CONFLICT    = 2    /* line already configured             */
} exti_err_t;

typedef enum {
    EXTI_PORT_A = 0U,
    EXTI_PORT_B = 1U,
    EXTI_PORT_C = 2U,
    EXTI_PORT_D = 3U,
    EXTI_PORT_E = 4U,
    EXTI_PORT_H = 7U    /* PH present on STM32L475 */
} exti_port_t;

typedef enum {
    EXTI_EDGE_RISING  = 0U,
    EXTI_EDGE_FALLING = 1U,
    EXTI_EDGE_BOTH    = 2U
} exti_edge_t;

/*
 * Configure SYSCFG_EXTICRx port mapping and EXTI trigger edge.
 * Does NOT enable the interrupt (does not touch IMR or NVIC).
 * Returns EXTI_ERR_CONFLICT if the line is already configured.
 * Call from Phase 1 (pre-scheduler).
 */
exti_err_t exti_configure(uint8_t line, exti_port_t port, exti_edge_t edge);

/*
 * Enable the EXTI interrupt: sets IMR bit and configures NVIC priority.
 * Caller must have called exti_configure() for this line first.
 * Call from Phase 2 (post-scheduler, inside driver's attach_callback()).
 */
exti_err_t exti_enable(uint8_t line, uint32_t nvic_priority);

/*
 * Disable the EXTI interrupt: clears IMR bit and disables NVIC.
 */
exti_err_t exti_disable(uint8_t line);

/*
 * Clear the EXTI pending flag for the given line.
 * Called from the ISR handler in stm32xxx_it.c before invoking the
 * driver-specific handler.
 */
void exti_clear_pending(uint8_t line);

#endif /* EXTI_DRIVER_H */
```

**Why `exti_configure()` and `exti_enable()` are separate:**

Matches the two-phase init pattern established across the project. Phase 1
(pre-scheduler) configures the hardware. Phase 2 (post-scheduler) enables
the interrupt once the owning task handle exists. ExtiDriver enforces this
split at the API level.

**Why `exti_clear_pending()` is exposed:**

ISR handlers in `stm32xxx_it.c` must clear the EXTI pending flag before
calling the driver-specific handler. Exposing this through ExtiDriver keeps
all EXTI register access inside the driver layer; `stm32xxx_it.c` never
touches `EXTI->PR1` or `EXTI->PR` directly.

### Dependency-conformance check

| Dependency | In `components.md` | Actual usage |
|------------|-------------------|--------------|
| CMSIS | Yes | Yes — EXTI_TypeDef, SYSCFG_TypeDef |

Consumers (MagnetometerDriver, ImuDriver, WifiDriver, TouchscreenDriver)
add ExtiDriver to their USES list. GpioDriver remains in those USES lists
— GpioDriver still configures the pin as input (MODER); ExtiDriver handles
SYSCFG + EXTI + NVIC. The two are complementary.

---

## 4. Internal design (Step 3)

### 4.1 Module structure

```
exti_driver.h   — public API (IExti)
exti_driver.c   — singleton state, SYSCFG/EXTI register access
```

### 4.2 Conflict detection

```c
static uint16_t s_configured = 0U;  /* bit N set = line N configured */
```

`exti_configure()` checks bit N before writing SYSCFG_EXTICRx. If set,
returns `EXTI_ERR_CONFLICT`. This catches misconfiguration at init time,
before the scheduler starts, where a debug build will hit an assert and
halt with a meaningful error rather than silently corrupting a shared
register.

### 4.3 SYSCFG_EXTICRx write

```c
/* EXTICR index and bit position for a given line */
uint8_t reg_idx  = line / 4U;          /* 0..3 → EXTICR[0..3]  */
uint8_t bit_pos  = (line % 4U) * 4U;  /* 0, 4, 8, or 12        */

/* Clear the 4-bit field, then write port */
SYSCFG->EXTICR[reg_idx] &= ~(0xFUL << bit_pos);
SYSCFG->EXTICR[reg_idx] |=  ((uint32_t)port << bit_pos);
```

The SYSCFG clock must be enabled before writing. ExtiDriver enables it
inside `exti_configure()` via the RCC APB2ENR SYSCFGEN bit if not already
set.

### 4.4 EXTI trigger configuration

```c
/* Rising edge */
if (edge == EXTI_EDGE_RISING || edge == EXTI_EDGE_BOTH) {
    EXTI_RTSR |= (1UL << line);   /* see §4.5 for macro */
}
/* Falling edge */
if (edge == EXTI_EDGE_FALLING || edge == EXTI_EDGE_BOTH) {
    EXTI_FTSR |= (1UL << line);
}
```

### 4.5 Platform register name abstraction

STM32L475 and STM32F469 use different EXTI register names for lines 0–15:

| Register | STM32L475 | STM32F469 |
|----------|-----------|-----------|
| Interrupt mask | `EXTI->IMR1` | `EXTI->IMR` |
| Pending register | `EXTI->PR1` | `EXTI->PR` |
| Rising trigger | `EXTI->RTSR1` | `EXTI->RTSR` |
| Falling trigger | `EXTI->FTSR1` | `EXTI->FTSR` |

Resolved with local macros at the top of `exti_driver.c`:

```c
#if defined(STM32L475xx)
    #define EXTI_IMR   EXTI->IMR1
    #define EXTI_PR    EXTI->PR1
    #define EXTI_RTSR  EXTI->RTSR1
    #define EXTI_FTSR  EXTI->FTSR1
#elif defined(STM32F469xx)
    #define EXTI_IMR   EXTI->IMR
    #define EXTI_PR    EXTI->PR
    #define EXTI_RTSR  EXTI->RTSR
    #define EXTI_FTSR  EXTI->FTSR
#else
    #error "ExtiDriver: unsupported target. Define STM32L475xx or STM32F469xx."
#endif
```

This is the only platform-conditional code in ExtiDriver. There is no
platform-split `.c` file — the difference is four register aliases.

### 4.6 NVIC configuration in `exti_enable()`

```c
/* Determine IRQn from line number */
IRQn_Type irqn = prv_line_to_irqn(line);   /* see table below */
NVIC_SetPriority(irqn, nvic_priority);
NVIC_EnableIRQ(irqn);
EXTI_IMR |= (1UL << line);
```

EXTI line → IRQn mapping (both boards, lines 0–15):

| Lines | IRQn |
|-------|------|
| 0 | EXTI0_IRQn |
| 1 | EXTI1_IRQn |
| 2 | EXTI2_IRQn |
| 3 | EXTI3_IRQn |
| 4 | EXTI4_IRQn |
| 5–9 | EXTI9_5_IRQn |
| 10–15 | EXTI15_10_IRQn |

`prv_line_to_irqn()` is a private static function implementing this table.

---

## 5. Hardware contract (Step 4)

ExtiDriver accesses two peripheral blocks:

| Peripheral | Purpose |
|------------|---------|
| `SYSCFG` (APB2) | `EXTICR[0..3]` — GPIO port selection per EXTI line |
| `EXTI` | `IMR`, `RTSR`, `FTSR`, `PR` (L475: `IMR1`, `RTSR1`, `FTSR1`, `PR1`) |
| `NVIC` (core) | Priority and enable per EXTI IRQn |

ExtiDriver does not own any GPIO pin. Pin direction (input, pull) is
configured by GpioDriver in each consumer's Phase 1 init, before
`exti_configure()` is called.

**NVIC priority values** are passed in by the caller (`exti_enable()`
parameter). ExtiDriver does not impose a fixed priority — consumers set
priorities consistent with their task hierarchy. Recommended values:

| Consumer | Suggested NVIC priority |
|----------|------------------------|
| WifiDriver (ISM43362 DRDY) | 6 |
| MagnetometerDriver (LIS3MDL DRDY) | 6 |
| ImuDriver (LSM6DSL INT1) | 6 |
| TouchscreenDriver (FT6206 IRQ) | 7 |

All set lower (higher number) than the FreeRTOS kernel tick (typically
priority 5 on Cortex-M4 with `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`).
Verify against `FreeRTOSConfig.h` at implementation (EXTI-O1).

---

## 6. Sequence integration (Step 5)

ExtiDriver calls occur within the two-phase init of each consumer driver.
No task interaction — ExtiDriver is passive, called synchronously.

```
[Pre-scheduler — board_init()]
  gpio_configure(port, pin, GPIO_MODE_INPUT, GPIO_PULL_NONE)  /* GpioDriver */
  exti_configure(line, port, EXTI_EDGE_RISING)                /* ExtiDriver */

[Post-scheduler — consumer driver's attach_callback()]
  exti_enable(line, nvic_priority)                            /* ExtiDriver */

[ISR — stm32xxx_it.c]
  exti_clear_pending(line)                                    /* ExtiDriver */
  consumer_irq_handler()                                      /* e.g. magnetometer_drdy_irq_handler() */
```

The existing ISR handlers in the companion documents are updated: the
direct `EXTI->PR1 = (1U << N)` write is replaced with
`exti_clear_pending(N)`.

---

## 7. Error handling (Step 6)

| Condition | Response |
|-----------|----------|
| `line > 15` | Return `EXTI_ERR_INVALID_ARG` |
| `port` not in `exti_port_t` enum | Return `EXTI_ERR_INVALID_ARG` |
| Line already configured (`s_configured` bit set) | Return `EXTI_ERR_CONFLICT`; no register written |
| `exti_enable()` called before `exti_configure()` | Return `EXTI_ERR_INVALID_ARG` (`s_configured` bit not set) |
| `exti_clear_pending()` with `line > 15` | No-op (function is void; invalid line produces no write) |

Consumers assert on non-OK returns in debug builds. Conflict detection
at init time means `EXTI_ERR_CONFLICT` should never occur in a correct
build; it is a programming error, not a runtime error.

---

## 8. Test plan (Step 7)

Host-platform tests (Unity). CMSIS `EXTI` and `SYSCFG` macros redirected
to mock structs via `#define EXTI (&mock_exti)` and
`#define SYSCFG (&mock_syscfg)`.

| Test ID | Scenario | Expected |
|---------|----------|----------|
| EXTI-T01 | `exti_configure(1, EXTI_PORT_E, EXTI_EDGE_RISING)` | SYSCFG->EXTICR[0] bits [7:4] = 4 (PE); EXTI_RTSR bit 1 set; EXTI_FTSR bit 1 clear; `s_configured` bit 1 set |
| EXTI-T02 | `exti_configure(8, EXTI_PORT_C, EXTI_EDGE_RISING)` | SYSCFG->EXTICR[2] bits [3:0] = 2 (PC); EXTI_RTSR bit 8 set |
| EXTI-T03 | `exti_configure(11, EXTI_PORT_C, EXTI_EDGE_RISING)` | SYSCFG->EXTICR[2] bits [15:12] = 2 (PC); EXTI_RTSR bit 11 set |
| EXTI-T04 | `exti_configure` same line twice | Second call returns `EXTI_ERR_CONFLICT`; SYSCFG not written twice |
| EXTI-T05 | `exti_configure(16, ...)` | Returns `EXTI_ERR_INVALID_ARG` |
| EXTI-T06 | `exti_configure(5, EXTI_PORT_A, EXTI_EDGE_FALLING)` | EXTI_FTSR bit 5 set; EXTI_RTSR bit 5 clear |
| EXTI-T07 | `exti_configure(3, EXTI_PORT_B, EXTI_EDGE_BOTH)` | Both EXTI_RTSR and EXTI_FTSR bit 3 set |
| EXTI-T08 | `exti_enable(1, 6)` after configure | EXTI_IMR bit 1 set; NVIC priority = 6; NVIC enabled |
| EXTI-T09 | `exti_enable(1, 6)` without prior configure | Returns `EXTI_ERR_INVALID_ARG` |
| EXTI-T10 | `exti_disable(1)` | EXTI_IMR bit 1 cleared; NVIC disabled |
| EXTI-T11 | `exti_clear_pending(8)` | EXTI_PR bit 8 set (write-1-to-clear semantics) |
| EXTI-T12 | SYSCFG->EXTICR[0] field isolation: configure line 0, then line 1 independently | Fields in EXTICR[0] do not overwrite each other |

Test file: `tests/drivers/test_exti_driver.c`.

---

## 9. Open items and decisions log (Step 8)

### Decisions

| ID | Decision | Rationale |
|----|----------|-----------|
| EXTI-D1 | ExtiDriver owns SYSCFG_EXTICRx, EXTI RTSR/FTSR/IMR, and NVIC for EXTI lines | Shared-register ownership. Without a single owner, concurrent read-modify-write on SYSCFG_EXTICRx by multiple drivers at init time is an undetectable defect. |
| EXTI-D2 | Conflict detection via `s_configured` bitmap | Catches misconfiguration at init (pre-scheduler, debug builds halt). Production code returns `EXTI_ERR_CONFLICT`; callers assert. |
| EXTI-D3 | Platform register alias via macros, not platform-split `.c` | The only difference between L475 and F469 is four register name suffixes. A macro alias is the minimal correct solution; a platform-split file would duplicate ~150 lines for four `#define` substitutions. |
| EXTI-D4 | NVIC priority passed in by caller | ExtiDriver does not know the task hierarchy. Each consumer knows its own scheduling priority and sets the NVIC priority accordingly. |
| EXTI-D5 | `exti_clear_pending()` is a public function, not a macro | Keeping all EXTI register access inside the driver layer prevents `stm32xxx_it.c` from bypassing the abstraction. ISR files should not include CMSIS peripheral headers directly above the driver layer boundary. |

### Open items

| ID | Item | Owner | Resolution path |
|----|------|-------|-----------------|
| EXTI-O1 | NVIC priority values must be verified against `FreeRTOSConfig.h` `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`. All EXTI ISRs that call FreeRTOS `FromISR` APIs must have NVIC priority ≥ this value (numerically equal or higher). | Luca | Confirm at implementation. Suggested values in §5 are safe starting points. |
| EXTI-O2 | TouchscreenDriver companion (`touchscreen-driver.md`) must be updated: Phase 1 EXTI config changes from direct CMSIS write to `exti_configure()` call. USES list gains ExtiDriver. | Luca | Update companion before coding. Commit as `docs: update TouchscreenDriver to use ExtiDriver`. |
| EXTI-O3 | MagnetometerDriver and ImuDriver companions must be updated: Phase 1 EXTI config changes from direct CMSIS write to `exti_configure()` call. USES list gains ExtiDriver. | Luca | Update companions before coding. Commit as `docs: update Group B sensor drivers to use ExtiDriver`. |
| EXTI-O4 | WifiDriver companion must be updated: Phase 1 DRDY EXTI config changes to `exti_configure()`. USES list gains ExtiDriver. | Luca | Update companion before coding. Commit as `docs: update WifiDriver to use ExtiDriver`. |
| EXTI-O5 | ISR handlers in `stm32xxx_it.c` sketched in Group B and WifiDriver companions use `EXTI->PR1 = (1U << N)` directly. Replace with `exti_clear_pending(N)` in all ISR handler sketches. | Luca | Update at coding stage; note in ISR implementation. |

---

## 10. components.md additions

Add the following entry to **both** the Field Device and Gateway driver
layer sections.

**Field Device — add after GpioDriver entry:**

```
**NAME:** ExtiDriver
**LAYER:** Driver
**RESPONSIBILITY:** Configures EXTI interrupt lines: maps GPIO ports via
SYSCFG_EXTICRx, sets trigger edges, manages IMR and NVIC enable/disable.
Provides conflict detection to prevent two drivers from claiming the same
line. Traces through consumers: REQ-LD-050 (TouchscreenDriver).
**PROVIDES (upward):** IExti
**USES (downward):** CMSIS
```

**Field Device — final driver list addition:**

Add `ExtiDriver` to the Final component list §3 driver layer.

**Field Device — TouchscreenDriver USES update:**

Change:
```
USES (downward): I2cDriver
```
To:
```
USES (downward): I2cDriver, ExtiDriver
```

**Gateway — add after GpioDriver entry:**

```
**NAME:** ExtiDriver
**LAYER:** Driver
**RESPONSIBILITY:** Configures EXTI interrupt lines: maps GPIO ports via
SYSCFG_EXTICRx, sets trigger edges, manages IMR and NVIC enable/disable.
Provides conflict detection to prevent two drivers from claiming the same
line. Traces through consumers: REQ-CC-050 (WifiDriver), REQ-SA-031
(MagnetometerDriver), REQ-SA-071 (ImuDriver).
**PROVIDES (upward):** IExti
**USES (downward):** CMSIS
```

**Gateway — final driver list addition:**

Add `ExtiDriver` to the Final component list §3 driver layer.

**Gateway — USES updates:**

WifiDriver: add ExtiDriver → `USES (downward): SpiDriver, GpioDriver, ExtiDriver`  
MagnetometerDriver: add ExtiDriver → `USES (downward): I2cDriver, GpioDriver, ExtiDriver`  
ImuDriver: add ExtiDriver → `USES (downward): I2cDriver, GpioDriver, ExtiDriver`  

---

*This document is the LLD companion for ExtiDriver. It is authored by
Luca Agrippino and reviewed by the project mentor.*
