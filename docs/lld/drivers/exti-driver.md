# LLD Companion — ExtiDriver

**Board:** Gateway (B-L475E-IOT01A) — Field Device deployment deferred, see EXTI-O6
**Branch:** `feature/lld-exti-driver`
**Status:** Phase H ready
**Methodology:** lld-methodology.md v1.1, steps 1–8
**Version:** 1.1
**Date:** July 2026

**HLD anchor:** ExtiDriver in `components.md` (GW driver layer)

**Pattern exception:** ExtiDriver is a platform singleton (free functions
with `exti_` prefix), not an ADT.  The MCU has exactly one EXTI
peripheral and one SYSCFG block; an opaque-handle pool would add
indirection for a resource that is inherently singular.  Same rationale
as CpuDriver.

---

## 1. Sources

ExtiDriver is the sole owner of the EXTI and SYSCFG_EXTICRx peripheral
configuration on the Gateway. It was identified during the WifiDriver
companion (WIFI-O2 root) as a missing driver required to prevent a class
of defects.

**The concrete problem it solves:**

The `SYSCFG_EXTICRx` registers map GPIO ports to EXTI lines. Each register
is 32 bits wide and covers four EXTI lines (4 bits each). Without a central
owner, every driver that needs an EXTI interrupt would read-modify-write the
same shared register in isolation — an error-prone pattern that is hard to
audit. ExtiDriver provides a single, conflict-detecting entry point for all
EXTI line configuration.

**Consumers:**

| Board | Driver | EXTI line | Trigger |
|-------|--------|-----------|---------|
| GW | WifiDriver | EXTI1 (ISM43362 DRDY) | Rising |
| GW | MagnetometerDriver | EXTI8 (LIS3MDL DRDY) | Rising |
| GW | ImuDriver | EXTI11 (LSM6DSL INT1) | Rising |
| FD | TouchscreenDriver | EXTIx (FT6206 IRQ) | Falling — **deferred, see EXTI-O6** |

The register layout and public API are already platform-portable (the
only board-conditional code is the L475/F469 register-name alias block
in §3.4), so adding Field Device support later is a relocation, not a
rewrite. It is deferred rather than built now because TouchscreenDriver
itself has not yet been implemented — see EXTI-O6.

**P6 traceability:** ExtiDriver has no dedicated SRS requirement. It traces
through its Gateway consumers: REQ-SA-031, REQ-SA-071, REQ-CC-050.
Infrastructure drivers that serve only as a shared-resource guard are an
accepted exception to P6's "one component per use case" guidance, provided
the need is demonstrated (it is — see above).

---

### 1.1 Source references

| Source | Relevant section |
|--------|-----------------|
| `components.md` | GpioDriver entry (peer pattern); ExtiDriver added — see §10 |
| `wifi-driver.md` | WIFI-O2 (origin of this driver) |
| `magnetometer-imu-drivers.md` | §4.2 Phase 1 EXTI config; GPB-O3 |
| `touchscreen-driver.md` | Phase 1 EXTI GPIO config — not yet updated, see EXTI-O6 |
| RM0351 §13 (STM32L475) | SYSCFG_EXTICRx, EXTI registers |
| RM0386 §10 (STM32F469) | SYSCFG_EXTICRx, EXTI registers (for the deferred FD port) |
| `stm32l475xx.h`, `stm32f469xx.h` | EXTI_TypeDef, SYSCFG_TypeDef register definitions |

---

## 2. Public API

