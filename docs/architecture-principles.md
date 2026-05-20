# Architecture Principles

**Version:** 1.0  
**Date:** May 2026  
**Status:** Baselined  

These numbered principles govern every architectural and LLD decision in the IoT Environmental Monitor project. Each companion document's §3 Internal design cites the principles that shaped its design.

---

## P1 — Strict directional layering

Dependencies flow in one direction only: Application → Middleware → Drivers → CMSIS/Hardware. A component at layer N may depend on components at layer N or lower. No component depends on a component at a higher layer. Violations are escalated as HLD defects; they are never silently absorbed at the LLD layer.

**The only legal upward flow is via inversion (P2):** a lower-layer component depends on an interface owned and injected by the higher layer (e.g., `IHealthReport` is owned by the Application layer but consumed by Middleware producers).

---

## P2 — Dependency Inversion

High-level components depend on abstractions (interfaces), not on concrete implementations. When a lower-layer component must communicate upward (e.g., a Middleware component reporting a fault to the Application layer), the interface is defined by the upper layer and injected into the lower-layer component at init time. The lower-layer component holds a pointer to the interface; it never `#include`s the upper layer's header directly.

---

## P3 — Interface Segregation

Split interfaces into focused abstractions when distinct consumers have distinct access needs. No consumer should depend on methods it does not use. Apply this principle at the point where two consumers have non-overlapping access sets: split the interface there, not proactively. Document the split decision — and document the rejection of a split when considered but declined.

Common patterns in this project: `IHealthSnapshot` (read-only) vs `IHealthReport` (write-only) vs `IHealthAdmin` (admin); `IConfigProvider` (read-only) vs `IConfigManager` (write).

---

## P4 — Cross-cutting concern exception

Logger and HealthMonitor are referenced concretely in `USES (downward)` lists throughout both boards. This is accepted: Logger and HealthMonitor are infrastructure, not domain components. Their concrete reference avoids a proliferation of abstract logging/health interfaces that would add structural complexity without clarity benefit. Every such reference is documented in the companion's §1 Sources table and the exception is stated explicitly.

---

## P5 — Bounded resources; no dynamic allocation post-init

All RAM consumed by the firmware is allocated statically or from the stack, in bounded quantities, before the FreeRTOS scheduler starts (or in the FreeRTOS task-init path using static allocation APIs). No `malloc`, `calloc`, `realloc`, `free`, or equivalent heap allocation occurs after the scheduler is running. Buffers are sized to their maximum worst-case extent at compile time. This guarantees deterministic memory usage and eliminates heap fragmentation as a failure mode.

---

## P6 — Responsibility traces to requirements

Every module's existence, and every function in its public API, traces to at least one SRS requirement or use case documented in `components.md`. No speculative API surface is designed for hypothetical future requirements. If a function cannot be traced, it is removed. This principle is the primary guard against scope creep at the LLD layer.

---

## P7 — Pull-based downstream consumption

Producers do not push data to multiple consumers. Consumers read on their own schedule via a polling call or a subscription callback registered at init time. This keeps ownership unambiguous: the producer owns the data; the consumer decides when to read. Pull-based access simplifies mocking in tests (call the getter; inspect the result) and decouples activation schedules.

**Callback exception:** when a consumer must react promptly to an event (e.g., a data-ready interrupt from a sensor), the producer notifies the consumer via a registered callback or FreeRTOS notification. The callback is registered once at init; it does not create a runtime coupling between producer and consumer task stacks.

---

## P8 — Total error propagation; no silent failures

Every function that can fail returns an error code of type `<module>_err_t`. Callers must not ignore non-OK returns — this is enforced at code review. `void` return is reserved for functions that cannot fail by construction (e.g., a getter that reads from a volatile memory-mapped register with no hardware state). No error is swallowed silently inside a module; if an error cannot be handled locally, it is propagated to the caller or reported via `IHealthReport`.

---

## P9 — BARR-C coding standard; no floating-point unless mandated

All firmware C source follows BARR-C v2.0:

- Fixed-width integer types (`uint8_t`, `int32_t`, etc.) everywhere. No `int`, `long`, or platform-dependent sizes.
- No floating-point unless a hardware peripheral or protocol mandates it (e.g., a sensor that reports IEEE 754 values). When floating-point is unavoidable, it is isolated to the driver companion that owns the conversion; no float leaks into Middleware or Application layer types.
- Const-correct: every read-only pointer parameter is `const`; every read-only global is `const`.
- No implicit type conversions; casts are explicit and commented where non-obvious.

---

## P10 — Naming conventions

| Element | Convention | Example |
|---------|-----------|---------|
| Interface type | `i<module>_t` (lower-snake, `_t` suffix) | `igpio_t`, `irtc_t`, `imqtt_client_t` |
| Interface singleton | module name, no `i` prefix | `gpio_driver`, `rtc_driver`, `mqtt_client` |
| Component name (HLD) | PascalCase | `GpioDriver`, `TimeProvider`, `AlarmService` |
| File name | kebab-case | `gpio-driver.md`, `time-provider.md` |
| Module C prefix | snake_case | `gpio_`, `rtc_`, `time_provider_` |
| Error type | `<module>_err_t` | `gpio_err_t`, `ntp_client_err_t` |
| Error values | `<MODULE>_ERR_<CONDITION>` | `GPIO_ERR_OK`, `NTP_CLIENT_ERR_ALL_FAILED` |
| Enum values (non-error) | `<MODULE>_<VALUE>` | `GPIO_PORT_A`, `LIFECYCLE_STATE_INIT` |
| Struct fields | snake_case, no prefix | `slave_addr`, `expected_map_version` |
| Macros / constants | `UPPER_SNAKE_CASE` | `NTP_CLIENT_MAX_SERVERS`, `RTC_BACKUP_MAX_IDX_F469` |

Interface names in `components.md` start with `I` (PascalCase). The `i<module>_t` C type and the `I<Component>` interface name are a 1:1 mapping.

---

*Referenced by every LLD companion §3 Internal design — Principles applied subsection. Cite by principle number and short title (e.g., "P1 (Strict directional layering)").*
