# Low-Level Design — IoT Environmental Monitoring Gateway

**Version:** 0.1 (initialised — scaffold only)
**Date:** May 2026
**Status:** In progress

---

## 1. Introduction

### 1.1 Purpose

This document is the master Low-Level Design for the IoT Environmental Monitoring Gateway. It refines the architectural decisions baselined in `hld.md` v1.0 into implementation-ready specifications, one component at a time, and records the cross-cutting conventions that every implementation must conform to.

The LLD does not restate the HLD. The HLD is the source of truth for components, interfaces, runtime state, sequencing, FreeRTOS task layout, Modbus register map, and flash partitioning. This document adds — and only adds — what the HLD deliberately deferred: public C APIs, internal data structures, register-level hardware contracts, error semantics, threading and reentrancy guarantees, and unit-test plans.

Detailed per-component specifications live in companion files under `docs/lld/`. This document is the index, the conventions registry, and the decisions log.

### 1.2 How to read this document

Section 2 establishes the boundary between the HLD and the LLD — what each owns and where to look for what. Section 3 fixes the cross-cutting conventions that every companion inherits, so each companion can focus on what is unique to its component. Section 4 catalogues the companions and tracks progress. Sections 5 and 6 maintain open items and the LLD-level decisions log. Section 7 holds the traceability matrix from companions back to the HLD and the SRS.

### 1.3 Methodology

The LLD phase continues the V-Model approach used during the HLD. Each companion is derived from a single, repeatable methodology — sourcing from the HLD, refining into an API, specifying the internals, validating against HLD sequence diagrams, and closing with a unit-test plan. The methodology is documented in full in `lld-methodology.md`. Every companion is produced by applying that methodology and is reviewed against it at gate.

---

## 2. Relationship to the HLD

The HLD is authoritative. The LLD refines.

| Topic | Authoritative document | LLD responsibility |
|---|---|---|
| Component inventory and responsibilities | `components.md` | Refine the responsibilities into function signatures. |
| Interface contracts (PROVIDES / USES) | `components.md`, `hld.md` §5–6 | Specify each interface as a C header (vtable or direct API). |
| Runtime state | `state-machines.md` | Specify entry / do / exit as concrete function calls; expose state via a status enum. |
| Runtime interaction | `sequence-diagrams.md` | Specify which function handles each message; specify IPC primitives in detail. |
| FreeRTOS task layout | `task-breakdown.md` | Specify task entry-point function names, measured WCETs, refined stack sizes, message types per queue. |
| Modbus register map | `modbus-register-map.md` | Specify the register-access dispatcher; specify the encoding helpers. |
| Flash partitions | `flash-partition-layout.md` | Specify the linker script regions; specify the bootloader contract code. |
| Architectural principles | `architecture-principles.md` | Cite P1–P10 in every companion's rationale section. |
| Hardware abstraction stance | `hld.md` §9 | Honour CMSIS-only above silicon, no HAL above the driver layer. |

If a companion appears to need a change to the HLD, the HLD changes first and the gate review on that change runs before the companion is drafted. The HLD does not drift backwards under LLD pressure.

---

## 3. Cross-cutting conventions

Every companion adheres to the conventions in this section. Companions specify only what is unique to their component; everything else is inherited from here.

### 3.1 Coding standard

A subset of BARR-C:2018 applies project-wide. The rules with the highest impact on companion content:

- Functions and variables: `snake_case`. Constants and macros: `UPPER_CASE`. Types: `snake_case_t` with the `_t` suffix.
- Every public symbol begins with the module prefix (e.g., `gpio_init`, `gpio_pin_t`, `GPIO_OK`). The prefix is the module's directory name with underscores instead of hyphens.
- Fixed-width integer types only (`uint8_t`, `int32_t`, …). No bare `int` or `unsigned`.
- Braces always — including single-line `if`/`else`/`while`/`for` bodies.
- `const` correctness on every pointer parameter where the pointee is not mutated.
- Header guards using `#ifndef MODULE_HEADER_H` / `#define MODULE_HEADER_H` / `#endif`.
- Doxygen on every public symbol: `@brief`, `@param`, `@return`, `@note` for threading or reentrancy contracts.
- One module per `.h` / `.c` pair. Headers expose only the public interface; everything else stays in the `.c` file as `static`.

### 3.2 Error handling

