# 08 · Implementation Plan

Build in this order. Each milestone is independently verifiable in the SDL simulator. Don't move on until the current milestone renders correctly against `design_reference/Field Device LCD - P + T.html` (the **P / top row**).

**Scope for this build:** the **P aesthetic only**, and the **Firmware Update screen is skipped**. That leaves the Splash screen plus the four tab screens (Sensors, Status, Alarms, Config) with their internal states.

---

## Milestone 0 — Toolchain & skeleton

- [ ] Install SDL2 on the dev host.
- [ ] Folder structure from `05_PROJECT_SETUP.md` created; shared `src/` tree empty stubs.
- [ ] **Simulator**: `target/sim/CMakeLists.txt` builds; `main_sim.c` opens a blank 800×480 SDL window in `COL_BG`.
- [ ] **Hardware**: STM32CubeIDE project for the STM32F469I-DISCO created, BSP enabled, clocks at 180 MHz, FreeRTOS (CMSIS-OS v2) enabled, LVGL pack added with `LV_COLOR_DEPTH=16` and 800×480.
- [ ] `lv_port_disp.c` / `lv_port_indev.c` skeletons in `target/f469/lv_port/` — DSI LCD on, touch initialised, LVGL `lv_timer_handler()` running on the LVGL task.

**Done when:** both targets boot to a dark 800×480 surface with no LVGL log errors. On the F469, blank screen + responsive touch (verify with a debug `printf` of touch coordinates).

---

## Milestone 1 — Theme & fonts

- [ ] Source TTFs (Inter, JetBrains Mono) dropped in `scripts/fonts/src/`.
- [ ] `scripts/convert_fonts.sh` runs, generates font `.c` files in `src/assets/fonts/`.
- [ ] Icon font (or PNG sprite) generated for the 8 line icons.
- [ ] `theme.c` initializes all colors and text styles from `01_DESIGN_TOKENS.md`.
- [ ] A scratch screen renders one label in each text style to eyeball the type scale.

**Done when:** the type-scale scratch screen matches the sizes/weights/colors in the tokens doc. Delete the scratch screen after.

---

## Milestone 2 — Chrome components

Build the three shared bars + the status pill. These appear on every tab.

- [ ] `tab_bar.c` — 4 columns, icons + labels, active highlight (3 px accent top stripe + tinted bg), 1 px dividers. Tap emits `app_goto_tab()`.
- [ ] `header.c` — title left, time + status pill right. `header_refresh_time()` updates the clock.
- [ ] `footer.c` — left/right caption text, configurable per screen.
- [ ] `status_pill.c` — all four states (running/init/alarm/update), alarm blinks.

**Done when:** a placeholder screen shows tab bar + header + footer correctly, tapping tabs logs the target, and the alarm pill blinks at 1 Hz.

---

## Milestone 3 — Models & mock backend

- [ ] `sensor_model`, `alarm_model`, `system_status`, `config_model` structs + accessors.
- [ ] `backend_mock.c` implementing `backend.h`, with all six scenarios.
- [ ] `backend_set_scenario()` wired to number keys 1–6 in the simulator.
- [ ] An `lv_timer` calls `backend_poll()` and logs sensor values so you can confirm the random-walk works.

**Done when:** pressing 1–6 in the sim flips scenarios and the logged model values change accordingly. No UI yet.

---

## Milestone 4 — Sensors screen (states 03–06)

This is the flagship screen; nail it before the others.

- [ ] `sparkline.c` — `lv_canvas`, stroke + filled area, dashed variant for stale.
- [ ] `sensor_card.c` — full anatomy from `03_SCREEN_SPECS.md`: stripe, label, hero value + unit, sparkline, footer status + delta.
- [ ] `sensor_card_bind()` switches between value / waiting and applies state colors.
- [ ] `screen_sensors.c` — 3-up grid, binds the three model sensors, `screen_sensors_refresh()` updates in place.

Verify each state with the scenario keys:

- [ ] `1` Normal — three valid cards, accent sparklines, deltas.
- [ ] `2` Waiting — "Waiting for data…", no sparkline, init pill.
- [ ] `3` Stale — S3 warn stripe + dashed warn sparkline.
- [ ] `4` Error — S1 err stripe, faded value, "✕ ERROR · last …".

**Done when:** all four states match the HTML reference side by side.

---

