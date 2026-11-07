/**
 * @file sensor_card.c
 * @brief Sensor card widget (see sensor_card.h).
 */

#include "lcd_ui/widgets/sensor_card.h"

#include <string.h>

#include "lcd_ui/theme.h"

#define CARD_PAD_Y THEME_SP_7 /* 03_SCREEN_SPECS.md: inner padding 16 px / 18 px */
#define CARD_PAD_X THEME_SP_8
#define CARD_VALUE_Y 34   /* top of the hero label: digits start about 50 px below the card top */
#define CARD_WAITING_Y 56 /* "Waiting for data…" sits in the value row */
#define CARD_UNIT_GAP THEME_SP_3 /* 8 px between number and unit */
#define CARD_SPARK_Y 116         /* sparkline top: label, value row (54) and 14 px gaps */
#define CARD_STATUS_Y 164        /* status row: sparkline bottom plus 14 px */
#define SPARK_STROKE_W 2         /* 1.5 px in the design, rounded up for the pixel grid */
#define SPARK_FILL_OPA 31        /* 12 % of 255, 03_SCREEN_SPECS.md "Filled area" */
#define SPARK_DASH 3             /* "stroke dash 3 3" */

static lv_color_t stripe_colour(card_state_t s)
{
    switch (s)
    {
    case CARD_STALE:
        return THEME_COL_WARN;
    case CARD_ERROR:
    case CARD_ALARM:
        return THEME_COL_ERR;
    case CARD_WAIT:
        return THEME_COL_DIM;
    case CARD_VALID:
    default:
        return THEME_COL_OK;
    }
}

static lv_color_t value_colour(card_state_t s)
{
    switch (s)
    {
    case CARD_ERROR:
        return THEME_COL_DIM; /* faded: the shown value is the last one before the error */
    case CARD_ALARM:
        return THEME_COL_ERR;
    default:
        return THEME_COL_INK; /* a stale value stays readable */
    }
}

static lv_color_t status_colour(card_state_t s)
{
    switch (s)
    {
    case CARD_STALE:
        return THEME_COL_WARN;
    case CARD_ERROR:
    case CARD_ALARM:
        return THEME_COL_ERR;
    case CARD_WAIT:
        return THEME_COL_MUTED;
    case CARD_VALID:
    default:
        return THEME_COL_OK;
    }
}

#ifndef TEST
/* Sparkline colour: status colour, faded for an error, accent when healthy. */
static lv_color_t spark_colour(card_state_t s)
{
    switch (s)
    {
    case CARD_STALE:
        return THEME_COL_WARN;
    case CARD_ERROR:
        return THEME_COL_DIM;
    case CARD_ALARM:
        return THEME_COL_ERR;
    case CARD_VALID:
    default:
        return THEME_COL_ACCENT;
    }
}

