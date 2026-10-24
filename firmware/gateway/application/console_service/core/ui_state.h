/**
 * @file ui_state.h
 * @brief Screen / focus state machine and console line editing.
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §5.2, §11.2
 */

#ifndef UI_STATE_H
#define UI_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "input.h"

#define UI_MENU_COUNT (7U)
#define UI_INPUT_MAX (120U) /* buffer size including the terminator */
#define UI_SCROLL_STEP (4U) /* lines per PgUp / PgDn */
#define UI_SCROLL_STEP (4U) /* lines per PgUp / PgDn */

typedef enum
{
    SCR_GRAPH,
    SCR_STATUS,
    SCR_ALARMS,
    SCR_CONFIG,
    SCR_NETWORK,
    SCR_CONSOLE,
    SCR_FIRMWARE,
} screen_t;

typedef enum
{
    FOCUS_MENU,
    FOCUS_PANEL,
} focus_t;

typedef struct
{
    uint8_t sel;              /**< Selected menu index 0..6 (maps to screen_t).    */
    focus_t focus;            /**< MENU or PANEL.                                  */
    uint8_t channel;          /**< Graph channel: 0 temp, 1 humid, 2 press.        */
    char input[UI_INPUT_MAX]; /**< REPL input buffer.                            */
    uint8_t input_len;
    bool cursor_on;      /**< Blink phase.                                    */
    bool confirm_active; /**< A y/N prompt replaces the REPL input line.      */
    uint16_t scroll;     /**< Lines the console view is scrolled back (0 = live). */
} ui_state_t;

/** @brief What the caller must do after a key was handled. */
typedef enum
{
    UI_ACT_NONE,
    UI_ACT_RUN_LINE,      /**< Enter on the console: run ui->input, then clear it.   */
    UI_ACT_LINE_OVERFLOW, /**< The input line was full; it was cleared.              */
    UI_ACT_CONFIRM_YES,   /**< Confirm prompt answered y / Y.                        */
    UI_ACT_CONFIRM_NO,    /**< Confirm prompt declined (any other key).              */
} ui_action_t;

/** @brief Reset to the start state: first item selected, menu focus, temperature channel. */
void ui_init(ui_state_t *ui);

/** @brief The screen shown for the current selection. */
screen_t ui_screen(const ui_state_t *ui);

/** @brief Apply one key. Terminal size reports are not handled here. */
ui_action_t ui_handle_key(ui_state_t *ui, const key_event_t *key);

/** @brief Empty the input line. */
void ui_clear_input(ui_state_t *ui);

#endif /* UI_STATE_H */
