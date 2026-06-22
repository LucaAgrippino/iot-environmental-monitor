/**
 * @file footer_bar.h
 * @brief 36 px footer bar widget — left and right caption text.
 *
 * Positioned at y=444 (bottom of canvas).  Background is COL_HEADER_BG
 * with a top 1 px COL_BORDER border.
 *
 * @see docs/lcd-ui-design/_reference-v9-claude-design/03_SCREEN_SPECS.md §Footer bar
 */

#ifndef FOOTER_BAR_H
#define FOOTER_BAR_H

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

/**
 * @brief Create the footer bar.
 *
 * @param parent  Parent object (normally lv_scr_act()).
 * @param left    Initial left-side caption string.
 * @param right   Initial right-side caption string.
 * @return Pointer to the created footer bar lv_obj_t.
 */
lv_obj_t *footer_bar_create(lv_obj_t *parent, const char *left, const char *right);

/**
 * @brief Update left and / or right caption text.
 *
 * Pass NULL for either argument to leave that side unchanged.
 *
 * @param footer_obj  Object returned by footer_bar_create().
 * @param left        New left-side text, or NULL.
 * @param right       New right-side text, or NULL.
 */
void footer_bar_set_text(lv_obj_t *footer_obj, const char *left, const char *right);

#endif /* FOOTER_BAR_H */
