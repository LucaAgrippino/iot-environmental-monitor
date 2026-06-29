# Exercise — CpuDriver (Gateway)

**Date:** 2026-06-29
**Target:** docs/lld/gateway/drivers/cpu-driver-companion.md

---

## Exercise 1 — DWT counter wrap-around

**Scenario:**
`cpu_delay_us()` measures elapsed time by computing `DWT->CYCCNT - start`. Both values are `uint32_t`. At 80 MHz the counter wraps every ~53 seconds, so any delay shorter than 53 seconds is safe: unsigned subtraction gives the correct positive result even when `CYCCNT` has wrapped past zero.

The current tests call `cpu_delay_us(1)` with `g_mock_dwt.CYCCNT` starting at 0. No test exercises the wrap boundary.

**Task:**
1. In `test_cpu.c`, add `TC_CPU_012b`: set `g_mock_dwt.CYCCNT = 0xFFFFFFF0U` before calling `cpu_delay_us(1)`. Under the `TEST` build exit-after-3-iterations guard the function should return without hanging — verify it returns without assertion failure.
2. Add `TC_CPU_012c`: set `g_mock_dwt.CYCCNT = 0xFFFFFFFFU` (max value). Call `cpu_delay_us(0)` — verify the function returns immediately (zero cycles required, condition `(CYCCNT - start) < 0` is always false).
3. Run `ceedling test:test_cpu` and confirm both new TCs pass.

**Expected learning:** Unsigned 32-bit subtraction is commutative with respect to wrap-around because the modular arithmetic gives the right "elapsed" value regardless of whether `CYCCNT` has crossed zero. This is the standard technique for all bare-metal spin-delay loops.

---

## Exercise 2 — Recursive panic guard

**Scenario:**
`cpu_panic()` sets `g_panic_active = true` on entry and checks it at the top of the function. If panic is called recursively (e.g., from `emit_panic_uart`), the second call must return immediately without re-entering the full panic sequence.

TC-CPU-024 verifies that a second call to `cpu_panic()` (after the first has set `g_panic_active`) does not increment `g_cpu_hw_disable_irq_count` again. TC-CPU-025 verifies `cpu_reset_for_test()` clears `g_panic_active`.

**Task:**
1. Add `TC_CPU_024b`: call `cpu_panic(CPU_PANIC_HARDFAULT, NULL)` twice in sequence. Verify that:
   - `g_cpu_hw_disable_irq_count == 1` (IRQ only disabled once)
   - `g_mock_rtc.BKP0R == CPU_PANIC_MAGIC` (record written by first call only — second call exits early before writing)
2. Add `TC_CPU_024c`: call `cpu_panic(CPU_PANIC_ASSERT, "first")` then `cpu_panic(CPU_PANIC_STACK_OVERFLOW, "second")`. Verify that `g_mock_rtc.BKP0R == CPU_PANIC_MAGIC` (still only one record) and the BKP registers reflect the FIRST panic source (not overwritten by the second).
3. In both cases, call `cpu_reset_for_test()` in tearDown (it already runs in `tearDown()` via `stm32l475_cmsis_mock_reset()` + `cpu_reset_for_test()`) to confirm state is clean for the next test.

**Expected learning:** The `g_panic_active` guard is a one-shot latch. It prevents `write_panic_record()` from being called twice, which is critical because the RTC backup domain write enables a PWR clock that should only be enabled once. The guard also protects against stack corruption if `emit_panic_uart` itself faults.

---

## Exercise 3 — Post-mortem record field verification

**Scenario:**
`write_panic_record()` maps CPU state into RTC BKP registers BKP1R–BKP16R with BKP0R written last as the magic commit word. The current TC-CPU-040 verifies BKP0R (magic) and BKP1R (CFSR). TC-CPU-041 checks that BKP0R is NOT set if `cpu_panic()` is never called.

