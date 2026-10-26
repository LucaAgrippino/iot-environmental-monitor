/**
 * @file repl.h
 * @brief REPL scrollback ring and command-line dispatcher.
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §5.7, §9.2
 */

#ifndef REPL_H
#define REPL_H

#include <stdint.h>

#include "console_service/console_types.h"

#define REPL_TEXT_MAX (64U) /* UTF-8 bytes of one line, including the terminator */
#define REPL_SCROLLBACK_MAX (200U)
#define REPL_VISIBLE_LINES (8U) /* tail shown above the live input line */

/** @brief One line of REPL output (one colour per line, LLD §5.7). */
typedef struct
{
    char text[REPL_TEXT_MAX];
    uint8_t fg; /**< Palette index. */
} scrollback_line_t;

/** @brief Empty the scrollback and print the greeting. */
void repl_init(void);

/** @brief Empty the scrollback (the `clear` command). */
void repl_clear(void);

/** @brief Append a line; text longer than the line buffer is cut at a character boundary. */
void repl_puts(uint8_t fg, const char *text);

/** @brief Number of lines held (0..REPL_SCROLLBACK_MAX). */
uint16_t repl_count(void);

/** @brief Line @p i counted from the oldest (0) to the newest (count - 1); NULL if out of range. */
const scrollback_line_t *repl_line(uint16_t i);

/**
 * @brief Echo and run one command line.
 *
 * Empty input just echoes a bare prompt. The echoed line, the handler output and any error line
 * all go to the scrollback.
 *
 * @param[in] line  NUL-terminated input, already trimmed of the newline.
 * @return The handler's result; CONSOLE_SERVICE_ERR_UNKNOWN_KEY for an unknown command.
 */
console_service_err_t repl_execute(const char *line);

#endif /* REPL_H */
