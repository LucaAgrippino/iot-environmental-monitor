/**
 * @file repl.h
 * @brief command output and command-line dispatcher.
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §5.7, §9.2
 */

#ifndef REPL_H
#define REPL_H

#include <stdint.h>

#include "console_service/console_types.h"

#define REPL_TEXT_MAX (64U) /* UTF-8 bytes of one line, including the terminator */

/** @brief Print the greeting into the south panel. */
void repl_init(void);

/** @brief Empty the south panel (the `clear` command). */
void repl_clear(void);

/** @brief Print one line in the south panel; text longer than the buffer is cut at a character. */
void repl_puts(uint8_t fg, const char *text);

/**
 * @brief Echo and run one command line.
 *
 * Empty input just echoes a bare prompt. The echoed line, the handler output and any error line
 * all go to the south panel.
 *
 * @param[in] line  NUL-terminated input, already trimmed of the newline.
 * @return The handler's result; CONSOLE_SERVICE_ERR_UNKNOWN_KEY for an unknown command.
 */
console_service_err_t repl_execute(const char *line);

#ifdef TEST
#define REPL_SCROLLBACK_MAX (200U)

/** @brief One printed line, kept for tests only (the firmware keeps no copy). */
typedef struct
{
    char text[REPL_TEXT_MAX];
    uint8_t fg;
} scrollback_line_t;

/** @brief Lines printed since the last clear (unit tests only). */
uint16_t repl_count(void);

/** @brief Line @p i from the oldest (0); NULL if out of range (unit tests only). */
const scrollback_line_t *repl_line(uint16_t i);
#endif /* TEST */

#endif /* REPL_H */
