#ifndef NUMPAD_H
#define NUMPAD_H

/**
 * @file numpad.h
 * @brief The Config screen's numeric keypad: an entry display and a 3 x 4 key grid, 280 px wide.
 *
 * docs/lcd-ui-design/configuration.html (.numpad): display on COL_SURF2 with the entry in
 * COL_ACCENT (FONT_VALUE, COL_ERR when invalid), then keys 7 8 9 / 4 5 6 / 1 2 3 / . 0 backspace on
 * COL_SURF, 6 px gaps; the backspace key is red-tinted. The design has no sign key although
 * temperatures can be negative, so the display carries a small "+/-" toggle at its right end (an
 * addition, noted in the LLD). Every key is at least 44 px high.
 *
 * The keypad edits a text entry; the screen reads it through the callback after every key.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lcd_ui/widgets/button.h"

#define NUMPAD_W 280 /* .cfg-layout: grid-template-columns 1fr 280px */
#define NUMPAD_TEXT_MAX 8U

/** Called after every key with the new entry text. */
typedef void (*numpad_changed_cb_t)(const char *text);

typedef struct
{
    lv_obj_t *box;
    lv_obj_t *display;
    lv_obj_t *display_text;
    button_t sign;
    button_t key[12];
    char buf[NUMPAD_TEXT_MAX + 1U];
    uint32_t len;
    bool active;
    bool invalid;
    numpad_changed_cb_t changed;
} numpad_t;

/**
 * @brief Build the keypad.
 *
 * @param[out] np       Keypad instance (caller-owned, static).
 * @param[in]  parent   Parent panel.
 * @param[in]  x        Left edge.
 * @param[in]  y        Top edge.
 * @param[in]  h        Total height (display plus keys).
 * @param[in]  changed  Called after every key.
 */
void numpad_create(numpad_t *np, lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t h,
                   numpad_changed_cb_t changed);

/**
 * @brief Load an entry (when a field gets the focus) or clear it with NULL (no field focused).
 *
 * @param[in,out] np    Keypad instance.
 * @param[in]     text  Entry text, or NULL.
 */
void numpad_load(numpad_t *np, const char *text);

/** @brief Show the entry in the error colour or the accent colour. */
void numpad_set_invalid(numpad_t *np, bool invalid);

/**
 * @brief Apply a key. Names: "0".."9", ".", "DEL", "SIGN".
 *
 * @param[in,out] np   Keypad instance.
 * @param[in]     key  Key name.
 */
void numpad_press(numpad_t *np, const char *key);

/** @return The entry text. */
const char *numpad_text(const numpad_t *np);

#endif /* NUMPAD_H */
