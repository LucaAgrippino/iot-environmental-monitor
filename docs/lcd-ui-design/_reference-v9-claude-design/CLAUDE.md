# Claude Code — Orientation

You're implementing the **EnvMon Field Device LCD GUI** in **LVGL 9.x** on the **STM32F469I-DISCO** board, developed in **STM32CubeIDE**.

## Read order

Read these files first, in order. Do NOT start writing code until you've read all of them.

0. **`00_PROJECT_OVERLAY.md`** — **project-specific overrides; takes precedence over every other file in this pack**
1. **`README.md`** — defaults and bundle contents (you've probably already read it)
2. **`01_DESIGN_TOKENS.md`** — the color/type/spacing system
3. **`02_FONTS_AND_ICONS.md`** — which fonts to convert, how, and the icon glyph table
4. **`03_SCREEN_SPECS.md`** — pixel specs for every in-scope screen
5. **`04_LVGL_PATTERNS.md`** — LVGL 9 idioms you'll use everywhere
6. **`05_PROJECT_SETUP.md`** — STM32CubeIDE `.ioc`, BSP, `lv_conf.h`, simulator
7. **`06_DATA_MODEL.md`** — models, mock backend, persistence (emulated EEPROM)
8. **`07_NAVIGATION.md`** — screen graph and state machine
9. **`08_IMPLEMENTATION_PLAN.md`** — the step-by-step build order — follow it

Look at the original HTML in `design_reference/` whenever the docs are ambiguous. The HTML is the authoritative visual.

## Hard rules

- **LVGL 9 only.** Don't use any v8 API names that were renamed in v9 (e.g. it's `lv_obj_set_style_bg_color`, not `lv_obj_set_style_local_bg_color`).
- **No screen-specific magic numbers in shared code.** All colors, sizes, and font references come from `theme.h` / `theme.c`. Adding a new screen must not require touching the theme module.
- **Backend is pluggable.** Every screen reads its data through the `backend_*` interface declared in `model/`. Screens must not call mock-data functions directly.
- **All screens are 800×480 absolute.** Don't try to make them responsive. The board's DSI LCD is fixed at 800×480 in landscape.
- **Touch hit-targets are ≥44 px.** The tab bar is 64 px tall by design.
- **Dark theme only.** Variant T (light/Grafana-style) is reserved for a later theme switch; don't fork the whole style system for it now.
- **Touch is the only input.** No encoder, no buttons. Config field editing uses an on-screen numeric keypad (restyled `lv_keyboard`).

## When to stop and ask

Stop and ask the user before:

- Picking a different STM32 family or board than the F469I-DISCO.
- Pulling in a third-party LVGL example repo wholesale — the patterns in this handoff are specific to this project's structure.
- Adding new screens or fields not in the design.
- Removing screens or states because they "look redundant" — every state has a use-case ID tracked in the design.

## Working style

- Build the simulator target first. Get screen 1 rendering on your desktop before touching the F469.
- Iterate one screen at a time, in the order listed in `08_IMPLEMENTATION_PLAN.md`.
- After each screen, take a screenshot of the simulator and visually diff against `design_reference/Field Device LCD - P + T.html`. Adjust until it matches.
- Don't write tests for visuals; do write tests for the data model and persistence.

Start now: read `01_DESIGN_TOKENS.md`.

