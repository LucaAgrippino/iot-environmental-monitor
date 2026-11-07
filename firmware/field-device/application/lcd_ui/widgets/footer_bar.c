/**
 * @file footer_bar.c
 * @brief Footer bar widget (see footer_bar.h).
 */

#include "lcd_ui/widgets/footer_bar.h"

#include <string.h>

#include "lcd_ui/theme.h"

void footer_bar_create(footer_bar_t *ftr, lv_obj_t *parent)
{
    ftr->box = theme_box_create(parent, 0, THEME_FOOTER_BAR_Y, THEME_CANVAS_W, THEME_FOOTER_BAR_H);
    lv_obj_add_style(ftr->box, &theme_st_footer, LV_PART_MAIN);
    lv_obj_t *line = theme_box_create(ftr->box, 0, 0, THEME_CANVAS_W, THEME_BORDER_W);
    lv_obj_set_style_bg_color(line, THEME_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);

    const lv_font_t *font = THEME_FONT_CAPTION_MONO;
    const lv_coord_t y = (lv_coord_t) ((THEME_FOOTER_BAR_H - font->line_height) / 2);
    ftr->left = theme_label_create(ftr->box, &theme_st_txt_caption_mono, &theme_st_label_muted,
                                   THEME_HEADER_INSET, y, "");
    ftr->right = theme_label_create(ftr->box, &theme_st_txt_caption_mono, &theme_st_label_muted,
                                    THEME_HEADER_INSET, y, "");
}

void footer_bar_set(footer_bar_t *ftr, const char *left, const char *right)
{
    const lv_font_t *font = THEME_FONT_CAPTION_MONO;
    lv_label_set_text(ftr->left, left);
    lv_label_set_text(ftr->right, right);
    const lv_coord_t w =
        lv_txt_get_width(right, (uint32_t) strlen(right), font, 0, LV_TEXT_FLAG_NONE);
    lv_obj_set_x(ftr->right, (lv_coord_t) (THEME_CANVAS_W - THEME_HEADER_INSET - w));
}
