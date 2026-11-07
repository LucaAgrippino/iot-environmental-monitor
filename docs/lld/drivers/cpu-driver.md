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
| SRS | REQ-NF-507 (added in SRS v1.6 for this module): record the cause, report it, restart, report it once at the next start. Related: REQ-NF-504 (diagnostic output), REQ-NF-202/203 (restart within 5 s) | `SRS.md` |

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
| `cpu_init` | Gateway: clock tree, wait states, LSE, DWT (section 3.1), then step 3 below. Field Device: enables the Bus, Memory and Usage fault handlers, then step 3. Step 3: if a panic record from the previous boot is valid, print it on the panic UART and invalidate it. Returns `STATUS_OK`; the Gateway also returns `STATUS_ERR_TIMEOUT` (PLL or LSE did not lock) and `STATUS_ERR_HW` (no DWT). The Field Device always returns `STATUS_OK` and keeps `status_t` so the call matches. | Once, from `main()`, before the scheduler. The Field Device calls it right after `system_clock_init()` because the panic UART baud rate depends on the APB1 clock. |
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

1. If `g_panic_active` is already set (a fault inside the panic path): go straight to step 6 without any output.
2. Set `g_panic_active`, `CPU_HW_DISABLE_IRQ()`.
3. Read `SCB->CFSR` and `SCB->HFSR`, and `SCB->BFAR` only when `CFSR.BFARVALID` (bit 15) is set; otherwise BFAR is recorded as 0, because the register keeps a stale value after a fault that is not a bus fault.
4. Write the post-mortem record to the RTC backup registers (section 3.3). The record is written before any output, because the UART path is the part most likely to hang.
5. Emit the report on the panic UART (section 3.4).
6. End (`panic_end`): in a `DEBUG` build with a debugger attached (`CoreDebug->DHCSR.C_DEBUGEN`) execute `CPU_HW_BREAKPOINT()` and spin. Otherwise wait (bounded) for the UART to drain, when a report was written, and `CPU_HW_SYSTEM_RESET()`.

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
| 6 | SP (the frame address; 0 when there is no frame) | 15 | RTC time (`RTC->TR`, BCD hh:mm:ss) |
| 7 | xPSR | 16 | RTC date `RTC->DR` (BCD) |
| 8 | R0 | | |

Without a frame (assert, hook, user panic) offsets 4 to 12 are 0. Writing needs backup-domain access: set `RCC` PWR clock enable and `PWR` DBP. The magic is written last so a partial record is never reported.

Words 15 and 16 are the RTC time and date at the moment of the panic: the wall-clock time once TimeProvider has set the RTC, and the time since the RTC was started before that. They replace a cycle-counter uptime that wrapped in under a minute (CPU-D10). The registers are read as they are, without waiting for shadow synchronisation.

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

Symbols (both boards): PC and LR are resolved to `function+offset file.o` from a table generated out of the linker map of a finished build (`scripts/gen-panic-symbols.py`, `panic_symbols.c`, `panic_symbols_data.c`, gitignored; an empty weak fallback keeps a fresh checkout linking). The table is generated from one build and compiled into the next, so `cpu.c` checks that it belongs to the running image before using it: the generator records the address of `cpu_panic` and the end of the code (`_etext`), and `panic_symbols_match()` requires both to equal the running image's values. A table from an earlier build is shown as `(no symbols for this image)` instead of a wrong function name (CPU-D11). `scripts/gen-panic-symbols.sh gateway|field-device [--flash]` runs the three builds that make the table and the image agree. The Field Device object names are whole paths in its build script; the generator keeps the file name.

### 3.5 Debug and release behaviour

A `DEBUG` build stops at a breakpoint only when a debugger is attached (`DHCSR.C_DEBUGEN`). With no debugger it resets exactly like a release build, because a `BKPT` with no debugger is itself a fault: it used to end in a core lockup that only the watchdog (when running) could clear. Release builds always reset. Decision CPU-D3.

`C_DEBUGEN` is set by openocd or gdb and stays set after they disconnect, until a power-on reset. A DEBUG image that panics after a debug session therefore halts silently, with the report already on the console; power-cycle the board or clear the bit (`mww 0xE000EDF0 0xA05F0000` in openocd).

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
| Cycle counter | `CoreDebug->DEMCR.TRCENA`, `DWT->CTRL.CYCCNTENA`, `DWT->CYCCNT` (delays) | not used |
| Debugger present | `CoreDebug->DHCSR.C_DEBUGEN` | same |
| Time stamp | `RTC->TR`, `RTC->DR` | same |
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
| Reset loop | A panic early in boot resets again and again; the record is overwritten each time. No counter: see CPU-D12 |
| Table from another image | The symbols are replaced by `(no symbols for this image)` (section 3.4) |
| Debugger left attached | A DEBUG image halts at the breakpoint instead of resetting (section 3.5) |

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
| TC-CPU-038 | `cpu_init` (Field Device) returns `STATUS_OK` and leaves the cycle counter alone | Field Device |
| TC-CPU-039 | Release build resets, no breakpoint | both |
| TC-CPU-039a..e | DEBUG build (own test file `test_cpu_debug*.c`): debugger attached stops at the breakpoint without a reset; no debugger resets without a breakpoint; a nested panic follows the same rule; `CPU_ASSERT` panics when false and not when true | both |
| TC-CPU-042 | The record holds `RTC->TR` and `RTC->DR` | both |
| TC-PS-MATCH | `panic_symbols_match()`: equal anchor and code end (Thumb bit ignored), another build, a code-size change after `cpu_panic`, empty table, missing anchors | both |
| TC-CPU-041 | `cpu_reset` issues a barrier before the reset | both |

