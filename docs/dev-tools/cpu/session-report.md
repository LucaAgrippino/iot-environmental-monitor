# Session Report — CpuDriver (Gateway)

**Date:** 2026-06-29
**Branch:** feature/phase-4-gw-cpu
**Companion:** docs/lld/gateway/drivers/cpu-driver-companion.md

---

## Files produced

| File | Lines | Notes |
|------|-------|-------|
| firmware/gateway/drivers/cpu/cpu.h | ~130 | Public API, singleton exception, `CPU_ASSERT`, `_Noreturn` stripped under `#ifdef TEST`, `cpu_fault_entry` / `cpu_reset_for_test` / `cpu_cfsr_cause_string_for_test` exposed under `#ifdef TEST` |
| firmware/gateway/drivers/cpu/cpu.c | ~540 | Clock init (MSI→PLL 80 MHz), DWT spin-delay, panic subsystem, CFSR 17-bit decode table, UART1 one-shot init, RTC backup-register post-mortem, FreeRTOS hooks |
| firmware/gateway/drivers/cpu/cpu_hw.h | ~30 | Macro indirection: `CPU_HW_DISABLE_IRQ`, `CPU_HW_BREAKPOINT`, `CPU_HW_SYSTEM_RESET` — expand to `__disable_irq`/`__BKPT(0)`/`NVIC_SystemReset()` on target, counter stubs in test builds |
| firmware/gateway/drivers/cpu/cpu_fault.c | ~30 | Four assembly ISR trampolines (`HardFault_Handler`, `BusFault_Handler`, `MemManage_Handler`, `UsageFault_Handler`) using TST LR #4 / ITE EQ pattern to select MSP vs PSP before calling `cpu_fault_entry` |
| firmware/shared/status.h | 14 | New project-wide `status_t` enum (`STATUS_OK`, `STATUS_ERR_TIMEOUT`, `STATUS_ERR_HW`, `STATUS_ERR_NULL_PTR`, `STATUS_ERR_NO_RESOURCE`, `STATUS_ERR_INVALID_ARG`) |
| tests/gateway/drivers/cpu/test_cpu.c | ~420 | 25 unit tests; `arrange_pll_ready()` helper pre-sets PLLRDY + SWS bits; all TC IDs from §7 of companion |
| tests/support/cpu_driver.h | 4 | Ceedling trigger stub — causes `cpu.c` to be auto-linked into the test binary |

---

## Reused infrastructure (extended)

| File | Status | Symbols added |
|------|--------|---------------|
| tests/mocks/stm32l475xx.h | extended | `RCC_TypeDef`: `CR`, `CFGR`, `PLLCFGR`, `APB2ENR` fields + 20+ bit constants; `PWR_TypeDef` (CR1/VOS); `FLASH_TypeDef` (ACR/LATENCY); `CoreDebug_TypeDef` (DEMCR); `DWT_TypeDef` (CTRL, CYCCNT); `SCB_TypeDef` (CFSR, HFSR, BFAR, MMFAR); `RTC_TypeDef` (BKP0R–BKP16R); `USART1` instance; `cpu_hw_*` stub externs |
| tests/mocks/stm32l475_cmsis_mock.c | extended | Storage for `PWR`, `FLASH`, `CoreDebug`, `DWT`, `SCB`, `RTC`, `USART1`; `cpu_hw_disable_irq()` / `cpu_hw_breakpoint()` / `cpu_hw_system_reset()` counter-incrementing stubs; all fields zeroed in `stm32l475_cmsis_mock_reset()` |
| tests/project.yml | extended | `gateway/drivers/**` added to `:paths:test:`; `../firmware/gateway/drivers/**` added to `:paths:source:`; `:test_cpu:` define block (`STM32L475xx`, `TEST`) |
| cppcheck-suppressions.txt | extended | Three file-scoped suppressions for `cpu.c`: `staticFunction` (public API with all callers in-TU), `duplicateExpression` (volatile `DWT->CYCCNT` re-read), `constParameterPointer` (FreeRTOS ABI requires `char*`) |

---

## Unit test results

| Suite | Tests | Passed | Failed | Ignored |
|-------|-------|--------|--------|---------|
| test_cpu | 25 | 25 | 0 | 0 |

---

## TC coverage

| LLD section | TCs specified | TCs implemented |
|-------------|---------------|-----------------|
| §3.1 Clock init | TC-CPU-001..005 | all |
| §3.2 DWT delay | TC-CPU-010..013 | all |
| §3.3 Panic subsystem | TC-CPU-020..027 | all |
| §3.4 Fault entry | TC-CPU-030..031 | all |
| §3.5 Post-mortem read-back | TC-CPU-040..043 | all |
| §3.6 CFSR decode | TC-CPU-027 (shared) | all |
| §3.7 FreeRTOS hooks | TC-CPU-050..051 | all |
| §3.8 Accessors / reset helper | TC-CPU-060..061 | all |

---

## Design decisions

1. **Singleton exception** — `cpu_` free functions instead of ADT handle. Documented in companion §1.3 as the single authorised exception: only one CPU exists, and a handle pool would add indirection with no benefit.

2. **`cpu_fault.c` split** — Assembly ISR trampolines live in a separate file so Ceedling never compiles them for host tests (x86 assembler cannot process `TST LR, #4` / `MRS`). `cpu_fault_entry()` lives in `cpu.c` where it can be called directly by TC-CPU-030.

3. **`cpu_hw.h` indirection** — `__disable_irq()`, `__BKPT(0)`, `NVIC_SystemReset()` are Cortex-M intrinsics with no host equivalent. Each maps to a counter-incrementing stub under `#ifdef TEST`, making interrupt-disable and breakpoint/reset calls assertable.

4. **`_Noreturn` stripped in tests** — `cpu_panic` and `cpu_reset` are declared `_Noreturn` on target. Under `#ifdef TEST`, the keyword is removed so Unity can make assertions after the call returns (the function body simply does not branch to reset in test builds).

5. **DWT spin-loop guard** — The mock `DWT->CYCCNT` never auto-advances, so the spin loop would hang. Under `#ifdef TEST`, the loop exits after `CPU_DELAY_MAX_ITER = 3` iterations. The PLL poll loops use `CPU_POLL_TIMEOUT = 3` for the same reason.

6. **FreeRTOS hooks use `void *`** — `TaskHandle_t` is `typedef void *` in FreeRTOS. Using `void *` directly avoids pulling FreeRTOS headers into the driver layer (a Tier 0 CMSIS-only module).

7. **RTC backup domain used for post-mortem** — The 17 32-bit BKP registers (BKP0R–BKP16R) survive a system reset on the STM32L475, making them the natural location for a post-mortem record. Magic word `0xDEADC0DE` is written to BKP0R last — this is the atomic commit flag that `check_panic_record()` checks on the next boot.

8. **DJB2 hash for reason string** — 32-bit DJB2 over the `reason` C-string is stored in BKP14R. This lets post-mortem analysis identify the panic source even when the reason string is not in the backup domain.
