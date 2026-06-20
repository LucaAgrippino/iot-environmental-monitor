# Bug Log — GraphicsLibrary

## Seeded bugs

No intentional seeded bug was placed in the GraphicsLibrary module.
The companion (v0.3) does not specify one for this module.

---

## BUG-GFX-001 — `graphics_init()` missing double-init guard

| Field | Detail |
|-------|--------|
| **File** | `firmware/field-device/middleware/graphics_library/graphics_library.c` |
| **Line** | 108 |
| **Category** | Missing guard — re-entrant init corrupts state |
| **Severity** | High (undefined LVGL behaviour; mutex handle overwritten) |
| **Detectable by** | Code review / integration test; NOT by TC-GFX-001..008 (no double-init test case) |

### What the code does

```c
graphics_err_t graphics_init(void)
{
    s_gl.mutex = xSemaphoreCreateMutexStatic(&s_gl.mutex_storage);
    lv_init();
    /* ... */
    s_gl.initialised = true;
    return GRAPHICS_ERR_OK;
}
```

There is no guard checking `s_gl.initialised` at the top of `graphics_init()`.
A second call overwrites `s_gl.mutex` (leaking the first handle), calls `lv_init()`
again (LVGL documents this as undefined behaviour), and creates a second software timer
whose first timer handle is never stopped or deleted.

### What it should do

```c
graphics_err_t graphics_init(void)
{
    if (s_gl.initialised)
    {
        return GRAPHICS_ERR_OK;   /* idempotent */
    }
    s_gl.mutex = xSemaphoreCreateMutexStatic(&s_gl.mutex_storage);
    /* ... */
}
```

### Why it passes CI

TC-GFX-001 calls `graphics_init()` once per test (each `setUp()` resets state via
`graphics_reset_for_test()`). No test case calls `graphics_init()` twice. The failure
mode only manifests at run-time if the LcdUi task incorrectly calls init twice —
possible during error-recovery flows or if `main.c` calls `graphics_init()` before
and after a peripheral reset.

### How to detect it

1. Add a test case: call `graphics_init()` twice; assert the second call returns
   `GRAPHICS_ERR_OK` and that `g_mock_xSemaphoreCreateMutexStatic_call_count == 1`
   (not 2).
2. During integration: set a breakpoint inside `lv_init()` and trigger a double-call.

---

## Bugs found and fixed during implementation

| ID | File | Error | Root cause | Fix |
|----|------|-------|------------|-----|
| F1 | `test_graphics_library.c` | Linker: undefined reference to `lvgl_stub_reset`, `g_lvgl_lv_disp_drv_register_fail`, `lv_init`, etc. | Ceedling's auto-link scanner only matches `#include` lines present literally in the test TU. `lvgl_stub.h` was only included transitively via `graphics_library.h`; the scanner never saw it | Added `#include "lvgl_stub.h"` directly to `test_graphics_library.c` before the SUT header |
| F2 | `lv_conf.h` | clang-format violations on all `#define` lines | LVGL's own config template uses column-aligned values; the project's `.clang-format` rules disallow manual alignment | Resolved by `test-module.ps1 -Fix` (auto-formatter rewrites to single space) |