```c
/* exti_driver.h */

#ifndef EXTI_DRIVER_H
#define EXTI_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    EXTI_ERR_OK             = 0,
    EXTI_ERR_INVALID_ARG    = 1,   /* line > 15 or invalid port/edge   */
    EXTI_ERR_CONFLICT       = 2,   /* line already configured          */
    EXTI_ERR_NOT_CONFIGURED = 3    /* enable/disable on unconfigured   */
} exti_err_t;

typedef enum {
    EXTI_PORT_A = 0U,
    EXTI_PORT_B = 1U,
    EXTI_PORT_C = 2U,
    EXTI_PORT_D = 3U,
    EXTI_PORT_E = 4U,
    EXTI_PORT_F = 5U,    /* present on STM32F469 */
    EXTI_PORT_G = 6U,    /* present on STM32F469 */
    EXTI_PORT_H = 7U     /* present on STM32L475 */
} exti_port_t;

typedef enum {
    EXTI_EDGE_RISING  = 0U,
    EXTI_EDGE_FALLING = 1U,
    EXTI_EDGE_BOTH    = 2U
} exti_edge_t;

/**
 * @brief Configure an EXTI line for a given GPIO port and trigger edge.
 *
 * Programs SYSCFG_EXTICRx to map the line to the specified port, and
 * sets the trigger edge in EXTI RTSR/FTSR.  Enables the SYSCFG clock
 * if not already enabled.
 *
 * Does NOT enable the interrupt (does not touch IMR or NVIC).
 * Returns EXTI_ERR_CONFLICT if the line is already configured.
 *
 * Call from Phase 1 (pre-scheduler).
 *
 * @param[in] line  EXTI line number (0–15).
 * @param[in] port  GPIO port to map to the EXTI line.
 * @param[in] edge  Trigger edge selection.
 * @return EXTI_ERR_OK on success; EXTI_ERR_INVALID_ARG if line > 15 or
 *         port/edge invalid; EXTI_ERR_CONFLICT if line already configured.
 * @note Threading: pre-scheduler or single task context.  Not ISR-safe.
 */
exti_err_t exti_configure(uint8_t line, exti_port_t port, exti_edge_t edge);

/**
 * @brief Enable the EXTI interrupt for a previously configured line.
 *
 * Sets the IMR bit and configures the NVIC priority and enable for the
 * corresponding IRQn.  Caller must have called exti_configure() for
 * this line first.
 *
 * Call from Phase 2 (post-scheduler, inside driver's attach_callback()).
 *
 * @param[in] line           EXTI line number (0–15).
 * @param[in] nvic_priority  NVIC priority value to assign.
 * @return EXTI_ERR_OK on success; EXTI_ERR_INVALID_ARG if line > 15;
 *         EXTI_ERR_NOT_CONFIGURED if line was not previously configured.
 * @note Threading: task-context only.  Not ISR-safe.
 */
exti_err_t exti_enable(uint8_t line, uint32_t nvic_priority);

/**
 * @brief Disable the EXTI interrupt for a previously configured line.
 *
 * Clears the IMR bit and disables the NVIC for the corresponding IRQn.
 *
 * @param[in] line  EXTI line number (0–15).
 * @return EXTI_ERR_OK on success; EXTI_ERR_INVALID_ARG if line > 15;
 *         EXTI_ERR_NOT_CONFIGURED if line was not previously configured.
 * @note Threading: task-context only.  Not ISR-safe.
 */
exti_err_t exti_disable(uint8_t line);

/**
 * @brief Clear the pending flag for an EXTI line.
 *
 * Writes 1 to the corresponding bit in EXTI PR (write-1-to-clear).
 * Called from the ISR handler in stm32xxx_it.c before invoking the
 * driver-specific handler.
 *
 * @param[in] line  EXTI line number (0–15).  No validation — the ISR
 *                  path must be fast and the line is known at compile time.
 * @note Threading: ISR-safe.  This is the only ExtiDriver function
 *       callable from interrupt context.
 */
void exti_clear_pending(uint8_t line);

#ifdef TEST
/**
 * @brief Reset all internal state for unit testing.
 *
 * Clears the configured-lines bitmap and zeroes mock register state.
 * Guarded by TEST — never compiled into production firmware.
 */
void exti_reset_for_test(void);
#endif

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

**Why no `exti_init()` function:**

The only initialisation action is enabling the SYSCFG clock, which
`exti_configure()` does on first call. A separate `exti_init()` would be
a one-line function called exactly once before the first `exti_configure()`
— the ceremony adds nothing. If a future need arises (e.g. a bulk reset
at shutdown), an init function can be introduced without API breakage.

### Dependency-conformance check

| Dependency | In `components.md` | Actual usage |
|------------|-------------------|--------------|
| CMSIS | Yes | Yes — EXTI_TypeDef, SYSCFG_TypeDef, NVIC functions |

ExtiDriver does **not** depend on GpioDriver. The two are complementary
peers: GpioDriver configures pin direction/pull; ExtiDriver configures
SYSCFG port mapping and EXTI trigger/mask. Neither imports the other's
header. Consumers call both independently.

---

## 3. Internal design

### 3.1 Conflict detection

```c
static uint16_t s_configured = 0U;  /* bit N set = line N configured */
```

`exti_configure()` checks bit N before writing SYSCFG_EXTICRx. If set,
returns `EXTI_ERR_CONFLICT`. This catches misconfiguration at init time,
before the scheduler starts, where a debug build will hit an assert and
halt with a meaningful error rather than silently corrupting a shared
register.

### 3.2 SYSCFG_EXTICRx write

```c
/* EXTICR index and bit position for a given line */
uint8_t reg_idx  = line / 4U;          /* 0..3 → EXTICR[0..3]  */
uint8_t bit_pos  = (line % 4U) * 4U;  /* 0, 4, 8, or 12        */