Every function that can fail returns an error code from the module's error enum. No silent failure, no global `errno`.

The error-code convention:

- Each module defines its error enum: `<module>_err_t`.
- `<MODULE>_OK = 0` is success. Non-zero values are failures.
- Negative values are reserved for fatal conditions (caller cannot recover, must escalate); positive values are recoverable.
- Where multiple modules surface the same error category (e.g., a generic "busy"), the value is duplicated locally rather than shared across modules. Module independence is preferred over a single global enum.

Functions that produce data return the error code; the data is returned through an output pointer parameter. Functions that consume data take the data as input and return only the error code.

### 3.3 Object-oriented patterns in C

The project uses three OOP-in-C idioms uniformly:

- **Opaque type.** Public headers expose a typedef'd pointer to an incomplete struct (`typedef struct gpio_driver_s *gpio_driver_t`). The struct definition lives in the `.c` file. Callers cannot inspect or modify internal state directly.
- **Init / deinit lifecycle.** Every module has `<module>_init(...)` returning a handle and a status, and `<module>_deinit(handle)` returning a status. Initialisation happens once at boot (P5 — no dynamic allocation after init). Storage for the underlying struct is either a static singleton inside the module or a caller-provided buffer of `<module>_size()` bytes — the choice is documented per component.
- **Vtable for polymorphism.** Interfaces that admit multiple implementations (e.g., `IBarometer` implemented by hardware on the Gateway and by simulation on the Field Device) expose a const struct of function pointers. Each implementation provides a `<module>_get_interface(handle)` function returning a const pointer to its vtable. Callers depend on the vtable type, not the implementation.

### 3.4 Threading, reentrancy and ISR safety

Every public function in every companion is annotated with one of:

- **Task-context only** — safe to call from any FreeRTOS task; may block.
- **Task-context only, non-blocking** — safe from any task; guaranteed not to block.
- **ISR-safe** — may be called from an ISR; uses `FromISR` FreeRTOS primitives only.
- **ISR-only** — must be called from an ISR (e.g., interrupt handlers themselves).

Modules that hold internal state declare their thread-safety contract: serialised internally (mutex held during public calls), single-owner (caller must serialise), or stateless. Reentrancy is the explicit exception, not the default.

### 3.5 Memory

No dynamic memory allocation after the initialisation phase ends. All FreeRTOS objects (tasks, queues, mutexes, timers) are created in `main` before the scheduler starts, using `xQueueCreateStatic`-family APIs. Module storage is static or caller-provided; `malloc` and `free` are not used.

The initialisation phase is defined as: from reset up to `vTaskStartScheduler()`. After that call, no allocation occurs.

### 3.6 File and directory layout

Per the repository structure baselined in the HLD:

```
firmware/
├── field-device/
│   ├── drivers/   <module>.h, <module>.c
│   ├── middleware/
│   ├── app/
│   └── config/
└── gateway/
    ├── drivers/
    ├── middleware/
    ├── app/
    └── config/
tests/
└── <mirrors the firmware tree>
    └── test_<module>.c
```

Each companion under `docs/lld/` specifies which firmware directory hosts its sources and which test directory hosts its tests.

### 3.7 Documentation in source

Every `.h` file opens with a file-level Doxygen block: `@file`, `@brief`, one-paragraph description of the module's role, and a back-reference to the LLD companion path. Every public symbol carries a Doxygen comment immediately above its declaration. Internal symbols in `.c` files carry comments where the intent is non-obvious; trivial helpers do not.

---

## 4. Companion catalogue

The companions are produced bottom-up within the layered architecture, dependency-ordered within each layer. The ordering rationale is recorded in §6 (D1).

Companions are organised by layer under `docs/lld/`:

```
docs/lld/
├── lld.md                  (this document)
├── lld-methodology.md
├── drivers/
├── middleware/
├── application/
└── cross-cutting/
```

