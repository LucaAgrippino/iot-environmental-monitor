# 00 · Project Overlay (read before CLAUDE.md)

This file is an **overlay** on top of the generic design pack. The design pack
was written assuming a greenfield project; this repository is mid-flight in
Phase 4 of a V-Model lifecycle with established conventions. Where this file
conflicts with `CLAUDE.md` or any of `01_*`–`08_*`, **this file wins**.

Read this file first. Then proceed to the design pack read order from
`CLAUDE.md`, applying every override below as you read.

---

## 1. Library versions — overrides

| Topic | Design pack assumes | This project's reality |
|---|---|---|
| LVGL | 9.x | **v8.3.11 LTS** |
| Persistence | X-CUBE-EEPROM (AN3969) | `ConfigStore` over QSPI flash (already implemented) |
| RTOS | FreeRTOS via CMSIS-OS v2 (CubeMX-generated) | FreeRTOS, **static allocation only**, hand-configured (no CMSIS-OS wrapper) |
| Display init | BSP via CubeMX | HAL adopted **inside `lcd_driver.c` only** (port of `BSP_LCD_InitEx`) — application calls our driver, not BSP directly |
| Touch | BSP `BSP_TS_*` | Our `TouchscreenDriver` (already implemented) — call its interface, not BSP |

### LVGL v8 vs v9 — API deltas you will hit

Every code example in `04_LVGL_PATTERNS.md` is v9 syntax. Validate against
v8 before using. Known renames / removals:

| v9 (in pack) | v8 (this project) |
|---|---|
| `lv_display_*`, `lv_display_create` | `lv_disp_*`, `lv_disp_drv_register` |
| `lv_screen_load` | `lv_scr_load` |
| `lv_image_*`, `lv_image_dsc_t` | `lv_img_*`, `lv_img_dsc_t` |
| `lv_point_precise_t` | `lv_point_t` |
| `lv_obj_remove_flag` | `lv_obj_clear_flag` |
| `lv_indev_create` + `lv_indev_set_*` | `lv_indev_drv_register` after filling `lv_indev_drv_t` |
| `lv_sdl_window_create` | not available — use SDL2 + custom flush in simulator |

If a v9 API has no v8 equivalent, **stop and ask** before inventing one.

---

## 2. Data model — supersedes section 06

`06_DATA_MODEL.md` defines `sensor_model.h`, `alarm_model.h`,
`system_status.h`, `config_model.h`, and `backend.h`. **None of these
exist in this project, and none will be created.** They are superseded by
existing service interfaces:

| Design pack symbol | This project's equivalent | Header |
|---|---|---|
| `sensor_t`, `sensor_get()`, `sensor_snapshot()` | `sensor_snapshot_t`, `isensor_service_t->get_snapshot()` | `application/sensor_service/sensor_service.h` |
| `alarm_t`, `alarm_count()`, `alarm_get()` | `alarm_state_t`, `ialarm_service_t->get_state()`, `get_all_states()` | `application/alarm_service/alarm_service.h` |
| `cfg_field_t`, `cfg_validate()`, `cfg_apply()` | `iconfig_provider_t->get_params()`, `iconfig_manager_t->set_param()`, `validate_param()`, `snapshot()`, `restore_snapshot()` | `application/config_service/config_service.h` |
| `system_status_t`, `sysstatus_get()` | `device_health_snapshot_t`, `ihealth_snapshot_t->get_snapshot()` | `application/health_monitor/health_monitor.h` |
| `backend_poll()`, `backend.h` | `lcd_ui_task_body()` drives refresh via `on_refresh()` callbacks (already in `lcd_ui.h`) | `application/lcd_ui/lcd_ui.h` |
| `backend_set_scenario()` | Synthetic vtables in simulator main (same designated-initialiser pattern as integration tests) | `simulator/lcd-ui/sim_providers.c` (new) |

### Fixed-point sensor values — critical

`sensor_reading_t.value` is a **fixed-point integer**, not a float:
- TEMPERATURE: 0.01 °C → `2340` displays as `"23.4"`
- HUMIDITY:    0.01 %RH → `5820` displays as `"58.2"`
- PRESSURE:    0.1 hPa  → `10132` displays as `"1013"` (no decimal per
  `03_SCREEN_SPECS.md` §03)

