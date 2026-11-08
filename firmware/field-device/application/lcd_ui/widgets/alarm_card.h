#ifndef ALARM_CARD_H
#define ALARM_CARD_H

/**
 * @file alarm_card.h
 * @brief One active-alarm row: severity stripe, sensor and direction, current and threshold, chip.
 *
 * 03_SCREEN_SPECS.md "Alarm row": COL_SURF with a 1 px border, a 5 px stripe in the severity
 * colour, padding 14 px / 18 px, grid 2fr 1fr 1fr 1fr with a 14 px gap, margin-bottom 10 px.
 */

#include <stdbool.h>

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

/** @brief Height of an alarm card, in pixels. */
#define ALARM_CARD_HEIGHT                                                                          \
    66                    /* 14 px padding, two text lines, 14 px padding, rounded to the grid     \
                           */
#define ALARM_CARD_GAP 10 /**< margin-bottom */

/** @brief Alarm card widget: one active alarm. */
typedef struct
{
    lv_obj_t *box;        /**< Card surface. */
    lv_obj_t *stripe;     /**< Status stripe at the left edge. */
    lv_obj_t *name;       /**< Name of the sensor. */
    lv_obj_t *tag;        /**< HIGH or LOW, in the severity colour. */
    lv_obj_t *time;       /**< Time the alarm was raised. */
    lv_obj_t *cur_title;  /**< Title of the current value. */
    lv_obj_t *cur_value;  /**< Current value. */
    lv_obj_t *thr_title;  /**< Title of the threshold. */
    lv_obj_t *thr_value;  /**< Threshold value. */
    lv_obj_t *chip_box;   /**< Box of the state chip. */
    lv_obj_t *chip_label; /**< Text of the state chip. */
    lv_coord_t w;         /**< Width, in pixels. */
    lv_coord_t name_x;    /**< Left edge of the name column (the tag follows the name). */
} alarm_card_t;

/**
 * @brief Create a row (initially hidden).
 *
 * @param[out] card    Row instance (caller-owned, static).
 * @param[in]  parent  Parent panel.
 * @param[in]  x       Left edge.
 * @param[in]  y       Top edge.
 * @param[in]  w       Width.
 */
void alarm_card_create(alarm_card_t *card, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w);

/**
 * @brief Fill and show the row.
 *
 * @param[in,out] card       Row instance.
 * @param[in]     critical   true: error colour; false: warning colour.
 * @param[in]     name       Sensor name, e.g. "Temperature · S1".
 * @param[in]     tag        "HIGH" or "LOW".
 * @param[in]     triggered  "Triggered 12:18:07".
 * @param[in]     current    Current value with unit.
 * @param[in]     threshold  Threshold with unit.
 */
void alarm_card_show(alarm_card_t *card, bool critical, const char *name, const char *tag,
                     const char *triggered, const char *current, const char *threshold);

/**
 * @brief Hide the row.
 *
 * @param card Card to hide.
 */
void alarm_card_hide(alarm_card_t *card);

#endif /* ALARM_CARD_H */