static void spark_draw_cb(lv_event_t *e)
{
    sensor_card_t *card = (sensor_card_t *) lv_event_get_user_data(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t box;

    if ((card == NULL) || (card->spark_n < 2U))
    {
        return;
    }
    lv_obj_get_coords(card->spark, &box);

    const lv_coord_t w = (lv_coord_t) (box.x2 - box.x1);
    const bool dashed = (card->state == CARD_STALE);
    const lv_color_t colour = spark_colour(card->state);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = colour;
    line.width = SPARK_STROKE_W;
    line.round_start = 1U;
    line.round_end = 1U;
    if (dashed)
    {
        line.dash_width = SPARK_DASH;
        line.dash_gap = SPARK_DASH;
    }

    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color = colour;
    fill.bg_opa = SPARK_FILL_OPA;
    fill.border_width = 0;

    for (uint32_t i = 1U; i < card->spark_n; i++)
    {
        const int16_t ya = card->spark_y[i - 1U];
        const int16_t yb = card->spark_y[i];
        if ((ya == SPARK_GAP) || (yb == SPARK_GAP))
        {
            continue; /* REQ-LD-190: no line across an invalid sample */
        }
        const lv_coord_t xa =
            (lv_coord_t) (box.x1 + (((i - 1U) * (uint32_t) w) / (card->spark_n - 1U)));
        const lv_coord_t xb = (lv_coord_t) (box.x1 + ((i * (uint32_t) w) / (card->spark_n - 1U)));
        lv_point_t pa = {.x = xa, .y = (lv_coord_t) (box.y1 + ya)};
        lv_point_t pb = {.x = xb, .y = (lv_coord_t) (box.y1 + yb)};

        if (!dashed)
        {
            /* area under the segment, one column per pixel, 12 % of the line colour */
            for (lv_coord_t x = xa; x < xb;
                 x++) /* xb belongs to the next segment: no double paint */
            {
                const lv_coord_t span = (lv_coord_t) (xb - xa);
                const lv_coord_t y_line =
                    (span == 0) ? pa.y : (lv_coord_t) (pa.y + (((pb.y - pa.y) * (x - xa)) / span));
                lv_area_t col = {.x1 = x, .y1 = y_line, .x2 = x, .y2 = box.y2};
                lv_draw_rect(ctx, &fill, &col);
            }
        }
        lv_draw_line(ctx, &line, &pa, &pb);
    }
}
#endif /* TEST */

void sensor_card_create(sensor_card_t *card, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h, const char *eyebrow, const char *unit)
{
    card->state = CARD_WAIT;
    card->spark_n = 0U;
    card->inner_w = (lv_coord_t) (w - (2 * CARD_PAD_X));

    card->box = theme_box_create(parent, x, y, w, h);
    lv_obj_add_style(card->box, &theme_st_card, LV_PART_MAIN);

    card->stripe = theme_box_create(card->box, 0, 0, THEME_CARD_STRIPE_W, h);
    lv_obj_set_style_bg_opa(card->stripe, LV_OPA_COVER, LV_PART_MAIN);

    card->eyebrow = theme_label_create(card->box, &theme_st_txt_eyebrow, &theme_st_label_muted,
                                       CARD_PAD_X, CARD_PAD_Y, eyebrow);

    card->value_lbl = theme_label_create(card->box, &theme_st_txt_hero, &theme_st_label_ink,
                                         CARD_PAD_X, CARD_VALUE_Y, "");
    card->unit_lbl = theme_label_create(card->box, &theme_st_txt_body_mono, &theme_st_label_muted,
                                        CARD_PAD_X, CARD_VALUE_Y, unit);
    card->waiting_lbl =
        theme_label_create(card->box, &theme_st_txt_value, &theme_st_label_muted, CARD_PAD_X,
                           CARD_WAITING_Y, "Waiting for data\xE2\x80\xA6"); /* … U+2026 */

    card->spark =
        theme_box_create(card->box, CARD_PAD_X, CARD_SPARK_Y, card->inner_w, THEME_SPARKLINE_H);
#ifndef TEST
    lv_obj_add_event_cb(card->spark, spark_draw_cb, LV_EVENT_DRAW_MAIN, card);
#endif

    card->status_lbl = theme_label_create(card->box, &theme_st_txt_caption_mono, &theme_st_label_ok,
                                          CARD_PAD_X, CARD_STATUS_Y, "");
    card->delta_lbl = theme_label_create(card->box, &theme_st_txt_caption_mono,
                                         &theme_st_label_muted, CARD_PAD_X, CARD_STATUS_Y, "");

    sensor_card_set(card, CARD_WAIT, "", "\xE2\x80\xA6 WAITING", "\xE2\x80\x94"); /* … and — */
}

void sensor_card_set(sensor_card_t *card, card_state_t state, const char *value,
                     const char *status_text, const char *delta_text)
{
    card->state = state;
    const bool waiting = (state == CARD_WAIT);

    lv_obj_set_style_bg_color(card->stripe, stripe_colour(state), LV_PART_MAIN);

    if (waiting)
    {
        lv_obj_add_flag(card->value_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card->unit_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card->spark, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(card->waiting_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        const lv_font_t *hero = THEME_FONT_HERO;
        const lv_font_t *unit_font = THEME_FONT_BODY_MONO;

        lv_label_set_text(card->value_lbl, value);
        lv_obj_set_style_text_color(card->value_lbl, value_colour(state), LV_PART_MAIN);

        /* unit on the number's baseline, CARD_UNIT_GAP right of the last digit */
        const lv_coord_t num_w = lv_txt_get_width(value, (uint32_t) strlen(value), hero,
                                                  THEME_TRACK_HERO, LV_TEXT_FLAG_NONE);
        const lv_coord_t baseline =
            (lv_coord_t) (CARD_VALUE_Y + hero->line_height - hero->base_line);
        lv_obj_set_pos(card->unit_lbl, (lv_coord_t) (CARD_PAD_X + num_w + CARD_UNIT_GAP),
                       (lv_coord_t) (baseline - (unit_font->line_height - unit_font->base_line)));

        lv_obj_clear_flag(card->value_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(card->unit_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(card->spark, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card->waiting_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(card->spark);
    }

    lv_label_set_text(card->status_lbl, status_text);
    lv_obj_set_style_text_color(card->status_lbl, status_colour(state), LV_PART_MAIN);

    const lv_font_t *cap = THEME_FONT_CAPTION_MONO;
    lv_label_set_text(card->delta_lbl, delta_text);
    const lv_coord_t dw =
        lv_txt_get_width(delta_text, (uint32_t) strlen(delta_text), cap, 0, LV_TEXT_FLAG_NONE);
    lv_obj_set_x(card->delta_lbl, (lv_coord_t) (CARD_PAD_X + card->inner_w - dw));
}

void sensor_card_set_spark(sensor_card_t *card, const int16_t *y, uint32_t n)
{
    if (n > SPARK_MAX_POINTS)
    {
        n = SPARK_MAX_POINTS;
    }
    for (uint32_t i = 0U; i < n; i++)
    {
        card->spark_y[i] = y[i];
    }
    card->spark_n = n;
    lv_obj_invalidate(card->spark);
}