Use integer division for formatting. No `float`, no `printf("%f")`,
no `snprintf("%.1f")`. P9 of `architecture-principles.md` forbids
floating-point in display code.

---

## 3. Folder structure — overrides section 05

`05_PROJECT_SETUP.md` describes a `src/ui/`, `src/model/`, `src/backend/`
layout. **Ignore it.** Use the existing project structure:

| Design pack path | This project's path |
|---|---|
| `src/ui/theme.{c,h}` | `firmware/field-device/application/lcd_ui/theme.{c,h}` (new) |
| `src/ui/components/*.{c,h}` | `firmware/field-device/application/lcd_ui/widgets/*.{c,h}` (new folder) |
| `src/ui/screens/screen_sensors.{c,h}` | `firmware/field-device/application/lcd_ui/lcd_ui.c` (rewrite — see §6 scope) |
| `src/model/*` | **Do not create.** Use existing services. |
| `src/backend/*` | **Do not create.** Use existing services. |
| `src/app.c` | `lcd_ui_init()` + `lcd_ui_task_body()` already in `lcd_ui.c` |
| `target/sim/` | `simulator/lcd-ui/` (already exists, scaffold only) |
| `target/f469/` | `firmware/field-device/` (CubeIDE project root) |

The CubeMX `.ioc`, `lv_port_disp.c`, `lv_port_indev.c`, and `lv_conf.h`
described in `05_PROJECT_SETUP.md` §1–§6 **already exist** in this repo
and must not be regenerated or modified.

---

## 4. Coding standards — additions

The design pack does not specify a coding standard. This project mandates:

- **BARR-C:2018** (Barr Group Embedded C Coding Standard, project subset)
- **British spelling** in all comments, identifiers, strings, and docs
  (initialise, colour, behaviour, centre, organise, optimisation)
- **snake_case** for functions and variables, **UPPER_CASE** for macros and
  constants, **module prefix** on every public symbol
  (`theme_init()`, `sensor_card_create()`, `tab_bar_set_active()`)
- **Designated initialisers** for every struct with more than one member —
  no exceptions. Past sessions produced positional-initialiser bugs that
  cost real time
- **Fixed-width integer types** (`uint8_t`, `int32_t`, `uint32_t`) — never
  `int`, `unsigned`, `long`
- **`<module>_err_t` enums** with `<MODULE>_ERR_OK = 0` for every function
  that can fail
- **No magic numbers** anywhere — every numeric literal in widget or screen
  code traces to a design pack section, cited in an inline comment:
  `/* COL_ACCENT — 01_DESIGN_TOKENS.md §Color palette */`
- **`const` correctness** on every pointer-to-const-data parameter
- **No dynamic allocation post-init** — all FreeRTOS objects, all LVGL
  styles, all widget structs are file-scope `static`
- **Doxygen-style comments** on every public header symbol
- **Header guards** `#ifndef MODULE_H` / `#define MODULE_H` / `#endif`
- **`clang-format -i`** on every file in every commit

---

## 5. Architecture & service consumption

This project follows ten architectural principles in
`architecture-principles.md` (Claude project knowledge). The principles
relevant to LcdUi:

- **P1 — Strict directional layering.** LcdUi is in the Application layer.
  It consumes service interfaces (vtables) declared by other Application
  components. It does not call drivers directly.
- **P2 — Dependency Inversion.** LcdUi depends on the **abstractions**
  (`isensor_service_t`, `ialarm_service_t`, `iconfig_provider_t`,
  `ihealth_snapshot_t`), not on concrete implementations. The simulator
  proves this by supplying synthetic vtables.
- **P3 — Interface Segregation.** LcdUi consumes `iconfig_provider_t`
  (read side, single slot `get_params`) and `iconfig_manager_t` (write
  side) **separately**. The integration test bug from a recent session
  was a violation of this — don't repeat it.
- **P9 — No floating-point in display code.** Use integer division for
  fixed-point formatting (see §2 above).

When `lcd_ui_init()` is called, it receives six pointers:
```c
lcd_ui_err_t lcd_ui_init(
    const isensor_service_t  *sensors,
    const ialarm_service_t   *alarms,
    const iconfig_provider_t *cfg_read,
    const iconfig_manager_t  *cfg_write,
    const ihealth_snapshot_t *health,
    const ihealth_report_t   *report);
```

