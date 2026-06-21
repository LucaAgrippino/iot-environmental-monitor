# Exercise — LcdUi

**Topic:** LVGL v8, Strategy pattern, pending/committed state machines, FreeRTOS UI task, host test stubs
**Level:** Intermediate–Advanced embedded C
**Time:** 90–120 minutes

---

## Background

The IoT Environmental Monitor Field Device has a 4.3-inch 800×480 LCD (STM32F469I-DISCO).
`LcdUi` is the application-layer module that owns four display screens — Sensor, Status, Alarm,
and Config — driven by a 200 ms refresh loop via LVGL v8.

Key design decisions:

1. **Strategy pattern** — each screen is a struct with an `on_enter` / `on_exit` / `on_refresh`
   vtable. The tabview callback calls these function pointers rather than branching on the current
   screen index.
2. **Module-static singleton** — all state lives in `s_ui` (an `lcd_ui_t`). There is no `self`
   pointer in the public API; consistent with all other application-layer modules in the project.
3. **Pending/committed split** — the Config screen maintains two `lcd_ui_editable_params_t`
   snapshots. The user edits `pending`; only a confirmed apply propagates to `committed` and
   calls `set_param()`. A cancel or timeout discards `pending` without touching `committed`.
4. **Dependency Inversion** — real services (SensorService, AlarmService, ConfigService,
   HealthMonitor) are accessed through vtable interfaces (`isensor_service_t`,
   `ialarm_service_t`, `iconfig_provider_t`, `iconfig_manager_t`, `ihealth_snapshot_t`,
   `ihealth_report_t`). In test builds, `lcd_ui.h` swaps in stub headers.

---

## Given files

You are given (read-only):

| File | Purpose |
|------|---------|
| `lcd_ui.h` | Public API, `LCD_UI_TEST_VISIBLE` macro, test-hook declarations, deviation list D1–D6 |
| `screen_internal.h` | All private types: `screen_t`, four screen context structs, `lcd_ui_t`, editable params, state enum |
| `tests/support/lvgl_stub.h` + `lvgl_stub.c` | Full LVGL widget/timer stub; pool allocator; `lvgl_stub_fire_event` |
| `tests/support/alarm_service_stub.h` | `ialarm_service_t` with `get_all_states`; `alarm_state_t` enum |
| `tests/support/config_service_stub.h` | `iconfig_provider_t`, `iconfig_manager_t`, `config_param_id_t` |
| `tests/support/graphics_library_stub.h` | `graphics_process`, `graphics_get_display` (bodies inline in test TU) |
| `tests/support/sensor_service_stub.h` | `isensor_service_t` with `get_snapshot` |
| `tests/support/health_monitor_stub.h` | `device_health_snapshot_t`, `ihealth_snapshot_t`, `HEALTH_EVENT_LCD_FAIL` |
| `tests/field-device/application/lcd_ui/test_lcd_ui.c` | 27 unit tests (scaffold; some assertions removed) |

You must implement:

| File | Task |
|------|------|
| `lcd_ui.c` | Full implementation: `lcd_ui_init`, four screen structs and their vtable functions, 7 LVGL callbacks, test hooks |

You must NOT modify `lcd_ui.h`, `screen_internal.h`, or any stub file.

---

## Questions

Answer these before writing any code.

**Q1.** `test_lcd_ui.c` includes `lvgl_stub.h` directly (before `lcd_ui/lcd_ui.h`), even though
`lcd_ui.h` already includes `lvgl_stub.h` via the `#ifdef TEST` chain.
Why is the direct include in the test file necessary for Ceedling? What error would you see
without it?

**Q2.** `LCD_UI_TEST_VISIBLE` expands to nothing in test builds and to `static` in production.
`s_ui` and all internal callback functions are declared with this macro.
Explain why these symbols cannot be `static` in the test build, and what risks arise in
production if you simply remove `static` from them.

**Q3.** The Config screen maintains two `lcd_ui_editable_params_t` snapshots (`committed` and
`pending`). The `pending` snapshot is only written from the `config_field_changed_cb` callback
(EDITING state) and wiped with `memset` after a confirmed apply.
What race condition would arise if both the LVGL timer callback (`confirm_timeout_cb`) and the
`confirm_tapped_cb` could run concurrently? How does FreeRTOS / LVGL's single-threaded task
model prevent this?

**Q4.** `tab_change_cb` blocks navigation away from the Config screen while the sub-state is
EDITING or CONFIRMING by calling `lv_tabview_set_act(tabview, s_ui.current_tab_idx, LV_ANIM_OFF)`.
Why is this insufficient on real hardware if LVGL's scroll animation is enabled for tabviews?
What LVGL API would make the revert robust?

**Q5.** `apply_block_to_config` issues 7 sequential `set_param()` calls and returns `false` on
the first failure. What is the user-visible consequence if call #4 fails? Is the Config screen
left in a consistent state? Describe a safer approach that preserves atomicity.

---

