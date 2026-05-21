# ResetDriver — LLD Companion

**Document:** `docs/lld/drivers/reset-driver.md`
**Version:** 0.1 (draft — pending Luca review)
**Board scope:** Gateway (B-L475E-IOT01A) only
**Layer:** Driver
**Status:** Draft
**Date:** May 2026

**HLD anchor:** ResetDriver in `components.md` (GW driver layer)

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Triggers a full software reset of the MCU | `components.md` |
| PROVIDES (upward) | `IReset` | `components.md` |
| USES (downward) | CMSIS | `components.md` |
| Root requirements | REQ-DM-010, REQ-NF-202 | `SRS.md` |
| Board | Gateway only | `components.md` — absent from Field Device driver list |

**Consumers:**

| Consumer | Layer | Task | Call site |
|---|---|---|---|
| `LifecycleController` | Application | `LifecycleTask` | SD-05 msg 13 — controlled restart (UC-17) |
| `UpdateService` | Application | `UpdateServiceTask` | SD-06c msg 8 — OTA bank swap; SD-06d msg 12' — rollback |

**REQ-DM-010:** "The system shall support a controlled software reset triggered by the operator via the CLI."
**REQ-NF-202:** "The system shall recover from a watchdog reset within 5 s." (Recovery time post-reset, not the reset trigger itself — cited here because `reset_trigger` is the gateway to the recovery path.)

---

## 2. Public API

### 2.1 Dependency-conformance check

`reset_driver.h` includes only `core_cm4.h` (via the CMSIS device header) for `NVIC_SystemReset()`. No FreeRTOS headers. Confirmed clean.

### 2.2 Return type rationale

`reset_trigger()` returns `void`. An error return type would imply the function can fail or return normally — it cannot. `NVIC_SystemReset()` is a one-way operation that clears the pipeline and resets the MCU within a few clock cycles. Returning any value would be unreachable code and would misrepresent the function's semantics.

### 2.3 Pre-condition — caller responsibility

`reset_trigger()` does not flush any in-RAM state to NV storage. All state that must survive the reset (restart flags, OTA pending flags, rollback flags) must be written to QSPI flash by the caller before invoking `reset_trigger()`. This is explicitly documented in both call sites:

- SD-05 msg 12: `LifecycleController` sets `restart_flag` in NV storage before calling msg 13 `reset_trigger()`.
- SD-06c msg 5: `FirmwareStore` writes `pending_self_check` flag before `UpdateService` calls msg 8 `reset_trigger()`.
- SD-06d msg 9'/10': `FirmwareStore` sets rollback flags before msg 12' `reset_trigger()`.

The driver cannot enforce this ordering — it is a caller contract documented here and in the consumer LLD companions.

### 2.4 Public API (`reset_driver.h`)

```c
/**
 * @brief Trigger an immediate full MCU software reset.
 *
 * Issues NVIC_SystemReset() (CMSIS core_cm4.h). All in-RAM state is
 * lost. This function does not return.
 *
 * Pre-condition: the caller must have flushed all state that must
 * survive the reset to non-volatile storage before calling this function.
 *
 * Called from:
 *   - LifecycleController (LifecycleTask) for controlled restart (UC-17).
 *   - UpdateService (UpdateServiceTask) for OTA bank swap (SD-06c) and
 *     rollback (SD-06d).
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
void reset_trigger(void);
```

---

## 3. Internal design

### 3.0 Private struct

```c
/* No mutable runtime state — reset_trigger() is a one-shot stateless call. */
typedef struct { uint8_t _reserved; } reset_driver_t;
```


### 3.1 Implementation

```c
void reset_trigger(void)
{
    __DSB(); /* ensure all pending memory writes complete before reset */
    NVIC_SystemReset();
    /* unreachable */
}
```

The `__DSB()` (Data Synchronisation Barrier) instruction ensures that any pending write-buffer drain completes before the reset is asserted. Without it, a write issued immediately before `NVIC_SystemReset()` (e.g., a GPIO toggle for debugging) may not complete. This is a defensive measure; in practice, the callers have already completed their NV writes before calling `reset_trigger()`.

