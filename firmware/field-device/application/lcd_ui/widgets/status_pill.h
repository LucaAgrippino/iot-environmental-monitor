#ifndef STATUS_PILL_H
#define STATUS_PILL_H

/**
 * @file status_pill.h
 * @brief Rectangular state chip: RUNNING, INIT, ALARM (blinking) and UPDATE.
 *
 * 03_SCREEN_SPECS.md "Status pill": 1 px border and text in the state colour, pre-blended tint
 * fill, padding 3 px / 10 px, FONT_PILL, no radius. The ALARM pill blinks at 1 Hz, 50 % duty
 * (style-guide.md section 8).
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

typedef enum
{
    PILL_RUNNING = 0, /**< "● RUNNING", COL_OK     */
    PILL_INIT = 1,    /**< "● INIT",    COL_ACCENT */
    PILL_ALARM = 2,   /**< "■ ALARM",   COL_ERR, blinking */
    PILL_UPDATE = 3   /**< "⇪ UPDATE",  COL_ACCENT */
} pill_state_t;

typedef struct
{
    lv_obj_t *box;   /**< Bordered chip. */
    lv_obj_t *label; /**< Text inside the chip. */
    pill_state_t state;
    lv_coord_t
        right_edge; /**< x of the chip's right edge in the parent (the chip grows leftwards). */
    lv_coord_t
        w; /**< Current chip size, kept here because LVGL reports 0 before the first layout. */
    lv_coord_t h;
    bool visible_phase; /**< Current blink phase. */
} status_pill_t;

/**
 * @brief Create a pill whose right edge sits at x_right.
 *
 * @param[out] pill     Pill instance (caller-owned, static).
 * @param[in]  parent   Parent object.
 * @param[in]  x_right  Right edge in the parent.
 * @param[in]  y        Top edge in the parent.
 */
void status_pill_create(status_pill_t *pill, lv_obj_t *parent, lv_coord_t x_right, lv_coord_t y);

/**
 * @brief Change the state: text, colours and width follow.
 *
 * @param[in,out] pill   Pill instance.
 * @param[in]     state  New state.
 */
void status_pill_set_state(status_pill_t *pill, pill_state_t state);

/**
 * @brief Drive the ALARM blink; call from the UI tick.
 *
 * @param[in,out] pill    Pill instance.
 * @param[in]     now_ms  Monotonic milliseconds.
 */
void status_pill_blink_tick(status_pill_t *pill, uint32_t now_ms);

#endif /* STATUS_PILL_H */
