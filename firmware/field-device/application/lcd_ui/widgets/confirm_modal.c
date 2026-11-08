/**
 * @file confirm_modal.c
 * @brief Confirm modal widget (see confirm_modal.h).
 */

#include "lcd_ui/widgets/confirm_modal.h"

#include "lcd_ui/theme.h"

#define MODAL_W 380            /**< .modal width */
#define MODAL_PAD THEME_SP_10  /**< padding 24 px */
#define MODAL_BACKDROP_OPA 153 /**< 60 % of 255 */
#define MODAL_TITLE_H 24
#define MODAL_TITLE_GAP THEME_SP_5 /**< margin-bottom 12 px */
#define MODAL_BODY_H 36            /**< two 13 px lines at line-height 1.5 */
#define MODAL_LINE_H 17            /**< one change line, 12 px mono */
#define MODAL_BLOCK_GAP 20         /**< margin-bottom of the body */
#define MODAL_BTN_H 33             /**< padding 10 px, 11 px text */
#define MODAL_BTN_GAP THEME_SP_4   /**< grid gap 10 px */

void confirm_modal_create(confirm_modal_t *m, lv_obj_t *parent, lv_event_cb_t on_cancel,
                          lv_event_cb_t on_confirm)
{
    const lv_coord_t inner_w = (lv_coord_t) (MODAL_W - (2 * MODAL_PAD));
    const lv_coord_t btn_w = (lv_coord_t) ((inner_w - MODAL_BTN_GAP) / 2);
    const lv_coord_t panel_h =
        (lv_coord_t) ((2 * MODAL_PAD) + MODAL_TITLE_H + MODAL_TITLE_GAP + MODAL_BODY_H +
                      (CONFIRM_MODAL_MAX_LINES * MODAL_LINE_H) + MODAL_BLOCK_GAP + MODAL_BTN_H);

    m->open = false;
    m->backdrop = theme_box_create(parent, 0, 0, THEME_CANVAS_W, THEME_CANVAS_H);
    lv_obj_set_style_bg_color(m->backdrop, lv_color_hex(0x000000U), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(m->backdrop, MODAL_BACKDROP_OPA, LV_PART_MAIN);
    lv_obj_add_flag(m->backdrop, LV_OBJ_FLAG_CLICKABLE); /* swallows touches: no dismiss outside */

    m->panel = theme_box_create(m->backdrop, (lv_coord_t) ((THEME_CANVAS_W - MODAL_W) / 2),
                                (lv_coord_t) ((THEME_CANVAS_H - panel_h) / 2), MODAL_W, panel_h);
    lv_obj_add_style(m->panel, &theme_st_card, LV_PART_MAIN);

    (void) theme_label_create(m->panel, &theme_st_txt_head, &theme_st_label_ink, MODAL_PAD,
                              MODAL_PAD, "Apply Configuration?");
    m->body =
        theme_label_create(m->panel, &theme_st_txt_label, &theme_st_label_muted, MODAL_PAD,
                           (lv_coord_t) (MODAL_PAD + MODAL_TITLE_H + MODAL_TITLE_GAP),
                           "The following changes will be written to non-volatile storage and "
                           "take effect immediately:");
    lv_obj_set_width(m->body, inner_w);
    m->lines = theme_label_create(
        m->panel, &theme_st_txt_meta, &theme_st_label_ink, MODAL_PAD,
        (lv_coord_t) (MODAL_PAD + MODAL_TITLE_H + MODAL_TITLE_GAP + MODAL_BODY_H), "");
    lv_obj_set_width(m->lines, inner_w);

    const lv_coord_t btn_y = (lv_coord_t) (panel_h - MODAL_PAD - MODAL_BTN_H);
    button_create(&m->cancel, m->panel, MODAL_PAD, btn_y, btn_w, MODAL_BTN_H, "CANCEL",
                  BUTTON_GHOST);
    button_create(&m->confirm, m->panel, (lv_coord_t) (MODAL_PAD + btn_w + MODAL_BTN_GAP), btn_y,
                  btn_w, MODAL_BTN_H, "CONFIRM", BUTTON_TINT);
    button_on_click(&m->cancel, on_cancel, NULL);
    button_on_click(&m->confirm, on_confirm, NULL);

    lv_obj_add_flag(m->backdrop, LV_OBJ_FLAG_HIDDEN);
}

void confirm_modal_show(confirm_modal_t *m, const char *text, uint32_t count)
{
    if (count < 1U)
    {
        count = 1U;
    }
    if (count > CONFIRM_MODAL_MAX_LINES)
    {
        count = CONFIRM_MODAL_MAX_LINES;
    }
    /* height follows the number of change lines; the panel stays centred, buttons at its foot */
    const lv_coord_t h =
        (lv_coord_t) ((2 * MODAL_PAD) + MODAL_TITLE_H + MODAL_TITLE_GAP + MODAL_BODY_H +
                      (count * MODAL_LINE_H) + MODAL_BLOCK_GAP + MODAL_BTN_H);
    const lv_coord_t btn_y = (lv_coord_t) (h - MODAL_PAD - MODAL_BTN_H);
    lv_obj_set_height(m->panel, h);
    lv_obj_set_y(m->panel, (lv_coord_t) ((THEME_CANVAS_H - h) / 2));
    lv_obj_set_y(m->cancel.box, btn_y);
    lv_obj_set_y(m->confirm.box, btn_y);
    lv_label_set_text(m->lines, text);
    m->open = true;
    lv_obj_clear_flag(m->backdrop, LV_OBJ_FLAG_HIDDEN);
}

void confirm_modal_hide(confirm_modal_t *m)
{
    m->open = false;
    lv_obj_add_flag(m->backdrop, LV_OBJ_FLAG_HIDDEN);
}

bool confirm_modal_is_open(const confirm_modal_t *m)
{
    return m->open;
}