/* Clear the 4-bit field, then write port */
SYSCFG->EXTICR[reg_idx] &= ~(0xFUL << bit_pos);
SYSCFG->EXTICR[reg_idx] |=  ((uint32_t) port << bit_pos);
```

The SYSCFG clock must be enabled before writing. ExtiDriver enables it
inside `exti_configure()` via the RCC APB2ENR SYSCFGEN bit if not already
set.

### 3.3 EXTI trigger configuration

```c
/* Rising edge */
if ((edge == EXTI_EDGE_RISING) || (edge == EXTI_EDGE_BOTH))
{
    EXTI_RTSR |= (1UL << line);
}
else
{
    EXTI_RTSR &= ~(1UL << line);
}

/* Falling edge */
if ((edge == EXTI_EDGE_FALLING) || (edge == EXTI_EDGE_BOTH))
{
    EXTI_FTSR |= (1UL << line);
}
else
{
    EXTI_FTSR &= ~(1UL << line);
}
```

Note: explicitly clears the opposite edge register when not requested,
rather than only setting. Prevents stale configuration from a prior
consumer (defensive, relevant if `exti_reset_for_test()` is used to
reconfigure in test scenarios).

### 3.4 Platform register name abstraction

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

This is the only platform-conditional code in ExtiDriver — it already
resolves both boards even though the source currently lives under
`firmware/gateway/` only (EXTI-O6). There is no platform-split `.c`
file — the difference is four register aliases.

### 3.5 exti_enable implementation

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

### 3.6 exti_disable implementation

```c
EXTI_IMR &= ~(1UL << line);
IRQn_Type irqn = prv_line_to_irqn(line);
NVIC_DisableIRQ(irqn);
```

Note for shared IRQn lines (5–9, 10–15): `NVIC_DisableIRQ` disables the
entire IRQn vector. If two EXTI lines share the same IRQn (e.g., lines 5
and 8 both use EXTI9_5_IRQn), disabling one disables the NVIC for both.
The IMR bit is still cleared per-line, so the EXTI hardware filters
correctly. A future enhancement could reference-count shared IRQn enables;
for the current Gateway consumer set (lines 1, 8, 11) there are no
shared-IRQn conflicts.

### 3.7 exti_clear_pending implementation

```c
EXTI_PR = (1UL << line);   /* write-1-to-clear */
```

No validation — ISR path must be fast. The line number is known at compile
time from the vector that triggered.

### Synchronisation

Caller serialises. The driver holds no FreeRTOS synchronisation primitives.
All entry points except `exti_clear_pending` are intended to be called from
a single task context or from `main()` before the scheduler starts.
`exti_clear_pending` is ISR-safe by design (single atomic write, no
read-modify-write).

### Principles applied

- **P1 (Strict directional layering).** ExtiDriver depends only on CMSIS
  device headers. No upward dependency on any middleware or application
  component.
- **P6 (Responsibility traces to requirements).** Traces through Gateway
  consumers: REQ-CC-050, REQ-SA-031, REQ-SA-071. Shared-resource guard
  is a documented P6 exception.
- **P8 (Total error propagation, no silent failures).** All configuration
  and enable functions return `exti_err_t`. Conflict detection surfaces
  misconfiguration rather than silently overwriting.
- **P9 (BARR-C coding standard).** Fixed-width types, braces on all
  control structures, `const` on unmodified pointer parameters.
- **P10 (Naming conventions).** Module prefix `exti_`; errors
  `EXTI_ERR_*`.

**Principles considered and found not to apply:**

- **P2 (Dependency Inversion)** — does not apply. ExtiDriver is a
  driver-layer singleton consumed by other drivers in the same layer.
  There is no cross-layer upward data flow requiring an abstraction.
- **P3 (Interface Segregation)** — single consumer profile (configure +
  enable + clear). No reader/writer split needed.
- **P5 (Metric Producer)** — ExtiDriver produces no runtime metrics.
- **P7 (Pull-based consumption)** — consumers call ExtiDriver
  synchronously at init time. No data production to pull.

---

## 4. Hardware contract

ExtiDriver accesses two peripheral blocks:

| Peripheral | Purpose |
|------------|---------|
| `SYSCFG` (APB2) | `EXTICR[0..3]` — GPIO port selection per EXTI line |
| `EXTI` | `IMR`, `RTSR`, `FTSR`, `PR` (L475: `IMR1`, `RTSR1`, `FTSR1`, `PR1`) |
| `NVIC` (core) | Priority and enable per EXTI IRQn |

ExtiDriver does not own any GPIO pin. Pin direction (input, pull) is
configured by GpioDriver in each consumer's Phase 1 init, before
`exti_configure()` is called.

### Registers used

| Peripheral | Registers | Purpose |
|---|---|---|
| `SYSCFG` (APB2) | `EXTICR[0]`..`EXTICR[3]` | Selects GPIO port for each EXTI line (4 bits per line). |
| `EXTI` | `IMR` / `IMR1` | Interrupt mask — enables/disables each EXTI line. |
| `EXTI` | `RTSR` / `RTSR1` | Rising-trigger selection register. |
| `EXTI` | `FTSR` / `FTSR1` | Falling-trigger selection register. |
| `EXTI` | `PR` / `PR1` | Pending register — cleared in ISR by writing 1 to the bit. |

Register aliases: F469 uses bare `IMR`, `RTSR`, `FTSR`, `PR`; L475 uses
the `1`-suffixed variants. Both are accessed via the CMSIS `EXTI` macro.
Note also that `RCC_APB2ENR_SYSCFGEN` is **bit 0** on L475 vs **bit 14**
on F469 — `exti_configure()` reads/writes the bit via the named constant,
never a hard-coded shift, so this difference is transparent to callers.

### Pins

N/A — ExtiDriver does not configure GPIO pins. Pin direction, pull, and
alternate function are GpioDriver's responsibility. ExtiDriver wires an
already-configured input pin to the EXTI interrupt logic by programming
`SYSCFG_EXTICRx`.

### Clocks

`SYSCFG` clock: `RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN`. ExtiDriver
enables this bit inside `exti_configure()` if not already set. The `EXTI`
peripheral itself requires no separate clock enable.

### NVIC

NVIC priority is a **caller-supplied parameter** to `exti_enable()`.
ExtiDriver does not prescribe priority values. Callers that register a
callback invoking FreeRTOS API (`FromISR` functions) must assign priority
≥ `configMAX_SYSCALL_INTERRUPT_PRIORITY` (see lld.md §6.3).

**Suggested NVIC priority values:**

| Consumer | Suggested NVIC priority |
|----------|------------------------|
| WifiDriver (ISM43362 DRDY) | 6 |
| MagnetometerDriver (LIS3MDL DRDY) | 6 |
| ImuDriver (LSM6DSL INT1) | 6 |
| TouchscreenDriver (FT6206 IRQ) | 7 — for when EXTI-O6 is resolved |

All set lower (higher number) than the FreeRTOS kernel tick (typically
priority 5 on Cortex-M4 with `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`).
Verify against `FreeRTOSConfig.h` at implementation (EXTI-O1).

---

## 5. Sequence integration

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
  consumer_irq_handler()                                      /* e.g. wifi_datardy_irq_handler() */
```

