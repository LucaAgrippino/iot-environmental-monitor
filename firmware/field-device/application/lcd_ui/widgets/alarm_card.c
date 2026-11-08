/**
 * @file alarm_card.c
 * @brief Alarm row widget (see alarm_card.h).
 */

#include "lcd_ui/widgets/alarm_card.h"

#include <string.h>

#include "lcd_ui/theme.h"

#define CARD_PAD_Y THEME_SP_6 /**< padding 14 px / 18 px */
#define CARD_PAD_X THEME_SP_8
#define COL_GAP THEME_SP_6    /**< grid gap 14 px */
#define CHIP_PAD_X THEME_SP_4 /**< chip padding 4 px / 10 px */
#define CHIP_PAD_Y THEME_SP_1
#define TAG_GAP THEME_SP_2 /**< 6 px left margin of the direction tag */

void alarm_card_create(alarm_card_t *card, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w)
{
    const lv_font_t *body = THEME_FONT_BODY_SB;
    const lv_font_t *tiny = THEME_FONT_TINY;
    const lv_font_t *value = THEME_FONT_VALUE;

    card->w = w;
    card->box = theme_box_create(parent, x, y, w, ALARM_CARD_HEIGHT);
    lv_obj_add_style(card->box, &theme_st_card, LV_PART_MAIN);
    card->stripe = theme_box_create(card->box, 0, 0, THEME_ALARM_STRIPE_W, ALARM_CARD_HEIGHT);
    lv_obj_set_style_bg_opa(card->stripe, LV_OPA_COVER, LV_PART_MAIN);

    /* grid 2fr 1fr 1fr 1fr inside the padding, after the stripe */
    const lv_coord_t inner =
        (lv_coord_t) (w - THEME_ALARM_STRIPE_W - (2 * CARD_PAD_X) - (3 * COL_GAP));
    const lv_coord_t fr = (lv_coord_t) (inner / 5);
    const lv_coord_t c1 = (lv_coord_t) (THEME_ALARM_STRIPE_W + CARD_PAD_X);
    card->name_x = c1;
    const lv_coord_t c2 = (lv_coord_t) (c1 + (2 * fr) + COL_GAP);
    const lv_coord_t c3 = (lv_coord_t) (c2 + fr + COL_GAP);

    card->name = theme_label_create(card->box, &theme_st_txt_body_sb, &theme_st_label_ink, c1,
                                    CARD_PAD_Y, "");
    card->tag = theme_label_create(card->box, &theme_st_txt_body_sb, &theme_st_label_err, c1,
                                   CARD_PAD_Y, "");
    card->time = theme_label_create(card->box, &theme_st_txt_caption_mono, &theme_st_label_muted,
                                    c1, (lv_coord_t) (CARD_PAD_Y + body->line_height + 2), "");

    card->cur_title = theme_label_create(card->box, &theme_st_txt_tiny, &theme_st_label_muted, c2,
                                         CARD_PAD_Y, "CURRENT");
    card->cur_value = theme_label_create(card->box, &theme_st_txt_value, &theme_st_label_err, c2,
                                         (lv_coord_t) (CARD_PAD_Y + tiny->line_height), "");
    card->thr_title = theme_label_create(card->box, &theme_st_txt_tiny, &theme_st_label_muted, c3,
                                         CARD_PAD_Y, "THRESHOLD");
    card->thr_value = theme_label_create(card->box, &theme_st_txt_value, &theme_st_label_ink, c3,
                                         (lv_coord_t) (CARD_PAD_Y + tiny->line_height), "");
    (void) value;

    card->chip_box = theme_box_create(card->box, 0, 0, 0, 0);
    lv_obj_set_style_border_width(card->chip_box, THEME_BORDER_W, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card->chip_box, LV_OPA_COVER, LV_PART_MAIN);
    card->chip_label = theme_label_create(card->chip_box, &theme_st_txt_pill, &theme_st_label_err,
                                          CHIP_PAD_X, CHIP_PAD_Y, "\xE2\x97\x8F ACTIVE"); /* ● */
    const char *chip = "\xE2\x97\x8F ACTIVE";
    const lv_font_t *pill = THEME_FONT_PILL;
    const lv_coord_t cw = (lv_coord_t) (lv_txt_get_width(chip, (uint32_t) strlen(chip), pill,
                                                         THEME_TRACK_PILL, LV_TEXT_FLAG_NONE) +
                                        (2 * CHIP_PAD_X) + (2 * THEME_BORDER_W));
    const lv_coord_t ch =
        (lv_coord_t) (pill->line_height + (2 * CHIP_PAD_Y) + (2 * THEME_BORDER_W));
    lv_obj_set_size(card->chip_box, cw, ch);
    lv_obj_set_pos(card->chip_box, (lv_coord_t) (w - CARD_PAD_X - cw),
                   (lv_coord_t) ((ALARM_CARD_HEIGHT - ch) / 2));
    lv_obj_set_pos(card->chip_label, CHIP_PAD_X, CHIP_PAD_Y);

    alarm_card_hide(card);
}

void alarm_card_show(alarm_card_t *card, bool critical, const char *name, const char *tag,
                     const char *triggered, const char *current, const char *threshold)
{
    const lv_color_t sev = critical ? THEME_COL_ERR : THEME_COL_WARN;
    const lv_color_t tint = critical ? THEME_COL_ERR_TINT : THEME_COL_WARN_TINT;
    const lv_font_t *body = THEME_FONT_BODY_SB;

    lv_label_set_text(card->name, name);
    lv_label_set_text(card->tag, tag);
    /* the tag follows the sensor name, 6 px to the right */
    const lv_coord_t nw =
        lv_txt_get_width(name, (uint32_t) strlen(name), body, 0, LV_TEXT_FLAG_NONE);
    lv_obj_set_x(card->tag, (lv_coord_t) (card->name_x + nw + TAG_GAP));
    lv_label_set_text(card->time, triggered);
    lv_label_set_text(card->cur_value, current);
    lv_label_set_text(card->thr_value, threshold);

    lv_obj_set_style_bg_color(card->stripe, sev, LV_PART_MAIN);
    lv_obj_set_style_text_color(card->tag, sev, LV_PART_MAIN);
    lv_obj_set_style_text_color(card->cur_value, sev, LV_PART_MAIN);
    lv_obj_set_style_text_color(card->chip_label, sev, LV_PART_MAIN);
    lv_obj_set_style_border_color(card->chip_box, sev, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card->chip_box, tint, LV_PART_MAIN);
    lv_obj_clear_flag(card->box, LV_OBJ_FLAG_HIDDEN);
}

void alarm_card_hide(alarm_card_t *card)
{
    lv_obj_add_flag(card->box, LV_OBJ_FLAG_HIDDEN);
}