## Milestone 5 — Status screen (states 07–08)

- [ ] `status_row.c` — dot + label + value, bottom divider, status colors.
- [ ] `screen_status.c` — 2-column section layout, all rows from the spec.
- [ ] Degraded banner (state 08) toggled by `system_status.banner_active`.
- [ ] `screen_status_refresh()` updates values + row statuses.

**Done when:** scenario `1` shows the healthy grid; scenario `6` (degraded) shows the red banner, alarm pill, and the flipped row statuses.

---

## Milestone 6 — Alarms screen (states 09–10)

- [ ] `alarm_card.c` — 4-cell grid row (label/current/threshold/pill), severity stripe.
- [ ] `screen_alarms.c` — empty state (centered check + copy) vs active list.
- [ ] Chosen by `alarm_count()`.

**Done when:** scenario `1` shows the empty state; scenario `5` (alarm) shows the two-row active list matching the reference.

---

## Milestone 7 — Config screen (states 11–13)

The most interactive screen.

- [ ] `field.c` — label + hint, input box, focus border + blinking caret, error message + error border.
- [ ] `screen_config.c` — 3-column section layout, all eight fields, footer with pending count + CANCEL/APPLY.
- [ ] `cfg_validate()` wired so out-of-range fields show error (state 12) and disable APPLY.
- [ ] `modal.c` — confirm dialog (state 13) with diff block; CONFIRM routes through `cfg_apply()` → persistence.
- [ ] On-screen numeric keypad: restyled `lv_keyboard` (`LV_KEYBOARD_MODE_NUMBER`), slides up on focus, themed to match `theme.h`.

**Done when:** you can edit a field, drive it out of range to see the error state, bring it back, tap APPLY, see the confirm modal with a correct diff, and confirm → values persist (check `~/.envmon/cfg` in the sim).

---

## Milestone 8 — Splash & app state machine

- [ ] `screen_splash.c` — brand lockup, animated progress bar, boot annotations, version/target footer.
- [ ] `app.c` state machine: SPLASH → RUNNING on first valid reading or 5 s timeout.
- [ ] Leave the `STATE_FIRMWARE` enum slot + `// TODO: OTA` comment, but no firmware screen.
- [ ] Boot sequence: splash → sensors-waiting → sensors-normal feels real via the mock auto-transition.

**Done when:** cold start shows the splash for ≥1.5 s, then lands on the Sensors tab in waiting → normal.

---

## Milestone 9 — Persistence & polish

- [ ] `persistence_file.c` (sim) round-trips config across restarts.
- [ ] Header clock ticks every second.
- [ ] LVGL mem monitor stable over a 10-min soak (no growth).
- [ ] Visual diff pass: screenshot each screen/state, overlay against the HTML reference, fix spacing/color drift.

**Done when:** every screen state matches the reference and the sim survives a soak with flat memory.

---

## Milestone 10 — Sensor hardware integration

The board itself is the target from milestone 0 onward; this milestone is about the *sensor* side of the I/O.

- [ ] `backend_hw.c` reads real sensors over whichever bus the device exposes (I²C, Modbus over UART, analog).
- [ ] `backend_mock.c` stays in the tree behind a compile flag for bench testing.
- [ ] X-CUBE-EEPROM (`persistence_eeprom.c`) confirmed working: write a value, power-cycle, value persists. Verify the last two flash sectors are reserved correctly in the linker.
- [ ] DMA2D-accelerated `flush_cb` confirmed (frame rate ≥30 FPS while a sparkline animates).
- [ ] Touch hit-targets sanity-checked on the actual LCD (finger, not just stylus).
- [ ] RGB565 tints (`COL_*_TINT`) checked for banding — re-blend with a slightly different alpha if a tint looks dirty.
- [ ] `TARGET_NAME_STR` set to `"STM32F469 · LVGL · 800×480"`.

**Done when:** the device boots to the Sensors tab on real hardware, reads live sensors, and persists config across power cycles.

---

## Verification checklist (per screen)

For each milestone screen, confirm:

1. Layout matches the reference at 800×480 (positions, gaps, paddings).
2. Colors pulled from theme tokens — no stray hex literals in screen code.
3. Fonts/sizes match the type scale.
4. All states reachable via scenario keys and visually correct.
5. `*_refresh()` updates in place without leaking objects (watch the mem monitor).
6. No LVGL log warnings at runtime.
