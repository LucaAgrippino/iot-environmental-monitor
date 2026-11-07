#ifndef CONFIG_FIELD_H
#define CONFIG_FIELD_H

/**
 * @file config_field.h
 * @brief A labelled value box of the Config screen.
 *
 * docs/lcd-ui-design/configuration.html (.cfg-field): an UPPERCASE label (10 px bold, tracking,
 * COL_MUTED) above an input box (COL_SURF2, 1 px border; COL_ACCENT when focused, COL_ERR when
 * invalid; padding 9 px / 12 px; FONT_BODY_MONO) with the unit at its right end (FONT_CAPTION_MONO,
 * COL_DIM). A saved value is COL_MUTED; the focused or edited value is COL_INK, with a blinking
 * caret while focused. An error message sits under the box in COL_ERR.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

#define CONFIG_FIELD_LABEL_H 17 /* eyebrow line plus the 4 px gap */
#define CONFIG_FIELD_INPUT_H 39 /* 9 px padding, 19 px text line, 9 px padding, 2 px border */
#define CONFIG_FIELD_ERR_H 18   /* error line under the box */
#define CONFIG_FIELD_PITCH 80   /* label 17 + input 39 + error line 18 + 6 px gap */

typedef struct
{
    lv_obj_t *box;
    lv_obj_t *label;
    lv_obj_t *input;
    lv_obj_t *value;
    lv_obj_t *unit;
    lv_obj_t *caret;
    lv_obj_t *err;
    lv_coord_t w;
    bool focused;
    bool invalid;
    bool modified;
} config_field_t;

/**
 * @brief Create a field.
 *
 * @param[out] f       Field instance (caller-owned, static).
 * @param[in]  parent  Parent panel (the scrolling field list).
 * @param[in]  x       Left edge.
 * @param[in]  y       Top edge.
 * @param[in]  w       Width.
 * @param[in]  label   Label, UPPERCASE, e.g. "TEMPERATURE · HIGH ALARM".
 * @param[in]  unit    Unit text, e.g. "°C".
 */
void config_field_create(config_field_t *f, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                         lv_coord_t w, const char *label, const char *unit);

/** @brief Show a value (text prepared by the screen). */
void config_field_set_value(config_field_t *f, const char *text);

/** @brief Mark the value as edited (COL_INK) or saved (COL_MUTED). */
void config_field_set_modified(config_field_t *f, bool modified);

/** @brief Focus look (accent border, caret) or not. */
void config_field_set_focus(config_field_t *f, bool focused);

/** @brief Show an error message under the box and the error look, or clear it with NULL. */
void config_field_set_error(config_field_t *f, const char *message);

/** @brief Drive the caret blink (1 Hz, 50 % duty); call from the UI tick. */
void config_field_blink_tick(config_field_t *f, uint32_t now_ms);

/** @brief Register a tap handler on the input box. */
void config_field_on_click(config_field_t *f, lv_event_cb_t cb, void *user_data);

#endif /* CONFIG_FIELD_H */
