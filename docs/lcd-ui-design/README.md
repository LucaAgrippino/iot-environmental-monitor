# LCD UI Design Pack — Field Device

Design mockups for the STM32F469 Discovery field device LCD (800 × 480, RGB565, LVGL v8.3.11).

All files are self-contained HTML — open directly in any browser with no server or CDN required.

---

## File index

| File | Contents |
|------|----------|
| `splash.html` | Boot splash — 3 states: booting 28%, booting 62%, boot complete 100% |
| `sensor-readings.html` | Sensor readings — 4 states: normal, waiting, stale (S3), error (S1) |
| `sensor-trend.html` | Sensor trend (new in SRS v1.2) — 2 states: accumulating, full 5-min window with gap |
| `system-status.html` | System status — 2 states: healthy, degraded (S3 offline + 1 alarm) |
| `active-alarms.html` | Active alarms — 2 states: empty, 2 active alarms (1 critical, 1 warning) |
| `configuration.html` | Configuration — 3 states: idle, validation error, confirm modal |
| `style-guide.md` | Complete design token reference (colours, typography, spacing, layout zones, components) |
| `navigation-map.md` | App state machine, tab transition table, per-screen state triggers |
| `component-inventory.md` | Component → LVGL v8.3.11 widget mapping, cross-checked against lv_conf.h |

---

## Design reference

These files were evolved from the v9 P-aesthetic reference at
`_reference-v9-claude-design/`. The v9 reference (`p-aesthetic.jsx`) is the
authoritative visual spec; any property not explicitly overridden here matches v9 exactly.

Key changes from v9:

- **5-tab navigation bar** (was 4 tabs) — SRS v1.2 REQ-LD-000 adds the sensor trend screen.
  Tab width changes from 200 px to 160 px.
- **Sensor trend screen** — new screen not present in v9. Designed in v9 visual language.
  See `sensor-trend.html` and REQ-LD-160..195.
- **LVGL version corrected** — v9 showed "LVGL 9.2" in the splash footer. The vendored
  library (`firmware/field-device/middleware/graphics_library/lv_conf.h`) is LVGL **v8.3.11**.
  All screens show `LVGL v8.3.11`.
- **No external CDNs** — v9 loaded React, Babel, and Google Fonts from the internet.
  New files use system font stacks only and contain no external dependencies.

---

## How to view

Open any `.html` file in a browser. Each file renders all its states stacked vertically so
you can compare them without switching. The 800 × 480 frame outline is drawn in `COL_BORDER`
to show the exact LCD boundary.

Recommended viewport: at least 900 px wide. Zoom to 100 % for pixel-accurate measurements.

---

## Hardware target

| Property | Value |
|----------|-------|
| MCU | STM32F469NIH6 |
| Display | 4 inch capacitive touch, 800 × 480 |
| Colour depth | RGB565 |
| Graphics library | LVGL v8.3.11 (vendored) |
| Available fonts | Montserrat 14, 20, 28 (built-in LVGL); custom Inter + JetBrains Mono via lv_font_conv |

---

## SRS traceability

| Requirement range | Screen |
|-------------------|--------|
| REQ-LD-000 | Navigation (5-tab bar, all screens) |
| REQ-LD-010..060 | `sensor-readings.html` |
| REQ-LD-070 | `system-status.html` |
| REQ-LD-080..090 | `active-alarms.html` |
| REQ-LD-100..150 | `configuration.html` |
| REQ-LD-160..195 | `sensor-trend.html` |
| REQ-LD-200..240 | `splash.html` |
