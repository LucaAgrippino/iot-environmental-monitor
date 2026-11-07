# Style Guide — Field Device LCD UI

Extracted from the v9 P-aesthetic reference (`_reference-v9-claude-design/`).
All subsequent screens must match these tokens exactly.

---

## 1. Canvas

| Property | Value |
|---|---|
| Width | **800 px** |
| Height | **480 px** |
| Pixel aspect | 1 : 1 (square pixels) |
| Colour depth | RGB565 on hardware (LVGL v8.3.11), RGB888 in the browser simulator |
| Background | `#0D1117` (`COL_BG`) |

---

## 2. Colour Palette

### Primary tokens

| Token | Hex | RGB565 | Use |
|---|---|---|---|
| `COL_BG` | `#0D1117` | `0x10A2` | App background |
| `COL_SURF` | `#161B22` | `0x18C4` | Cards, tiles, surfaces |
| `COL_SURF2` | `#1B2129` | `0x2105` | Input field background, recessed surface |
| `COL_BORDER` | `#2A2E34` | `0x2965` | All 1 px borders and dividers |
| `COL_INK` | `#E8F2FA` | `0xEF9F` | Primary text |
| `COL_MUTED` | `#A0A8B0` | `0xA516` | Secondary text, captions, labels |
| `COL_DIM` | `#6B7280` | `0x6B6C` | Disabled / tertiary |
| `COL_OK` | `#6FBF8E` | `0x6DF1` | Valid / running / success |
| `COL_WARN` | `#D4A84C` | `0xD529` | Stale / degraded / warning |
| `COL_ERR` | `#CC6666` | `0xCB2C` | Error / alarm |
| `COL_ACCENT` | `#7BAFD4` | `0x7D7A` | Brand accent / focus / primary action |
| `COL_HEADER_BG` | `#0A0E14` | `0x0861` | Tab bar, footer band |

### Tints (pre-blended, opaque)

Pre-blend these once at startup — do not rely on LVGL runtime alpha on RGB565 (banding artefacts).

| Token | Hex | How derived |
|---|---|---|
| `COL_OK_TINT` | `#152019` | `COL_OK` at 12 % over `COL_BG` |
| `COL_WARN_TINT` | `#20211A` | `COL_WARN` at 12 % |
| `COL_ERR_TINT` | `#21171A` | `COL_ERR` at 12 % |
| `COL_ACCENT_TINT` | `#13202B` | `COL_ACCENT` at 12 % |

For chip / pill elements: full-saturation token for border and text; tint for fill.

---

## 3. Typography

### Font families

The design uses **Inter** (sans-serif) and **JetBrains Mono** (monospaced).

For the LVGL implementation, convert these fonts with `lv_font_conv` as specified in
`_reference-v9-claude-design/02_FONTS_AND_ICONS.md`.

The three built-in LVGL Montserrat sizes enabled in `lv_conf.h` (14, 20, 28) are
**not used for the primary UI** — they serve as a bootstrap fallback while custom font
assets are being built.

### Scale

| Token | Family | Size (px) | Weight | Letter-spacing | Use |
|---|---|---|---|---|---|
| `FONT_HERO` | Mono | **54** | 600 | `−0.03 em` → `−2 px` | Sensor card primary value |
| `FONT_TITLE_LG` | Sans | 30 | 700 | `−0.02 em` → `−1 px` | Splash brand wordmark |
| `FONT_TITLE` | Sans | 22 | 600 | `−0.01 em` → `0 px` | Screen title-size labels |
| `FONT_HEAD` | Sans | 18 | 600 | normal | Modal title |
| `FONT_VALUE` | Mono | 18 | 600 | `−0.02 em` → `0 px` | Status row values, FW version |
| `FONT_BODY` | Sans | 14 | 500 | normal | Alarm row label, modal body |
| `FONT_BODY_MONO` | Mono | 14 | 500 | normal | Config input value |
| `FONT_LABEL` | Sans | 13 | 500 | normal | Status row label |
| `FONT_VALUE_SM` | Mono | 13 | 500 | normal | Status row value (default) |
| `FONT_META` | Mono | 12 | 500 | normal | Header time string |
| `FONT_CAPTION` | Sans | 11 | 500 | `+0.04 em` | Field label |
| `FONT_CAPTION_MONO` | Mono | 11 | 500 | normal | Sparkline delta, footer text |
| `FONT_TAB` | Sans | 11 | 600 | `+0.14 em` UPPERCASE | Tab bar labels |
| `FONT_PILL` | Mono | 10 | 700 | `+0.16 em` UPPERCASE | Status pill, ACTIVE chip |
| `FONT_EYEBROW` | Sans | 10 | 700 | `+0.22 em` UPPERCASE | Section headers above content |
| `FONT_FOOTER` | Mono | 10 | 500 | `+0.14 em` UPPERCASE | Splash footer / corner annotations |
| `FONT_TINY` | Sans | 9 | 600 | `+0.18 em` UPPERCASE | CURRENT / THRESHOLD labels |

