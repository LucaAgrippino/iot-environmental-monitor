/**
 * @file screen.h
 * @brief The three-panel console screen, drawn with a terminal scroll region and nothing else.
 *
 * @code
 *  row 1..2     NORTH    header: two status lines (drawn once, refreshed in place)
 *  row 3        rule
 *  row 4..N     CENTRE   log lines; the terminal scrolls this region, the firmware keeps no copy
 *  row D        divider
 *  row D+1..R-1 SOUTH    what the typed commands answered; scrolls the same way
 *  row R        prompt   the line being typed
 * @endcode
 *
 * There is no frame buffer. A log line is "go to the last log row, scroll, print, go back to the
 * prompt" and the terminal does the rest, so the module needs only one scratch buffer. Every
 * entry point takes the lock supplied at init, so the Logger task, the console task and a
 * command cannot interleave their escape sequences.
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §10
 */

#ifndef SCREEN_H
#define SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCREEN_ROWS_DEFAULT (36U)
#define SCREEN_COLS_DEFAULT (100U)
#define SCREEN_ROWS_MIN (20U)
#define SCREEN_ROWS_MAX (80U)
#define SCREEN_COLS_MIN (60U)
#define SCREEN_COLS_MAX (160U)
/** Rows of the south (command result) panel. */
#define SCREEN_RESULT_ROWS (10U)
/** Rows of the header, including its rule. */
#define SCREEN_HEADER_ROWS (3U)

/** @brief What the screen needs from the platform. */
typedef struct
{
    /** Write bytes to the terminal; false on failure. Required. */
    bool (*send)(const uint8_t *data, size_t len);
    /** Take / release the output lock. Optional (host tests); must tolerate being called from
     * two tasks. */
    void (*lock)(void);
    void (*unlock)(void);
} screen_port_t;

/** @brief Store the platform hooks and reset to the default size. Does not draw. */
void screen_init(const screen_port_t *port);

/**
 * @brief Set the terminal size the layout is computed for; values are clamped to the limits.
 * @note Takes effect at the next screen_open().
 */
void screen_set_size(uint16_t rows, uint16_t cols);

/** @brief Current layout size. */
void screen_size(uint16_t *rows, uint16_t *cols);

/** @brief Clear the terminal and draw the empty layout (rule, divider, prompt row). */
void screen_open(void);

/** @brief Give the terminal back: full-screen scrolling again, cursor below everything. */
void screen_close(void);

/** @brief Whether screen_open() has run (log lines are written plain before that). */
bool screen_is_open(void);

/**
 * @brief Replace the two header lines. Text may hold SGR colour sequences; it is clipped to the
 *        width. NULL leaves a line unchanged.
 */
void screen_header(const char *line1, const char *line2);

/**
 * @brief Append one log line to the centre panel.
 *
 * Trailing CR / LF are dropped, other control bytes skipped, SGR sequences kept and not counted
 * as width, and the visible text is cut to the terminal width. Before screen_open() the line is
 * sent as it is. Safe to call from any task.
 */
void screen_log(const char *line, size_t len);

/** @brief Append one line to the south panel in the colour of palette token @p pal. */
void screen_result(uint8_t pal, const char *text);

/** @brief Empty the south panel (the `clear` command). */
void screen_results_clear(void);

/**
 * @brief Redraw the prompt row: @p label in the prompt colour, then @p text, with the cursor left
 *        at its end. Long text shows its tail.
 */
void screen_prompt(const char *label, const char *text);

#endif /* SCREEN_H */