**Task:**
1. Add `TC_CPU_040b`: set `g_mock_scb.HFSR = 0x40000000U` (FORCED bit), call `cpu_panic(CPU_PANIC_HARDFAULT, NULL)`, and verify `g_mock_rtc.BKP2R == 0x40000000U` (HFSR stored in BKP2R).
2. Add `TC_CPU_040c`: set `g_mock_scb.BFAR = 0xDEADBEEFU`, call `cpu_panic(CPU_PANIC_HARDFAULT, NULL)`, and verify `g_mock_rtc.BKP3R == 0xDEADBEEFU` (BFAR stored in BKP3R).
3. Add `TC_CPU_040d`: call `cpu_fault_entry(frame)` with a synthetic frame where `frame[6] = 0x08001234U` (PC). Verify `g_mock_rtc.BKP5R == 0x08001234U` (PC stored in BKP5R, zero-indexed from BKP4R which holds R0).

**Expected learning:** Knowing which BKP register holds each fault field is critical when writing a post-mortem parser (e.g., a Python script that reads the BKP registers over SWD or UART). Any mismatch between the writer (`write_panic_record`) and reader (`check_panic_record` / offline parser) silently corrupts the diagnosis. These TCs pin the mapping.

---

## Exercise 4 — CFSR decode table completeness

**Scenario:**
`cfsr_cause_string()` (exposed as `cpu_cfsr_cause_string_for_test()`) maps 17 individual CFSR bit positions to human-readable strings. TC-CPU-027 verifies three bits. The remaining 14 bits are not individually tested.

**Task:**
1. Add `TC_CPU_027b` through `TC_CPU_027e` for the four MemManage fault bits:
   - `IACCVIOL` (CFSR bit 0): expect string contains `"IACCVIOL"` (or equivalent)
   - `DACCVIOL` (CFSR bit 1): expect string contains `"DACCVIOL"`
   - `MUNSTKERR` (CFSR bit 3): expect string contains `"MUNSTKERR"`
   - `MSTKERR` (CFSR bit 4): expect string contains `"MSTKERR"`
2. Add `TC_CPU_027f`: pass `CFSR = 0x00000000U` (no bits set). Verify the returned string is not NULL and does not contain `"IACCVIOL"`, `"DIVBYZERO"`, or any other fault name — it should return a "no fault" or "unknown" sentinel.
3. Add `TC_CPU_027g`: pass `CFSR = 0xFFFFFFFFU` (all bits set). Verify the function returns a non-NULL string without crashing (some bits in CFSR are reserved/undefined, so this exercises robustness).

**Expected learning:** Fault decode tables are only as useful as their test coverage. A silent gap (e.g., bit 3 not handled) means the wrong string is returned for MUNSTKERR faults, leading an engineer to investigate the wrong fault type during a field debug session.

---

## Exercise 5 — Clock accessor values after init

**Scenario:**
`cpu_get_sysclk_hz()`, `cpu_get_pclk1_hz()`, and `cpu_get_pclk2_hz()` return the cached clock values set during `cpu_init()`. TC-CPU-005 verifies `cpu_get_sysclk_hz()` returns `80000000U` after a successful init. TC-CPU-060 verifies accessors return 0 before init.

**Task:**
1. Add `TC_CPU_005b`: after a successful `cpu_init()`, verify `cpu_get_pclk1_hz() == 80000000U` and `cpu_get_pclk2_hz() == 80000000U`.
2. Add `TC_CPU_005c`: call `cpu_reset_for_test()` after a successful init, then verify all three accessors return 0 (reset clears the cache).
3. Extend `TC_CPU_060` (or add `TC_CPU_060b`): verify that calling `cpu_delay_ms(1)` before `cpu_init()` returns immediately (because `g_sysclk_hz == 0` makes the cycles calculation 0, so the spin condition is immediately false). Use `TEST_ASSERT_EQUAL_UINT32(0U, g_mock_dwt.CYCCNT)` as a proxy that the loop body was not entered.

**Expected learning:** Accessor functions caching hardware-computed values are a common pattern in embedded drivers. Testing the before/after and reset states prevents the silent bug where a driver layer returns a stale non-zero value from a previous init (e.g., after a warm reset that calls `cpu_reset_for_test()` but not `cpu_init()`).
