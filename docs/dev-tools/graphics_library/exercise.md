# Exercise — GraphicsLibrary

**Topic:** LVGL integration, FreeRTOS static allocation, middleware isolation, host test stubs
**Level:** Intermediate embedded C
**Time:** 60–90 minutes

---

## Background

The IoT Environmental Monitor needs a middleware layer between LVGL v8 and the hardware
drivers. Rather than calling LVGL directly from the UI task, `GraphicsLibrary` owns the
three integration points that LVGL requires:

1. **Display flush** — LVGL calls `flush_cb` with a dirty rectangle; the callback must
   copy it to the SDRAM framebuffer and signal LVGL when the copy is done.
2. **Touch input** — LVGL polls `touch_read_cb` from within `lv_task_handler()`; the
   callback must query the FT6x06 touchscreen controller and map the result.
3. **Tick source** — LVGL needs a monotonic millisecond counter; a 1 ms FreeRTOS software
   timer calls `lv_tick_inc()` on each expiry.

Because LVGL is a Git submodule (`vendor/lvgl/`), it is **not available on the host**.
Your host unit tests must compile and link without it using a hand-written stub.

---

## Given files

You are given (read-only):

| File | Purpose |
|------|---------|
| `graphics_library.h` | Complete public API, `#ifdef TEST` include switch, `GRAPHICS_TEST_VISIBLE` macro, `graphics_reset_for_test()` |
| `tests/support/lvgl_stub.h` | LVGL type surface + call counter externs |
| `tests/support/lvgl_stub.c` | Stub implementations; `g_lvgl_lv_disp_drv_register_fail` failure flag |
| `tests/support/lcd_driver_stub.h` | Narrow LCD stub (`lcd_blit`, `lcd_flush`) |
| `tests/support/touchscreen_driver_stub.h` | Narrow touchscreen stub (`touchscreen_read`) |
| `tests/field-device/middleware/graphics_library/test_graphics_library.c` | 8 unit tests (scaffold; some assertions removed) |

You must implement:

| File | Task |
|------|------|
| `graphics_library.c` | Full implementation of all five public functions + three static callbacks |
| `lv_conf.h` | LVGL v8 configuration (`LV_COLOR_DEPTH 32`, `LV_TICK_CUSTOM 0`, `LV_MEM_CUSTOM 0`) |

You must NOT modify `graphics_library.h` or the stub files.

---

## Questions

Answer these before writing any code.

**Q1.** `graphics_library.h` includes `lvgl_stub.h` via an `#ifdef TEST` block.
Yet `test_graphics_library.c` also includes `lvgl_stub.h` directly (before the SUT header).
Why is the direct include in the test file necessary? What would happen at link time
if it were removed?

**Q2.** `GRAPHICS_TEST_VISIBLE` expands to nothing in test builds and to `static` in
production. Explain why `s_gl` cannot be `static` in the test build, and what would
break in production if you simply declared it without `static`.

**Q3.** `graphics_tick_increment()` does NOT take the internal mutex before calling
`lv_tick_inc()`. All other functions that touch LVGL state (`graphics_process`,
`graphics_get_display`, `graphics_get_indev`) do take it. What property of `lv_tick_inc`
makes this safe, and what would be the consequence of taking the mutex here anyway?

