/**
 * @file header_bar.h
 * @brief 40 px header bar widget — screen title and system status row.
 *
 * Positioned at y=64 (below the tab bar).  Shows the screen title
 * (FONT_EYEBROW, left-inset 22 px) and a "00:00:00" time placeholder on
 * the right side.
 *
 * TODO(clock): replace static time string with RTC read once time_provider
 * is integrated into LcdUi.
 *
 * @see docs/lcd-ui-design/_reference-v9-claude-design/03_SCREEN_SPECS.md §Header bar
 */

#ifndef HEADER_BAR_H
#define HEADER_BAR_H

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

/**
 * @brief Create the header bar.
 *
 * @param parent  Parent object (normally lv_scr_act()).
 * @param title   Initial title string shown on the left side.
 * @return Pointer to the created header bar lv_obj_t.
 */
lv_obj_t *header_bar_create(lv_obj_t *parent, const char *title);

/**
 * @brief Update the header title text.
 *
 * @param header_obj  Object returned by header_bar_create().
 * @param title       New title string.
 */
void header_bar_set_title(lv_obj_t *header_obj, const char *title);

#endif /* HEADER_BAR_H */