Apply letter-spacing in LVGL via `lv_obj_set_style_text_letter_space(obj, px, sel)`.
Values in the table are rounded to the nearest integer pixel.

---

## 4. Spacing Scale

The design uses a 2 px base grid. **Only pull values from this table.**

| Token | px |
|---|---|
| `SP_1` | 4 |
| `SP_2` | 6 |
| `SP_3` | 8 |
| `SP_4` | 10 |
| `SP_5` | 12 |
| `SP_6` | 14 |
| `SP_7` | 16 |
| `SP_8` | 18 |
| `SP_9` | 22 |
| `SP_10` | 24 |
| `SP_11` | 30 |
| `SP_12` | 36 |

### Common shorthands

| Use | Value |
|---|---|
| Card inner padding | `16 18` (top/bottom · left/right) |
| Screen edge padding | `22 px` horizontal |
| Card-to-card gap (sensor grid) | `14 px` |
| Config column gap | `22 px` |
| Status 2-column gap | `36 px` |

---

## 5. Fixed Layout Zones

| Zone | Position (x, y, w, h) | Height |
|---|---|---|
| Tab bar | `0, 0, 800, 64` | 64 px |
| Header bar | `0, 64, 800, 40` | 40 px |
| Content area | `0, 104, 800, 340` | 340 px |
| Footer bar | `0, 444, 800, 36` | 36 px |

### Tab bar — 5 tabs (SRS v1.2, REQ-LD-000)

SRS v1.2 added the **sensor trend** screen; navigation now covers five operational screens.
The tab bar therefore has **five equal columns** of 160 px each (was 4 × 200 px in v9).

| Tab index | Label | Icon codepoint |
|---|---|---|
| 0 | SENSORS | `sensors` |
| 1 | TREND | `trend` (new) |
| 2 | STATUS | `status` |
| 3 | ALARMS | `alarms` |
| 4 | CONFIG | `config` |

Active tab: 3 px `COL_ACCENT` top stripe, `COL_ACCENT_TINT` background fill.
Inactive tab: icon and label `#888888`.
Tab dividers: 1 px `COL_BORDER` vertical.
Each tab is a touch button covering its full 160 × 64 px area.

### Header bar

- Left: title (`FONT_EYEBROW`, `COL_INK`), inset 22 px.
- Right: `[ time · status-pill ]`, inset 22 px. Time format `HH:MM:SS`, `FONT_META`, `COL_MUTED`.

### Footer bar

- Background `COL_HEADER_BG`, 1 px `COL_BORDER` top edge.
- Left and right text: `FONT_CAPTION_MONO`, `COL_MUTED`, inset 22 px.
- Contents vary per screen (see each screen spec).

---

## 6. Borders and Radii

The P aesthetic is **rectangular**. No border radius on cards, modals, or buttons.

| Element | Radius |
|---|---|
| Cards, tiles, modals, buttons, pills | **0 px** |
| Status dot | Full circle (`border-radius: 50%`), diameter 10 px |
| Splash boot indicator dot | Full circle, diameter 8 px |

Border widths:

| Use | Width |
|---|---|
| General borders, dividers | 1 px |
| Sensor-card left status stripe | 4 px |
| Alarm-row left status stripe | 5 px |
| Active tab top accent | 3 px |
| Firmware installing card left stripe | 3 px |

---