The existing ISR handlers in the companion documents are updated: the
direct `EXTI->PR1 = (1U << N)` write is replaced with
`exti_clear_pending(N)`.

---

## 6. Error and fault behaviour

All public functions except `exti_clear_pending` return `exti_err_t`;
callers must not ignore non-OK returns. No retry is performed internally —
the driver surfaces the error; the caller decides the retry and logging
policy.

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `EXTI_ERR_INVALID_ARG` | `line > 15` or invalid `exti_port_t` / `exti_edge_t` value | Return error; no register touched | Non-OK return | No retry — programming error | Caller logs at ERROR via ILogger |
| `EXTI_ERR_CONFLICT` | `exti_configure()` called on a line already assigned to a different port/edge | Return error; configuration unchanged | Non-OK return | No retry — programming error; caller must redesign init order | Caller logs at ERROR via ILogger |
| `EXTI_ERR_NOT_CONFIGURED` | `exti_enable()` or `exti_disable()` called on a line that was never configured | Return error; no register touched | Non-OK return | No retry — programming error; caller must call `exti_configure()` first | Caller logs at ERROR via ILogger |

---

## 7. Unit-test plan

Host-platform tests (Unity). CMSIS `EXTI` and `SYSCFG` macros redirected
to mock structs via `#define EXTI (&mock_exti)` and
`#define SYSCFG (&mock_syscfg)`. NVIC functions mocked via CMock or
manual stubs.

