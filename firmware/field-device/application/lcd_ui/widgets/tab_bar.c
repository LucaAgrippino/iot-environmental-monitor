/**
 * @file tab_bar.c
 * @brief Custom four-column tab bar — implementation.
 *
 * Uses file-scope static arrays (s_tab_btns[], s_tab_accents[]) to avoid
 * lv_obj_get_child() which the LVGL stub does not implement.
 *
 * @see firmware/field-device/application/lcd_ui/widgets/tab_bar.h
 */

#include "lcd_ui/widgets/tab_bar.h"
#include "lcd_ui/theme.h"

/* ===================================================================== */
/* File-scope state                                                     */
/* ===================================================================== */

static lv_obj_t *s_tab_btns[THEME_TAB_COL_COUNT];
static lv_obj_t *s_tab_accents[THEME_TAB_COL_COUNT];
static void (*s_on_change)(uint16_t idx);

/* ===================================================================== */
/* Internal helpers                                                     */
/* ===================================================================== */

static void tab_event_cb(lv_event_t *e)
{
    const lv_obj_t *btn = lv_event_get_target(e);

    for (uint16_t i = 0U; i < (uint16_t) THEME_TAB_COL_COUNT; i++)
    {
        if (s_tab_btns[i] == btn)
        {
            if (s_on_change != NULL)
            {
                s_on_change(i);
            }
            return;
        }
    }
}

/* Tab labels in declaration order — 03_SCREEN_SPECS.md §Tab bar */
static const char *const k_tab_labels[THEME_TAB_COL_COUNT] = {
    "SENSORS", /* TODO(icons): prepend icon glyph when icon font is available */
    "STATUS",
    "ALARMS",
    "CONFIG",
};

/* ===================================================================== */
/* tab_bar_create                                                        */
/* ===================================================================== */

lv_obj_t *tab_bar_create(lv_obj_t *parent, uint16_t active_idx, void (*on_change)(uint16_t idx))
{
    s_on_change = on_change;

    /* Container — full-width, THEME_TAB_BAR_H tall, anchored to top */
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, THEME_CANVAS_W, THEME_TAB_BAR_H);
    lv_obj_add_style(bar, &theme_st_tab_bar, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    for (uint16_t i = 0U; i < (uint16_t) THEME_TAB_COL_COUNT; i++)
    {
        lv_coord_t x = (lv_coord_t) ((lv_coord_t) i * THEME_TAB_COL_W);

        /* Button — covers the full column */
        lv_obj_t *btn = lv_btn_create(bar);
        lv_obj_remove_style_all(btn);
        lv_obj_set_pos(btn, x, 0);
        lv_obj_set_size(btn, THEME_TAB_COL_W, THEME_TAB_BAR_H);
        lv_obj_add_style(btn, &theme_st_tab_btn, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, tab_event_cb, LV_EVENT_SHORT_CLICKED, NULL);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, k_tab_labels[i]);
        lv_obj_center(lbl);

        /* Accent stripe — 3 px, shown only when active */
        lv_obj_t *accent = lv_obj_create(btn);
        lv_obj_remove_style_all(accent);
        lv_obj_set_pos(accent, 0, 0);
        lv_obj_set_size(accent, THEME_TAB_COL_W, THEME_TAB_ACCENT_H);
        lv_obj_set_style_bg_color(accent, THEME_COL_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(accent, THEME_RAD_NONE, LV_PART_MAIN);

        s_tab_btns[i] = btn;
        s_tab_accents[i] = accent;
    }

    tab_bar_set_active(bar, active_idx);
    return bar;
}

/* ===================================================================== */
/* tab_bar_set_active                                                   */
/* ===================================================================== */

void tab_bar_set_active(lv_obj_t *tab_bar_obj, uint16_t idx)
{
    (void) tab_bar_obj; /* accent array is indexed directly */

    for (uint16_t i = 0U; i < (uint16_t) THEME_TAB_COL_COUNT; i++)
    {
        bool is_active = (i == idx);

        if (is_active)
        {
            lv_obj_set_style_bg_color(s_tab_btns[i], THEME_COL_ACCENT_TINT, LV_PART_MAIN);
            lv_obj_set_style_text_color(s_tab_btns[i], THEME_COL_ACCENT, LV_PART_MAIN);
            lv_obj_clear_flag(s_tab_accents[i], LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_set_style_bg_color(s_tab_btns[i], THEME_COL_HEADER_BG, LV_PART_MAIN);
            lv_obj_set_style_text_color(s_tab_btns[i], THEME_COL_TAB_INACTIVE, LV_PART_MAIN);
            lv_obj_add_flag(s_tab_accents[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}