| Tier | Path | Layer | Board(s) | Status |
|---|---|---|---|---|
| 1 | `drivers/gpio-driver.md` | Drivers | Both | Baselined |
| 1 | `drivers/debug-uart-driver.md` | Drivers | Both | Baselined |
| 1 | `drivers/rtc-driver.md` | Drivers | Both | Baselined |
| 1 | `drivers/reset-driver.md` | Drivers | Gateway | Planned |
| 1 | `drivers/exti-driver.md` | Drivers | Both | Planned |
| 2 | `drivers/i2c-driver.md` | Drivers | Both | Planned |
| 2 | `drivers/spi-driver.md` | Drivers | Gateway | Planned |
| 2 | `drivers/modbus-uart-driver.md` | Drivers | Both | Planned |
| 2 | `drivers/qspi-flash-driver.md` | Drivers | Both | Planned |
| 2 | `drivers/sdram-driver.md` | Drivers | Field Device | Planned |
| 3 | `drivers/<one per sensor>.md` | Drivers | Both | Planned |
| 3 | `drivers/touchscreen-driver.md` | Drivers | Field Device | Planned |
| 3 | `drivers/lcd-driver.md` | Drivers | Field Device | Planned |
| 3 | `drivers/wifi-driver.md` | Drivers | Gateway | Planned |
| 3 | `drivers/led-driver.md` | Drivers | Both | Planned |
| — | `middleware/<…>.md` | Middleware | Both | Planned (post-driver layer) |
| — | `application/<…>.md` | Application | Both | Planned (post-middleware layer) |
| — | `cross-cutting/<…>.md` | Cross-cutting | — | As needed |

`exti-driver.md` is added to the Tier 1 list per the GpioDriver companion §8 (GPIO-O2) — EXTI configuration is intentionally outside the GPIO driver, so a separate driver is required for any consumer that needs interrupt-driven pin events.

The middleware and application phases will expand into individual companion entries once the driver tier is complete and any inherited TBDs have been resolved.

---

## 5. Inherited open items

Three items deferred from the HLD gate review are tracked here until the relevant companion resolves them.

| ID | Item | Resolution path | Status |
|---|---|---|---|
| O1 | WiFi SPI driver naming. The Gateway task-interaction diagram named the SPI peripheral driver `SpiDriver`, but the Gateway also uses SPI elsewhere via QSPI flash. The companion catalogue must distinguish them clearly. | Resolve in `spi-driver.md` (Tier 2). Confirm whether one SPI driver serves both WiFi and other SPI consumers, or whether separate driver instances are required. Update `components.md` if naming changes. | Open |
| O2 | Worst-case stack measurements. The HLD `task-breakdown.md` holds estimated stack sizes; the real numbers come from `uxTaskGetStackHighWaterMark()` after integration. | No companion resolves this directly. Tracked here through the implementation phase; replaced with measured figures at the first end-to-end integration milestone. | Open |
| O3 | Q10 — hardware watchdog in scope? The HLD did not commit to a watchdog driver. Confirmation is required before any LLD sequence references one. | Decide before drafting the middleware tier. If in scope, add `watchdog-driver.md` as a Tier 1 companion; if out of scope, document the decision in §6. | Open |

---

## 6. LLD decisions log

Decisions adopted during the LLD phase. Format mirrors `hld.md` §14.

| ID | Decision | Rationale | Settled in |
|---|---|---|---|
| D1 | Companions produced bottom-up within the layered architecture, dependency-ordered within each layer. GPIO driver first. | Top-down restates the HLD; just-in-time breaks the V-Model gate discipline established during the HLD; risk-driven starts on components whose dependencies are not yet specified. Bottom-up dependency-ordered honours P1 at design time, gives every completed companion concrete unblocking value downstream, and matches the standard embedded-development order. | Phase opening, this document. |

Subsequent decisions are added as companions surface them.

---

## 7. Traceability

Each completed companion adds one row.

| Companion | HLD component(s) | HLD section | SRS requirements | Use case(s) |
|---|---|---|---|---|
| drivers/gpio-driver.md | GpioDriver (Field Device, Gateway) | components.md  §4 (both boards, driver layer) | REQ-NF-202 | — |
| drivers/debug-uart-driver.md | DebugUartDriver (Field Device, Gateway) | components.md §4 (both boards, driver layer) | REQ-LI-010, REQ-LI-000 | UC-04 |
| docs/lld/drivers/rtc-driver.md | RtcDriver (Driver, both boards) | components.md — Field Device §4 Driver layer; Gateway §4 Driver layer | REQ-NF-213, REQ-NF-212, REQ-TS-020, REQ-NF-210, REQ-NF-211 | UC-13 |
  

---

*This document is the master LLD. It is updated as each companion is completed. Implementation-ready specifications live in the companion files referenced in §4.*
