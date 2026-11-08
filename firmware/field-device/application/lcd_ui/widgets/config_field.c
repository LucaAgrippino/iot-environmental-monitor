/**
 * @file config_field.c
 * @brief Config field widget (see config_field.h).
 */

#include "lcd_ui/widgets/config_field.h"

#include <string.h>

#include "lcd_ui/theme.h"

#define FIELD_PAD_X THEME_SP_5 /**< input padding 9 px / 12 px */
#define FIELD_PAD_Y 9
#define FIELD_ERR_GAP THEME_SP_1 /**< 4 px above the error line */
#define BLINK_PERIOD_MS 1000U    /**< style-guide.md section 8: 1 Hz, 50 % duty */
#define BLINK_ON_MS 500U

static void apply_look(config_field_t *f)
{
    lv_color_t border = THEME_COL_BORDER;
    lv_color_t text = THEME_COL_MUTED; /* a saved value is muted */

    if (f->invalid)
    {
        border = THEME_COL_ERR;
        text = THEME_COL_ERR;
    }
    else if (f->focused)
    {
        border = THEME_COL_ACCENT;
        text = THEME_COL_INK;
    }
    else if (f->modified)
    {
        text = THEME_COL_INK;
    }
    lv_obj_set_style_border_color(f->input, border, LV_PART_MAIN);
    lv_obj_set_style_text_color(f->value, text, LV_PART_MAIN);
}

void config_field_create(config_field_t *f, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                         lv_coord_t w, const char *label, const char *unit)
{
    const lv_font_t *cap = THEME_FONT_CAPTION_MONO;

    f->w = w;
    f->focused = false;
    f->invalid = false;
    f->modified = false;
    f->box = theme_box_create(parent, x, y, w, CONFIG_FIELD_PITCH);

    f->label =
        theme_label_create(f->box, &theme_st_txt_eyebrow, &theme_st_label_muted, 0, 0, label);

    f->input = theme_box_create(f->box, 0, CONFIG_FIELD_LABEL_H, w, CONFIG_FIELD_INPUT_H);
    lv_obj_set_style_bg_color(f->input, THEME_COL_SURF2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(f->input, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(f->input, THEME_BORDER_W, LV_PART_MAIN);
    lv_obj_add_flag(f->input, LV_OBJ_FLAG_CLICKABLE);

    f->value = theme_label_create(f->input, &theme_st_txt_body_mono, &theme_st_label_muted,
                                  FIELD_PAD_X, FIELD_PAD_Y, "");
    const lv_coord_t uw =
        lv_txt_get_width(unit, (uint32_t) strlen(unit), cap, 0, LV_TEXT_FLAG_NONE);
    f->unit =
        theme_label_create(f->input, &theme_st_txt_caption_mono, &theme_st_label_dim,
                           (lv_coord_t) (w - FIELD_PAD_X - uw),
                           (lv_coord_t) ((CONFIG_FIELD_INPUT_H - cap->line_height) / 2), unit);
    f->caret = theme_label_create(f->input, &theme_st_txt_body_mono, &theme_st_label_ink,
                                  FIELD_PAD_X, FIELD_PAD_Y, "|");
    lv_obj_add_flag(f->caret, LV_OBJ_FLAG_HIDDEN);

    f->err = theme_label_create(
        f->box, &theme_st_txt_caption_mono, &theme_st_label_err, 0,
        (lv_coord_t) (CONFIG_FIELD_LABEL_H + CONFIG_FIELD_INPUT_H + FIELD_ERR_GAP), "");
    lv_obj_add_flag(f->err, LV_OBJ_FLAG_HIDDEN);
    apply_look(f);
}

/** The caret follows the end of the value text. */
static void place_caret(config_field_t *f, const char *text)
{
    const lv_font_t *font = THEME_FONT_BODY_MONO;
    const lv_coord_t tw =
        lv_txt_get_width(text, (uint32_t) strlen(text), font, 0, LV_TEXT_FLAG_NONE);
    lv_obj_set_x(f->caret, (lv_coord_t) (FIELD_PAD_X + tw + 1));
}

void config_field_set_value(config_field_t *f, const char *text)
{
    lv_label_set_text(f->value, text);
    place_caret(f, text);
}

void config_field_set_modified(config_field_t *f, bool modified)
{
    f->modified = modified;
    apply_look(f);
}

void config_field_set_focus(config_field_t *f, bool focused)
{
    f->focused = focused;
    if (focused && !f->invalid)
    {
        lv_obj_clear_flag(f->caret, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(f->caret, LV_OBJ_FLAG_HIDDEN);
    }
    apply_look(f);
}

void config_field_set_error(config_field_t *f, const char *message)
{
    f->invalid = (message != NULL);
    if (message != NULL)
    {
        lv_label_set_text(f->err, message);
        lv_obj_clear_flag(f->err, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(f->caret, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(f->err, LV_OBJ_FLAG_HIDDEN);
    }
    apply_look(f);
}

void config_field_blink_tick(config_field_t *f, uint32_t now_ms)
{
    if (!f->focused || f->invalid)
    {
        return;
    }
    if ((now_ms % BLINK_PERIOD_MS) < BLINK_ON_MS)
    {
        lv_obj_clear_flag(f->caret, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(f->caret, LV_OBJ_FLAG_HIDDEN);
    }
}

void config_field_on_click(config_field_t *f, lv_event_cb_t cb, void *user_data)
{
    lv_obj_add_event_cb(f->input, cb, LV_EVENT_CLICKED, user_data);
}
