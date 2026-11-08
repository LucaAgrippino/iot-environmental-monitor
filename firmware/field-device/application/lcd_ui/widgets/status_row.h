#ifndef STATUS_ROW_H
#define STATUS_ROW_H

/**
 * @file status_row.h
 * @brief One row of the Status screen: status dot, label, value, 1 px bottom border.
 *
 * 03_SCREEN_SPECS.md "Status row": height about 33 px, dot 10 px, label FONT_LABEL COL_INK, value
 * FONT_VALUE_SM in COL_INK when ok, else in the status colour. An error dot gets a soft glow: a
 * second 14 px circle at 40 % opacity behind it.
 */

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

/** @brief Height of a status row, in pixels. */
#define STATUS_ROW_HEIGHT                                                                          \
    30 /* 7 px padding top and bottom, 16 px text line; compact so the banner and all rows fit 340 \
          px */
       /**< State of a status row. */
typedef enum
{
    ROW_OK = 0,
    ROW_WARN = 1,
    ROW_ERR = 2
} row_status_t;

/** @brief Status row widget: a label and its value. */
typedef struct
{
    lv_obj_t *box;    /**< Row surface. */
    lv_obj_t *glow;   /**< Glow behind the dot. */
    lv_obj_t *dot;    /**< Status dot. */
    lv_obj_t *label;  /**< Name of the item. */
    lv_obj_t *value;  /**< Value text. */
    lv_coord_t width; /**< Width, in pixels. */
} status_row_t;

/**
 * @brief Create a row.
 *
 * @param[out] row     Row instance (caller-owned, static).
 * @param[in]  parent  Parent panel.
 * @param[in]  x       Left edge.
 * @param[in]  y       Top edge.
 * @param[in]  w       Width.
 * @param[in]  label   Label text.
 */
void status_row_create(status_row_t *row, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, const char *label);

/**
 * @brief Set the status and the value text.
 *
 * @param[in,out] row     Row instance.
 * @param[in]     status  Dot and value colour.
 * @param[in]     value   Value text, right-aligned.
 */
void status_row_set(status_row_t *row, row_status_t status, const char *value);

#endif /* STATUS_ROW_H */
