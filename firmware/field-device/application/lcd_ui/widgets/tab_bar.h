#ifndef TAB_BAR_H
#define TAB_BAR_H

/**
 * @file tab_bar.h
 * @brief 64 px tab bar with five 160 px touch columns, each an icon above an UPPERCASE label.
 *
 * Replaces lv_tabview (forbidden, 00_PROJECT_OVERLAY.md section 9). 03_SCREEN_SPECS.md "Tab bar"
 * and style-guide.md section 5: COL_HEADER_BG fill, 1 px dividers, the active tab has a 3 px
 * COL_ACCENT top stripe and the pre-blended accent tint, icon in COL_ACCENT and label in COL_INK;
 * inactive tabs are #888888. Five tabs since SRS v1.2 (REQ-LD-000): SENSORS, TREND, STATUS, ALARMS,
 * CONFIG.
 */

#include <stdint.h>

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

#include "lcd_ui/theme.h"

/** Called with the tab index when a tab is tapped. */
typedef void (*tab_bar_change_cb_t)(uint16_t new_idx);

/** @brief Tab bar widget. */
typedef struct
{
    lv_obj_t *box;                         /**< Bar surface. */
    lv_obj_t *column[THEME_TAB_COL_COUNT]; /**< Touch area and fill of each tab. */
    lv_obj_t *stripe[THEME_TAB_COL_COUNT]; /**< 3 px top accent, shown on the active tab only. */
    lv_obj_t *icon[THEME_TAB_COL_COUNT];   /**< Icon of each tab. */
    lv_obj_t *label[THEME_TAB_COL_COUNT];  /**< Label of each tab. */
    uint16_t active;                       /**< Index of the active tab. */
    tab_bar_change_cb_t on_change;         /**< Called when the user selects another tab. */
} tab_bar_t;

/**
 * @brief Create the tab bar at the top of the canvas.
 *
 * @param[out] bar        Tab bar instance (caller-owned, static).
 * @param[in]  parent     Parent (the screen).
 * @param[in]  active     Initially active tab.
 * @param[in]  on_change  Callback for taps (may be NULL).
 */
void tab_bar_create(tab_bar_t *bar, lv_obj_t *parent, uint16_t active,
                    tab_bar_change_cb_t on_change);

/**
 * @brief Show a tab as active (does not call the callback).
 *
 * @param[in,out] bar  Tab bar instance.
 * @param[in]     idx  Tab index, 0..THEME_TAB_COL_COUNT-1; out-of-range values are ignored.
 */
void tab_bar_set_active(tab_bar_t *bar, uint16_t idx);

#endif /* TAB_BAR_H */
