/**
 * @file sensor_card.c
 * @brief Sensor card widget — implementation.
 *
 * Fixed-point formatting rules (01_DESIGN_TOKENS.md §Sensor data):
 *   CENTI (×100): int_part = val / 100, tenths = abs(val % 100) / 10
 *                 display e.g. "23.4" or "-4.5"
 *   DECI  (×10):  display val / 10 as plain integer e.g. "1013"
 *
 * @see firmware/field-device/application/lcd_ui/widgets/sensor_card.h
 */

#include <stdio.h>
#include <stdlib.h> /* abs() for int32_t */

#include "lcd_ui/widgets/sensor_card.h"
#include "lcd_ui/theme.h"

/* ===================================================================== */
/* Layout geometry within the card (relative to card top-left)         */
/* ===================================================================== */

/* Horizontal offset for text content (right of the 4 px stripe) */
#define CARD_TEXT_X (THEME_CARD_STRIPE_W + THEME_SP_7) /* = 20 */
#define CARD_TEXT_W (THEME_CARD_W - CARD_TEXT_X - THEME_SP_7)

/* Vertical positions (03_SCREEN_SPECS.md §Sensor card) */
#define CARD_EYEBROW_Y THEME_SP_8       /* = 18 */
#define CARD_VALUE_Y 52                 /* hero number top edge       */
#define CARD_UNIT_Y (CARD_VALUE_Y + 18) /* unit below value number */
#define CARD_WAITING_Y CARD_VALUE_Y     /* overlaps hero area         */
#define CARD_SPARK_Y (THEME_CARD_H - THEME_SP_8 - THEME_SPARKLINE_H - 20)
#define CARD_STATUS_Y (THEME_CARD_H - THEME_SP_8 - 14)
#define CARD_DELTA_X (THEME_CARD_W - THEME_SP_7 - 20)
#define CARD_DELTA_Y CARD_STATUS_Y

/* ===================================================================== */
/* Internal formatting                                                  */
/* ===================================================================== */

#define FMT_BUF_SIZE 16U

static void format_value(char *buf, int32_t value, sensor_card_fmt_t fmt)
{
    if (fmt == SENSOR_CARD_FMT_CENTI)
    {
        /* ×100 — one decimal place, e.g. 2340 → "23.4", -450 → "-4.5" */
        int32_t int_part = value / 100;
        int32_t remainder = abs((int) (value % 100));
        int32_t tenths = remainder / 10;
        (void) snprintf(buf, FMT_BUF_SIZE, "%d.%d", (int) int_part, (int) tenths);
    }
    else /* SENSOR_CARD_FMT_DECI */
    {
        /* ×10 — integer, e.g. 10132 → "1013" */
        (void) snprintf(buf, FMT_BUF_SIZE, "%d", (int) (value / 10));
    }
}

/* ===================================================================== */
/* sensor_card_create                                                   */
/* ===================================================================== */

