/**
 * @file footer_bar.c
 * @brief Footer bar widget — implementation.
 *
 * @see firmware/field-device/application/lcd_ui/widgets/footer_bar.h
 */

#include "lcd_ui/widgets/footer_bar.h"
#include "lcd_ui/theme.h"

/* ===================================================================== */
/* File-scope state                                                     */
/* ===================================================================== */

static lv_obj_t *s_left_lbl;
static lv_obj_t *s_right_lbl;

/* ===================================================================== */
/* footer_bar_create                                                    */
/* ===================================================================== */

lv_obj_t *footer_bar_create(lv_obj_t *parent, const char *left, const char *right)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, THEME_FOOTER_BAR_Y);
    lv_obj_set_size(bar, THEME_CANVAS_W, THEME_FOOTER_BAR_H);
    lv_obj_add_style(bar, &theme_st_footer, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_left_lbl = lv_label_create(bar);
    lv_obj_remove_style_all(s_left_lbl);
    lv_obj_add_style(s_left_lbl, &theme_st_label_dim, LV_PART_MAIN);
    lv_obj_align(s_left_lbl, LV_ALIGN_LEFT_MID, THEME_SP_7, 0);
    lv_label_set_text(s_left_lbl, (left != NULL) ? left : "");

    s_right_lbl = lv_label_create(bar);
    lv_obj_remove_style_all(s_right_lbl);
    lv_obj_add_style(s_right_lbl, &theme_st_label_dim, LV_PART_MAIN);
    lv_obj_align(s_right_lbl, LV_ALIGN_RIGHT_MID, -THEME_SP_7, 0);
    lv_label_set_text(s_right_lbl, (right != NULL) ? right : "");

    return bar;
}

/* ===================================================================== */
/* footer_bar_set_text                                                  */
/* ===================================================================== */

void footer_bar_set_text(lv_obj_t *footer_obj, const char *left, const char *right)
{
    (void) footer_obj;
    if (left != NULL)
    {
        lv_label_set_text(s_left_lbl, left);
    }
    if (right != NULL)
    {
        lv_label_set_text(s_right_lbl, right);
    }
}
