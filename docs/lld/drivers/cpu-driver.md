# CpuDriver — LLD Companion

**Document:** `docs/lld/drivers/cpu-driver.md`
**Version:** 1.0
**Board scope:** Gateway (B-L475E-IOT01A) and Field Device (STM32F469I-DISCO)
**Layer:** Driver (platform singleton, not an ADT)
**Status:** Implemented on both boards and hardware-validated 2026-11-07 (section 10). Gateway: existing driver with the changes of section 9. Field Device: new, panic subset only.
**Date:** November 2026

**HLD anchor:** CpuDriver, driver layer, both boards. This document replaces the placeholder `panic.md` ("Panic" middleware). The fault diagnostics are part of CpuDriver, as they already are on the Gateway, so there is no separate Panic module on either board.

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Platform services that exist once per MCU: fault capture and reporting (panic), software reset, and on the Gateway the clock tree and the cycle-counter delays | `cpu.c` header comment; placeholder `panic.md` |
| PROVIDES (upward) | Direct API in `cpu.h` (no vtable): `cpu_init`, `cpu_panic`, `cpu_reset`, `CPU_ASSERT`; Gateway only: `cpu_delay_us`, `cpu_delay_ms`, `cpu_get_sysclk_hz`, `cpu_get_pclk1_hz`, `cpu_get_pclk2_hz`, `cpu_get_vector_table_base` | this document |
| USES (downward) | CMSIS only (`SCB`, `DWT`, `CoreDebug`, `RTC` backup registers, `PWR`, `RCC`, one USART and one GPIO port). Never `ILogger`, `IRtc`, `IDebugUart` or any RTOS call | `hld.md` §9; placeholder `panic.md` |
| Consumers | `main()` of both boards (`cpu_init`), the FreeRTOS hooks (`cpu_panic`), any module (`CPU_ASSERT`), `ResetDriver` (Gateway, shares `cpu_hw.h`), `WifiDriver` (Gateway, delays) | code |
| SRS | No requirement names fault diagnosis. The reset it ends in is bound by REQ-NF-202 and REQ-NF-203 (restart and resume within 5 s). The placeholder's "REQ-NF-DIAG" was never added | `SRS.md` |

**Why a direct API and a singleton.** There is one CPU, one fault vector set and one clock tree, and the fault path must work when nothing else does, so it cannot be reached through an interface pointer that a corrupted object could break. This is the same documented deviation from the ADT default that the Gateway code already carries.

**Why one name.** The placeholder was called Panic. The Gateway built the same job inside CpuDriver (`cpu_panic`). The Field Device uses the same module name, directory (`drivers/cpu/`), file names and function names, so a reader moving between boards finds the same module.

## 2. Public API (`cpu.h`)

Common to both boards:

```c
typedef enum {
    CPU_PANIC_HARDFAULT = 0x01u, CPU_PANIC_BUSFAULT = 0x02u, CPU_PANIC_MEMMANAGE = 0x03u,
    CPU_PANIC_USAGEFAULT = 0x04u, CPU_PANIC_ASSERT = 0x05u, CPU_PANIC_STACK_OVERFLOW = 0x06u,
    CPU_PANIC_MALLOC_FAILED = 0x07u, CPU_PANIC_WATCHDOG = 0x08u, CPU_PANIC_USER = 0x09u
} cpu_panic_source_t;

status_t cpu_init(void);
_Noreturn void cpu_reset(void);
_Noreturn void cpu_panic(cpu_panic_source_t source, const char *reason);
#define CPU_ASSERT(expr)   /* DEBUG builds only: cpu_panic(CPU_PANIC_ASSERT, "expr @ file:line") */
```

