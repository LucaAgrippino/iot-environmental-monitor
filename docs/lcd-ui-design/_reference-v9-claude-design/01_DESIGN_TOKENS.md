# 01 · Design Tokens

All hex values are taken verbatim from `design_reference/p-aesthetic.jsx` (the `PT` object). Variant T is documented at the end for reference.

## Canvas

| | Value |
|---|---|
| Width | **800 px** |
| Height | **480 px** |
| Pixel aspect | 1:1 (square pixels) |
| Color depth | RGB565 on hardware, RGB888 in simulator |
| Background | `#0D1117` |

## Color palette — P (primary)

| Token | Hex | RGB565 | Use |
|---|---|---|---|
| `COL_BG`            | `#0D1117` | `0x10A2` | App background |
| `COL_SURF`          | `#161B22` | `0x18C4` | Cards, tiles, surfaces |
| `COL_SURF2`         | `#1B2129` | `0x2105` | Input field background, recessed surface |
| `COL_BORDER`        | `#2A2E34` | `0x2965` | All 1 px borders and dividers |
| `COL_INK`           | `#E8F2FA` | `0xEF9F` | Primary text |
| `COL_MUTED`         | `#A0A8B0` | `0xA516` | Secondary text, captions, labels |
| `COL_DIM`           | `#6B7280` | `0x6B6C` | Disabled / tertiary |
| `COL_OK`            | `#6FBF8E` | `0x6DF1` | Valid / running / success |
| `COL_WARN`          | `#D4A84C` | `0xD529` | Stale / degraded / warning |
| `COL_ERR`           | `#CC6666` | `0xCB2C` | Error / alarm |
| `COL_ACCENT`        | `#7BAFD4` | `0x7D7A` | Brand accent / focus / primary action |
| `COL_HEADER_BG`     | `#0A0E14` | `0x0861` | Tab bar background, footer band |

### Tints (used for chip / pill backgrounds)

