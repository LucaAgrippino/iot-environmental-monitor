# Component Inventory — Field Device LCD UI

Maps every UI component visible in the HTML mockups to its LVGL v8.3.11 widget.
Widget availability is cross-checked against `firmware/field-device/middleware/graphics_library/lv_conf.h`.

---

## LVGL widget enablement (relevant extracts from lv_conf.h)

| Widget | lv_conf.h macro | Enabled |
|--------|-----------------|---------|
| `lv_label` | `LV_USE_LABEL` | ✓ |
| `lv_btn` | `LV_USE_BTN` | ✓ |
| `lv_btnmatrix` | `LV_USE_BTNMATRIX` | ✓ |
| `lv_arc` | `LV_USE_ARC` | ✓ |
| `lv_bar` | `LV_USE_BAR` | ✓ |
| `lv_canvas` | `LV_USE_CANVAS` | ✓ |
| `lv_chart` | `LV_USE_CHART` | ✓ |
| `lv_checkbox` | `LV_USE_CHECKBOX` | ✓ |
| `lv_dropdown` | `LV_USE_DROPDOWN` | ✓ |
| `lv_img` | `LV_USE_IMG` | ✓ |
| `lv_keyboard` | `LV_USE_KEYBOARD` | ✓ |
| `lv_line` | `LV_USE_LINE` | ✓ |
| `lv_roller` | `LV_USE_ROLLER` | ✓ |
| `lv_slider` | `LV_USE_SLIDER` | ✓ |
| `lv_spinner` | `LV_USE_SPINNER` | ✓ |
| `lv_switch` | `LV_USE_SWITCH` | ✓ |
| `lv_table` | `LV_USE_TABLE` | ✓ |
| `lv_tabview` | `LV_USE_TABVIEW` | ✓ (not used — see note) |
| `lv_textarea` | `LV_USE_TEXTAREA` | ✓ |
| FLEX layout | `LV_USE_FLEX` | ✓ |
| GRID layout | `LV_USE_GRID` | ✓ |
| Montserrat 14 | `LV_FONT_MONTSERRAT_14` | ✓ |
| Montserrat 20 | `LV_FONT_MONTSERRAT_20` | ✓ |
| Montserrat 28 | `LV_FONT_MONTSERRAT_28` | ✓ |

> **Note on lv_tabview**: The design uses a custom 5-tab bar built from `lv_btn` + `lv_label`
> rather than `lv_tabview`, because `lv_tabview` does not support a fixed header-bar + footer-bar
> layout alongside the tab content area without substantial hacking. The custom approach
> gives full control over the 64 px zone heights.

---

## Component → LVGL widget mapping

### Global chrome (all operational screens)

| Component | LVGL implementation | Notes |
|-----------|---------------------|-------|
| Tab bar container | `lv_obj` with `LV_LAYOUT_GRID`, 5 columns × `LV_GRID_FR(1)` | 800 × 64 px, anchored top |
| Tab button (×5) | `lv_btn` | 160 × 64 px each; `lv_event_cb` on `LV_EVENT_CLICKED` calls `app_goto_tab()` |
| Tab icon | `lv_label` using custom font glyph (PUA codepoints) | Monochrome icon via `lv_font_conv` bitmap font |
| Tab label | `lv_label` | `FONT_TAB` (11 px sans, 700 weight, letter-space +2 px) |
| Active tab stripe | `lv_obj` child at top of `lv_btn`, 3 px height, `COL_ACCENT` | Set visible/hidden on tab switch |
| Header bar | `lv_obj` with `LV_LAYOUT_FLEX`, row direction | 800 × 40 px, `top: 64px` |
| Screen title | `lv_label` | `FONT_EYEBROW` — 10 px, 700 wt, letter-space +4 px, UPPERCASE |
| Clock label | `lv_label`, updated every second via `lv_timer` | `FONT_META` (12 px mono) |
| Status pill | `lv_label` inside `lv_obj` (border box) | Padding 3/10 px; styled per semantic state |
| Footer bar | `lv_obj` with `LV_LAYOUT_FLEX`, row direction | 800 × 36 px, anchored bottom |
| Footer labels | `lv_label` (left, right) | `FONT_CAPTION_MONO` (11 px mono) |

---

### Splash screen (`splash.html`)