| Function | Behaviour | Threading |
|---|---|---|
| `cpu_init` | Gateway: clock tree, wait states, LSE, DWT (section 3.1), then step 3 below. Field Device: enables the Bus, Memory and Usage fault handlers, enables the DWT cycle counter if it is off, then step 3. Step 3: if a panic record from the previous boot is valid, print it on the panic UART and invalidate it. Returns `STATUS_OK`; Gateway also `STATUS_ERR_TIMEOUT` (PLL or LSE did not lock) and `STATUS_ERR_HW` (no DWT); Field Device `STATUS_ERR_HW` (no DWT). | Once, from `main()`, before the scheduler. The Field Device calls it right after `system_clock_init()` because the panic UART baud rate depends on the APB1 clock. |
| `cpu_reset` | `CPU_HW_DATA_BARRIER()` then `NVIC_SystemReset()`. Does not return. | Any task or ISR. |
| `cpu_panic` | Section 3.2. Does not return. | Any context, including fault handlers and ISRs. No lock, no allocation, no RTOS call. |
| `CPU_ASSERT` | Compiled out unless `DEBUG` is defined. | Any context. |

`status_t` comes from `cpu/status.h`. The Field Device gets its own copy of that header (same content) because the two board trees never share files.

Gateway only (unchanged, section 3.1): `cpu_delay_us`, `cpu_delay_ms`, `cpu_get_sysclk_hz`, `cpu_get_pclk1_hz`, `cpu_get_pclk2_hz`, `cpu_get_vector_table_base`. They are not ported: the Field Device clock lives in `Src/system_clock.c` and has no consumer of these functions.

Test-only (`#ifdef TEST`): `cpu_reset_for_test`, `cpu_cfsr_cause_string_for_test`, `cpu_fault_entry`.

`cpu_fault_entry(uint32_t *frame, cpu_panic_source_t source)` is called by the assembly stubs in `cpu_fault.c`. It is a link-time symbol on target (not declared in `cpu.h` outside tests).

## 3. Internal design

### 3.1 Gateway-only services (as built)

Clock: MSI 4 MHz into the PLL (M 1, N 40, R 2), 80 MHz SYSCLK, Flash 4 wait states, PCLK1 and PCLK2 80 MHz, LSE started for the RTC, DWT cycle counter on. Delays spin on `DWT->CYCCNT` (wrap-safe by unsigned subtraction). These are unchanged by this document.

### 3.2 Panic path (both boards)

Private state: `g_panic_active` (bool), `s_fault_frame` (pointer to the stacked exception frame, or NULL), all in `.bss`. A fixed print buffer is not needed because the output is written byte by byte.

Flow of `cpu_panic(source, reason)`:

1. If `g_panic_active` is already set (a fault inside the panic path): `CPU_HW_BREAKPOINT()` in DEBUG builds, then `CPU_HW_SYSTEM_RESET()`. Nothing else is touched.
2. Set `g_panic_active`, `CPU_HW_DISABLE_IRQ()`.
3. Read `SCB->CFSR` and `SCB->HFSR`, and `SCB->BFAR` only when `CFSR.BFARVALID` (bit 15) is set; otherwise BFAR is recorded as 0, because the register keeps a stale value after a fault that is not a bus fault.
4. Write the post-mortem record to the RTC backup registers (section 3.3). The record is written before any output, because the UART path is the part most likely to hang.
5. Emit the report on the panic UART (section 3.4).
6. DEBUG build: `CPU_HW_BREAKPOINT()` then spin forever (a debugger lands here). Other builds: wait for the UART to drain, then `CPU_HW_SYSTEM_RESET()`.

Fault vectors (`cpu_fault.c`): `HardFault_Handler`, `BusFault_Handler`, `MemManage_Handler`, `UsageFault_Handler`. Each stub is a `naked` function (no compiler prologue, so MSP is exactly what the hardware left) that tests bit 2 of `EXC_RETURN` in LR to pick MSP or PSP, loads the frame pointer into R0 and its own `cpu_panic_source_t` into R1, and branches to `cpu_fault_entry(frame, source)`, which stores the frame pointer and calls `cpu_panic(source, NULL)`. The stubs live in their own translation unit so the host tests link `cpu.c` without assembly.