Used at ~8% opacity over `COL_BG`. Pre-compute these once and store them as opaque colors (LVGL's alpha blending on RGB565 has banding):

| Token | Hex (pre-blended on `#0D1117`) | Source |
|---|---|---|
| `COL_OK_TINT`     | `#152019` | `OK` at 12% |
| `COL_WARN_TINT`   | `#20211A` | `WARN` at 12% |
| `COL_ERR_TINT`    | `#21171A` | `ERR` at 12% |
| `COL_ACCENT_TINT` | `#13202B` | `ACCENT` at 12% |

For chip text + border, use the full-saturation token; for the chip fill, use the tint.

## Typography scale

The design uses two families:

- **Sans** — Inter (`weights: 400 / 500 / 600 / 700`)
- **Mono** — JetBrains Mono (`weights: 400 / 500 / 600 / 700`)

| Token | Family | Size (px) | Weight | Tracking | Use |
|---|---|---|---|---|---|
| `FONT_HERO`         | Mono | **54** | 600 | `-0.03em` | Sensor card primary value (`23.4`) |
| `FONT_TITLE_LG`     | Sans | 30 | 700 | `-0.02em` | Splash brand wordmark |
| `FONT_TITLE`        | Sans | 22 | 600 | `-0.01em` | Firmware "Updating firmware" |
| `FONT_HEAD`         | Sans | 18 | 600 | normal | Modal title, alarm row sensor name |
| `FONT_VALUE`        | Mono | 18 | 600 | `-0.02em` | Status row values, FW version |
| `FONT_BODY`         | Sans | 14 | 500 | normal | Alarm row label |
| `FONT_BODY_MONO`    | Mono | 14 | 500 | normal | Config input value |
| `FONT_LABEL`        | Sans | 13 | 500 | normal | Status row label |
| `FONT_VALUE_SM`     | Mono | 13 | 500 | normal | Status row value (default) |
| `FONT_META`         | Mono | 12 | 500 | normal | Header time string |
| `FONT_CAPTION`      | Sans | 11 | 500 | `+0.04em` | Field label, footer text |
| `FONT_CAPTION_MONO` | Mono | 11 | 500 | normal | Sparkline delta, mono captions |
| `FONT_TAB`          | Sans | 11 | 600 | `+0.14em` UPPERCASE | Tab bar labels |
| `FONT_PILL`         | Mono | 10 | 700 | `+0.16em` UPPERCASE | Status pill, ACTIVE chip |
| `FONT_EYEBROW`      | Sans | 10 | 700 | `+0.22em` UPPERCASE | Section headers above content |
| `FONT_FOOTER`       | Mono | 10 | 500 | `+0.14em` UPPERCASE | Splash footer / corner annotations |
| `FONT_TINY`         | Sans | 9 | 600 | `+0.18em` UPPERCASE | "CURRENT" / "THRESHOLD" labels |

**Critical:** LVGL fonts don't have CSS letter-spacing. Bake tracking into the font-conversion command, or simulate by adjusting `lv_obj_set_style_text_letter_space()` per-style. The tracking column is given in `em`; multiply by font size to get px. See `02_FONTS_AND_ICONS.md` for the conversion plan.

## Spacing scale

The design uses unitless multiples of 2 px. Treat these as a hard scale — do not pull other values.

| Token | px |
|---|---|
| `SP_1`  | 4 |
| `SP_2`  | 6 |
| `SP_3`  | 8 |
| `SP_4`  | 10 |
| `SP_5`  | 12 |
| `SP_6`  | 14 |
| `SP_7`  | 16 |
| `SP_8`  | 18 |
| `SP_9`  | 22 |
| `SP_10` | 24 |
| `SP_11` | 30 |
| `SP_12` | 36 |

Common shorthands used in the design:

- Card inner padding: `16 18` (top/bottom · left/right) → `SP_7 SP_8`
- Screen edge padding: `22` horizontal → `SP_9`
- Card-to-card gap on sensor grid: `14` → `SP_6`
- Header bar height: `40` px (fixed)
- Tab bar height: `64` px (fixed)
- Footer bar height: `36` px (fixed)
- Content area: `480 - 64 - 40 - 36 = 340` px

## Borders & radii

The P aesthetic is **rectangular** — no border radius on cards, modals, or buttons. The only rounded element is the status dot (full circle, ⌀10 px).

| Token | px |
|---|---|
| `RAD_NONE`   | 0 |
| `RAD_PILL`   | 9999 (only for status dots and the splash spinner dot) |

Border width is always **1 px** except:

- The left status stripe on sensor cards and alarm rows is **4 px** (cards) / **5 px** (alarm rows).
- The top accent on the active tab is **3 px**.
- The accent-bordered modal-target card uses **1 px** in `COL_ACCENT`.

## Elevation / shadows

There are **no shadows** in the P aesthetic. The only "glow" is the red status dot when in alarm state — implement as a second concentric circle behind the dot, not as a real shadow.

## Animation

- Status-pill blink (`alarm` state): 1 Hz, 50% duty, opacity 100% ↔ 0%.
- Caret blink in focused config field: 1 Hz, 50% duty (steps(2)).
- Splash progress bar: animate from current value toward 62% over boot duration, then jump to 100% on splash exit.
- Sparkline shimmer (waiting state): dashed line slowly scrolling left → right at 8 px/s.

No other motion. The design is intentionally calm — no shimmers, no slide-in transitions between tabs. Tabs swap instantly.

---

## T aesthetic (alternate — for reference only)

If the user later asks for the T variant, the tokens that differ:

- `BG = #0A0D12`, `SURF = #13171D`, `BORDER = #1F242C`, `BORDER_SOFT = #2A3038`
- All cards have **6 px** border radius; tiles **8 px**; modal **10 px**; buttons **6 px**
- Tab bar is **48 px** top bar + **32 px** segmented tab strip (not a 64 px icon-above-label strip)
- Hero numeric value is **36 px** (not 54 px)
- Status chips use translucent fill (`COL × 0x1F`) instead of bordered

Treat T as a theme variant: if you build it, fork `theme.c` only.
