#ifndef HEADER_BAR_H
#define HEADER_BAR_H

/**
 * @file header_bar.h
 * @brief 40 px header under the tab bar: title on the left, clock and status pill on the right.
 *
 * 03_SCREEN_SPECS.md "Header bar": COL_BG fill with a 1 px bottom border; title in FONT_EYEBROW
 * COL_INK inset 22 px; clock in FONT_META COL_MUTED then the pill, 14 px apart, inset 22 px.
 */

#include "lcd_ui/widgets/status_pill.h"

/** @brief Header bar widget: title, clock, state pill and alarm badge. */
typedef struct
{
    lv_obj_t *box;      /**< Bar surface. */
    lv_obj_t *title;    /**< Title of the current screen. */
    lv_obj_t *time;     /**< Time of day. */
    status_pill_t pill; /**< Device state pill. */
    lv_obj_t
        *badge_box; /**< Optional amber badge left of the clock, e.g. "ACCUMULATING 2:00 / 5:00". */
    lv_obj_t *badge_label;  /**< Text of the alarm count badge. */
    lv_coord_t badge_right; /**< x of the badge's right edge. */
} header_bar_t;

/**
 * @brief Create the header bar at y = 64.
 *
 * @param[out] hdr     Header instance (caller-owned, static).
 * @param[in]  parent  Parent (the screen).
 */
void header_bar_create(header_bar_t *hdr, lv_obj_t *parent);

/**
 * @brief Set the title (UPPERCASE text).
 *
 * @param hdr Header bar.
 * @param title Title text.
 */
void header_bar_set_title(header_bar_t *hdr, const char *title);

/**
 * @brief Set the clock text, "HH:MM:SS".
 *
 * @param hdr Header bar.
 * @param time_text Clock text.
 */
void header_bar_set_time(header_bar_t *hdr, const char *time_text);

/**
 * @brief Set the status pill state.
 *
 * @param hdr Header bar.
 * @param state New pill state.
 */
void header_bar_set_pill(header_bar_t *hdr, pill_state_t state);

/**
 * @brief Show an amber badge left of the clock, or hide it with NULL (Trend screen: accumulating).
 *
 * @param[in,out] hdr   Header instance.
 * @param[in]     text  Badge text, or NULL to hide.
 */
void header_bar_set_badge(header_bar_t *hdr, const char *text);

#endif /* HEADER_BAR_H */
