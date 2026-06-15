# Handoff: Field Device LCD GUI → LVGL on STM32F469 Discovery

This bundle is everything Claude Code needs to implement the **EnvMon Field Device LCD GUI** in **LVGL 9.x** on the **STM32F469I-DISCO** board, developed in **STM32CubeIDE** (Eclipse).

---

## What's in this bundle

```
design_handoff_envmon_lvgl/
├── README.md                       ← you are here
├── CLAUDE.md                       ← read first: orientation for Claude Code
├── 01_DESIGN_TOKENS.md             ← colors, spacing, type, derived from the HTML
├── 02_FONTS_AND_ICONS.md           ← font conversion plan, icon sources, glyph table
├── 03_SCREEN_SPECS.md              ← pixel-perfect specs for the screens we ship
├── 04_LVGL_PATTERNS.md             ← LVGL 9 idioms: styles, flex/grid, widget recipes
├── 05_PROJECT_SETUP.md             ← STM32CubeIDE project, CubeMX config, lv_conf.h
├── 06_DATA_MODEL.md                ← sensor/alarm/config models + mock backend + flash store
├── 07_NAVIGATION.md                ← screen graph, state machine, touch flow
├── 08_IMPLEMENTATION_PLAN.md       ← step-by-step build order
└── design_reference/               ← the original HTML + JSX sources
    ├── Field Device LCD - P + T.html
    ├── p-aesthetic.jsx              (the aesthetic we implement)
    ├── t-aesthetic.jsx              (alternate aesthetic, reference only)
    └── design-canvas.jsx
```

---

## About the design files

The HTML in `design_reference/` is **a visual reference, not source to port**. It was built in React for browser preview to communicate the look and feel exactly. Do **not** try to translate the JSX line-by-line — re-implement the same screens natively in LVGL using LVGL widgets, styles, and the patterns in `04_LVGL_PATTERNS.md`.

The design files show you:

- Exact colors, fonts, sizes, paddings, borders
- Component anatomy and content
- All states (waiting / normal / stale / error / alarm / modal / validation)

Everything in the design files has been transcribed into structured docs (01–07). When the docs and the HTML disagree, **the docs are the source of truth** — they were prepared with LVGL constraints in mind.

---

## Fidelity

**High-fidelity** — these are pixel-perfect mockups with final colors, typography, spacing, and content. Reproduce the layout and visual treatment faithfully on hardware. Where LVGL or the F469's RGB565 framebuffer forces minor compromises (font hinting, alpha banding on tints), document the deviation in code comments.

---

## Defaults this handoff assumes

| Decision | Choice | Rationale |
|---|---|---|
| Aesthetic | **P** ("operator grade") | Calm, dense, closer to the Schneider/Siemens HMI aesthetic the device is positioned against. Variant T is in scope as a future theme switch only. |
| LVGL version | **9.x** (latest stable) | Modern API (`lv_obj_set_style_*`, native flex/grid). |
| MCU | **STM32F469NI** on the **STM32F469I-DISCO** board | Onboard 4" 800×480 capacitive-touch LCD (DSI) matches the design canvas exactly; Chrom-ART (DMA2D) accelerates LVGL blits; 2 MB flash + 384 KB SRAM. |
| Display | Onboard 800×480 DSI LCD, RGB565 framebuffer in external SDRAM | Use the BSP's LTDC + DSI init. UI layer is panel-agnostic. |
| Input | Capacitive touch only (FT6x06 controller via I²C2) | All tap targets in the design are ≥44 px. |
| RTOS | **FreeRTOS** via CMSIS-OS, LVGL on its own task | Standard with STM32CubeMX. |
| Persistence | **Internal flash** sector via ST's EEPROM-emulation library (X-CUBE-EEPROM) | No NVS on STM32; emulation gives wear-levelled key/value over two sectors. |
| Backend | **Mock data** by default; real `backend_hw.c` stub provided | Lets the UI be developed and reviewed before sensor I/O is wired. |
| Simulator | **SDL2 desktop simulator** as a separate CMake target | Lets the UI be iterated without flashing the board. |
| IDE | **STM32CubeIDE** (Eclipse-based, ST's official fork) | Generates `.ioc` → HAL init code, has LVGL component, integrates GDB + ST-LINK. |
| Build | CubeIDE-managed Makefile for hardware; CMake + SDL for simulator | Same source tree, separate build configs. |
| Language | English only, UTF-8 source | i18n hook noted but not implemented. |

If any of these are wrong, tell Claude Code in the prompt — they're isolated to `05_PROJECT_SETUP.md`, `02_FONTS_AND_ICONS.md`, and a handful of build files.

---

## What "done" looks like

A repo that:

1. Builds for both the SDL simulator (CMake) and the STM32F469I-DISCO (CubeIDE) from the same `src/` tree.
2. Renders the in-scope screens identical (within hardware tolerance) to the HTML reference.
3. Drives screens from a mock backend that ticks sensor values, can be put into `waiting / stale / error` states, and can fire alarms.
4. Persists config changes through emulated EEPROM on the device; through a `.envmon.cfg` file in the simulator.
5. Survives a 24h soak with no leaks (LVGL heap monitor stable).
6. Has a clear `backend.h` interface so swapping the mock for real I²C / Modbus is a localised change.

---

## How to use this with Claude Code

From the project root in Claude Code:

```
claude "Read design_handoff_envmon_lvgl/CLAUDE.md and follow it."
```

`CLAUDE.md` lists the docs in order and the implementation plan. Start there.
