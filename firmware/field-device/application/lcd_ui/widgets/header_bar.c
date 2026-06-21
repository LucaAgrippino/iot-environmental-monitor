/**
 * @file header_bar.c
 * @brief Header bar widget — implementation.
 *
 * @see firmware/field-device/application/lcd_ui/widgets/header_bar.h
 */

#include "lcd_ui/widgets/header_bar.h"
#include "lcd_ui/theme.h"

/* Offset from the parent's origin where header bar child labels live.
 * Header occupies (0, THEME_HEADER_BAR_Y) relative to the root screen;
 * child labels are positioned relative to the header bar container. */

/* Internal child index conventions (not exposed) */
#define HDR_CHILD_TITLE_IDX  0
#define HDR_CHILD_TIME_IDX   1

/* ===================================================================== */
/* File-scope state                                                     */
/* ===================================================================== */

static lv_obj_t *s_title_lbl;
static lv_obj_t *s_time_lbl;

/* ===================================================================== */
/* header_bar_create                                                    */
/* ===================================================================== */

lv_obj_t *header_bar_create(lv_obj_t *parent, const char *title)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, THEME_HEADER_BAR_Y);
    lv_obj_set_size(bar, THEME_CANVAS_W, THEME_HEADER_BAR_H);
    lv_obj_add_style(bar, &theme_st_header, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Title — left-aligned with THEME_HEADER_INSET px from left edge */
    s_title_lbl = lv_label_create(bar);
    lv_obj_remove_style_all(s_title_lbl);
    lv_obj_set_pos(s_title_lbl, THEME_HEADER_INSET,
                   (THEME_HEADER_BAR_H - 14) / 2); /* vertically centred, 14 px font */
    lv_obj_add_style(s_title_lbl, &theme_st_label_muted, LV_PART_MAIN);
    lv_label_set_text(s_title_lbl, title);

    /* Time — right-aligned (static placeholder, TODO(clock)) */
    s_time_lbl = lv_label_create(bar);
    lv_obj_remove_style_all(s_time_lbl);
    lv_obj_add_style(s_time_lbl, &theme_st_label_dim, LV_PART_MAIN);
    lv_label_set_text(s_time_lbl, "00:00:00"); /* TODO(clock): read from RTC */
    lv_obj_align(s_time_lbl, LV_ALIGN_RIGHT_MID, -THEME_HEADER_INSET, 0);

    return bar;
}

/* ===================================================================== */
/* header_bar_set_title                                                 */
/* ===================================================================== */

void header_bar_set_title(lv_obj_t *header_obj, const char *title)
{
    (void)header_obj; /* title label accessed via s_title_lbl */
    if (title != NULL)
    {
        lv_label_set_text(s_title_lbl, title);
    }
}
