/**
 * @file theme.h
 * @brief Colour tokens for console output and the serial speed.
 *
 * Output is plain ANSI: each token maps to one of the 16 standard SGR colours, so any terminal
 * shows it. Command code asks for a token (PAL_OK, PAL_ERR ...) and never for an escape code.
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §7
 */

#ifndef THEME_H
#define THEME_H

#include <stdint.h>

/** @brief Colour tokens. */
typedef enum
{
    PAL_INK = 0, /**< Normal text (terminal default).   */
    PAL_MUTED,   /**< Secondary text.                   */
    PAL_OK,      /**< Healthy / applied.                */
    PAL_WARN,    /**< Needs attention.                  */
    PAL_ERR,     /**< Failed / alarm.                   */
    PAL_NUM,     /**< Numbers, identifiers.             */
    PAL_PROMPT,  /**< The prompt.                       */
    PAL_TOKEN_COUNT,
} palette_token_t;

/**
 * @brief SGR sequence that selects a token's colour ("\x1b[0;32m" style).
 * @param[in] token  A palette_token_t; unknown values give PAL_INK.
 * @return A string literal, never NULL.
 */
const char *theme_sgr(uint8_t token);

/** Resets every attribute. */
#define THEME_RESET "\x1b[0m"

/** Serial speed of the console (8N1). The terminal must be set to the same rate. */
#define CONSOLE_LINK_BAUD (460800U)

#endif /* THEME_H */
