/**
 * @file button.c
 * @brief Text button widget (see button.h).
 */

#include "lcd_ui/widgets/button.h"

#include <string.h>

#include "lcd_ui/theme.h"

static bool is_key(button_kind_t k)
{
    return (k == BUTTON_KEY) || (k == BUTTON_KEY_ERR);
}

static const lv_font_t *font_of(button_kind_t k)
{
    return is_key(k) ? THEME_FONT_BODY_MONO : THEME_FONT_PILL;
}

static void center_label(button_t *btn, const char *text)
{
    const lv_font_t *font = font_of(btn->kind);
    const lv_coord_t track = is_key(btn->kind) ? 0 : THEME_TRACK_PILL;
    const lv_coord_t tw =
        lv_txt_get_width(text, (uint32_t) strlen(text), font, track, LV_TEXT_FLAG_NONE);
    lv_obj_set_pos(btn->label, (lv_coord_t) ((btn->w - tw) / 2),
                   (lv_coord_t) ((btn->h - font->line_height) / 2));
}

void button_set_kind(button_t *btn, button_kind_t kind)
{
    lv_color_t fill = THEME_COL_BG;
    lv_color_t border = THEME_COL_BORDER;
    lv_color_t text = THEME_COL_INK;
    lv_opa_t fill_opa = LV_OPA_TRANSP;
    lv_coord_t border_w = THEME_BORDER_W;
    bool clickable = true;

    btn->kind = kind;
    switch (kind)
    {
    case BUTTON_PRIMARY:
        fill = THEME_COL_ACCENT;
        border = THEME_COL_ACCENT;
        text = THEME_COL_ON_ACCENT;
        fill_opa = LV_OPA_COVER;
        break;
    case BUTTON_TINT:
        fill = THEME_COL_ACCENT_TINT;
        border = THEME_COL_ACCENT;
        text = THEME_COL_ACCENT;
        fill_opa = LV_OPA_COVER;
        break;
    case BUTTON_DISABLED:
        fill = THEME_COL_SURF2;
        text = THEME_COL_DIM;
        fill_opa = LV_OPA_COVER;
        clickable = false;
        break;
    case BUTTON_KEY:
        fill = THEME_COL_SURF;
        fill_opa = LV_OPA_COVER;
        break;
    case BUTTON_KEY_ERR:
        fill = THEME_COL_ERR_TINT;
        border = THEME_COL_ERR;
        text = THEME_COL_ERR;
        fill_opa = LV_OPA_COVER;
        break;
    case BUTTON_BAR:
        text = THEME_COL_MUTED;
        border_w = 0;
        break;
    case BUTTON_BAR_TINT:
        fill = THEME_COL_ACCENT_TINT;
        text = THEME_COL_ACCENT;
        fill_opa = LV_OPA_COVER;
        border_w = 0;
        break;
    case BUTTON_BAR_OFF:
        text = THEME_COL_DIM;
        border_w = 0;
        clickable = false;
        break;
    case BUTTON_GHOST:
    default:
        text = THEME_COL_MUTED;
        break;
    }

    lv_obj_set_style_bg_color(btn->box, fill, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn->box, fill_opa, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn->box, border, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn->box, border_w, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn->label, text, LV_PART_MAIN);
    if (clickable)
    {
        lv_obj_add_flag(btn->box, LV_OBJ_FLAG_CLICKABLE);
    }
    else
    {
        lv_obj_clear_flag(btn->box, LV_OBJ_FLAG_CLICKABLE);
    }
}

void button_create(button_t *btn, lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                   lv_coord_t h, const char *text, button_kind_t kind)
{
    btn->w = w;
    btn->h = h;
    btn->kind = kind;
    btn->box = theme_box_create(parent, x, y, w, h);
    lv_obj_add_flag(btn->box, LV_OBJ_FLAG_CLICKABLE);
    btn->label =
        theme_label_create(btn->box, is_key(kind) ? &theme_st_txt_body_mono : &theme_st_txt_pill,
                           &theme_st_label_ink, 0, 0, text);
    button_set_kind(btn, kind);
    center_label(btn, text);
}

void button_set_text(button_t *btn, const char *text)
{
    lv_label_set_text(btn->label, text);
    center_label(btn, text);
}

void button_on_click(button_t *btn, lv_event_cb_t cb, void *user_data)
{
    lv_obj_add_event_cb(btn->box, cb, LV_EVENT_CLICKED, user_data);
}
