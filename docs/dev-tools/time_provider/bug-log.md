# Bug Log — TimeProvider

## TimeProvider — missing mutex release on RTC-read error path in set_time()

**File:** `firmware/field-device/middleware/time_provider/time_provider.c`  
**Line:** 252 (inside `time_provider_set_time()`, within the `if (s_tp.sync_state == TIME_SYNC_SYNCHRONISED)` block)  
**Category:** missing state-clear on error path

**What the code does:**  
When `rtc_driver->get_time()` fails during the sanity check, the function logs a warning and returns `TIME_PROVIDER_ERR_RTC_FAIL` without calling `xSemaphoreGive()` first.

**What it should do:**  
Release the mutex before returning on every error path, exactly as the sanity-delta rejection path and the `rtc_driver->set_time()` failure path do.

**Correct fix:**
```c
/* before */
        if (rtc_err != RTC_OK)
        {
            LOG_WARN(MODULE_TAG, "Sanity-check RTC read failed: %d", (int) rtc_err);
            return TIME_PROVIDER_ERR_RTC_FAIL;
        }

/* after */
        if (rtc_err != RTC_OK)
        {
            LOG_WARN(MODULE_TAG, "Sanity-check RTC read failed: %d", (int) rtc_err);
            (void) xSemaphoreGive(s_tp.mutex);
            return TIME_PROVIDER_ERR_RTC_FAIL;
        }
```

**How to find it with a debugger:**  
1. Inject an RTC hardware fault while the provider is SYNCHRONISED (e.g. corrupt the RTC peripheral clock in the debugger, or arrange for `rtc_driver->get_time()` to return `RTC_ERR_SYNC_TIMEOUT` via a conditional breakpoint that changes the return register).  
2. Call `time_provider_set_time()` — it returns `TIME_PROVIDER_ERR_RTC_FAIL` correctly.  
3. Call `time_provider_set_time()` a second time with any epoch — the call blocks indefinitely at `xSemaphoreTake()`.  
4. The watchdog fires after its timeout. In the debugger, break on `xSemaphoreTake()`, observe the mutex owner is still the task that executed the first failing call. Trace `s_tp.mutex` holder back to `time_provider_set_time()` — the semaphore was never released.  
5. Compare the RTC-fail path (line ~252) against the delta-rejection path (line ~260) — the delta path has `(void)xSemaphoreGive(s_tp.mutex);`, the fail path does not.

**Why it passes CI:**  
The unit tests check the return value of `set_time()` when `stub_rtc_get_time` is set to return `RTC_ERR_SYNC_TIMEOUT`, and correctly assert `TIME_PROVIDER_ERR_RTC_FAIL`. No test asserts the balance of `g_mock_xSemaphoreTake_call_count` versus `g_mock_xSemaphoreGive_call_count` for this specific error path. cppcheck does not track mutex ownership across code paths.
