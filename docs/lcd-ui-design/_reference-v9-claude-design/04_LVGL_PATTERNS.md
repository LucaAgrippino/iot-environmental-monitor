# 04 · LVGL Patterns

LVGL 9 idioms for this project. Read this once; then refer back when a screen needs a pattern you haven't used yet.

## Theme initialization

`theme.h` declares public style handles. `theme.c` initializes them at boot and exposes them as `extern`. Every screen pulls styles from here — no inline `lv_obj_set_style_*` calls in screen code unless they're truly screen-specific (e.g. dynamic value color).

```c
// theme.h
#pragma once
#include "lvgl.h"

// Colors
extern lv_color_t COL_BG, COL_SURF, COL_SURF2, COL_BORDER;
extern lv_color_t COL_INK, COL_MUTED, COL_DIM;
extern lv_color_t COL_OK, COL_WARN, COL_ERR, COL_ACCENT, COL_HEADER_BG;
extern lv_color_t COL_OK_TINT, COL_WARN_TINT, COL_ERR_TINT, COL_ACCENT_TINT;

// Styles
extern lv_style_t st_screen;     // root: bg, no border, no pad
extern lv_style_t st_card;       // COL_SURF, 1px COL_BORDER, no radius
extern lv_style_t st_card_accent;// same with COL_ACCENT border
extern lv_style_t st_surf2;      // COL_SURF2 input bg
extern lv_style_t st_divider;    // 1px COL_BORDER bottom (for status rows)

// Text styles — call with lv_obj_add_style after applying base font
extern lv_style_t st_tx_hero, st_tx_title_lg, st_tx_title, st_tx_head;
extern lv_style_t st_tx_value, st_tx_body, st_tx_body_mono;
extern lv_style_t st_tx_label, st_tx_value_sm, st_tx_meta;
extern lv_style_t st_tx_caption, st_tx_caption_mono;
extern lv_style_t st_tx_tab, st_tx_pill, st_tx_eyebrow;
extern lv_style_t st_tx_footer, st_tx_tiny;

void theme_init(void);
```

```c
// theme.c — fragment
lv_color_t COL_BG; lv_color_t COL_INK; // ...
lv_style_t st_card;
lv_style_t st_tx_hero;

void theme_init(void) {
    COL_BG    = lv_color_hex(0x0D1117);
    COL_SURF  = lv_color_hex(0x161B22);
    COL_INK   = lv_color_hex(0xE8F2FA);
    // ... all colors

    lv_style_init(&st_card);
    lv_style_set_bg_color(&st_card, COL_SURF);
    lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
    lv_style_set_border_color(&st_card, COL_BORDER);
    lv_style_set_border_width(&st_card, 1);
    lv_style_set_radius(&st_card, 0);
    lv_style_set_pad_all(&st_card, 0); // padding set per-instance

    lv_style_init(&st_tx_hero);
    lv_style_set_text_font(&st_tx_hero, &font_jb_sb_54);
    lv_style_set_text_color(&st_tx_hero, COL_INK);
    lv_style_set_text_letter_space(&st_tx_hero, -2);
    // ... all text styles
}
```

Apply a text style with `lv_obj_add_style(label, &st_tx_hero, 0)`.

## Screen objects

Every screen is a function that returns an `lv_obj_t *` rooted at an `lv_obj_create(NULL)` screen. The navigator (`ui.c`) is the only thing that calls `lv_scr_load()`.

```c
// screen_sensors.h
lv_obj_t *screen_sensors_create(sensor_variant_t v);
void      screen_sensors_update(lv_obj_t *scr); // called from data timer
```

Per-screen pattern:

```c
lv_obj_t *screen_sensors_create(sensor_variant_t v) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &st_screen, 0);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    tab_bar_create(scr, TAB_SENSORS);
    header_create(scr, "SENSOR READINGS", STATUS_RUNNING);

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, 0, 104);
    lv_obj_set_size(content, 800, 340);
    lv_obj_set_style_pad_all(content, 22, 0);
    lv_obj_set_layout(content, LV_LAYOUT_GRID);
    static int32_t cols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t rows[] = {LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_style_grid_column_dsc_array(content, cols, 0);
    lv_obj_set_style_grid_row_dsc_array(content, rows, 0);
    lv_obj_set_style_pad_gap(content, 14, 0);

    sensor_card_t *c1 = sensor_card_create(content, &model_temp());
    lv_obj_set_grid_cell(c1->obj, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_START, 0, 1);
    // ... c2, c3

    footer_create(scr, "POLL 5s · NEXT 3s", target_string());
    return scr;
}
```

