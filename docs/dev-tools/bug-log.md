<!-- DO NOT COMMIT until the developer has found and fixed the bug. -->

# Bug Log — Answer Key

This file records the intentional bugs planted in each module implementation.
It is the answer key for the hardware debugging exercise. Commit only after
the developer has independently found and fixed the bug on real hardware.

---

## LedDriver — Wrong GPIO output type in pin configuration

**File:** `firmware/field-device/drivers/led/led_driver.c`
**Line:** 95
**Category:** wrong constant

**What the code does:**
Configures all LED GPIO pins with `GPIO_OTYPE_OPEN_DRAIN`, making them
open-drain outputs with no external pull-up resistor.

**What it should do:**
Configure all LED GPIO pins with `GPIO_OTYPE_PUSH_PULL` so the driver can
actively drive both logic levels and light the LED.

**Correct fix:**
```c
/* before */
cfg.otype = GPIO_OTYPE_OPEN_DRAIN;
/* after */
cfg.otype = GPIO_OTYPE_PUSH_PULL;
```

**How to find it with a debugger:**
1. Flash the integration test firmware to the STM32F469I-DISCO board.
2. Observe that both LD3 (PG6) and LD4 (PD5) never illuminate regardless
   of which led_on() call is made.
3. Set a breakpoint at the return of led_on(LED_GREEN) and inspect GPIOG->ODR —
   bit 6 will be clear (0), confirming the gpio_write_pin(LOW) call reached
   the peripheral.
4. Open the Peripheral view for GPIOG in STM32CubeIDE. Check GPIOG_OTYPER
   bit 6 — it will read 1 (open-drain) instead of the expected 0 (push-pull).
5. Cross-reference with the companion §4.1: "Mode: Output push-pull."
   The register value contradicts the spec.
6. Apply the fix (`PUSH_PULL`), reflash, and verify both LEDs now respond.

**Why it passes CI:**
The unit-test stubs for `gpio_configure_pin` record that the function was
called and return `GPIO_OK`, but do not inspect the `otype` field of the
`gpio_pin_config_t` struct. The bug therefore leaves no observable trace in
the test output. cppcheck sees a valid enum assignment and raises no warning.

---

## HealthMonitor — Off-by-one in stack watermark copy loop

**File:** `firmware/field-device/application/health_monitor/health_monitor.c`
**Line:** 415
**Category:** off-by-one

**What the code does:**
The loop in `health_monitor_update_stack_watermarks()` iterates
`i < HEALTH_TASK_COUNT - 1U` (i.e., i = 0..5), so the last element
`watermarks[6]` is never copied into `s_hm.snapshot.stack_watermark_words[6]`.
The seventh task's watermark is always reported as 0 in the health snapshot.

**What it should do:**
The loop should iterate `i < HEALTH_TASK_COUNT` (i = 0..6) to copy all
seven watermark values into the snapshot.

**Correct fix:**
```c
/* before */
for (i = 0U; i < HEALTH_TASK_COUNT - 1U; i++)
/* after */
for (i = 0U; i < HEALTH_TASK_COUNT; i++)
```

**How to find it with a debugger:**
1. Boot the integration test firmware on the STM32F469I-DISCO board.
2. After `health_monitor_poll()` is called, break at the return of
   `health_monitor_update_stack_watermarks()`.
3. In the Watch window, inspect `s_hm.snapshot.stack_watermark_words[6]` —
   it will be 0 even though `watermarks[6]` passed in has a real value.
4. Set a watch on `watermarks[6]` (the parameter): it is non-zero.
5. Step through the loop: observe that `i` reaches 5 and then the loop
   exits, never writing index 6.
6. Cross-reference with companion §2: `HEALTH_TASK_COUNT = 7U` and the
   snapshot field `stack_watermark_words[HEALTH_TASK_COUNT]` — all seven
   entries should be populated.

**Why it passes CI:**
No unit test case verifies the seventh watermark entry specifically. The
companion §7 test plan checks that `update_modbus_slave_stats` copies fields,
but contains no test that calls `update_stack_watermarks` and then asserts
`stack_watermark_words[HEALTH_TASK_COUNT - 1U]`. cppcheck does not know
the intended iteration count and raises no warning for the off-by-one.

---

## config_store — off-by-one in slot erase range (7 of 8 sectors erased)

**File:** `firmware/field-device/middleware/config_store/config_store.c`
**Line:** 508
**Category:** off-by-one

**What the code does:**
Erases `CONFIG_STORE_SLOT_SIZE - CONFIG_STORE_SECTOR_SIZE` (28 KB = 7 sectors)
of the target slot instead of the full `CONFIG_STORE_SLOT_SIZE` (32 KB = 8 sectors),
leaving the last 4 KB sector (bytes 0x7000–0x7FFF, which contains the CRC field
at offset 0x7FF8) un-erased.