| Test ID | Scenario | Expected |
|---------|----------|----------|
| EXTI-T01 | `exti_configure(1, EXTI_PORT_E, EXTI_EDGE_RISING)` | SYSCFG->EXTICR[0] bits [7:4] = 4 (PE); RTSR bit 1 set; FTSR bit 1 clear; `s_configured` bit 1 set; returns `EXTI_ERR_OK` |
| EXTI-T02 | `exti_configure(8, EXTI_PORT_C, EXTI_EDGE_RISING)` | SYSCFG->EXTICR[2] bits [3:0] = 2 (PC); RTSR bit 8 set; returns `EXTI_ERR_OK` |
| EXTI-T03 | `exti_configure(11, EXTI_PORT_C, EXTI_EDGE_RISING)` | SYSCFG->EXTICR[2] bits [15:12] = 2 (PC); RTSR bit 11 set; returns `EXTI_ERR_OK` |
| EXTI-T04 | `exti_configure` same line twice | Second call returns `EXTI_ERR_CONFLICT`; SYSCFG not written twice |
| EXTI-T05 | `exti_configure(16, ...)` | Returns `EXTI_ERR_INVALID_ARG`; no register touched |
| EXTI-T06 | `exti_configure(5, EXTI_PORT_A, EXTI_EDGE_FALLING)` | FTSR bit 5 set; RTSR bit 5 clear; returns `EXTI_ERR_OK` |
| EXTI-T07 | `exti_configure(3, EXTI_PORT_B, EXTI_EDGE_BOTH)` | Both RTSR and FTSR bit 3 set; returns `EXTI_ERR_OK` |
| EXTI-T08 | `exti_enable(1, 6)` after configure | IMR bit 1 set; NVIC priority = 6; NVIC enabled; returns `EXTI_ERR_OK` |
| EXTI-T09 | `exti_enable(1, 6)` without prior configure | Returns `EXTI_ERR_NOT_CONFIGURED`; IMR unchanged |
| EXTI-T10 | `exti_disable(1)` after configure + enable | IMR bit 1 cleared; NVIC disabled; returns `EXTI_ERR_OK` |
| EXTI-T11 | `exti_disable(3)` without prior configure | Returns `EXTI_ERR_NOT_CONFIGURED` |
| EXTI-T12 | `exti_clear_pending(8)` | PR bit 8 written as 1 (write-1-to-clear semantics) |
| EXTI-T13 | SYSCFG->EXTICR[0] field isolation: configure line 0, then line 1 independently | Fields in EXTICR[0] do not overwrite each other |
| EXTI-T14 | `exti_configure` with invalid port value (e.g. 0xFF) | Returns `EXTI_ERR_INVALID_ARG` |
| EXTI-T15 | `exti_configure` with invalid edge value (e.g. 0xFF) | Returns `EXTI_ERR_INVALID_ARG` |
| EXTI-T16 | `exti_reset_for_test()` clears bitmap | After reset, a previously conflicting line can be re-configured |
| EXTI-T17 | SYSCFG clock enabled: after first `exti_configure()`, `RCC->APB2ENR` SYSCFGEN bit is set | Verifies clock enable logic |