## Layout

LVGL 9 has `LV_LAYOUT_FLEX` and `LV_LAYOUT_GRID`. Use them — don't do absolute positioning unless the design is genuinely absolute (status pill on tab bar, modal overlay).

| Design | LVGL approach |
|---|---|
| Tab bar (4 equal columns, 64 px tall) | Flex row, `flex_grow=1` on each column. Or `LV_LAYOUT_GRID` with 4× `LV_GRID_FR(1)`. |
| Sensors 3-up | Grid, 3× `LV_GRID_FR(1)`, `pad_gap=14`. |
| Status 2-column | Grid, 2× `LV_GRID_FR(1)`, `pad_column=36`. |
| Stack of alarm rows | Flex column, `pad_row=10`. |
| Sensor card value baseline-aligned | Flex row, `LV_FLEX_ALIGN_END` on cross axis. |

## Sparklines

Use `lv_line`. Pre-normalize the polyline once into static `lv_point_precise_t` arrays per data series; do not allocate per refresh.

```c
static lv_point_precise_t pts[11];
static const int xs[] = {0,20,40,60,80,100,120,140,160,180,200};

lv_obj_t *sparkline_create(lv_obj_t *parent, int w, int h, lv_color_t col, bool dashed) {
    lv_obj_t *line = lv_line_create(parent);
    lv_obj_set_size(line, w, h);
    lv_obj_set_style_line_color(line, col, 0);
    lv_obj_set_style_line_width(line, 2, 0); // 1.5 rounds to 2
    lv_obj_set_style_line_rounded(line, true, 0);
    if (dashed) {
        // LVGL 9 has no native dash on lv_line. Use lv_canvas and lv_draw_line_dsc_t,
        // or render dashes as multiple short lv_line segments. Simpler: lv_canvas.
    }
    return line;
}

void sparkline_set_data(lv_obj_t *line, const int16_t ys[11]) {
    for (int i = 0; i < 11; i++) {
        pts[i].x = xs[i]; pts[i].y = ys[i];
    }
    lv_line_set_points(line, pts, 11);
}
```

For the **filled area** beneath the line: easier to use `lv_canvas`. Allocate one canvas per sparkline at create time, redraw on data update with `lv_draw_polygon_dsc_t` (the area) + `lv_draw_line_dsc_t` (the stroke). One canvas per sensor card; 200×34 px @ ARGB8888 = 27 KB each — for 3 cards that's 81 KB. On ESP32-S3 with PSRAM fine; without, use indexed canvas or downscale.

## Status pill

```c
lv_obj_t *status_pill_create(lv_obj_t *parent, status_t s) {
    static const struct { lv_color_t *c; const char *t; } map[] = {
        [STATUS_RUNNING] = { &COL_OK,    LV_SYMBOL_OK " RUNNING" },
        [STATUS_INIT]    = { &COL_ACCENT, LV_SYMBOL_REFRESH " INIT" },
        [STATUS_ALARM]   = { &COL_ERR,    "\xE2\x96\xA0 ALARM" }, // "■"
        [STATUS_UPDATE]  = { &COL_ACCENT, "\xE2\x87\xAA UPDATE" }, // "⇪"
    };
    lv_obj_t *pill = lv_label_create(parent);
    lv_label_set_text(pill, map[s].t);
    lv_obj_add_style(pill, &st_tx_pill, 0);
    lv_obj_set_style_text_color(pill, *map[s].c, 0);
    lv_obj_set_style_border_color(pill, *map[s].c, 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_bg_color(pill, color_tint(*map[s].c, 0x12), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(pill, 3, 0);
    lv_obj_set_style_pad_hor(pill, 10, 0);
    if (s == STATUS_ALARM) pill_blink_anim(pill);
    return pill;
}
```

