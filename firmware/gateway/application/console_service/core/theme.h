/**
 * @file theme.h
 * @brief Console colour palette and the proximity ("fuzzy") gradient.
 *
 * Colours are palette indices, not RGB triples, so a cell needs one byte per colour
 * (GW-CS-O1). Indices below PAL_GRAD_FIRST are the fixed design tokens; the next
 * PAL_GRAD_STEPS + 1 entries are the cyan -> amber -> red gradient the chart uses.
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §7.2, §8.3
 */

#ifndef THEME_H
#define THEME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Fixed design tokens (design handoff 01_LAYOUT_AND_TOKENS.md). */
typedef enum
{
    PAL_BG = 0, /**< Terminal default; never emitted as a background. */
    PAL_BORDER,
    PAL_TITLE,
    PAL_TITLE_ACT,
    PAL_LOGO,
    PAL_MUTED,
    PAL_ACCENT,
    PAL_OK,
    PAL_WARN,
    PAL_ERR,
    PAL_NUM,
    PAL_NUM_SEL,
    PAL_INK,
    PAL_AXIS,
    PAL_THR,
    PAL_SEL_BG,
    PAL_SEL_FG,
    PAL_PROMPT,
    PAL_TOKEN_COUNT,
} palette_token_t;

/** Steps in each half of the gradient (BASE -> WARN, WARN -> OVER). */
#define PAL_GRAD_HALF (12U)
/** Index of the first gradient entry (BASE colour). */
#define PAL_GRAD_FIRST ((uint8_t) PAL_TOKEN_COUNT)
/** Number of gradient steps above BASE. */
#define PAL_GRAD_STEPS (2U * PAL_GRAD_HALF)
/** Total palette size. */
#define PAL_COUNT ((uint8_t) (PAL_GRAD_FIRST + PAL_GRAD_STEPS + 1U))

/** Proximity is a Q8 fixed-point value: 256 means "exactly at the threshold". */
#define PROX_ONE (256)

/**
 * @brief Gradient palette index for a proximity value.
 *
 * t <= 0 gives BASE; 0 < t <= 1 blends BASE to WARN; t > 1 blends WARN to OVER, reaching OVER
 * at t = 1.6.
 *
 * @param[in] t_q8  Proximity in Q8 (PROX_ONE = at threshold).
 * @return Palette index in [PAL_GRAD_FIRST, PAL_COUNT - 1].
 */
uint8_t theme_prox_index(int32_t t_q8);

/**
 * @brief Whether a proximity value earns the bold "glow" substitute (glow > 0.5).
 * @param[in] t_q8  Proximity in Q8.
 */
bool theme_prox_bold(int32_t t_q8);

/**
 * @brief RGB of a palette entry.
 *
 * Gradient entries include the brightness nudge that approximates the glow.
 *
 * @param[in]  idx  Palette index; out-of-range values return the INK colour.
 * @param[out] r    Red.
 * @param[out] g    Green.
 * @param[out] b    Blue.
 */
void theme_rgb(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b);

/**
 * @brief Write the SGR sequence that selects the given attributes.
 *
 * Always starts from a reset (ESC[0) so a run never inherits stale attributes. The
 * background is omitted for PAL_BG. With CONSOLE_256COLOR defined, colours are mapped to
 * the xterm-256 cube instead of truecolor.
 *
 * @param[out] buf   Destination, at least THEME_SGR_MAX bytes.
 * @param[in]  fg    Foreground palette index.
 * @param[in]  bg    Background palette index (PAL_BG = none).
 * @param[in]  bold  Bold attribute.
 * @return Number of bytes written (no terminator).
 */
size_t theme_sgr(char *buf, uint8_t fg, uint8_t bg, bool bold);

/** Longest sequence theme_sgr() writes. */
#define THEME_SGR_MAX (48U)

/** Serial speed of the console (8N1). The terminal must be set to the same rate. */
#define CONSOLE_LINK_BAUD (460800U)

#endif /* THEME_H */