| Component | LVGL implementation |
|-----------|---------------------|
| Brand logo SVG | `lv_img` from C array (converted via `lvgl-image-converter`) |
| Wordmark "ENVMON" | `lv_label`, `FONT_TITLE_LG` (30 px sans, 700 wt) |
| Subtitle | `lv_label`, `FONT_FOOTER` (10 px mono, letter-space +3 px) |
| Progress bar | `lv_bar`, `LV_BAR_MODE_NORMAL`, styled with gradient fill |
| Step name label | `lv_label`, updated at each boot sub-step callback |
| Percentage label | `lv_label`, updated alongside progress bar |
| Boot indicator dot | `lv_obj` circle (radius 4 px); outer glow = second `lv_obj` radius 7 px, 40 % opacity |
| Footer text | `lv_label`, `FONT_FOOTER` |

---

### Sensor readings (`sensor-readings.html`) — REQ-LD-010..060

| Component | LVGL implementation |
|-----------|---------------------|
| 3-column sensor grid | `lv_obj` with `LV_LAYOUT_GRID`, 3 × `LV_GRID_FR(1)`, gap 14 px, padding 22 px |
| Sensor card | `lv_obj`, `COL_SURF` bg, 1 px `COL_BORDER` border |
| Card status stripe | `lv_obj` child, width 4 px, full height; colour per semantic state |
| Channel label | `lv_label`, `FONT_EYEBROW` |
| Primary value | `lv_label`, `FONT_HERO` (54 px mono, 600 wt, letter-space −2 px) |
| Unit label | `lv_label`, `FONT_CAPTION_MONO` |
| Sparkline | `lv_line` (`LV_USE_LINE=1`), point array from history buffer tail (last 12 samples) |
| Sparkline fill area | `lv_canvas` bitmap drawn once and cached; redrawn on data update |
| "Waiting" text | `lv_label`, `FONT_VALUE` (18 px mono); visible only in `waiting` state |
| Card footer status | `lv_label`, `FONT_CAPTION_MONO`, left-aligned; colour per state |
| Card footer delta | `lv_label`, `FONT_CAPTION_MONO`, right-aligned |

---

### Sensor trend (`sensor-trend.html`) — REQ-LD-160..195

| Component | LVGL implementation |
|-----------|---------------------|
| Three stacked chart panels | `lv_obj` parent with `LV_LAYOUT_FLEX`, column direction, flex-grow 1 each |
| Chart panel | `lv_obj`, `COL_SURF` bg, 1 px `COL_BORDER` border |
| Label column (120 px) | `lv_obj` child, fixed width 120 px, `LV_FLEX_GROW(0)`, border-right 1 px |
| Channel name | `lv_label`, `FONT_EYEBROW` |
| Current value | `lv_label`, `FONT_VALUE` (18 px mono), colour per channel |
| Unit | `lv_label`, `FONT_CAPTION_MONO` |
| Min/max range | `lv_label` ×2, `FONT_CAPTION_MONO`, `COL_DIM` |
| Chart area | `lv_chart`, `LV_CHART_TYPE_LINE`, `LV_FLEX_GROW(1)` |
| Chart series (per channel) | `lv_chart_add_series()`, colour per channel |
| Invalid sample gap | `lv_chart_set_next_value(chart, series, LV_CHART_POINT_NONE)` — LVGL skips `NONE` in line rendering |
| Time axis labels | `lv_label` row below chart, 6 evenly-spaced labels (0 m … 5 m) |
| "ACCUMULATING" badge | `lv_label` inside styled `lv_obj` (border box), visible while buffer_fill < 60 |

---

### System status (`system-status.html`) — REQ-LD-070

| Component | LVGL implementation |
|-----------|---------------------|
| Section headers | `lv_label`, `FONT_EYEBROW`, with 1 px `COL_BORDER` bottom border |
| Status 2-column grid | `lv_obj` with `LV_LAYOUT_GRID`, 2 × `LV_GRID_FR(1)`, gap 1 px, `COL_BORDER` bg (creates hairline divider) |
| Status cell | `lv_obj`, `COL_SURF` bg |
| Cell label | `lv_label`, `FONT_EYEBROW`, `COL_MUTED` |
| Cell value | `lv_label`, `FONT_VALUE_SM` (13 px mono), colour per semantic meaning |
| Alert banner | `lv_obj`, `COL_ERR_TINT` bg, 1 px `COL_ERR` border; `lv_label` children |
| Degraded pill | Same structure as running pill; colour `COL_WARN`; blinks via `lv_anim` when alarms > 0 |

