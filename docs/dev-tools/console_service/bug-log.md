# Bug Log — ConsoleService

## config commit and prov commit do not flush to persistent storage

**File:** firmware/field-device/application/console_service/console_service.c
**Line:** 759 (config commit path, after the set_param loop)
**Category:** missing operation

**What the code does:**
After the operator confirms a `config commit` or `prov commit`, the code calls
`s_cfg_write->set_param()` for each staged field and, on success, clears the
staging struct and prints `[OK] Config applied.` — without ever calling
`s_cfg_write->flush()` to persist the changes to ConfigStore flash.

**What it should do:**
Call `s_cfg_write->flush()` after all `set_param()` calls succeed, before
clearing the staging struct, so that the new values survive a power cycle.
The "[OK]" response should only be printed if flush also returns OK; otherwise
the staging struct should be retained and `[ERR] persist failed` printed.

**Correct fix (config commit path):**

```c
/* before — line ~759 in console_service.c */
(void) memset(&s_cfg_pending, 0, sizeof(s_cfg_pending));
tx_str("[OK] Config applied.\r\n");
return CONSOLE_SERVICE_ERR_OK;

/* after */
config_service_err_t flush_rc = s_cfg_write->flush();
if (flush_rc != CONFIG_SERVICE_OK)
{
    tx_str("[ERR] persist failed\r\n");
    LOG_WARN(CS_MOD, "cfg commit: flush failed");
    return CONSOLE_SERVICE_ERR_APPLY_FAILED;
}
(void) memset(&s_cfg_pending, 0, sizeof(s_cfg_pending));
tx_str("[OK] Config applied and persisted.\r\n");
return CONSOLE_SERVICE_ERR_OK;
```

The same fix applies to `prov commit` (line ~871): a `flush()` call must be
inserted after the successful `set_param()` call(s) and before the `memset`
that clears `s_prov_pending`.

**How to find it with a debugger:**

1. Connect a serial terminal (115200/8N1) and a J-Link or ST-Link.
2. Open a live-watch window and add `s_cfg_pending` and `s_prov_pending`.
3. Type `config set polling-interval-ms 3000` — observe `s_cfg_pending.dirty`
   becomes `true` and `polling_interval_ms` becomes 3000 in the live-watch.
4. Type `config commit` then `y`.
5. Set a breakpoint at the start of `cmd_config` commit path. Step through
   the `set_param` loop — each call returns `CONFIG_SERVICE_OK`.
6. Observe that control reaches `memset(&s_cfg_pending, 0, ...)` and
   then `tx_str("[OK] Config applied.")` without ever hitting `flush()`.
7. Power-cycle the board. Type `config list` — `polling-interval-ms` shows
   the previous default, confirming the value was never written to flash.

**Why it passes CI:**
TC-CS-064 asserts only that `g_spy_set_call_count > 0` and that `[OK]` appears
in the TX buffer. The spy's `flush()` returns `CONFIG_SERVICE_OK` by default
but the `cmd_config` commit path never calls it, so `g_spy_flush_calls` remains
zero. No test currently asserts `g_spy_flush_calls > 0` after a `config commit`,
so the omission is invisible to the host-side harness. The bug only manifests
on hardware after a reboot.
