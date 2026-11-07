/**
 * @file header_bar.c
 * @brief Header bar widget (see header_bar.h).
 */

#include "lcd_ui/widgets/header_bar.h"

#include <string.h>

#include "lcd_ui/theme.h"

#define HEADER_TIME_W 64 /* "HH:MM:SS" in FONT_META is 8 glyphs of 7.2 px */

void header_bar_create(header_bar_t *hdr, lv_obj_t *parent)
{
    hdr->box = theme_box_create(parent, 0, THEME_HEADER_BAR_Y, THEME_CANVAS_W, THEME_HEADER_BAR_H);
    lv_obj_add_style(hdr->box, &theme_st_header, LV_PART_MAIN);
    /* bottom border only: a 1 px line under the bar */
    lv_obj_t *line = theme_box_create(hdr->box, 0, THEME_HEADER_BAR_H - THEME_BORDER_W,
                                      THEME_CANVAS_W, THEME_BORDER_W);
    lv_obj_set_style_bg_color(line, THEME_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);

    const lv_font_t *meta = THEME_FONT_META;
    const lv_font_t *eyebrow = THEME_FONT_EYEBROW;
    const lv_coord_t title_y = (lv_coord_t) ((THEME_HEADER_BAR_H - eyebrow->line_height) / 2);
    hdr->title = theme_label_create(hdr->box, &theme_st_txt_eyebrow, &theme_st_label_ink,
                                    THEME_HEADER_INSET, title_y, "");

    /* right group: [ time  pill ], pill right edge 22 px from the canvas edge */
    const lv_coord_t pill_right = (lv_coord_t) (THEME_CANVAS_W - THEME_HEADER_INSET);
    status_pill_create(&hdr->pill, hdr->box, pill_right, 0);
    lv_obj_set_y(hdr->pill.box, (lv_coord_t) ((THEME_HEADER_BAR_H - hdr->pill.h) / 2));

    const lv_coord_t time_x = (lv_coord_t) (pill_right - hdr->pill.w - THEME_SP_6 - HEADER_TIME_W);
    hdr->time = theme_label_create(hdr->box, &theme_st_txt_meta, &theme_st_label_muted, time_x,
                                   (lv_coord_t) ((THEME_HEADER_BAR_H - meta->line_height) / 2), "");
    lv_obj_set_width(hdr->time, HEADER_TIME_W);

    /* optional badge (hidden), 14 px left of the clock */
    hdr->badge_right = (lv_coord_t) (time_x - THEME_SP_6);
    hdr->badge_box = theme_box_create(hdr->box, 0, 0, 0, 0);
    lv_obj_set_style_bg_color(hdr->badge_box, THEME_COL_WARN_TINT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hdr->badge_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr->badge_box, THEME_BORDER_W, LV_PART_MAIN);
    lv_obj_set_style_border_color(hdr->badge_box, THEME_COL_WARN, LV_PART_MAIN);
    hdr->badge_label = theme_label_create(hdr->badge_box, &theme_st_txt_pill, &theme_st_label_warn,
                                          THEME_SP_4, THEME_SP_1 - 1, "");
    lv_obj_add_flag(hdr->badge_box, LV_OBJ_FLAG_HIDDEN);
}

void header_bar_set_title(header_bar_t *hdr, const char *title)
{
    lv_label_set_text(hdr->title, title);
}

void header_bar_set_time(header_bar_t *hdr, const char *time_text)
{
    lv_label_set_text(hdr->time, time_text);
}

void header_bar_set_pill(header_bar_t *hdr, pill_state_t state)
{
    if (hdr->pill.state != state)
    {
        status_pill_set_state(&hdr->pill, state);
        /* the chip width changed: keep the clock 14 px left of it */
        const lv_coord_t time_x =
            (lv_coord_t) (hdr->pill.right_edge - hdr->pill.w - THEME_SP_6 - HEADER_TIME_W);
        lv_obj_set_x(hdr->time, time_x);
        hdr->badge_right = (lv_coord_t) (time_x - THEME_SP_6);
        lv_obj_set_y(hdr->pill.box, (lv_coord_t) ((THEME_HEADER_BAR_H - hdr->pill.h) / 2));
    }
}

void header_bar_set_badge(header_bar_t *hdr, const char *text)
{
    if (text == NULL)
    {
        lv_obj_add_flag(hdr->badge_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const lv_font_t *pill = THEME_FONT_PILL;
    const lv_coord_t tw =
        lv_txt_get_width(text, (uint32_t) strlen(text), pill, THEME_TRACK_PILL, LV_TEXT_FLAG_NONE);
    const lv_coord_t w = (lv_coord_t) (tw + (2 * THEME_SP_4) + (2 * THEME_BORDER_W));
    const lv_coord_t h = (lv_coord_t) (pill->line_height + (2 * THEME_SP_1) + (2 * THEME_BORDER_W));

    lv_label_set_text(hdr->badge_label, text);
    lv_obj_set_size(hdr->badge_box, w, h);
    lv_obj_set_pos(hdr->badge_box, (lv_coord_t) (hdr->badge_right - w),
                   (lv_coord_t) ((THEME_HEADER_BAR_H - h) / 2));
    lv_obj_clear_flag(hdr->badge_box, LV_OBJ_FLAG_HIDDEN);
}