## Implementation hints

- **Screen vtable registration**: populate `on_enter`, `on_exit`, `on_refresh` in a single
  compound initialiser for each screen context struct and cast `&scr->base` into the
  `s_ui.screens[SCR_*]` array.
- **`config_field_changed_cb` value extraction**: use `lv_spinbox_get_value(obj)` to read the
  new value, then compare against `k_field_min[idx]` and `k_field_max[idx]`. If out of range,
  revert with `lv_spinbox_set_value(obj, get_param_as_i32(&scr->pending, idx))` and show the
  error label.
- **LVGL null check before init**: `lcd_ui_init` must call `graphics_get_display()` and return
  `LCD_UI_ERR_GRAPHICS_INIT` if it returns NULL. Call `report->push_event(HEALTH_EVENT_LCD_FAIL)`
  directly — do not add a redundant NULL guard on `report` after already validating it.
- **`STATUS_METRIC_COUNT`**: the status screen has 7 fixed metric labels plus one label per
  health task stack reading. In test builds use `HEALTH_STUB_TASK_COUNT`; in production use
  `HEALTH_TASK_COUNT`. Define it in `screen_internal.h` using `LCD_UI_STATUS_STACK_LABELS`.
- **LVGL timer**: `lv_timer_create(confirm_timeout_cb, LCD_CONFIRM_TIMEOUT_MS, scr)` creates the
  timer. Store the handle. Call `lv_timer_pause` immediately after creating it (only start
  when the user taps Apply). Call `lv_timer_pause` again after firing or cancellation.

---

## Marking guide

| Criterion | Marks |
|-----------|-------|
| TC-LCDUI-001..009 pass (init/NULL/double-init/graphics-fail) | 20 |
| TC-LCDUI-010..014 pass (Sensor screen) | 15 |
| TC-LCDUI-015 pass (Status screen) | 5 |
| TC-LCDUI-016..017 pass (Alarm screen) | 10 |
| TC-LCDUI-018..027 pass (Config screen full state machine) | 30 |
| cppcheck exits 0 (no findings) | 10 |
| clang-format exits 0 (no violations) | 5 |
| Q1–Q5 answers correct and concise | 5 |
| **Total** | **100** |

### Model answers (Q1–Q5)

**A1.** Ceedling's source-dependency scanner performs a literal `#include` regex scan of the
test translation unit even with `use_test_preprocessor: TRUE`. It does not recursively evaluate
transitive includes through firmware source headers. `lvgl_stub.h` is referenced inside
`lcd_ui.h` behind `#ifdef TEST`, which Ceedling never expands during scanning. Without the
direct include in the test file, Ceedling does not detect `lvgl_stub.c` as a required
compilation unit and the linker reports undefined references to every LVGL stub symbol.

**A2.** Unity's `TEST_ASSERT_*` macros and the test TU's `extern` declarations require external
linkage to read `s_ui.initialised`, inspect widget handles, and call the internal callbacks.
`static` limits a symbol's linkage to the translation unit where it is defined; the test TU
cannot access it even with an `extern` declaration. In production, removing `static` exposes
`s_ui` and all internal callbacks in the global symbol namespace, creating potential naming
conflicts with identically-named objects in other modules (C has no namespacing mechanism).

**A3.** If both `confirm_timeout_cb` and `confirm_tapped_cb` ran concurrently, `committed`
could be partially written by `confirm_tapped_cb` (mid-copy from `pending`) while
`confirm_timeout_cb` simultaneously calls `reset_cfg_to_viewing`, wiping the sub-state and
re-populating spinboxes from `committed` in an inconsistent intermediate state. FreeRTOS /
LVGL prevents this because all LVGL event callbacks and timer callbacks execute on the same
task (LcdUiTask) within `lv_task_handler()`. The call stack is strictly sequential; there
is no preemption between LVGL callbacks.

**A4.** LVGL tabview scrolls are animated: the actual scroll position is updated incrementally
across multiple `lv_task_handler()` cycles. Calling `lv_tabview_set_act()` synchronously inside
the tab-change callback reverts the internal active-tab index, but the display may finish
rendering the in-flight animation frame to the forbidden tab before the revert takes effect,
causing a visible flash. `lv_async_call(revert_fn, NULL)` queues the revert until the top of
the next `lv_task_handler()` iteration, after all pending animations have been fully committed
or cancelled.

**A5.** If call #4 fails, calls #1–#3 have already persisted changes to flash via
`config_store`. The Config screen returns to VIEWING with `committed` unchanged (showing
pre-edit values), while the store holds a partial update. This is inconsistent: three params
were persisted, four were not. A safer approach: (1) validate all seven values before calling
`set_param` at all, or (2) implement a two-phase protocol — write to a staging region first,
then atomically commit. In this codebase, the simplest defence is a dry-run validation pass
through `k_field_min`/`k_field_max` in `apply_block_to_config` before issuing any `set_param`
call, so the function either applies all seven or none.