Hardware (`integration-tests/cpu/main_test_cpu.c` on each board; the run is unattended, see the file header):

| ID | Case |
|---|---|
| TC-HW-CPU-007 | Forced fault (null-pointer read, divide trap, unaligned access with the trap on, undefined instruction): report on the console, reset, post-mortem on the next boot |
| TC-HW-CPU-008 | `cpu_reset()` leaves no post-mortem |
| TC-HW-CPU-009 | `CPU_ASSERT(false)`: compiled out in a release build (then a clean reset); `Assert` panic with the expression and line in a DEBUG build |
| TC-HW-CPU-010 | Task stack overflow raises `StackOverflow` with the task name (`vApplicationStackOverflowHook`) |
| TC-HW-CPU-011 | TimeProvider's sync flag in register 0 survives a panic reset |
| TC-HW-CPU-012 | A fault inside the panic path (frame pointer into reserved memory) resets with no post-mortem and does not loop |
| TC-HW-CPU-013 | DEBUG build with a debugger attached halts at the breakpoint inside `panic_end` (openocd, by hand) |
| TC-HW-CPU-014 | PC and LR print as `function+offset file.o` with a matching table, and as `(no symbols for this image)` with an old table or after a code-size change |

## 8. Open items

| ID | Item | Status |
|---|---|---|
| CPU-O1 | No SRS requirement for fault diagnosis | Closed: REQ-NF-507, SRS v1.6 |
| CPU-O2 | The uptime hint wrapped in under a minute | Closed: the record holds the RTC time and date (CPU-D10) |
| CPU-O3 | Field Device had no symbol resolution | Closed (section 3.4) |
| CPU-O4 | Reset-loop counter | Closed as a decision: not built (CPU-D12) |
| CPU-O5 | The symbol table could be stale | Closed: checked against the running image (CPU-D11) |
| CPU-O6 | Malloc-failed and watchdog sources are never raised | Closed as a decision: kept in the enum so the record format does not change; nothing in the images can raise them |
| CPU-O7 | Enabling the Bus, Memory and Usage handlers changes the vector some faults take | Closed: confirmed on both boards (TC-HW-CPU-007) |
| CPU-O8 | The Gateway image has 1.3 % of its RAM left (98.7 % used) | Open, outside this module: tracked with the Gateway memory budget |

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
| CPU-D10 | Words 15 and 16 of the record hold `RTC->TR` and `RTC->DR` | The cycle-counter uptime wrapped every 24 to 54 s and told nothing about a panic hours after boot. The RTC already runs on both boards and is set by TimeProvider |
| CPU-D11 | Symbols are used only when the table matches the running image (`cpu_panic` address and `_etext`) | The first anchor, `cpu_panic` alone, did not notice a code-size change in a module linked after it; the code end does. Hardware-checked on both boards |
| CPU-D12 | No reset-loop counter | Stopping the resets would leave an unattended device dead instead of repeatedly restarting; the watchdog and the reset-cause report already expose a loop. Revisit only with a requirement |
| CPU-D13 | A DEBUG build stops only with a debugger attached | See section 3.5 |

## 10. Hardware validation (2026-11-07)

Both harnesses run unattended: a step counter and a sentinel in RTC backup registers 31 (Gateway), 19 (Field Device) and 0 make each boot run the next fault and the next boot print the post-mortem. Each ran as a DEBUG build, as a release build (`-UDEBUG`) and, for the symbol cases, with a generated table. Gateway harness built by a minimal script (CpuDriver, kernel, hooks, harness); Field Device with `MAIN=... scripts/build-fd-app.sh`.

| ID | Result |
|---|---|
| TC-HW-CPU-007 | PASS on both boards, DEBUG and release. Bus fault (read of `0x2FFF0000`): `BusFault`, `CFSR 0x00008200`, `BFAR 0x2FFF0000`. Undefined instruction: `UsageFault`, `CFSR 0x00010000`. Divide by zero with `CCR.DIV_0_TRP`: `UsageFault`, `CFSR 0x02000000`. User panic: `User`, no frame. Each report was followed by a reset and a matching post-mortem, with the RTC time and date |
| PC check | The reported PC resolves with `addr2line` to the faulting source line on both boards. The old Gateway stubs would have printed the stacked LR (CPU-D5) |
| TC-HW-CPU-008 | PASS on both boards. `cpu_reset()` left no post-mortem |
| TC-HW-CPU-009 | PASS on both boards. DEBUG: `Source: Assert`, reason `step == 99U @ <file>:<line>`. Release: compiled out, then a clean reset |
| TC-HW-CPU-010 | PASS on both boards. A task overrunning a 96-word stack raised `StackOverflow` with `Reason: ovf_task`, then reset |
| TC-HW-CPU-011 | PASS on both boards. Backup register 0 kept its sentinel across every panic reset |
| TC-HW-CPU-012 | PASS on both boards. The nested fault reset the board with no post-mortem and the sequence went on |
| TC-HW-CPU-013 | PASS on both boards. With openocd attached the DEBUG build stopped in `panic_end` (`halted due to breakpoint, current mode: Handler UsageFault`) after printing the report. Without a debugger the same image resets |
| TC-HW-CPU-014 | PASS on both boards. With a table generated from the same image: `PC: 0x080012B4 (trigger+0x00000040 main_test_cpu.c.o)`. With the Gateway's older table, and with the Field Device image grown by one statement, the line read `(no symbols for this image)` |
| Product images | Field Device and Gateway boot to Operational with the final code (rebuilt and flashed after the last change) |
| Not run | Nothing from section 7 is outstanding. The DEBUG halt was checked with openocd, not with gdb |