**Q4.** The flush callback casts `lv_color_t *color_p` to `const uint32_t *` before
passing it to `lcd_blit()`. Under what compile-time condition is this cast valid without
invoking undefined behaviour (consider `LV_COLOR_DEPTH` and the project's `lv_conf.h`)?

**Q5.** `graphics_init()` creates the FreeRTOS mutex with `xSemaphoreCreateMutexStatic`.
What happens at runtime if `graphics_init()` is called a second time (before the
scheduler starts)? Which resource is leaked, and which LVGL guarantee is violated?

---

## Implementation hints

- Use `#ifdef TEST` in `graphics_library.c` to switch between `lvgl_stub.h`,
  `lcd_driver_stub.h`, `touchscreen_driver_stub.h` and the real headers. This is the same
  mechanism the header uses.
- In `flush_cb`, LVGL area coordinates are **inclusive** on both ends.
  `area->x2 - area->x1 + 1` gives the pixel width; forgetting the `+ 1` causes a
  one-pixel-narrow flush on every render cycle (fence-post error).
- `lv_disp_drv_register()` can return NULL if LVGL's internal heap is exhausted
  (tuned by `LV_MEM_SIZE` in `lv_conf.h`). Check the return value and propagate
  `GRAPHICS_ERR_LVGL_FAIL` immediately without setting `s_gl.initialised`.
- The tick timer uses `xTimerCreateStatic` (static allocation) and `pdTRUE` for
  `uxAutoReload` (repeating, not one-shot). Call `xTimerStart` before setting
  `s_gl.initialised = true`.
- Use `GFX_DRAW_BUF_PIXELS (4096U)` for the two draw buffers.
  Two buffers × 4096 pixels × 4 bytes = **32 KB SRAM** — match this in your comment.

---

## Marking guide

| Criterion | Marks |
|-----------|-------|
| All 8 unit tests pass (`ceedling test:test_graphics_library`) | 40 |
| cppcheck exits 0 (no findings) | 20 |
| clang-format exits 0 (no violations) | 10 |
| Q1–Q5 answers correct and concise | 25 |
| `lv_conf.h` present; `LV_TICK_CUSTOM 0`, `LV_COLOR_DEPTH 32`, `LV_MEM_CUSTOM 0` set correctly | 5 |
| **Total** | **100** |

### Model answers (Q1–Q5)

**A1.** Ceedling's auto-link scanner performs a literal regex scan of `#include` directives
in the test TU (even with `use_test_preprocessor: TRUE`). It does not follow transitive
includes that appear inside other headers. Because `lvgl_stub.h` is only referenced inside
`graphics_library.h` (behind `#ifdef TEST`), Ceedling never sees it as a link trigger.
Without the direct include, the linker reports undefined references to every LVGL stub
symbol (`lv_init`, `lv_task_handler`, `lv_tick_inc`, `lv_disp_drv_register`, etc.).

**A2.** Unity's `TEST_ASSERT_*` macros and the test's `extern` declarations require
external linkage to read `s_gl.initialised` and `s_gl.display`. `static` limits a
symbol's linkage to its translation unit; the test TU could not access it even with
an `extern` declaration. In production, removing `static` would expose `s_gl` in the
global symbol namespace, potentially conflicting with similarly named objects in other
modules (C has no namespacing).

**A3.** LVGL v8 documents `lv_tick_inc()` as interrupt-safe and re-entrant; it only
increments an internal `uint32_t` counter using an atomic write. Taking the same mutex
that `graphics_process()` holds for the duration of `lv_task_handler()` would cause the
1 ms timer callback to block every millisecond waiting for `lv_task_handler()` to finish.
Since the timer callback runs in the FreeRTOS timer-service task (which has its own stack
and priority), this would invert priorities between the timer task and LcdUiTask,
and could starve the tick source.

**A4.** The cast is valid because `lv_conf.h` sets `LV_COLOR_DEPTH 32`, which causes LVGL
to define `lv_color_t` as a type with the same size and representation as `uint32_t`
(effectively a 32-bit unsigned integer holding ARGB8888 data). With `LV_COLOR_DEPTH 16`
or `24`, `sizeof(lv_color_t)` would differ from `sizeof(uint32_t)`, and the pointer cast
would produce an incorrectly sized copy in `memcpy` inside `lcd_blit()`.

**A5.** A second call to `graphics_init()` calls `xSemaphoreCreateMutexStatic` again,
overwriting `s_gl.mutex` with a new handle. The original mutex handle is lost (memory
leak of the static semaphore state — FreeRTOS static allocation uses a caller-supplied
`StaticSemaphore_t`, so the "leak" is structural, not heap-based, but the handle is gone).
Additionally, calling `lv_init()` twice is explicitly undefined behaviour per LVGL's
documentation; the internal LVGL heap may be re-initialised, erasing any previously
registered drivers or allocated widgets.