Test file: `tests/gateway/drivers/exti_driver/test_exti_driver.c`

Written against the L475 register layout only for now (EXTI-O6); the
production source's platform-alias macros already resolve F469 too, so
adding an `STM32F469xx`-defined counterpart later is a copy of this file
with mock includes swapped, not new logic.

---

## 8. Open items

### Decisions

| ID | Decision | Rationale |
|----|----------|-----------|
| EXTI-D1 | ExtiDriver owns SYSCFG_EXTICRx, EXTI RTSR/FTSR/IMR, and NVIC for EXTI lines | Shared-register ownership. Without a single owner, concurrent read-modify-write on SYSCFG_EXTICRx by multiple drivers at init time is an undetectable defect. |
| EXTI-D2 | Conflict detection via `s_configured` bitmap | Catches misconfiguration at init (pre-scheduler, debug builds halt). Production code returns `EXTI_ERR_CONFLICT`; callers assert. |
| EXTI-D3 | Platform register alias via macros, not platform-split `.c` | The only difference between L475 and F469 is four register name suffixes. A macro alias is the minimal correct solution; a platform-split file would duplicate ~150 lines for four `#define` substitutions. |
| EXTI-D4 | NVIC priority passed in by caller | ExtiDriver does not know the task hierarchy. Each consumer knows its own scheduling priority and sets the NVIC priority accordingly. |
| EXTI-D5 | `exti_clear_pending()` is a public function, not a macro | Keeping all EXTI register access inside the driver layer prevents `stm32xxx_it.c` from bypassing the abstraction. ISR files should not include CMSIS peripheral headers directly above the driver layer boundary. |
| EXTI-D6 | Singleton pattern, not ADT | MCU has exactly one EXTI peripheral and one SYSCFG block. ADT indirection would add ceremony for a resource that is inherently singular. Same rationale as CpuDriver. |
| EXTI-D7 | Source lives at `firmware/gateway/drivers/exti/`, not `firmware/shared/` | The design is board-portable (§3.4) and was originally specified as shared, but Field Device has no consumer yet (TouchscreenDriver is unimplemented). Placing it under `firmware/gateway/` avoids carrying an unused shared-folder dependency into the Field Device CubeIDE project until it's actually needed. Relocating later is a `git mv` plus re-adding the FD companion-doc updates in §10 — no logic changes. |

### Open items

