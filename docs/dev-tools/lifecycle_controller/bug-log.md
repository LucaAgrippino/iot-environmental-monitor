# Bug Log — LifecycleController

**Date:** 2026-06-24
**Branch:** feature/phase-4-lifecycle_controller

---

## BUG-LC-001 — `console_service_stub.h` vtable field ordering mismatch

**Severity:** Medium (would cause ABI confusion; caught before any test referenced it)
**Status:** Fixed

**Description:**
The initial draft of `console_service_stub.h` declared `iconsole_service_t` with `run_once` at position 0 and `init_finalise` at position 1. The production `console_service.h` (after adding `init_finalise` in this session) has `init_finalise` at position 0 and `run_once` at position 1. If a test used position-based vtable initialisation, the wrong function would be dispatched.

**Root cause:** Stub written before the production header was finalised.

**Fix:** Swapped field order in `console_service_stub.h` to match production:
```c
typedef struct {
    console_service_err_t (*init_finalise)(void);  /* position 0 */
    console_service_err_t (*run_once)(void);        /* position 1 */
} iconsole_service_t;
```

---

## BUG-LC-002 — `BOARD_FIELD_DEVICE` redefinition warning causes Ceedling failure

**Severity:** High (blocked test compilation)
**Status:** Fixed

**Description:**
Initial drafts of `test_lifecycle_controller_fd.c` and `test_lifecycle_controller_gw.c` contained explicit `#define BOARD_FIELD_DEVICE` / `#define BOARD_GATEWAY` directives. These collide with the same defines supplied via Ceedling's `:defines:` block in `project.yml`, producing a `"BOARD_FIELD_DEVICE" redefined` preprocessor warning that causes the Ceedling build to abort.

**Root cause:** Defensive define added in the test file to ensure correct branch selection, not realising project.yml already injects it.

**Fix:** Removed the explicit `#define` lines from both test files. The `:test_lifecycle_controller_fd:` / `:test_lifecycle_controller_gw:` blocks in `project.yml` supply `BOARD_FIELD_DEVICE` / `BOARD_GATEWAY` respectively.

---

## BUG-LC-003 — `freertos_mock.c` not linked in lifecycle controller tests

**Severity:** High (linker failure: ~15 undefined FreeRTOS symbols)
**Status:** Fixed

**Description:**
`lifecycle_controller.c` calls FreeRTOS APIs (`xQueueCreateStatic`, `xTimerCreateStatic`, `xEventGroupCreateStatic`, `xQueueReceive`, `xQueueSend`, `xTimerStart`, `xTimerStop`, `xEventGroupSetBits`) at test time because the module uses them in `lifecycle_controller_init()` and in the single-step event loop. The linker produced ~15 undefined-reference errors.

**Root cause:** Ceedling's auto-link mechanism matches source files to headers by basename. `freertos_mock.c` is only linked when a test file (or a file it includes) explicitly includes `freertos_mock.h`. The test files were relying on the transitive include `FreeRTOS.h` (via `lifecycle_controller.h`), which has no matching `.c`.

**Fix:** Added `#include "freertos_mock.h"` to both test files. The `freertos_mock.h` header (in `tests/support/`) re-includes `FreeRTOS.h` and its basename gives Ceedling the match it needs to link `freertos_mock.c`.

---

## BUG-LC-004 — `gateway/**` test path exposes pre-existing `test_i2c_driver_l4` linker failure

**Severity:** Medium (would have broken `test:all`; caught and fixed before push)
**Status:** Fixed

**Description:**
Adding `- gateway/**` to the `:test:` paths in `project.yml` caused Ceedling's `test:all` to discover `tests/gateway/drivers/i2c/test_i2c_driver_l4.c`, which has a pre-existing linker error (undefined references to `i2c_write`, `i2c_read`, `i2c_write_read`). This is a separate module with its own unresolved dependency, not related to LifecycleController.

**Root cause:** The `gateway/**` glob was too broad; GW driver tests were not previously in the test sweep and have unresolved source dependencies.

**Fix:** Narrowed the path to `- gateway/application/**` so only application-layer GW tests (currently just `test_lifecycle_controller_gw`) are discovered by `test:all`. GW driver tests remain accessible via explicit `test:<name>` invocations when their dependencies are resolved.

---

## BUG-LC-005 — `lvgl_stub.h` absent causes compile error via `graphics_library_stub.h`

**Severity:** High (would block FD test compilation)
**Status:** Fixed

**Description:**
`graphics_library_stub.h` (in `tests/support/`) includes `lvgl_stub.h` to bring in `lv_disp_t` and `lv_indev_t` type definitions for the `graphics_get_display()` / `graphics_get_indev()` direct-function declarations. `lvgl_stub.h` did not exist, so any test that includes `graphics_library_stub.h` (including the lifecycle controller FD test, via `lifecycle_controller.h`) would fail to compile.

**Root cause:** The LVGL stub header was referenced but never created.

**Fix:** Created `tests/support/lvgl_stub.h` with minimal opaque struct typedefs. The FD test file provides stub bodies for the three direct functions (`graphics_process`, `graphics_get_display`, `graphics_get_indev`) since they are declared in `graphics_library_stub.h` but never called by `lifecycle_controller.c`.