FreeRTOS hooks (`Src/freertos_hooks.c`, both boards): `vApplicationStackOverflowHook(task, name)` calls `cpu_panic(CPU_PANIC_STACK_OVERFLOW, name)`. There is no malloc-failed hook: both images use static allocation only (`configSUPPORT_DYNAMIC_ALLOCATION` is 0), so the kernel never calls it. `CPU_PANIC_MALLOC_FAILED` and `CPU_PANIC_WATCHDOG` stay in the enum so the record format does not change; no code raises them today (a watchdog reset is reported through the reset cause, not through a panic).

### 3.3 Post-mortem record

Stored in `CPU_PANIC_RECORD_WORDS` (17) consecutive RTC backup registers starting at index `CPU_PANIC_BKP_BASE`, which is **2** on both boards. Index 0 belongs to TimeProvider (`TIME_PROVIDER_BKUP_REG`, REQ-TS-040, REQ-NF-212) and index 1 is kept free. The F469 has registers 0 to 19 and the L475 0 to 31, so 2 to 18 fits both.

| Offset | Content | Offset | Content |
|---|---|---|---|
| 0 | magic `0xDEADC0DE` (written last, cleared after the report) | 9 | R1 |
| 1 | CFSR | 10 | R2 |
| 2 | HFSR | 11 | R3 |
| 3 | BFAR | 12 | R12 |
| 4 | PC | 13 | source (`cpu_panic_source_t`) |
| 5 | LR | 14 | DJB2 hash of `reason` (0 for NULL) |
| 6 | SP (the frame address; 0 when there is no frame) | 15 | reserved, 0 |
| 7 | xPSR | 16 | uptime hint in ms (`DWT->CYCCNT / (SYSCLK / 1000)`) |
| 8 | R0 | | |

Without a frame (assert, hook, user panic) offsets 4 to 12 are 0. Writing needs backup-domain access: set `RCC` PWR clock enable and `PWR` DBP. The magic is written last so a partial record is never reported.

The uptime hint wraps every 53.7 s at 80 MHz and 23.9 s at 180 MHz (32-bit cycle counter). It is a hint only; see CPU-O2.

### 3.4 Panic UART

A one-shot, polled, transmit-only path that does not use `DebugUartDriver`, DMA, interrupts or any buffer.

| Board | Peripheral | Pin | Clock | Baud |
|---|---|---|---|---|
| Gateway | USART1 | PB6, AF7 | PCLK2 80 MHz, BRR 694 | 115 200 8N1 |
| Field Device | USART3 | PB10, AF7 | PCLK1 45 MHz, BRR 391 | 115 200 8N1 |

These are the same peripheral and pin as the board's DebugUartDriver, so the report appears on the console the operator already watches. The path enables the GPIO and USART clocks, sets the pin to alternate function, and writes CR1 with only TE and UE. This switches the console receiver off, which is acceptable because interrupts are already disabled and the next step is a reset or a halt.

Every wait on TXE (and on TC at the end) is bounded by `CPU_POLL_TIMEOUT` loop passes; on expiry the byte or the drain is abandoned. A hang in the panic path would defeat the reset.

Report format (one block, identical on both boards):

```text
========== PANIC ==========
Source: <name>
CFSR:   0x........
Cause:  <first set CFSR bit, text>
HFSR:   0x........
BFAR:   0x........
PC:     0x........ (<symbol>)        <- only when a frame exists
LR:     0x........ (<symbol>)
xPSR:   0x........
R0-R3:  0x........ 0x........ 0x........ 0x........
R12:    0x........
Reason: <text or (none)>
================================
```

The next boot prints the stored record under `--- POST-MORTEM (previous boot) ---` (source, CFSR, cause, HFSR, BFAR, PC, LR) and invalidates it.

Symbols: the Gateway resolves PC and LR through a table generated from the linker map (`scripts/gen-panic-symbols.py`, `panic_symbols.c`, `panic_symbols_data.c`, gitignored, weak empty fallback). The Field Device prints the raw addresses in this version; decode them with `addr2line` (CPU-O3).

### 3.5 Debug and release behaviour

`DEBUG` selects halt (breakpoint and spin) or reset. A DEBUG image with no debugger attached ends in lockup after the BKPT, so the independent watchdog (REQ-NF-109), when running, is what resets it. Release images reset directly. Decision CPU-D3.

