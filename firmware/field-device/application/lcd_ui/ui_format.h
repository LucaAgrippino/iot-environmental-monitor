#ifndef UI_FORMAT_H
#define UI_FORMAT_H

/**
 * @file ui_format.h
 * @brief Text formatting for the LCD UI: fixed-point values, deltas, clock and uptime.
 *
 * Pure functions, no LVGL and no floating point (architecture principle P9, overlay section 2):
 * sensor values are fixed-point integers (temperature and humidity in 0.01 units, pressure in
 * 0.1 hPa) and are formatted with integer arithmetic only. The minus sign is the true minus
 * U+2212 (02_FONTS_AND_ICONS.md "Inline glyphs").
 */

#include <stdbool.h>
#include <stdint.h>

/** Channels shown on the Sensors and Trend screens (03_SCREEN_SPECS.md: S1, S2, S3). */
typedef enum
{
    UI_CH_TEMPERATURE = 0, /**< 0.01 degC per unit, shown with one decimal, e.g. "23.4". */
    UI_CH_HUMIDITY = 1,    /**< 0.01 %RH per unit, shown with one decimal, e.g. "58.2". */
    UI_CH_PRESSURE = 2,    /**< 0.1 hPa per unit, shown as an integer, e.g. "1013". */
    UI_CH_COUNT = 3
} ui_channel_t;

/** True minus sign U+2212 as UTF-8. */
#define UI_MINUS "\xE2\x88\x92"

/**
 * @brief Format a raw sensor value for the hero number of a card.
 *
 * @param[out] out      Destination buffer.
 * @param[in]  out_len  Size of out in bytes (at least 8 recommended).
 * @param[in]  ch       Channel, selects the scale.
 * @param[in]  raw      Fixed-point value as delivered by SensorService.
 */
void ui_fmt_value(char *out, uint32_t out_len, ui_channel_t ch, int32_t raw);

/**
 * @brief Format the change over a window, e.g. "+0.3 / 5m", "[minus]0.4 / 5m" or "+0 / 1m".
 *
 * @param[out] out      Destination buffer (at least 16 bytes recommended).
 * @param[in]  out_len  Size of out.
 * @param[in]  ch       Channel, selects the scale.
 * @param[in]  delta    Difference in the raw fixed-point unit.
 * @param[in]  minutes  Length of the window the difference covers (1..5).
 */
void ui_fmt_delta(char *out, uint32_t out_len, ui_channel_t ch, int32_t delta, uint32_t minutes);

/**
 * @brief Format seconds as "HH:MM:SS" (the time of day for an epoch, or an uptime below 24 h).
 *
 * @param[out] out      Destination buffer (at least 9 bytes).
 * @param[in]  out_len  Size of out.
 * @param[in]  seconds  Seconds; reduced modulo one day.
 */
void ui_fmt_clock(char *out, uint32_t out_len, uint32_t seconds);

/**
 * @brief Format an uptime as "3d 14:22" (days, hours, minutes) or "14:22:05" below one day.
 *
 * @param[out] out      Destination buffer (at least 12 bytes).
 * @param[in]  out_len  Size of out.
 * @param[in]  seconds  Uptime in seconds.
 */
void ui_fmt_uptime(char *out, uint32_t out_len, uint32_t seconds);

/**
 * @brief Format a fixed-point number with a given count of decimals, e.g. (-500, 100, 1) is "-5.0".
 *
 * @param[out] out       Destination buffer.
 * @param[in]  out_len   Size of out.
 * @param[in]  raw       Fixed-point value.
 * @param[in]  divisor   Units per whole number (10, 100 or 1000).
 * @param[in]  decimals  Decimals to show (0..3); the value is rounded half away from zero.
 */
void ui_fmt_fixed(char *out, uint32_t out_len, int32_t raw, uint32_t divisor, uint32_t decimals);

/**
 * @brief Format an elapsed time as "just now", "5m ago", "3h 12m ago" or "2d 4h ago".
 *
 * @param[out] out      Destination buffer (at least 16 bytes).
 * @param[in]  out_len  Size of out.
 * @param[in]  seconds  Elapsed seconds.
 */
void ui_fmt_ago(char *out, uint32_t out_len, uint32_t seconds);

/**
 * @brief Parse a keypad entry such as "30.0", "-5", "7." or ".5" into tenths (30.0 is 300).
 *
 * At most one decimal digit is accepted, so the result is exact for every field of the Config
 * screen.
 *
 * @param[in]  text    Entry text.
 * @param[out] tenths  Value times ten.
 * @return true when the text is a complete number that fits in an int32_t.
 */
bool ui_parse_tenths(const char *text, int32_t *tenths);

#endif /* UI_FORMAT_H */
