# Bug Log — LcdDriver

## BUG-LCD-001 — LTDC_ER_IRQn configured at wrong priority

| Field | Detail |
|-------|--------|
| **File** | `firmware/field-device/drivers/lcd_driver/lcd_driver.c` |
| **Line** | 140 |
| **Category** | Wrong constant — off-by-one in NVIC priority |
| **Severity** | Medium (silent at runtime; symptom only under error conditions) |
| **Detectable by** | Hardware debugger; NOT by CI (intentional design) |

### What the code does

```c
/* line 138 */ NVIC_SetPriority(LTDC_IRQn,    LCD_IRQ_PRIORITY);        /* 6 — correct */
/* line 139 */ NVIC_EnableIRQ(LTDC_IRQn);
/* line 140 */ NVIC_SetPriority(LTDC_ER_IRQn, LCD_IRQ_PRIORITY + 1U);  /* BUG: 7 */
/* line 141 */ NVIC_EnableIRQ(LTDC_ER_IRQn);
```

### What it should do

Both `LTDC_IRQn` and `LTDC_ER_IRQn` should be configured at priority **6** (`LCD_IRQ_PRIORITY`),
consistent with the project's FreeRTOS interrupt priority band (5–7 = below
`configMAX_SYSCALL_INTERRUPT_PRIORITY = 5`).

```c
NVIC_SetPriority(LTDC_ER_IRQn, LCD_IRQ_PRIORITY);  /* correct: 6 */
```

### Why it passes CI

`TC-LCD-007` checks:
- `g_mock_ltdc.IER & LTDC_IER_LIE` — correct
- `g_mock_nvic_enabled[LTDC_IRQn] == 1` — correct
- `g_mock_nvic_priority[LTDC_IRQn] == 6U` — correct

It does **not** check `g_mock_nvic_priority[LTDC_ER_IRQn]`, so the error interrupt's
priority of 7 goes undetected. The line-interrupt path (the primary operational path)
is unaffected; only LTDC error events (bus-error, register-reload-error) use the wrong
priority.

### Consequence on hardware

Priority 7 is still below `configMAX_SYSCALL_INTERRUPT_PRIORITY` so the error ISR
can still call FreeRTOS ISR-safe APIs. The symptom is subtle:

- Under normal operation: invisible (LTDC_ER_IRQn fires only on FIFO underrun or SYNC error)
- Under heavy bus load: the error interrupt fires at priority 7 instead of 6, allowing
  priority-6 non-LCD interrupts to preempt it. In pathological cases, an LTDC error
  could be masked behind another ISR for longer than expected.

### How to find it with a debugger (STM32CubeIDE / J-Link)

1. Flash the firmware. Open **Debug** → **Registers** → **NVIC**.
2. Navigate to `NVIC_IPR22` (IPR register covering IRQs 88–91; LTDC_IRQn=88, LTDC_ER_IRQn=89).
3. Observe: bits [23:16] (LTDC_IRQn field) = `0x60`; bits [31:24] (LTDC_ER_IRQn field) = `0x70`.
4. `0x70` = priority 7 in the upper nibble. Expected value is `0x60`.

Alternatively in gdb:
```
(gdb) x/1wx 0xE000E45C    # NVIC_IPR22 for IRQn 88..91
```
Expected: `0x60600000`. Actual (buggy): `0x70600000`.

### Fix

```c
- NVIC_SetPriority(LTDC_ER_IRQn, LCD_IRQ_PRIORITY + 1U);
+ NVIC_SetPriority(LTDC_ER_IRQn, LCD_IRQ_PRIORITY);
```

To catch this class of bug in future, add a TC-LCD-007 assertion:
```c
TEST_ASSERT_EQUAL_UINT32(6U, g_mock_nvic_priority[LTDC_ER_IRQn]);
```

---

## Bugs found and fixed during implementation (not seeded)

| ID | File | Error | Root cause | Fix |
|----|------|-------|------------|-----|
| F1 | `lcd_driver.h` | `implicit declaration of 'LTDC_IRQHandler'` | ISR called directly in TC-LCD-008 but not declared in any header visible to the test TU | Added `void LTDC_IRQHandler(void);` to `lcd_driver.h` under the public API section |
| F2 | `bsp_shims/stm32469i_discovery_sdram.c` | cppcheck `constParameterPointer` on `BSP_SDRAM_WriteData` | `pData` parameter only read (source to `memcpy`), not modified | Changed `uint32_t *pData` to `const uint32_t *pData` in both header and implementation |
| F3 | `lcd_driver.c` | cppcheck `redundantAssignment` on `s_lcd_init_stage` | Volatile stage sentinel written sequentially without reads between writes; cppcheck flags it despite `volatile` qualifier | Added file-scoped suppression `redundantAssignment:lcd_driver.c` to `cppcheck-suppressions.txt` with rationale |
| F4 | Multiple files | clang-format alignment violations | Column-aligned `=` in enum values and struct field assignments; project `.clang-format` does not enable `AlignConsecutiveAssignments` | Resolved by `test-module.ps1 -Fix` pass |