**Keep this signature byte-for-byte.** It is locked by the LLD companion
v0.2 (`docs/lld/application/lcd-ui-lld.md`) and exercised by the Pass-H
integration test (`firmware/field-device/integration-tests/lcd_ui/`).

---

## 6. Scope of this work package — overrides section 08

`08_IMPLEMENTATION_PLAN.md` lists 10 milestones spanning the full UI.
**This work package targets Milestones 1, 2, and 4 only** (Theme, Chrome,
Sensors screen), end to end. Milestones 5–9 are deferred to separate
follow-up packages.

### In scope

- `theme.c` / `theme.h` — colours, text styles, structural styles
- `widgets/tab_bar.{c,h}` — custom tab bar (not `lv_tabview`)
- `widgets/header_bar.{c,h}` — title + clock + status pill
- `widgets/footer_bar.{c,h}` — left/right caption text
- `widgets/status_pill.{c,h}` — four states, ALARM blink
- `widgets/sensor_card.{c,h}` — full card anatomy
- **Rewrite from scratch** `lcd_ui.c` and `screen_internal.h` — sensors
  screen built with the widgets above, Status/Alarms/Config kept as
  minimal stubs that satisfy the existing `lcd_ui_init()` contract
- `simulator/lcd-ui/sim_providers.c` — synthetic vtables
- `simulator/lcd-ui/main.c` — modified to call `lcd_ui_init()` and
  drive the refresh loop
- `simulator/lcd-ui/CMakeLists.txt` — add firmware sources to build

### Deferred (do not produce — separate work packages)

- Status, Alarms, Config screen visual rewrites (keep minimal stubs)
- Splash screen
- Firmware Update screen (out of scope per `07_NAVIGATION.md`)
- Real Inter / JetBrains Mono font conversion via `lv_font_conv`
  → use `lv_font_montserrat_*` placeholders at closest size, with
  `TODO(fonts):` comments at every binding site
- Real `lv_canvas` sparkline → render a 200×34 placeholder box with
  a 1 px border in status colour, `TODO(sparkline):` comment
- Tab bar icons → text-only labels for now,
  `TODO(icons):` comment
- 5-minute delta calculation → display `"—"` for now,
  `TODO(delta):` comment
- Persistence (already implemented via `ConfigStore` — out of scope)

### Stubs for Status / Alarms / Config

These three screens already have working logic in the current `lcd_ui.c`
that reads from health, alarm, and config services. **Preserve that logic**
in the rewrite — copy the `on_refresh()` bodies and the metric-formatting
code verbatim into the new file, just without the elaborate styling. Each
of these screens should render bare labels under the new tab bar + header
chrome until its dedicated work package lands.

---

## 7. Hard prohibitions

These are project-wide rules and apply unconditionally:

- **Never `git add -f`**. If a file is git-ignored, it must stay ignored.
  Find a different solution.
- **Never modify `.gitignore`** to enable an addition that was blocked.
- **Never touch** `.cproject`, `.project`, `.settings/`, or any other
  CubeIDE-generated file. These are managed by the IDE.
- **Never commit directly to `main`**. All work goes on
  `feature/lcd-ui-design-pack` (this branch). PR + green CI before merge.
- **Never create files outside the F469 Field Device tree** during this
  work package. No Gateway artefacts, no Gateway tests, no Gateway
  simulator entries.
- **Never invent file existence**. If a file is not visible via `view`
  or `ls`, it does not exist — do not reference it in code or commits.

---

## 8. Workflow — 13-step Claude Code module prompt

Follow `docs/dev-tools/claude-code-module-prompt.md` end to end.
Specifically:

- **Step 0**: branch from latest `main`. Branch name:
  `feature/lcd-ui-design-pack`. Verify clean working tree before any work.
- **One logical commit per deliverable** (theme, each widget, lcd_ui
  rewrite, sim wiring). Conventional prefixes (`feat:`, `fix:`, `style:`,
  `docs:`, `chore:`), **no parenthesised scope**.