## 7. Elevation and Shadows

**No shadows.** The P aesthetic relies on border contrast only.

Exception: the error-state status dot uses a concentric outer ring at 40 % opacity
to simulate a soft glow — draw as a second `lv_obj` circle (⌀14 px, 40 % opacity)
behind the dot, or skip if it bands on RGB565.

---

## 8. Animation

| Element | Animation |
|---|---|
| `■ ALARM` status pill | 1 Hz blink, 50 % duty, opacity 100 % ↔ 0 %, `steps(2)` path |
| Config focused-field caret `\|` | 1 Hz blink, 50 % duty, `steps(2)` |
| Splash progress bar | Animate from 0 % → 62 % over boot, then jump to 100 % on exit |
| Sparkline shimmer (waiting) | Dashed stroke slowly scrolling, 8 px / s |

No other motion. Tab switches are **instant** — no slide transitions.

---

## 9. Component Vocabulary

### Status pill

Rectangular chip. Padding `3 px / 10 px`. Font `FONT_PILL`. Border 1 px. No radius.

| State | Label | Colour |
|---|---|---|
| `running` | `● RUNNING` | `COL_OK` |
| `init` | `● INIT` | `COL_ACCENT` |
| `alarm` | `■ ALARM` | `COL_ERR` (blink) |
| `update` | `⇪ UPDATE` | `COL_ACCENT` |

### Sensor card

Background `COL_SURF`, border 1 px `COL_BORDER`. 4 px left status stripe.
Inner padding `16 px / 18 px`.

Status stripe colour by state:

| State | Stripe colour | Value colour |
|---|---|---|
| `valid` | `COL_OK` | `COL_INK` |
| `stale` | `COL_WARN` | `COL_INK` (value still readable) |
| `error` | `COL_ERR` | `COL_DIM` (faded) |
| `alarm` | `COL_ERR` | `COL_ERR` |
| `wait` | `COL_DIM` | — (waiting text shown instead) |

### Config field

- Background `COL_SURF2`, border 1 px (`COL_BORDER` default / `COL_ACCENT` focused / `COL_ERR` error).
- Padding `9 px / 12 px`. Font `FONT_BODY_MONO`.
- Error message below: `FONT_CAPTION_MONO`, `COL_ERR`, 4 px top margin.

### Modal

Width 380 px, height auto. Background `COL_SURF`, border 1 px `COL_BORDER`. Padding 24 px.
Full-screen backdrop `rgba(0,0,0,0.60)`. Touch outside does **not** dismiss.

### Trend chart panel

Background `COL_SURF`, border 1 px `COL_BORDER`.
Left label column: 120 px wide, border-right 1 px `COL_BORDER`.
Chart area: remaining width, full panel height.
Time axis label: `FONT_CAPTION_MONO`, `COL_MUTED`, displayed as minutes elapsed (0 m … 5 m).
Y-axis labels: `FONT_CAPTION_MONO`, `COL_MUTED`, 3–4 tick marks.
Series line: 1.5 px stroke, colour by channel (Temperature `COL_ACCENT`, Humidity `COL_OK`, Pressure `COL_WARN`).
Invalid entry gaps: break in the plotted line (no interpolation across invalid samples — REQ-LD-190).

---

## 10. Touch Targets

| Element | Area | Notes |
|---|---|---|
| Tab bar button | 160 × 64 px | 5-tab layout |
| Config footer CANCEL / APPLY | ~80 × 33 px | At lower bound — acceptable |
| Modal CANCEL / CONFIRM buttons | ~120 × 33 px | |
| Sensor card (no tap action) | — | Informational only |

Minimum recommended touch target: 44 × 44 px per platform guideline.
Tab bar buttons comfortably exceed this. Footer buttons are a designed exception at their current size.

---

## 11. Firmware Version String

The splash screen footer should display the **actual** LVGL version from the vendored library.
The vendored LVGL is **v8.3.11** (as declared in `firmware/field-device/middleware/graphics_library/lv_conf.h`).
The v9 reference incorrectly shows `LVGL 9.2` — this is corrected in the new design pack.

Format: `FW v2.4.1 · build YYYYMMDD · LVGL v8.3.11`