### 3.6 Design rules applied

One responsibility: platform-wide services only, with no policy (the panic does not decide what to log or publish). No interface indirection, for the reason in section 1. Fail safe: every wait is bounded and the second entry takes the short path. No hidden state: all state is the two statics above and the backup registers. Testable: every privileged intrinsic goes through `cpu_hw.h`, which maps to counters in the host mock.

---

## 4. Hardware contract

| Item | Gateway (STM32L475) | Field Device (STM32F469) |
|---|---|---|
| Fault registers | `SCB->CFSR`, `HFSR`, `BFAR`, `SHCSR` | same (Cortex-M4 core registers) |
| Fault handler enable | `SCB->SHCSR` MEMFAULTENA, BUSFAULTENA, USGFAULTENA set in `cpu_init` | same |
| Backup registers | `RTC->BKP0R..BKP31R`, record at 2 to 18 | `RTC->BKP0R..BKP19R`, record at 2 to 18 |
| Backup write access | `RCC->APB1ENR1.PWREN`, `PWR->CR1.DBP` | `RCC->APB1ENR.PWREN`, `PWR->CR.DBP` |
| Panic UART | USART1 (`ISR`/`TDR`, flags TXE, TC) on PB6 | USART3 (`SR`/`DR`, flags TXE, TC) on PB10 |
| GPIO clock | `RCC->AHB2ENR.GPIOBEN` | `RCC->AHB1ENR.GPIOBEN` |
| Cycle counter | `CoreDebug->DEMCR.TRCENA`, `DWT->CTRL.CYCCNTENA`, `DWT->CYCCNT` | same |
| Reset | `NVIC_SystemReset()` | same |
| Fault stack | The handlers run on MSP. A stack overflow of a task is on PSP, so the report still has a valid MSP | same |

NVIC: none used. Clock dependencies: the panic UART baud rate is computed for the clocks above; if a board's clock configuration changes, `CPU_PANIC_UART_BRR` changes with it.

## 5. Sequence integration

| Message | Producer | Effect |
|---|---|---|
| `cpu_init()` | `main()` first statement (Gateway) or right after `system_clock_init()` (Field Device) | Platform ready; previous panic reported once |
| Fault vector | Hardware | `cpu_fault_entry` then `cpu_panic` |
| `vApplicationStackOverflowHook` | FreeRTOS (context switch) | `cpu_panic(CPU_PANIC_STACK_OVERFLOW, task name)` |
| `CPU_ASSERT(expr)` | Any module, DEBUG | `cpu_panic(CPU_PANIC_ASSERT, ...)` |
| `cpu_reset()` | ResetDriver path, operator `/control restart` on the Gateway | Orderly reset, no record |

Boot order with a stored record: reset, `cpu_init`, post-mortem lines on the console, then the normal boot. `LifecycleController` reports the reset cause separately (RCC flags); a panic reset shows as a software reset there and the post-mortem lines name the panic.

## 6. Error and fault behaviour

| Failure | Handling |
|---|---|
| PLL or LSE does not lock (Gateway) | `cpu_init` returns `STATUS_ERR_TIMEOUT`; the caller halts |
| No DWT | `cpu_init` returns `STATUS_ERR_HW` |
| UART never ready | The byte is dropped after `CPU_POLL_TIMEOUT` passes; the panic continues to the reset |
| Fault inside the panic path | Second entry: breakpoint (DEBUG) and reset only |
| Corrupt backup registers | A wrong magic is ignored. A right magic with garbage fields prints garbage once and is cleared |
| Reset loop | A panic early in boot resets again and again; the record is overwritten each time. There is no counter (CPU-O4) |

Observability: the console report and the next-boot post-mortem. Nothing is sent to the cloud.

## 7. Unit-test plan

Host tests mock `SCB`, `DWT`, `CoreDebug`, `RTC`, `PWR`, `RCC`, the USART and `GPIOB`, and count the `cpu_hw` intrinsics. The Gateway suite is `tests/gateway/drivers/cpu/test_cpu.c` (TC-CPU-001 to 040). The Field Device suite is `tests/field-device/drivers/cpu/test_cpu_fd.c` with the same IDs for the shared behaviour.

