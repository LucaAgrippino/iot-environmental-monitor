#ifndef BUTTON_H
#define BUTTON_H

/**
 * @file button.h
 * @brief Rectangular text buttons used by the Config screen, its keypad and its confirm modal.
 *
 * Looks follow docs/lcd-ui-design/configuration.html: numeric keys (.numkey), the action bar
 * (.cfg-btn: CANCEL muted, APPLY accent on the accent tint, dim when disabled) and the modal
 * buttons
 * (.modal-btn: CANCEL muted with a border, CONFIRM accent with a border on the accent tint).
 */

#include <stdbool.h>

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

typedef enum
{
    BUTTON_GHOST = 0,
    BUTTON_PRIMARY = 1,
    BUTTON_DISABLED = 2,
    BUTTON_KEY = 3, /**< Numeric key: COL_SURF fill, 1 px border, COL_INK text, FONT_BODY_MONO. */
    BUTTON_KEY_ERR = 4,  /**< Backspace key: error tint, error border and text. */
    BUTTON_TINT = 5,     /**< Modal CONFIRM: accent text and border on the accent tint. */
    BUTTON_BAR = 6,      /**< Action-bar CANCEL: transparent, muted text, no border. */
    BUTTON_BAR_TINT = 7, /**< Action-bar APPLY, enabled: accent text on the accent tint. */
    BUTTON_BAR_OFF = 8   /**< Action-bar APPLY, disabled: dim text, transparent. */
} button_kind_t;

typedef struct
{
    lv_obj_t *box;
    lv_obj_t *label;
    button_kind_t kind;
    lv_coord_t w;
    lv_coord_t h;
} button_t;

/**
 * @brief Create a button.
 *
 * @param[out] btn     Button instance (caller-owned, static).
 * @param[in]  parent  Parent object.
 * @param[in]  x       Left edge.
 * @param[in]  y       Top edge.
 * @param[in]  w       Width (at least 44 px for a touch target where the layout allows).
 * @param[in]  h       Height.
 * @param[in]  text    Label.
 * @param[in]  kind    Look.
 */
void button_create(button_t *btn, lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                   lv_coord_t h, const char *text, button_kind_t kind);

/** @brief Change the look; a disabled button ignores taps (CLICKABLE flag removed). */
void button_set_kind(button_t *btn, button_kind_t kind);

/** @brief Change the text; it stays centred. */
void button_set_text(button_t *btn, const char *text);

/** @brief Register a tap handler (LV_EVENT_CLICKED). */
void button_on_click(button_t *btn, lv_event_cb_t cb, void *user_data);

#endif /* BUTTON_H */