`color_tint()` should pre-compute the blended tint at startup or use a lookup table — alpha blending RGB565 at runtime tends to band.

## Blink animation

```c
static void blink_cb(void *obj, int32_t v) { lv_obj_set_style_opa(obj, v, 0); }

void pill_blink_anim(lv_obj_t *obj) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, blink_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 500);
    lv_anim_set_playback_time(&a, 500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_step);
    lv_anim_start(&a);
}
```

The `step` path gives the boxy on/off you see in the design (CSS `steps(2)`). Sine/ease will look wrong.

## Caret blink (config focus)

Same animation pattern on a 1×16 `lv_obj` styled as `bg_color = COL_ACCENT`.

## Touch event handling

Tab bar buttons emit `LV_EVENT_SHORT_CLICKED`. Don't use `LV_EVENT_CLICKED` — it fires on every press regardless of slide. Short-click ensures the user committed.

```c
lv_obj_add_event_cb(tab_btn, on_tab_clicked, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);
```

Hit-targets: every tab is 200×64, comfortably ≥44 px. Buttons in the config footer are 80×33 — at the lower bound of comfortable; keep them.

## Sensor card recipe

```c
typedef struct {
    lv_obj_t *obj;            // root
    lv_obj_t *stripe;          // 4px left bar
    lv_obj_t *label_lbl;
    lv_obj_t *value_lbl;
    lv_obj_t *unit_lbl;
    lv_obj_t *waiting_lbl;     // hidden in normal; shown in waiting
    lv_obj_t *sparkline;       // lv_canvas
    lv_obj_t *status_lbl;
    lv_obj_t *delta_lbl;
} sensor_card_t;

sensor_card_t *sensor_card_create(lv_obj_t *parent, const sensor_t *s);
void sensor_card_bind(sensor_card_t *c, const sensor_t *s); // re-render on data change
```

`sensor_card_bind` switches between value-shown and waiting-state by toggling `LV_OBJ_FLAG_HIDDEN` on the value vs. waiting labels — don't destroy and recreate.

## Modal pattern

LVGL has `lv_msgbox` but its style is fixed and not what we want. Build manually:

```c
lv_obj_t *modal_open(lv_obj_t *screen) {
    lv_obj_t *backdrop = lv_obj_create(screen);
    lv_obj_remove_style_all(backdrop);
    lv_obj_set_size(backdrop, 800, 480);
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_60, 0);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE); // swallow clicks

    lv_obj_t *card = lv_obj_create(backdrop);
    lv_obj_add_style(card, &st_card, 0);
    lv_obj_set_size(card, 380, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_pad_all(card, 24, 0);
    return card;
}
```

Don't dismiss on backdrop tap (per spec). Wire Cancel / Confirm buttons explicitly.

## Tabs without screen rebuild

When the user taps a tab, free the current screen and create the new one. This is simpler than caching, and screen creation is fast (a few hundred microseconds for these screens). Memory profile is also simpler.

The single exception: the Splash and Firmware screens are never reached via tabs — they're loaded by the app state machine (`07_NAVIGATION.md`).

## RGB565 caveats

- Pre-compute tint colors. Don't rely on `LV_OPA_*` to look smooth on text/borders.
- Disable LVGL's anti-aliasing only on tiny fonts (8–10 px) if banding shows. Test on actual panel.
- LVGL 9's draw layer defaults are fine; don't reach for `LV_USE_DRAW_SW_GRADIENT` unless a hatched/gradient progress bar is hurting frame time.

## Common gotchas

- Forgetting `lv_obj_remove_style_all()` before applying a custom style on `lv_obj_create()` — LVGL's default screen has a 10 px scroll padding that breaks edge-flush layouts. Call it first.
- Using `LV_GRID_TEMPLATE_LAST` is required as the last entry of column/row descriptors.
- Mixing `lv_obj_set_size` with `LV_SIZE_CONTENT` on flex/grid children — pick one.
- Calling `lv_label_set_text_fmt()` every frame allocates on the heap. Cache the last string and only update on change.
- LVGL's `lv_timer_create` runs on the LVGL thread — that's the thread you must hold the lock for from FreeRTOS code (`lv_lock()` / `lv_unlock()`).
