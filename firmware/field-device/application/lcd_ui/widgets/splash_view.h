#ifndef SPLASH_VIEW_H
#define SPLASH_VIEW_H

/**
 * @file splash_view.h
 * @brief Boot splash: logo, product name, progress bar with step name, BOOT/READY badge, footer.
 *
 * docs/lcd-ui-design/splash.html (REQ-LD-200..240). A full-screen overlay created last, so it is
 * drawn over every screen until it is hidden.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

typedef struct
{
    lv_obj_t *root;      /**< Full-screen overlay. */
    lv_obj_t *logo;      /**< Panel glyph with the ECG trace (custom-drawn). */
    lv_obj_t *fill;      /**< Filled part of the progress bar. */
    lv_obj_t *step_lbl;  /**< Name of the running boot step. */
    lv_obj_t *pct_lbl;   /**< Progress, "62%". */
    lv_obj_t *dot;       /**< Badge dot. */
    lv_obj_t *badge_lbl; /**< "BOOT" or "READY". */
    uint8_t percent;     /**< Last progress shown, 0-100. */
} splash_view_t;

/** Width of the progress bar track in pixels (splash.html: 420). */
#define SPLASH_BAR_W (420)

/**
 * @brief Build the overlay (hidden) under @p parent.
 *
 * @param[in] footer_left  Text at the bottom left, e.g. "FW v1.0.0 . build 20261107 . LVGL v8.3.11".
 * @param[in] footer_right Text at the bottom right, e.g. "STM32F469 . 800x480".
 */
void splash_view_create(splash_view_t *s, lv_obj_t *parent, const char *footer_left,
                        const char *footer_right);

/**
 * @brief Show the progress and the step name; 100 switches the badge to READY.
 *
 * @param[in] percent  0-100; larger values are clamped.
 * @param[in] step     Step name, shown as given; NULL keeps the current name.
 */
void splash_view_set(splash_view_t *s, uint8_t percent, const char *step);

/** @brief Show or hide the overlay. */
void splash_view_show(splash_view_t *s, bool visible);

/** @brief True while the overlay is shown. */
bool splash_view_visible(const splash_view_t *s);

#endif /* SPLASH_VIEW_H */
