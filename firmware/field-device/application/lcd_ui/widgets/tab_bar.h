/**
 * @file tab_bar.h
 * @brief Custom four-column tab bar widget for LcdUi.
 *
 * Replaces lv_tabview (forbidden — overlay §9).  Provides a 64 px tall
 * strip at the canvas top with one 200 px-wide button per screen.
 * The active column shows a 3 px COL_ACCENT top stripe and a
 * COL_ACCENT_TINT background.  Inactive columns use COL_HEADER_BG.
 *
 * Navigation is driven by lcd_ui_goto_tab(); tab_bar_set_active() is the
 * visual side-effect that follows a goto call.
 *
 * @see docs/lcd-ui-design/_reference-v9-claude-design/03_SCREEN_SPECS.md §Tab bar
 */

#ifndef TAB_BAR_H
#define TAB_BAR_H

#include <stdint.h>

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

/**
 * @brief Create the tab bar and wire up touch callbacks.
 *
 * @param parent      Parent object (normally lv_scr_act()).
 * @param active_idx  Initially active tab index [0, THEME_TAB_COL_COUNT).
 * @param on_change   Called with the new tab index on a short-click.
 *                    Must not be NULL.
 * @return Pointer to the created tab bar lv_obj_t.
 */
lv_obj_t *tab_bar_create(lv_obj_t *parent, uint16_t active_idx, void (*on_change)(uint16_t idx));

/**
 * @brief Update the visual highlight to reflect a new active tab.
 *
 * Safe to call from any context after tab_bar_create() succeeds.
 *
 * @param tab_bar_obj  Object returned by tab_bar_create() (unused internally;
 *                     kept for API symmetry with lv_obj ownership).
 * @param idx          New active tab index [0, THEME_TAB_COL_COUNT).
 */
void tab_bar_set_active(lv_obj_t *tab_bar_obj, uint16_t idx);

#endif /* TAB_BAR_H */
