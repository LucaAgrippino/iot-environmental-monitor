/**
 * @file status_row.c
 * @brief Status row widget (see status_row.h).
 */

#include "lcd_ui/widgets/status_row.h"

#include <string.h>

#include "lcd_ui/theme.h"

#define ROW_DOT_GAP THEME_SP_4 /* 10 px dot to label */
#define ROW_GLOW_D 14          /* outer glow circle */
#define ROW_GLOW_OPA 102       /* 40 % of 255 */
#define ROW_RADIUS_CIRCLE 7    /* half of the largest circle */

static lv_color_t colour_of(row_status_t s)
{
    switch (s)
    {
    case ROW_WARN:
        return THEME_COL_WARN;
    case ROW_ERR:
        return THEME_COL_ERR;
    case ROW_OK:
    default:
        return THEME_COL_OK;
    }
}

static lv_obj_t *circle(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t d)
{
    lv_obj_t *c = theme_box_create(parent, x, y, d, d);
    lv_obj_set_style_radius(c, ROW_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
    return c;
}

void status_row_create(status_row_t *row, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, const char *label)
{
    const lv_font_t *font = THEME_FONT_LABEL;
    const lv_coord_t mid = (lv_coord_t) ((STATUS_ROW_HEIGHT - THEME_BORDER_W) / 2);

    row->width = w;
    row->box = theme_box_create(parent, x, y, w, STATUS_ROW_HEIGHT);

    lv_obj_t *line =
        theme_box_create(row->box, 0, STATUS_ROW_HEIGHT - THEME_BORDER_W, w, THEME_BORDER_W);
    lv_obj_set_style_bg_color(line, THEME_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);

    row->glow =
        circle(row->box, (lv_coord_t) (-2), (lv_coord_t) (mid - (ROW_GLOW_D / 2)), ROW_GLOW_D);
    lv_obj_set_style_bg_opa(row->glow, ROW_GLOW_OPA, LV_PART_MAIN);
    lv_obj_add_flag(row->glow, LV_OBJ_FLAG_HIDDEN);

    row->dot = circle(row->box, 0, (lv_coord_t) (mid - (THEME_DOT_D / 2)), THEME_DOT_D);
    lv_obj_set_style_radius(row->dot, THEME_DOT_D / 2, LV_PART_MAIN);

    row->label = theme_label_create(row->box, &theme_st_txt_label, &theme_st_label_ink,
                                    (lv_coord_t) (THEME_DOT_D + ROW_DOT_GAP),
                                    (lv_coord_t) (mid - (font->line_height / 2)), label);

    const lv_font_t *vfont = THEME_FONT_VALUE_SM;
    row->value = theme_label_create(row->box, &theme_st_txt_value_sm, &theme_st_label_ink, 0,
                                    (lv_coord_t) (mid - (vfont->line_height / 2)), "");
    status_row_set(row, ROW_OK, "");
}

void status_row_set(status_row_t *row, row_status_t status, const char *value)
{
    const lv_font_t *vfont = THEME_FONT_VALUE_SM;
    const lv_coord_t vw =
        lv_txt_get_width(value, (uint32_t) strlen(value), vfont, 0, LV_TEXT_FLAG_NONE);

    lv_obj_set_style_bg_color(row->dot, colour_of(status), LV_PART_MAIN);
    lv_obj_set_style_bg_color(row->glow, colour_of(status), LV_PART_MAIN);
    if (status == ROW_ERR)
    {
        lv_obj_clear_flag(row->glow, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(row->glow, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(row->value, value);
    lv_obj_set_style_text_color(row->value, (status == ROW_OK) ? THEME_COL_INK : colour_of(status),
                                LV_PART_MAIN);
    lv_obj_set_x(row->value, (lv_coord_t) (row->width - vw));
}