**What it should do:**
Erase all 8 sectors (32 KB) of the target slot before writing new content, so
that the CRC field at 0x7FF8–0x7FFB is guaranteed to be 0xFF before the new CRC
is committed.

**Correct fix:**
```c
/* before */
err = cs_flash_erase_range(target_base, CONFIG_STORE_SLOT_SIZE - CONFIG_STORE_SECTOR_SIZE);
/* after */
err = cs_flash_erase_range(target_base, CONFIG_STORE_SLOT_SIZE);
```

**How to find it with a debugger:**
1. Flash the integration test and observe the first run succeeds (green LED).
2. Press RESET — the test runs again, writing seq=3 and seq=4.
3. On subsequent resets the save/load cycle should still work because the CRC
   field was 0xFF after factory sim_reset (first run). The bug only manifests
   when the SAME physical slot is used for the SECOND TIME in a row (i.e. seq=5
   or seq=7 etc.) — at that point the CRC field in that sector already has old
   data from a previous write and was not erased.
4. Set a JTAG breakpoint at the `cs_flash_erase_range` call inside
   `config_store_save`. Single-step through: observe that `len` passed is
   0x7000 (28 KB) instead of 0x8000 (32 KB).
5. After the erase completes, read the flash slot in the Memory view. Bytes
   0x7000–0x7FFF of the slot will NOT be 0xFF — they will contain the CRC from
   the previous write to that slot.
6. Continue execution: the new CRC write at 0x7FF8 ANDs the new CRC value with
   the residual old CRC (NOR flash can only clear bits). The resulting stored
   CRC does not match the computed CRC. On the next boot, cs_validate_slot
   detects the mismatch and rejects the slot.

**Why it passes CI:**
The unit test RAM simulation uses `memcpy` for writes, which overwrites any byte
value regardless of prior content. The NOR flash constraint (can only clear bits
without prior erase) is not enforced in the RAM sim. Starting from a fresh
`sim_reset` (all 0xFF), the first use of each slot sees an already-erased CRC
field, so the short erase causes no observable difference in the host test.

---

## ConfigService — mutex not released on validation failure in set_param()

**File:** `firmware/field-device/application/config_service/config_service.c`
**Line:** 347
**Category:** missing state-clear

**What the code does:**
After acquiring the mutex in `config_service_set_param()`, the function calls
`validate_param_internal()` and, if validation fails, returns the error code
directly — without calling `xSemaphoreGive(s_cs.mutex)`.

**What it should do:**
Release the mutex before returning on any error path, including validation
failure, so that subsequent callers can acquire the mutex for their own
operations.

**Correct fix:**
```c
/* before */
    if (v_err != CONFIG_SERVICE_ERR_OK)
    {
        return v_err;
    }
/* after */
    if (v_err != CONFIG_SERVICE_ERR_OK)
    {
        (void) xSemaphoreGive(s_cs.mutex);
        return v_err;
    }
```

**How to find it with a debugger:**
1. Flash the ConfigService integration test to the STM32F469I-DISCO.
2. Observe that step 5 (`set poll_interval=50: ERR_INVALID`) prints correctly.
3. Observe that step 6 onwards never prints — the task blocks indefinitely.
4. Pause execution in the debugger and inspect the call stack.  The test task
   will be blocked in `xSemaphoreTake()` inside `config_service_flush()` or
   `config_service_restore_snapshot()`.
5. In the FreeRTOS task viewer, confirm the mutex is held by the test task
   itself (mutex owner = test task handle, but test task is blocked waiting
   for the same mutex — a self-deadlock because the priority-inheritance mutex
   is not recursive).
6. Set a breakpoint at line 347 (`return v_err`) in config_service.c.
7. Re-run and confirm the breakpoint is hit when the invalid set_param call
   is made.  Step through and observe that no `xSemaphoreGive` is called before
   the function returns.
8. Apply the fix, reflash, and verify all 12 checklist items appear on the
   terminal.

**Why it passes CI:**
The FreeRTOS mock (`freertos_mock.c`) implements `xSemaphoreTake` and
`xSemaphoreGive` as counter increments that always return `pdTRUE`.  The mock
does not track mutex ownership, does not enforce that a mutex must be given
before it can be taken again by the same notional holder, and does not block.
TC-CSVC-006 calls `set_param` with an invalid value in one test function and a
valid value in a separate test function (each starting with a fresh `setUp()`
that calls `config_service_reset_for_test()` — clearing s_cs including the
mutex handle).  No test exercises invalid-then-valid within the same function
with the same initialized state, so the hung mutex never surfaces.
