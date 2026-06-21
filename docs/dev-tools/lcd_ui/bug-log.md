# Bug Log — LcdUi

## Seeded bugs

No intentional seeded bug was placed in the LcdUi module.
The companion (v0.2) does not specify one for this module.

---

## BUG-LCDUI-001 — Config field callbacks: second `lv_obj_add_event_cb` overwrites first

| Field | Detail |
|-------|--------|
| **File** | `firmware/field-device/application/lcd_ui/lcd_ui.c` |
| **Line** | ~350 (spinbox event registration loop) |
| **Category** | LVGL single-slot event stub; silent overwrite |
| **Severity** | Low in production (LVGL supports multiple event callbacks per object); Informational for test builds |
| **Detectable by** | Code inspection of `lvgl_stub.c` single-slot design; NOT by TC-LCDUI-019..021 (test hooks bypass dispatch) |

### What the code does

Each spinbox has two event callbacks registered:

```c
lv_obj_add_event_cb(scr->spinbox[i], config_field_tapped_cb,   LV_EVENT_CLICKED,       NULL);
lv_obj_add_event_cb(scr->spinbox[i], config_field_changed_cb,  LV_EVENT_VALUE_CHANGED, NULL);
```

The LVGL stub in `lvgl_stub.c` stores only a single callback slot per object (`obj->event_cb`).
The second `lv_obj_add_event_cb` call overwrites the first, so querying `obj->event_cb` only
returns `config_field_changed_cb`.

### Why it does not affect production

Real LVGL v8 maintains a linked list of event descriptors per object. Both callbacks are stored
and both fire independently when their matching event code is sent. The stub's single-slot
simplification is adequate for testing because all test cases invoke callbacks directly via
`lcd_ui_test_fire_cfg_field_tapped()` and `lcd_ui_test_fire_cfg_field_changed()` without going
through the LVGL dispatch path.

### How to detect it

Inspect `lvgl_stub.c`: find that `lv_obj_add_event_cb` sets `obj->event_cb = cb` unconditionally.
Add a unit test that reads back `obj->event_cb` after two registrations and asserts both
are reachable — it will fail with the current stub.

---

## BUG-LCDUI-002 — `tab_change_cb` navigation block does not restore current tab on first tick

| Field | Detail |
|-------|--------|
| **File** | `firmware/field-device/application/lcd_ui/lcd_ui.c` |
| **Line** | ~420 (`tab_change_cb`) |
| **Category** | LVGL tabview state: revert call may fire before internal scroll animation completes |
| **Severity** | Cosmetic (flash of wrong tab; self-corrects on next 200 ms refresh) |
| **Detectable by** | On-target integration test only; host stubs are synchronous |

### What the code does

When the Config screen is EDITING/CONFIRMING and the user swipes to a different tab,
`tab_change_cb` calls `lv_tabview_set_act(tabview, s_ui.current_tab_idx, LV_ANIM_OFF)`
to revert the navigation. LVGL's tabview scroll animation is asynchronous; if the
revert fires while the animation is in-flight, the display briefly shows the forbidden
tab before snapping back.

### What it should do

Defer the revert to the next `lv_task_handler()` cycle by using `lv_async_call()`, which
queues a callback to execute at the top of the next LVGL task iteration after any
pending animations have been resolved.

### Why it does not affect tests

`lvgl_stub_fire_event` is synchronous; there is no animation subsystem in the stub.
`lv_tabview_set_act` updates the internal index immediately and the test reads it
back on the same call stack.

---

## Bugs found and fixed during implementation

| ID | File | Error | Root cause | Fix |
|----|------|-------|------------|-----|
| F1 | `test_lcd_ui.c` | Linker: undefined references to all LVGL stub symbols | Ceedling auto-link scanner does not follow transitive includes: `test_lcd_ui.c → lcd_ui.h → graphics_library_stub.h → lvgl_stub.h`. Ceedling never saw `lvgl_stub.h` as a direct include trigger | Added `#include "lvgl_stub.h"` directly at the top of `test_lcd_ui.c` with explanatory comment |
| F2 | `lcd_ui.c:755` | cppcheck `knownConditionTrueFalse`: `if (report != NULL)` always true | `report` was already validated non-NULL by the guard block at the top of `lcd_ui_init`; redundant inner check | Removed the inner NULL guard; call `report->push_event(...)` directly |
| F3 | `lcd_ui.c:504` | cppcheck `constVariablePointer`: `config_screen_t *scr` should be `const` | `config_screen_on_refresh` only reads `scr->sub_state`; no mutation through the pointer | Changed to `const config_screen_t *scr = (const config_screen_t *)self;` |
| F4 | All three module files | clang-format violations: column-aligned enum values, aligned struct assignments, aligned pointer declarators | Column alignment used for readability in hand-written code; project `.clang-format` disables `AlignConsecutive*` | Ran `clang-format -i` on `lcd_ui.c`, `lcd_ui.h`, `screen_internal.h` |
