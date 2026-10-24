/**
 * @file fmt.h
 * @brief Fixed-point text helpers for the console (no floating point).
 *
 * Sensor values arrive as integers in fixed scales (SensorService / ConfigService units).
 * These helpers print them as decimals and parse operator input back into those units.
 */

#ifndef FMT_H
#define FMT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Print @p value / @p scale with @p decimals decimal places, rounding half away from zero.
 *
 * @param[out] buf       Destination.
 * @param[in]  size      Size of @p buf.
 * @param[in]  value     Fixed-point value.
 * @param[in]  scale     Units per 1.0: 1, 10 or 100.
 * @param[in]  decimals  0, 1 or 2; must not exceed the digits of @p scale.
 * @return Characters written (excluding the terminator).
 */
size_t fmt_fixed(char *buf, size_t size, int32_t value, int32_t scale, uint8_t decimals);

/**
 * @brief Parse a decimal such as "24", "-3.5" or "1013.2" into fixed point.
 *
 * More decimal places than the scale can hold are rejected rather than rounded.
 *
 * @param[in]  text   NUL-terminated input.
 * @param[in]  scale  Units per 1.0: 1, 10 or 100.
 * @param[out] out    Receives value * scale.
 * @return true if @p text is a well-formed number that fits an int32_t.
 */
bool fmt_parse_fixed(const char *text, int32_t scale, int32_t *out);

/**
 * @brief Parse an unsigned decimal integer (digits only).
 * @return true if @p text is 1..9 digits.
 */
bool fmt_parse_u32(const char *text, uint32_t *out);

/**
 * @brief Copy @p text into @p dst padded with spaces to @p width columns.
 *
 * Width is counted in UTF-8 code points, not bytes, so strings holding "●" or "°" align.
 * Longer text is copied whole (no truncation to the width).
 *
 * @return Bytes written (excluding the terminator).
 */
size_t fmt_pad(char *dst, size_t size, const char *text, size_t width);

/** @brief Number of UTF-8 code points in @p text. */
size_t fmt_cols(const char *text);

#endif /* FMT_H */