| ID | Item | Owner | Resolution path |
|----|------|-------|-----------------|
| EXTI-O1 | NVIC priority values must be verified against `FreeRTOSConfig.h` `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`. All EXTI ISRs that call FreeRTOS `FromISR` APIs must have NVIC priority ≥ this value (numerically equal or higher). | Luca | Confirm at implementation. Suggested values in §4 are safe starting points. |
| EXTI-O2 | TouchscreenDriver companion (`touchscreen-driver.md`) must be updated: Phase 1 EXTI config changes from direct CMSIS write to `exti_configure()` call. USES list gains ExtiDriver. | Luca | Deferred — see EXTI-O6. Update companion when TouchscreenDriver is implemented. Commit as `docs: update TouchscreenDriver to use ExtiDriver`. |
| EXTI-O3 | MagnetometerDriver and ImuDriver companions must be updated: Phase 1 EXTI config changes from direct CMSIS write to `exti_configure()` call. USES list gains ExtiDriver. | Luca | Update companions before coding those modules. Commit as `docs: update Group B sensor drivers to use ExtiDriver`. |
| EXTI-O4 | WifiDriver companion must be updated: Phase 1 DRDY EXTI config changes to `exti_configure()`. USES list gains ExtiDriver. | Luca | Done — `docs/lld/drivers/wifi-driver.md` WIFI-D9 updated, USES now `SpiDriver, GpioDriver, ExtiDriver`. |
| EXTI-O5 | ISR handlers in `stm32xxx_it.c` sketched in Group B and WifiDriver companions use `EXTI->PR1 = (1U << N)` directly. Replace with `exti_clear_pending(N)` in all ISR handler sketches. | Luca | Update at coding stage; note in ISR implementation. |
| EXTI-O6 | ExtiDriver is Gateway-only for now. Field Device deployment (for TouchscreenDriver) requires: (1) `git mv firmware/gateway/drivers/exti/ firmware/shared/drivers/exti/`, (2) re-add the FD `components.md` entries and TouchscreenDriver USES update from the original §10 draft, (3) add a `test_exti_driver_fd.c` counterpart (STM32F469xx defines) alongside the existing GW test, (4) wire `firmware/shared/` into the Field Device CubeIDE project (linked resource + sourcePath — Gateway's `.project`/`.cproject` already has this pattern to copy). No production logic changes needed — `exti_driver.c`'s platform-alias block already resolves both targets. | Luca | Resolve when TouchscreenDriver is scheduled for implementation. |

---

## 9. File layout

```
firmware/gateway/drivers/exti/
├── exti_driver.h       # public API
└── exti_driver.c       # implementation + platform register macros
```

Gateway-only for now (EXTI-O6). The register-name alias block already
supports Field Device; only the physical file location and CubeIDE
project wiring would need to move.

---

## 10. components.md additions (Gateway)

**Pre-requisite:** ExtiDriver must be added to `components.md` before
implementation begins — **done** for the Gateway driver layer.

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

Added to the Gateway Final component list §3 driver layer.

**Gateway — USES updates (done):**

WifiDriver: `USES (downward): SpiDriver, GpioDriver, ExtiDriver`
MagnetometerDriver: `USES (downward): I2cDriver, ExtiDriver`
ImuDriver: `USES (downward): I2cDriver, ExtiDriver`

**Field Device additions are deferred — see EXTI-O6.** The
TouchscreenDriver USES update (`I2cDriver, ExtiDriver`) and the FD driver
layer's ExtiDriver entry are not applied to `components.md` until Field
Device deployment is scheduled.

---

## 11. Phase H checklist

- [x] §2 API fully specified — all `@param` descriptions filled, directions correct.
- [x] §3 internal design matches §2 API — no phantom vtable, no phantom callbacks.
- [x] Error codes complete — `EXTI_ERR_OK`, `EXTI_ERR_INVALID_ARG`, `EXTI_ERR_CONFLICT`, `EXTI_ERR_NOT_CONFIGURED`.
- [x] `exti_reset_for_test()` hook present and `#ifdef TEST` guarded.
- [x] §7 test plan covers every public function (happy + error paths).
- [x] §9 file layout specified.
- [x] Singleton exception documented (header and EXTI-D6).
- [x] Platform register aliases documented (§3.4).
- [x] `exti_clear_pending` correctly marked ISR-safe.
- [x] Principles section accurate — no false claims.
- [x] Dependency-conformance check passes (CMSIS only, no GpioDriver dependency).
- [x] `components.md` updated with ExtiDriver entries for the Gateway driver layer (§10) — Field Device entries deferred per EXTI-O6.

---

*This document is the LLD companion for ExtiDriver. It is authored by
Luca Agrippino and reviewed by the project mentor.*