| ID | Case | Boards |
|---|---|---|
| TC-CPU-001..008, 014, 040 | Clock tree, LSE, getters, vector table base | Gateway |
| TC-CPU-010..013 | Delays, wrap-around, zero | Gateway |
| TC-CPU-020 | Panic disables interrupts on entry | both |
| TC-CPU-021 | Magic written, at `CPU_PANIC_BKP_BASE`, not at index 0 | both |
| TC-CPU-022 | CFSR, HFSR, BFAR stored; BFAR recorded as 0 when `BFARVALID` is clear (022b) | both |
| TC-CPU-023 | Frame PC, LR, xPSR, R0..R3, R12 and SP stored | both |
| TC-CPU-024, 025 | Panic active flag; recursive entry takes the short path | both |
| TC-CPU-026 | UART configured and written; waits for TC before the reset | both |
| TC-CPU-027 | All 17 CFSR bits decode; unknown text | both |
| TC-CPU-030..033 | Post-mortem printed, magic cleared, bad magic ignored; Field Device 033: no record, nothing printed. The Gateway 033 also checks the cause text | both |
| TC-CPU-034 | A record in registers 2 to 18 does not touch register 0 or 1 | both |
| TC-CPU-035 | Each fault stub source (Hard, Bus, MemManage, Usage) reaches the record | both |
| TC-CPU-036 | `cpu_init` sets MEMFAULTENA, BUSFAULTENA, USGFAULTENA | both |
| TC-CPU-038 | `cpu_init` (Field Device) returns `STATUS_OK` and enables the cycle counter. The `STATUS_ERR_HW` branch cannot be reached on the host (the mock keeps a written bit) and the Cortex-M4 always has the counter | Field Device |
| TC-CPU-039 | DEBUG halts, release resets (checked through the counters; the host build runs the release branch) | both |
| TC-CPU-041 | `cpu_reset` issues a barrier before the reset | both |

Hardware (`integration-tests/cpu/main_test_cpu.c` on each board; the run is unattended, see the file header):

| ID | Case |
|---|---|
| TC-HW-CPU-007 | Forced fault (null-pointer read, divide trap, unaligned access with the trap on, undefined instruction): report on the console, reset, post-mortem on the next boot |
| TC-HW-CPU-008 | `cpu_reset()` leaves no post-mortem |
| TC-HW-CPU-009 | `CPU_ASSERT(false)` in a release build compiles out; in DEBUG it panics (not run on hardware) |
| TC-HW-CPU-010 | Task stack overflow raises `StackOverflow` with the task name (`vApplicationStackOverflowHook`) |
| TC-HW-CPU-011 | TimeProvider's sync flag in register 0 survives a panic reset |
| TC-HW-CPU-012 | Second fault inside the panic path does not loop (host test TC-CPU-024/025 only) |

## 8. Open items

| ID | Item | Resolution path |
|---|---|---|
| CPU-O1 | No SRS requirement for fault diagnosis | Add one in the next SRS revision; the companion traces to REQ-NF-202/203 until then |
| CPU-O2 | The uptime hint wraps in under a minute | Replace by the RTC sub-second clock or a free-running 32 kHz timer when a use for it appears |
| CPU-O3 | Field Device has no symbol resolution | Port `panic_symbols.c` and the generator to the F469 map; the generator is already map-driven |
| CPU-O4 | No reset-loop counter | Count consecutive panic resets in the spare register 1 and stop resetting after N, on a later requirement |
| CPU-O5 | The Gateway symbol table is gitignored and goes stale when the image changes (observed in gateway-main F-9: the dump named the wrong function) | Print the image's build id with the report so a mismatched table is visible |
| CPU-O6 | Malloc-failed and watchdog sources are never raised | Keep until a dynamic allocator or a watchdog pre-warning exists |
| CPU-O7 | Enabling the Bus, Memory and Usage fault handlers changes the vector some faults take (they no longer escalate to HardFault) | Confirmed by TC-HW-CPU-007 on both boards |

