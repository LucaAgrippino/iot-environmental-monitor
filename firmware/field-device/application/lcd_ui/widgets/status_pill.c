/**
 * @file status_pill.c
 * @brief Status pill widget (see status_pill.h).
 */

#include "lcd_ui/widgets/status_pill.h"

#include <string.h>

#include "lcd_ui/theme.h"

#define PILL_PAD_X 10 /* 03_SCREEN_SPECS.md: padding 3 px / 10 px */
#define PILL_PAD_Y 3
#define PILL_BLINK_PERIOD_MS 1000U /* style-guide.md section 8: 1 Hz */
#define PILL_BLINK_ON_MS 500U      /* 50 % duty */

/* Text, border/text colour, tint fill per state — 03_SCREEN_SPECS.md "Status pill" table */
static lv_color_t colour_of(pill_state_t s)
{
    switch (s)
    {
    case PILL_ALARM:
        return THEME_COL_ERR;
    case PILL_INIT:
    case PILL_UPDATE:
        return THEME_COL_ACCENT;
    case PILL_RUNNING:
    default:
        return THEME_COL_OK;
    }
}

static lv_color_t tint_of(pill_state_t s)
{
    switch (s)
    {
    case PILL_ALARM:
        return THEME_COL_ERR_TINT;
    case PILL_INIT:
    case PILL_UPDATE:
        return THEME_COL_ACCENT_TINT;
    case PILL_RUNNING:
    default:
        return THEME_COL_OK_TINT;
    }
}

static const char *text_of(pill_state_t s)
{
    switch (s)
    {
    case PILL_ALARM:
        return "\xE2\x96\xA0 ALARM"; /* ■ U+25A0 */
    case PILL_INIT:
        return "\xE2\x97\x8F INIT"; /* ● U+25CF */
    case PILL_UPDATE:
        return "\xE2\x87\xAA UPDATE"; /* ⇪ U+21EA */
    case PILL_RUNNING:
    default:
        return "\xE2\x97\x8F RUNNING";
    }
}

void status_pill_create(status_pill_t *pill, lv_obj_t *parent, lv_coord_t x_right, lv_coord_t y)
{
    pill->right_edge = x_right;
    pill->state = PILL_RUNNING;
    pill->visible_phase = true;

    pill->box = theme_box_create(parent, x_right, y, 0, 0);
    lv_obj_set_style_border_width(pill->box, THEME_BORDER_W, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill->box, LV_OPA_COVER, LV_PART_MAIN);

    pill->label = theme_label_create(pill->box, &theme_st_txt_pill, &theme_st_label_ok, PILL_PAD_X,
                                     PILL_PAD_Y, "");
    status_pill_set_state(pill, PILL_RUNNING);
}

void status_pill_set_state(status_pill_t *pill, pill_state_t state)
{
    const char *text = text_of(state);
    const lv_font_t *font = THEME_FONT_PILL;

    pill->state = state;
    lv_label_set_text(pill->label, text);

    const lv_coord_t text_w =
        lv_txt_get_width(text, (uint32_t) strlen(text), font, THEME_TRACK_PILL, LV_TEXT_FLAG_NONE);
    const lv_coord_t w = (lv_coord_t) (text_w + (2 * PILL_PAD_X) + (2 * THEME_BORDER_W));
    const lv_coord_t h = (lv_coord_t) (font->line_height + (2 * PILL_PAD_Y) + (2 * THEME_BORDER_W));

    pill->w = w;
    pill->h = h;
    lv_obj_set_size(pill->box, w, h);
    lv_obj_set_x(pill->box, (lv_coord_t) (pill->right_edge - w));
    lv_obj_set_pos(pill->label, (lv_coord_t) (PILL_PAD_X - THEME_BORDER_W + 1),
                   (lv_coord_t) (PILL_PAD_Y));
    lv_obj_set_style_bg_color(pill->box, tint_of(state), LV_PART_MAIN);
    lv_obj_set_style_border_color(pill->box, colour_of(state), LV_PART_MAIN);
    lv_obj_set_style_text_color(pill->label, colour_of(state), LV_PART_MAIN);
    lv_obj_set_style_opa(pill->box, LV_OPA_COVER, LV_PART_MAIN);
    pill->visible_phase = true;
}

void status_pill_blink_tick(status_pill_t *pill, uint32_t now_ms)
{
    if (pill->state != PILL_ALARM)
    {
        return;
    }
    const bool on = (now_ms % PILL_BLINK_PERIOD_MS) < PILL_BLINK_ON_MS;
    if (on != pill->visible_phase)
    {
        pill->visible_phase = on;
        lv_obj_set_style_opa(pill->box, on ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    }
}
