#ifndef CONFIRM_MODAL_H
#define CONFIRM_MODAL_H

/**
 * @file confirm_modal.h
 * @brief "Apply Configuration?" modal listing each change as "name: old -> new unit".
 *
 * docs/lcd-ui-design/configuration.html (.modal): 380 px wide, COL_SURF, 1 px COL_BORDER, padding
 * 24 px, backdrop 60 % black; a title, a muted body line, one bold mono line per change, then two
 * equal buttons (CANCEL muted, CONFIRM accent on the accent tint). Touching outside does not
 * dismiss.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lcd_ui/widgets/button.h"

#define CONFIRM_MODAL_MAX_LINES 7U /**< one line per editable field */

/** @brief Confirmation dialog widget. */
typedef struct
{
    lv_obj_t *backdrop; /**< Dimmed layer behind the dialog. */
    lv_obj_t *panel;    /**< Dialog panel. */
    lv_obj_t *lines;    /**< Container of the change lines. */
    lv_obj_t *body;     /**< Body text. */
    button_t cancel;    /**< CANCEL button. */
    button_t confirm;   /**< CONFIRM button. */
    bool open;          /**< True while the dialog is shown. */
} confirm_modal_t;

/**
 * @brief Build the modal (hidden).
 *
 * @param[out] m           Modal instance (caller-owned, static).
 * @param[in]  parent      Parent (the screen).
 * @param[in]  on_cancel   Handler for CANCEL.
 * @param[in]  on_confirm  Handler for CONFIRM.
 */
void confirm_modal_create(confirm_modal_t *m, lv_obj_t *parent, lv_event_cb_t on_cancel,
                          lv_event_cb_t on_confirm);

/**
 * @brief Show the modal with the change lines.
 *
 * @param[in,out] m      Modal instance.
 * @param[in]     text   One line per change, "\n" separated.
 * @param[in]     count  Number of lines (1..CONFIRM_MODAL_MAX_LINES); sets the height.
 */
void confirm_modal_show(confirm_modal_t *m, const char *text, uint32_t count);

/**
 * @brief Hide the modal.
 *
 * @param m Dialog.
 */
void confirm_modal_hide(confirm_modal_t *m);

/**
 * @param m Dialog.
 * @return true while shown.
 *
 */
bool confirm_modal_is_open(const confirm_modal_t *m);

#endif /* CONFIRM_MODAL_H */
