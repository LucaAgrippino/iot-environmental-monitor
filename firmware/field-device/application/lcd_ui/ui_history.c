/**
 * @file ui_history.c
 * @brief Five-minute sample history (see ui_history.h).
 */

#include "lcd_ui/ui_history.h"

#include <stddef.h>

#define MS_PER_MINUTE (60000U) /**< 60 s * 1000 */

static int32_t s_value[UI_CH_COUNT][UI_HISTORY_LEN];
static bool s_valid[UI_CH_COUNT][UI_HISTORY_LEN];
static uint32_t s_stamp[UI_CH_COUNT][UI_HISTORY_LEN];
static uint32_t s_head;  /* index of the next write */
static uint32_t s_count; /* samples held */
static uint32_t s_last_ms;
static bool s_have_last;

void ui_history_reset(void)
{
    for (uint32_t c = 0U; c < (uint32_t) UI_CH_COUNT; c++)
    {
        for (uint32_t i = 0U; i < UI_HISTORY_LEN; i++)
        {
            s_value[c][i] = 0;
            s_valid[c][i] = false;
            s_stamp[c][i] = 0U;
        }
    }
    s_head = 0U;
    s_count = 0U;
    s_last_ms = 0U;
    s_have_last = false;
}

void ui_history_push(const int32_t value[UI_CH_COUNT], const bool valid[UI_CH_COUNT],
                     const uint32_t stamp[UI_CH_COUNT])
{
    for (uint32_t c = 0U; c < (uint32_t) UI_CH_COUNT; c++)
    {
        s_value[c][s_head] = value[c];
        s_valid[c][s_head] = valid[c];
        s_stamp[c][s_head] = stamp[c];
    }
    s_head = (s_head + 1U) % UI_HISTORY_LEN;
    if (s_count < UI_HISTORY_LEN)
    {
        s_count++;
    }
}

bool ui_history_tick(uint32_t now_ms, const int32_t value[UI_CH_COUNT],
                     const bool valid[UI_CH_COUNT], const uint32_t stamp[UI_CH_COUNT])
{
    if (s_have_last && ((uint32_t) (now_ms - s_last_ms) < UI_HISTORY_PERIOD_MS))
    {
        return false;
    }
    s_last_ms = now_ms;
    s_have_last = true;
    ui_history_push(value, valid, stamp);
    return true;
}

bool ui_history_get_time(ui_channel_t ch, uint32_t index, uint32_t *stamp)
{
    if (((uint32_t) ch >= (uint32_t) UI_CH_COUNT) || (index >= s_count) || (stamp == NULL))
    {
        return false;
    }
    *stamp = s_stamp[ch][(s_head + UI_HISTORY_LEN - s_count + index) % UI_HISTORY_LEN];
    return true;
}

uint32_t ui_history_count(void)
{
    return s_count;
}

/** Ring index of sample number n counted from the oldest. */
static uint32_t slot_of(uint32_t n)
{
    return (s_head + UI_HISTORY_LEN - s_count + n) % UI_HISTORY_LEN;
}

bool ui_history_get(ui_channel_t ch, uint32_t index, int32_t *value)
{
    if (((uint32_t) ch >= (uint32_t) UI_CH_COUNT) || (index >= s_count) || (value == NULL))
    {
        return false;
    }
    const uint32_t slot = slot_of(index);
    if (!s_valid[ch][slot])
    {
        return false;
    }
    *value = s_value[ch][slot];
    return true;
}

bool ui_history_delta(ui_channel_t ch, int32_t *delta, uint32_t *minutes)
{
    if ((delta == NULL) || (minutes == NULL))
    {
        return false;
    }
    bool have_first = false;
    bool have_last = false;
    int32_t first = 0;
    int32_t last = 0;
    uint32_t first_idx = 0U;
    uint32_t last_idx = 0U;

    for (uint32_t i = 0U; i < s_count; i++)
    {
        int32_t v;
        if (ui_history_get(ch, i, &v))
        {
            if (!have_first)
            {
                have_first = true;
                first = v;
                first_idx = i;
            }
            last = v;
            last_idx = i;
            have_last = true;
        }
    }
    if (!have_first || !have_last || (last_idx == first_idx))
    {
        return false;
    }

    *delta = last - first;
    const uint32_t span_ms = (last_idx - first_idx) * UI_HISTORY_PERIOD_MS;
    uint32_t m = (span_ms + (MS_PER_MINUTE / 2U)) / MS_PER_MINUTE;
    if (m < 1U)
    {
        m = 1U;
    }
    *minutes = m;
    return true;
}