void sensor_card_create(sensor_card_t *card, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h, const char *eyebrow_text, const char *unit_text,
                        sensor_card_fmt_t fmt)
{
    card->fmt = fmt;

    /* ── Card container ───────────────────────────────────────────────── */
    card->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(card->obj);
    lv_obj_set_pos(card->obj, x, y);
    lv_obj_set_size(card->obj, w, h);
    lv_obj_add_style(card->obj, &theme_st_card, LV_PART_MAIN);
    lv_obj_clear_flag(card->obj, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Left stripe (4 px, full height, COL_DIM until first reading) ─── */
    card->stripe = lv_obj_create(card->obj);
    lv_obj_remove_style_all(card->stripe);
    lv_obj_set_pos(card->stripe, 0, 0);
    lv_obj_set_size(card->stripe, THEME_CARD_STRIPE_W, h);
    lv_obj_set_style_bg_color(card->stripe, THEME_COL_DIM, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card->stripe, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card->stripe, THEME_RAD_NONE, LV_PART_MAIN);

    /* ── Eyebrow label ("TEMPERATURE" etc.) ──────────────────────────── */
    card->eyebrow_lbl = lv_label_create(card->obj);
    lv_obj_remove_style_all(card->eyebrow_lbl);
    lv_obj_set_pos(card->eyebrow_lbl, CARD_TEXT_X, CARD_EYEBROW_Y);
    lv_obj_add_style(card->eyebrow_lbl, &theme_st_label_muted, LV_PART_MAIN);
    /* TODO(fonts): replace with FONT_EYEBROW (12 px bold uppercase) */
    lv_label_set_text(card->eyebrow_lbl, eyebrow_text);

    /* ── Hero value label (large number) ─────────────────────────────── */
    card->value_lbl = lv_label_create(card->obj);
    lv_obj_remove_style_all(card->value_lbl);
    lv_obj_set_pos(card->value_lbl, CARD_TEXT_X, CARD_VALUE_Y);
    lv_obj_add_style(card->value_lbl, &theme_st_label_ink, LV_PART_MAIN);
    /* TODO(fonts): replace with FONT_HERO (48 px) */
    lv_label_set_text(card->value_lbl, "--");
    lv_obj_add_flag(card->value_lbl, LV_OBJ_FLAG_HIDDEN);

    /* ── Unit label ───────────────────────────────────────────────────── */
    card->unit_lbl = lv_label_create(card->obj);
    lv_obj_remove_style_all(card->unit_lbl);
    lv_obj_set_pos(card->unit_lbl, CARD_TEXT_X + 60, CARD_UNIT_Y);
    lv_obj_add_style(card->unit_lbl, &theme_st_label_muted, LV_PART_MAIN);
    lv_label_set_text(card->unit_lbl, unit_text);
    lv_obj_add_flag(card->unit_lbl, LV_OBJ_FLAG_HIDDEN);

    /* ── Waiting overlay — covers hero area when no data yet ─────────── */
    card->waiting_lbl = lv_label_create(card->obj);
    lv_obj_remove_style_all(card->waiting_lbl);
    lv_obj_set_pos(card->waiting_lbl, CARD_TEXT_X, CARD_WAITING_Y);
    lv_obj_add_style(card->waiting_lbl, &theme_st_label_dim, LV_PART_MAIN);
    lv_label_set_text(card->waiting_lbl, "Waiting for data...");

    /* ── Sparkline placeholder ───────────────────────────────────────── */
    card->sparkline = lv_obj_create(card->obj);
    lv_obj_remove_style_all(card->sparkline);
    lv_obj_set_pos(card->sparkline, CARD_TEXT_X, CARD_SPARK_Y);
    lv_obj_set_size(card->sparkline, THEME_SPARKLINE_W, THEME_SPARKLINE_H);
    lv_obj_set_style_bg_color(card->sparkline, THEME_COL_SURF2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card->sparkline, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card->sparkline, THEME_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card->sparkline, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card->sparkline, THEME_RAD_NONE, LV_PART_MAIN);
    lv_obj_add_flag(card->sparkline, LV_OBJ_FLAG_HIDDEN);
    /* TODO(sparkline): replace placeholder with lv_chart when series data is available */

    /* ── Status label ("OK" / "ERROR" / "WAITING") ───────────────────── */
    card->status_lbl = lv_label_create(card->obj);
    lv_obj_remove_style_all(card->status_lbl);
    lv_obj_set_pos(card->status_lbl, CARD_TEXT_X, CARD_STATUS_Y);
    lv_obj_add_style(card->status_lbl, &theme_st_label_dim, LV_PART_MAIN);
    lv_label_set_text(card->status_lbl, "WAITING");
    lv_obj_add_flag(card->status_lbl, LV_OBJ_FLAG_HIDDEN);

    /* ── Delta label (em-dash placeholder) ──────────────────────────── */
    card->delta_lbl = lv_label_create(card->obj);
    lv_obj_remove_style_all(card->delta_lbl);
    lv_obj_set_pos(card->delta_lbl, CARD_DELTA_X, CARD_DELTA_Y);
    lv_obj_add_style(card->delta_lbl, &theme_st_label_dim, LV_PART_MAIN);
    lv_label_set_text(card->delta_lbl, "-");
    lv_obj_add_flag(card->delta_lbl, LV_OBJ_FLAG_HIDDEN);
    /* TODO(delta): replace with computed Δ per refresh cycle */
}

/* ===================================================================== */
/* sensor_card_show_waiting                                             */
/* ===================================================================== */

void sensor_card_show_waiting(sensor_card_t *card)
{
    lv_obj_set_style_bg_color(card->stripe, THEME_COL_DIM, LV_PART_MAIN);

    lv_obj_clear_flag(card->waiting_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(card->value_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card->unit_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card->sparkline, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card->status_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card->delta_lbl, LV_OBJ_FLAG_HIDDEN);
}

/* ===================================================================== */
/* sensor_card_update                                                   */
/* ===================================================================== */

void sensor_card_update(sensor_card_t *card, int32_t value, bool valid)
{
    /* Hide the waiting overlay on first data arrival */
    lv_obj_add_flag(card->waiting_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Show persistent widgets */
    lv_obj_clear_flag(card->value_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(card->unit_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(card->sparkline, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(card->status_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(card->delta_lbl, LV_OBJ_FLAG_HIDDEN);

    if (valid)
    {
        char buf[FMT_BUF_SIZE];
        format_value(buf, value, card->fmt);
        lv_label_set_text(card->value_lbl, buf);
        lv_obj_set_style_text_color(card->value_lbl, THEME_COL_INK, LV_PART_MAIN);

        lv_obj_set_style_bg_color(card->stripe, THEME_COL_OK, LV_PART_MAIN);
        lv_obj_set_style_text_color(card->status_lbl, THEME_COL_OK, LV_PART_MAIN);
        lv_label_set_text(card->status_lbl, "OK");
    }
    else
    {
        lv_label_set_text(card->value_lbl, "--");
        lv_obj_set_style_text_color(card->value_lbl, THEME_COL_DIM, LV_PART_MAIN);

        lv_obj_set_style_bg_color(card->stripe, THEME_COL_ERR, LV_PART_MAIN);
        lv_obj_set_style_text_color(card->status_lbl, THEME_COL_ERR, LV_PART_MAIN);
        lv_label_set_text(card->status_lbl, "ERROR");
    }
}
