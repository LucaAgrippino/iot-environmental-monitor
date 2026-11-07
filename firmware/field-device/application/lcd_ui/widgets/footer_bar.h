#ifndef FOOTER_BAR_H
#define FOOTER_BAR_H

/**
 * @file footer_bar.h
 * @brief 36 px footer: left and right caption, FONT_CAPTION_MONO COL_MUTED, inset 22 px.
 *
 * 03_SCREEN_SPECS.md "Footer bar": COL_HEADER_BG fill, 1 px top border. The text differs per
 * screen.
 */

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

typedef struct
{
    lv_obj_t *box;
    lv_obj_t *left;
    lv_obj_t *right;
} footer_bar_t;

/**
 * @brief Create the footer bar at y = 444.
 *
 * @param[out] ftr     Footer instance (caller-owned, static).
 * @param[in]  parent  Parent (the screen).
 */
void footer_bar_create(footer_bar_t *ftr, lv_obj_t *parent);

/**
 * @brief Set both captions; the right one stays right-aligned.
 *
 * @param[in] ftr    Footer instance.
 * @param[in] left   Left text.
 * @param[in] right  Right text.
 */
void footer_bar_set(footer_bar_t *ftr, const char *left, const char *right);

#endif /* FOOTER_BAR_H */
