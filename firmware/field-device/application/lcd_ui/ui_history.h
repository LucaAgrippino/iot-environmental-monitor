#ifndef UI_HISTORY_H
#define UI_HISTORY_H

/**
 * @file ui_history.h
 * @brief Five-minute sample history for the sparklines, deltas and the Trend screen.
 *
 * REQ-SA-180..240 ask for a rolling history sampled every 5 s over a 5 minute window, independent
 * of the polling rate. SensorService has no history API on the Field Device, so LcdUi keeps its own
 * ring buffer: 60 samples per channel, static storage, no heap (REQ-NF-408). A sample whose
 * reading was invalid is stored as invalid so the Trend screen can draw a gap (REQ-LD-190).
 */

#include <stdbool.h>
#include <stdint.h>

#include "lcd_ui/ui_format.h"

#define UI_HISTORY_PERIOD_MS (5000U) /* REQ-SA-200: fixed 5 s sampling interval */
#define UI_HISTORY_LEN (60U)         /* REQ-SA-190: 5 min window / 5 s = 60 samples */

/** Forget all samples. */
void ui_history_reset(void);

/**
 * @brief Append one sample for every channel (one call per sampling instant).
 *
 * @param[in] value  Raw fixed-point value per channel.
 * @param[in] valid  Whether each channel's reading was valid.
 */
void ui_history_push(const int32_t value[UI_CH_COUNT], const bool valid[UI_CH_COUNT]);

/**
 * @brief Call from the UI tick with a millisecond clock; pushes a sample when the period elapsed.
 *
 * @param[in] now_ms  Monotonic milliseconds.
 * @param[in] value   Current raw value per channel.
 * @param[in] valid   Current validity per channel.
 * @return true when a sample was stored.
 */
bool ui_history_tick(uint32_t now_ms, const int32_t value[UI_CH_COUNT],
                     const bool valid[UI_CH_COUNT]);

/** @return Number of samples held (0..UI_HISTORY_LEN). */
uint32_t ui_history_count(void);

/**
 * @brief Read one stored sample.
 *
 * @param[in]  ch     Channel.
 * @param[in]  index  0 is the oldest sample, ui_history_count() - 1 the newest.
 * @param[out] value  Raw value when the sample was valid.
 * @return true when the sample exists and was valid.
 */
bool ui_history_get(ui_channel_t ch, uint32_t index, int32_t *value);

/**
 * @brief Change of a channel over the stored window (newest minus oldest valid sample).
 *
 * @param[in]  ch       Channel.
 * @param[out] delta    Newest minus oldest valid value, raw units.
 * @param[out] minutes  Window length covered, 1..5 (rounded to the nearest minute, at least 1).
 * @return true when at least two valid samples exist.
 */
bool ui_history_delta(ui_channel_t ch, int32_t *delta, uint32_t *minutes);

#endif /* UI_HISTORY_H */