## 9. Decisions log and changes against the Gateway code

| ID | Decision | Rationale |
|---|---|---|
| CPU-D1 | The placeholder Panic module is folded into CpuDriver on both boards | The Gateway already built it there; one name for one job |
| CPU-D2 | The record moves from backup registers 0 to 16 to 2 to 18 on the Gateway as well | Register 0 is TimeProvider's sync flag on both boards. Before this change a panic overwrote it with the panic magic, and the reported-and-cleared record then left it at 0, so every panic dropped the time-sync state |
| CPU-D3 | `DEBUG` selects halt or reset; the existing behaviour is kept | Matches the bench images |
| CPU-D4 | Write the record before the report; wait for TC before the reset | The old order lost the record if the UART hung; the old reset could cut the last bytes of the report |
| CPU-D5 | Each fault stub is `naked`, passes its own source, and the frame address is stored as SP; `cpu_init` enables the three configurable fault handlers | Before, the stubs were ordinary C functions: at `-O0` the compiler's `push {r7}` ran before the assembly, so MSP was 4 bytes low and the report printed the stacked LR as PC and R12 as LR (seen on the Gateway disassembly of `HardFault_Handler`, and the likely cause of the wrong function name in gateway-main F-9). Every stub also reported `HardFault` and SP was always 0 |
| CPU-D6 | The stack-overflow hook calls `cpu_panic` on both boards | The hooks only spun; the report and the reset were missing |
| CPU-D7 | The Field Device is the panic subset: no clock, no delays | Its clock is `system_clock.c`; nothing calls delays |
| CPU-D8 | Test projects: the Gateway CpuDriver, ResetDriver and WifiDriver tests run from `project_gateway.yml`, the Field Device CpuDriver tests from `project.yml`. CI runs both projects | Same file names on both boards (`cpu.c`, `cpu.h`, `cpu_hw.h`) and a test that mocks `cpu.h` (ResetDriver, WifiDriver) would otherwise get the other board's header. CI ran only `project.yml`, so moving those tests needed the second CI step |
| CPU-D9 | BFAR is recorded only when `CFSR.BFARVALID` is set | The first Field Device run printed `BFAR 0xE000ED38` (stale) for an undefined-instruction fault |

## 10. Hardware validation (2026-11-07)

Both harnesses run unattended: a step counter and a sentinel in RTC backup registers 31 (Gateway) and 19 (Field Device) and 0 make each boot run the next fault and the next boot print the post-mortem. Built without `DEBUG` so a panic resets.

| ID | Result |
|---|---|
| TC-HW-CPU-007 | PASS on both boards. Bus fault (read of `0x2FFF0000`): `BusFault`, `CFSR 0x00008200`, `BFAR 0x2FFF0000`. Undefined instruction: `UsageFault`, `CFSR 0x00010000`. Divide by zero with `CCR.DIV_0_TRP`: `UsageFault`, `CFSR 0x02000000`. User panic: `User`, no frame. Each report was followed by a reset and a matching post-mortem |
| PC check | The reported PC resolves with `addr2line` to the faulting source line (bus fault, undefined instruction and divide) on both boards. The old Gateway stubs would have printed the stacked LR here (CPU-D5) |
| TC-HW-CPU-008 | PASS on both boards. `cpu_reset()` left no post-mortem |
| TC-HW-CPU-010 | PASS on the Field Device. A task overrunning a 96-word stack raised `StackOverflow` with `Reason: ovf_task`, then reset. Not run on the Gateway (same hook code, no kernel in the minimal Gateway harness) |
| TC-HW-CPU-011 | PASS on both boards. Backup register 0 kept its sentinel across every panic reset |
| Product images | Field Device (RAM 62 664 B of 256 KB) and Gateway (RAM 97 056 B of 96 KB region, 98.73 %, 88 B more than before) boot to Operational |
| Not run | TC-HW-CPU-009 (assert), TC-HW-CPU-012 (nested fault), the Gateway stack overflow, and the DEBUG halt with a debugger |
