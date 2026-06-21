/**
 * @file status_pill.h
 * @brief System-status pill widget shown in the header bar.
 *
 * Displays a coloured label badge with one of four states:
 *   PILL_RUNNING — COL_OK  "RUNNING"
 *   PILL_INIT    — COL_DIM "INIT"
 *   PILL_ALARM   — COL_ERR "ALARM" (blinks via lv_anim_path_step)
 *   PILL_UPDATE  — COL_WARN "UPDATE"
 *
 * @see docs/lcd-ui-design/_reference-v9-claude-design/03_SCREEN_SPECS.md §Status pill
 */

#ifndef STATUS_PILL_H
#define STATUS_PILL_H

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

/* ===================================================================== */
/* State enum                                                            */
/* ===================================================================== */

typedef enum
{
    PILL_RUNNING = 0, /**< Normal operation — COL_OK   */
    PILL_INIT    = 1, /**< Startup / not ready — COL_DIM */
    PILL_ALARM   = 2, /**< Active alarm — COL_ERR, blinks */
    PILL_UPDATE  = 3, /**< Firmware update — COL_WARN  */
	PILL_STATE_MAX,
} pill_state_t;

/* ===================================================================== */
/* Public API                                                            */
/* ===================================================================== */

/**
 * @brief Create the status pill.
 *
 * @param parent  Parent object (normally the header bar container).
 * @param state   Initial pill state.
 * @return Pointer to the created pill lv_obj_t.
 */
lv_obj_t *status_pill_create(lv_obj_t *parent, pill_state_t state);

/**
 * @brief Transition the pill to a new state.
 *
 * Stops any running blink animation before updating colour and text.
 *
 * @param pill_obj  Object returned by status_pill_create().
 * @param state     New state.
 */
void status_pill_set_state(lv_obj_t *pill_obj, pill_state_t state);

#endif /* STATUS_PILL_H */