`NVIC_SystemReset()` is defined in `core_cm4.h` as an inline function that writes to `SCB->AIRCR` with the `SYSRESETREQ` bit, then enters a `while(1)` loop. The MCU resets within a few clock cycles.

### 3.2 Module-level state

None. The function is stateless.

### 3.3 No ISR, no DMA, no singleton considerations

`reset_trigger()` is called from task context only. There is no peripheral to initialise, no state to manage, and no init function required. This driver has no `reset_init()`.

---

### 3.4 Principles applied

- **P1 (Strict directional layering).** Depends only on CMSIS SCB register (NVIC_SystemReset); no RTOS, no middleware.
- **P2 (Dependency Inversion).** Exposes `ireset_driver_t` vtable; LifecycleController (GW) depends on `IResetDriver`.
- **P5 (Bounded resources, no dynamic allocation post-init).** No internal state; the reset call is a one-shot hardware operation.
- **P6 (Responsibility traces to requirements).** `reset_driver_trigger_reset()` traces to REQ-DM-030 (soft-reset after OTA self-check).
- **P8 (Total error propagation, no silent failures).** Function returns void — cannot fail by construction; NVIC_SystemReset has no error path.
- **P10 (Naming conventions).** Prefix `reset_driver_`; interface `IResetDriver` -> `ireset_driver_t`.

### reset_trigger

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).


## 4. Hardware contract

`NVIC_SystemReset()` writes to `SCB->AIRCR[SYSRESETREQ]` (bit 2), which requests a system reset from the MCU's SCS (System Control Space). This is a Cortex-M4 architectural feature — identical on both STM32F469 and STM32L475. Because ResetDriver is Gateway-only there is no cross-target concern here.

The reset clears all CPU registers, peripheral registers (except those in the backup domain), and SRAM. It does not reset the debug interface, which is why ST-Link survives a software reset during debugging.

---

## 5. Sequence integration

`ResetDriver` appears in three sequence diagrams:

| SD | Message | Timing constraint |
|---|---|---|
| SD-05 (controlled restart) | msg 13: `LifecycleController → ResetDriver` sync | recovery ≤ 5 s (REQ-NF-203) |
| SD-06c (OTA bank swap) | msg 8: `UpdateService → ResetDriver` sync | recovery ≤ 5 s (REQ-NF-203) |
| SD-06d (rollback) | msg 12': `UpdateService → ResetDriver` sync | rollback total ≤ 10 s (REQ-NF-204) |

No SD changes required.

---

## 6. Error and fault behaviour

None applicable. The function cannot fail and does not return.

---

## 7. Unit-test plan

`NVIC_SystemReset()` cannot be called in a host-platform test (it would reset or crash the test runner). The test strategy is to mock it via a weak-symbol override or function pointer substitution.

| ID | Test case | Expected result |
|---|---|---|
| T-RST-01 | Call `reset_trigger()` with `NVIC_SystemReset` mocked to set a global flag | Mock flag is set; `__DSB()` issued before mock call |
| T-RST-02 | Verify `reset_driver.h` compiles cleanly without `FreeRTOS.h` on the include path | No compilation errors or warnings |

Test file: `tests/drivers/test_reset_driver.c`.

---

## 8. Open items

None. This driver has no unresolved dependencies. The implementation is complete at the companion stage — the code is one line plus a DSB.

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| RSTD-D1 | Return type `void` | The function cannot return; any other return type would be misleading |
| RSTD-D2 | `__DSB()` before `NVIC_SystemReset()` | Defensive memory barrier; prevents a hypothetical write-buffer stall from leaving peripheral state inconsistent at reset |
| RSTD-D3 | No `reset_init()` | No peripheral to configure; no state to initialise; adding an init function would be ceremony without substance |
| RSTD-D4 | Pre-condition documented, not enforced | The driver cannot know whether the caller has flushed NV state; this is a caller contract. Enforcing it would require the driver to inspect application-level state, violating P1 |
