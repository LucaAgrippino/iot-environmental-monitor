/**
 * @file editor.h
 * @brief The command line being typed: a small line editor.
 *
 * Turns key events into edits of one line and tells the caller what to do next. It draws
 * nothing; the caller redraws the prompt row when told the line changed.
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §11.2
 */

#ifndef EDITOR_H
#define EDITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "input.h"

#define EDITOR_LINE_MAX (120U) /* buffer size including the terminator */

typedef struct
{
    char line[EDITOR_LINE_MAX]; /**< The line being typed.                  */
    char last[EDITOR_LINE_MAX]; /**< The last line that was run (Up key).   */
    uint8_t len;
} editor_t;

/** @brief What the caller must do after a key was handled. */
typedef enum
{
    ED_NONE,          /**< Nothing to do.                                              */
    ED_CHANGED,       /**< The line changed: redraw the prompt.                        */
    ED_RUN_LINE,      /**< Enter: run ed->line, then call editor_clear().              */
    ED_LINE_OVERFLOW, /**< The line was full; it was cleared. Say so and redraw.       */
    ED_REDRAW,        /**< Ctrl-L: repaint the whole screen.                           */
    ED_CONFIRM_YES,   /**< Answer to a y/N prompt: yes.                                */
    ED_CONFIRM_NO,    /**< Answer to a y/N prompt: anything but y declines.            */
} editor_action_t;

/** @brief Empty the line and forget the history. */
void editor_init(editor_t *ed);

/** @brief Empty the line (history stays). */
void editor_clear(editor_t *ed);

/**
 * @brief Apply one key.
 *
 * @param[in] confirm  A y/N prompt is waiting: y / Y answers yes, Enter, Escape or any other
 *                     character answers no, and nothing is typed into the line.
 */
editor_action_t editor_key(editor_t *ed, const key_event_t *key, bool confirm);

#endif /* EDITOR_H */