- **Per-commit `clang-format -i`** on every modified file.
- **Six CI checks** must stay green after every push: Ceedling host tests,
  F469 ARM build, L475 ARM build, cppcheck, clang-format, markdown lint.
- **Hard-fail guard**: if any rule in §7 trips, **stop and ask** — do not
  work around it.

---

## 9. Anti-patterns — explicitly forbidden

These failure modes have occurred in past sessions on this project.

- ❌ **Bare `lv_label_create()` with no style.** Every label MUST have a
  text style applied via `lv_obj_add_style()` and a position set. The
  current `lcd_ui.c` is the cautionary tale.
- ❌ **LVGL's stock `lv_tabview`.** The design pack specifies a custom
  64 px tab bar. Use the `tab_bar` widget.
- ❌ **Magic numbers.** No raw `64`, `0x7BAFD4`, `22`, `200` in screen
  or widget code without an inline comment citing a design pack section.
- ❌ **Positional vtable initialisers.** Every vtable instance —
  production and simulator — uses designated initialisers
  (`.get_snapshot = ...`). A recent integration test had a function in
  the wrong slot because of positional initialisation.
- ❌ **Raw fixed-point display.** Temperature `2340` must display as
  `"23.4"`, not `"2340"`. Pressure `10132` must display as `"1013"`.
- ❌ **LVGL v9 API.** This project is LVGL v8.3.11. See §1 for deltas.
- ❌ **Floating-point in display code.** Violates P9. Use integer
  division.
- ❌ **Inventing files Luca hasn't confirmed.** Do not reference
  Gateway-phase files, do not invent test files, do not assume a header
  exists because the pattern suggests it should. **`view` first.**
- ❌ **Rewriting `lcd_ui_init()` signature.** Locked by Pass-H test plan.

---

## 10. Verification gate — completion definition

You are not done until **every one of these** passes.

### Build gates

- [ ] Firmware builds clean: F469 ARM build CI check green
- [ ] L475 ARM build CI check green (should be unaffected, verify)
- [ ] Ceedling host tests green
- [ ] cppcheck clean
- [ ] clang-format clean
- [ ] Markdown lint clean
- [ ] Simulator builds: `cd simulator/lcd-ui && make build` succeeds
- [ ] Simulator runs: `make run` opens an 800×480 SDL window on WSLg

### Visual gate

- [ ] Simulator Sensors screen screenshot saved to
      `docs/lcd-ui-design/screenshots/sensors-iter1.png`
- [ ] Side-by-side comparison with
      `docs/lcd-ui-design/_reference-v9-claude-design/` HTML
      (browser-render). The layout, chrome, card anatomy, and values must
      match within hardware tolerance. Values must read `"23.4 °C"`,
      `"58.2 %RH"`, `"1013 hPa"` — NOT `"2340"`, `"5820"`, `"10132"`.

### Hardware gate

- [ ] Flash to F469. Sensors screen renders with the same layout as the
      simulator. Tab labels readable. Tap on Status / Alarms / Config
      switches to the minimal-stub screens (expected for this iteration).

### Citation report

- [ ] `docs/dev-tools/lcd-ui-rebuild-citations.md` exists and lists every
      numeric value introduced in this work package with its source:
      ```
      | Value | File:line | Source |
      |---|---|---|
      | 0x0D1117 | theme.c:42 | 01_DESIGN_TOKENS.md §Color palette `COL_BG` |
      | 64 | tab_bar.c:18 | 01_DESIGN_TOKENS.md §Spacing scale Tab bar height |
      ```

This report is the deliverable that proves no values were invented.

---

## 11. When to stop and ask

Beyond `CLAUDE.md` §When to stop and ask, also stop before:

- A v9 API in `04_LVGL_PATTERNS.md` has no v8 equivalent
- A design pack value conflicts with a project-knowledge document
  (`architecture-principles.md`, the LLD companion, `components.md`)
- The simulator wiring requires editing firmware sources beyond
  `#ifndef TEST` guards
- A CI check fails and the cause is not immediately localisable to your
  changes
- You hit a policy fork on state-mapping logic (valid/stale/error
  thresholds), time formatting, or fixed-point precision

---

Begin with §1 of this file, then proceed to `CLAUDE.md` §Read order,
applying every override above as you read. Acknowledge the read order
before writing any code.