---

### Active alarms (`active-alarms.html`) — REQ-LD-080..090

| Component | LVGL implementation |
|-----------|---------------------|
| Empty-state icon | `lv_label` (icon glyph) inside circular `lv_obj` border-box |
| Empty-state text | `lv_label` ×2, `FONT_BODY` + `FONT_CAPTION_MONO` |
| Alarm table | `lv_table` (`LV_USE_TABLE=1`), 5 columns with fixed widths |
| Alarm row stripe | Cell in column 0, width 5 px, background colour per severity (not achievable directly in `lv_table`; alternative: `lv_obj` overlay or custom draw callback) |
| Severity chip | `lv_label` inside styled `lv_obj` border-box, per severity colour |
| ALARM pill (header) | Same as status pill; blinks 1 Hz when alarm list non-empty |

> **lv_table stripe workaround**: `lv_table` does not support per-row custom left borders natively.
> Options: (a) custom draw event callback on `LV_EVENT_DRAW_PART_BEGIN` to paint the 5 px stripe;
> (b) replace `lv_table` with a `lv_obj` FLEX column of manually constructed rows,
> each with a 5 px `lv_obj` stripe child on the left. Option (b) is recommended for design fidelity.

---

### Configuration (`configuration.html`) — REQ-LD-100..150

| Component | LVGL implementation |
|-----------|---------------------|
| Left–right layout | `lv_obj` with `LV_LAYOUT_GRID`, `[LV_GRID_FR(1), 280, LV_GRID_TEMPLATE_LAST]` |
| Field container | `lv_obj` with `LV_LAYOUT_FLEX`, column direction, gap 12 px |
| Config field label | `lv_label`, `FONT_EYEBROW` |
| Config field input | `lv_textarea`, single-line, `LV_USE_TEXTAREA=1`; styled border changes on focus/error |
| Caret | Built into `lv_textarea` — visible when focused |
| Validation error text | `lv_label`, `FONT_CAPTION_MONO`, `COL_ERR`; visible only on error |
| Numeric keyboard | `lv_btnmatrix` (`LV_USE_BTNMATRIX=1`), 3-column grid, custom key labels |
| Keyboard display | `lv_label` inside `lv_obj` border-box, shows current input |
| CANCEL / APPLY bar | `lv_obj` with `LV_LAYOUT_FLEX`, two `lv_btn` children; APPLY disabled style when invalid |
| Confirm modal | `lv_obj` full-screen container (`COL_BG` 60 % tint) + centred `lv_obj` card 380 px |
| Modal title | `lv_label`, `FONT_HEAD` (18 px sans, 600 wt) |
| Modal body | `lv_label`, `FONT_BODY` (14 px sans), `COL_MUTED`; change-summary inline |
| CANCEL / CONFIRM buttons | `lv_btn` ×2 in `LV_LAYOUT_GRID` 1:1 |

---

## Font requirements summary

All custom fonts must be converted with `lv_font_conv` and compiled into the firmware.
The three built-in Montserrat sizes (14, 20, 28) act as fallback only.

| UI token | Target family | Size | Weight | lv_font_conv output |
|----------|--------------|------|--------|---------------------|
| `FONT_HERO` | JetBrains Mono | 54 | 600 | `lv_font_jbmono_54.c` |
| `FONT_TITLE_LG` | Inter | 30 | 700 | `lv_font_inter_30.c` |
| `FONT_VALUE` | JetBrains Mono | 18 | 600 | `lv_font_jbmono_18.c` |
| `FONT_BODY` | Inter | 14 | 500 | `lv_font_inter_14.c` |
| `FONT_BODY_MONO` | JetBrains Mono | 14 | 500 | `lv_font_jbmono_14.c` |
| `FONT_CAPTION_MONO` | JetBrains Mono | 11 | 500 | `lv_font_jbmono_11.c` |
| Icons | Custom (PUA) | 18 | — | `lv_font_icons_18.c` |

See `_reference-v9-claude-design/02_FONTS_AND_ICONS.md` for full `lv_font_conv` command-lines.
