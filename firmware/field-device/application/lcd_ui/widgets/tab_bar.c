/**
 * @file tab_bar.c
 * @brief Tab bar widget (see tab_bar.h).
 */

#include "lcd_ui/widgets/tab_bar.h"

#include <stddef.h>
#include <string.h>

typedef struct
{
    const char *icon;  /**< Icon-font glyph, 02_FONTS_AND_ICONS.md "Line icons". */
    const char *label; /**< UPPERCASE label baked into the text. */
} tab_def_t;

/* Tab order, style-guide.md section 5 table */
static const tab_def_t k_tabs[THEME_TAB_COL_COUNT] = {
    {.icon = ICON_SENSORS, .label = "SENSORS"}, {.icon = ICON_TREND, .label = "TREND"},
    {.icon = ICON_STATUS, .label = "STATUS"},   {.icon = ICON_ALARMS, .label = "ALARMS"},
    {.icon = ICON_CONFIG, .label = "CONFIG"},
};

/* Only one tab bar exists, so the event handler reaches it through this pointer. */
static tab_bar_t *s_bar;

static void tab_event_cb(lv_event_t *e)
{
    const uint16_t idx = (uint16_t) (uintptr_t) lv_event_get_user_data(e);
    if ((s_bar != NULL) && (s_bar->on_change != NULL))
    {
        s_bar->on_change(idx);
    }
}

static void apply_look(tab_bar_t *bar, uint16_t i)
{
    const bool active = (i == bar->active);
    lv_obj_set_style_bg_color(bar->column[i], active ? THEME_COL_ACCENT_TINT : THEME_COL_HEADER_BG,
                              LV_PART_MAIN);
    if (active)
    {
        lv_obj_clear_flag(bar->stripe[i], LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(bar->stripe[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_color(bar->icon[i], active ? THEME_COL_ACCENT : THEME_COL_TAB_INACTIVE,
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(bar->label[i], active ? THEME_COL_INK : THEME_COL_TAB_INACTIVE,
                                LV_PART_MAIN);
}

void tab_bar_create(tab_bar_t *bar, lv_obj_t *parent, uint16_t active,
                    tab_bar_change_cb_t on_change)
{
    const lv_font_t *icon_font = THEME_ICON_TAB;
    const lv_font_t *label_font = THEME_FONT_TAB;

    s_bar = bar;
    bar->active = (active < THEME_TAB_COL_COUNT) ? active : 0U;
    bar->on_change = on_change;

    bar->box = theme_box_create(parent, 0, 0, THEME_CANVAS_W, THEME_TAB_BAR_H);
    lv_obj_add_style(bar->box, &theme_st_tab_bar, LV_PART_MAIN);

    /* icon above label, the pair centred vertically (03_SCREEN_SPECS.md "Tab bar") */
    const lv_coord_t stack_h =
        (lv_coord_t) (icon_font->line_height + THEME_TAB_ICON_GAP + label_font->line_height);
    const lv_coord_t top = (lv_coord_t) ((THEME_TAB_BAR_H - stack_h) / 2);

    for (uint16_t i = 0U; i < THEME_TAB_COL_COUNT; i++)
    {
        const lv_coord_t x = (lv_coord_t) (i * THEME_TAB_COL_W);

        bar->column[i] = theme_box_create(bar->box, x, 0, THEME_TAB_COL_W, THEME_TAB_BAR_H);
        lv_obj_set_style_bg_opa(bar->column[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_add_flag(bar->column[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(bar->column[i], tab_event_cb, LV_EVENT_SHORT_CLICKED,
                            (void *) (uintptr_t) i);

        /* 1 px divider on the right edge of every column but the last */
        if (i != (THEME_TAB_COL_COUNT - 1U))
        {
            lv_obj_t *divider = theme_box_create(bar->column[i], THEME_TAB_COL_W - THEME_BORDER_W,
                                                 0, THEME_BORDER_W, THEME_TAB_BAR_H);
            lv_obj_set_style_bg_color(divider, THEME_COL_BORDER, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
        }

        bar->stripe[i] =
            theme_box_create(bar->column[i], 0, 0, THEME_TAB_COL_W, THEME_TAB_ACCENT_H);
        lv_obj_set_style_bg_color(bar->stripe[i], THEME_COL_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar->stripe[i], LV_OPA_COVER, LV_PART_MAIN);

        /* icon and label are centred by measuring: LVGL labels are left-aligned at x */
        const lv_coord_t icon_w = lv_txt_get_width(
            k_tabs[i].icon, (uint32_t) strlen(k_tabs[i].icon), icon_font, 0, LV_TEXT_FLAG_NONE);
        bar->icon[i] =
            theme_label_create(bar->column[i], &theme_st_plain, &theme_st_label_muted,
                               (lv_coord_t) ((THEME_TAB_COL_W - icon_w) / 2), top, k_tabs[i].icon);
        lv_obj_set_style_text_font(bar->icon[i], icon_font, LV_PART_MAIN);

        const lv_coord_t text_w =
            lv_txt_get_width(k_tabs[i].label, (uint32_t) strlen(k_tabs[i].label), label_font,
                             THEME_TRACK_TAB, LV_TEXT_FLAG_NONE);
        bar->label[i] = theme_label_create(
            bar->column[i], &theme_st_txt_tab, &theme_st_label_muted,
            (lv_coord_t) ((THEME_TAB_COL_W - text_w) / 2),
            (lv_coord_t) (top + icon_font->line_height + THEME_TAB_ICON_GAP), k_tabs[i].label);

        apply_look(bar, i);
    }

    /* 1 px line under the whole bar */
    lv_obj_t *base = theme_box_create(bar->box, 0, THEME_TAB_BAR_H - THEME_BORDER_W, THEME_CANVAS_W,
                                      THEME_BORDER_W);
    lv_obj_set_style_bg_color(base, THEME_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, LV_PART_MAIN);
}

void tab_bar_set_active(tab_bar_t *bar, uint16_t idx)
{
    if (idx >= THEME_TAB_COL_COUNT)
    {
        return;
    }
    const uint16_t previous = bar->active;
    bar->active = idx;
    apply_look(bar, previous);
    apply_look(bar, idx);
}
