/**
 * @file chart.c
 * @brief Block-element area chart with proximity colouring.
 */

#include "chart.h"

#define Q8 (256)
#define PAD_PERCENT (12)
#define NEAR_NUM (3) /* near from 0.6 = 3/5 */
#define NEAR_DEN (5)
#define LEVELS_PER_ROW (8)

void chart_history_reset(chart_history_t *h)
{
    for (uint32_t c = 0U; c < CHART_CHANNELS; c++)
    {
        for (uint32_t i = 0U; i < CHART_HISTORY; i++)
        {
            h->samples[c][i] = 0;
        }
    }
    h->head = 0U;
    h->count = 0U;
}

void chart_history_push(chart_history_t *h, const int16_t values[CHART_CHANNELS])
{
    for (uint32_t c = 0U; c < CHART_CHANNELS; c++)
    {
        h->samples[c][h->head] = values[c];
    }
    h->head = (uint8_t) ((h->head + 1U) % CHART_HISTORY);
    if (h->count < CHART_HISTORY)
    {
        h->count++;
    }
}

/* Sample i of a channel, 0 = oldest. */
static int32_t prv_sample(const chart_history_t *h, uint8_t channel, uint32_t i)
{
    const uint32_t oldest = (h->head + CHART_HISTORY - h->count) % CHART_HISTORY;

    return h->samples[channel][(oldest + i) % CHART_HISTORY];
}

void chart_resample(const chart_history_t *h, uint8_t channel, int32_t out[CHART_COLS])
{
    const uint32_t n = h->count;

    if (n == 0U)
    {
        for (uint32_t c = 0U; c < CHART_COLS; c++)
        {
            out[c] = 0;
        }
        return;
    }
    for (uint32_t c = 0U; c < CHART_COLS; c++)
    {
        if (n == 1U)
        {
            out[c] = prv_sample(h, channel, 0U);
            continue;
        }
        const uint32_t pos_q8 = (c * (n - 1U) * (uint32_t) Q8) / (CHART_COLS - 1U);
        const uint32_t i0 = pos_q8 / (uint32_t) Q8;
        const int32_t frac = (int32_t) (pos_q8 % (uint32_t) Q8);
        const uint32_t i1 = (i0 + 1U < n) ? (i0 + 1U) : (n - 1U);
        const int32_t a = prv_sample(h, channel, i0);
        const int32_t b = prv_sample(h, channel, i1);

        out[c] = a + (((b - a) * frac) / Q8);
    }
}

int32_t chart_proximity_q8(int32_t v, const chart_limits_t *limits)
{
    int32_t t = 0;

    if (limits->has_hi)
    {
        const int32_t x =
            (int32_t) (((int64_t) (v - (limits->hi - limits->band)) * Q8) / limits->band);

        t = (x > t) ? x : t;
    }
    if (limits->has_lo)
    {
        const int32_t x =
            (int32_t) (((int64_t) ((limits->lo + limits->band) - v) * Q8) / limits->band);

        t = (x > t) ? x : t;
    }
    return t;
}

chart_status_t chart_status(int32_t t_q8, bool stale)
{
    if (stale)
    {
        return CHART_ST_STALE;
    }
    if (t_q8 >= PROX_ONE)
    {
        return CHART_ST_ALARM;
    }
    if ((t_q8 * NEAR_DEN) >= (PROX_ONE * NEAR_NUM))
    {
        return CHART_ST_NEAR;
    }
    return CHART_ST_OK;
}

static int32_t prv_clamp(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

chart_range_t chart_render(cell_grid_t *grid, uint8_t row0, uint8_t col0,
                           const int32_t data[CHART_COLS], const chart_limits_t *limits)
{
    int32_t lo = data[0];
    int32_t hi = data[0];

    for (uint32_t c = 1U; c < CHART_COLS; c++)
    {
        lo = (data[c] < lo) ? data[c] : lo;
        hi = (data[c] > hi) ? data[c] : hi;
    }

    /* The guide follows whichever limit the latest sample is closer to, and only that limit
     * widens the y-range: pulling in a far limit would squash the data into a flat block. */
    bool has_thr = false;
    int32_t thr = 0;

    if (limits->has_hi || limits->has_lo)
    {
        const int32_t last = data[CHART_COLS - 1U];

        has_thr = true;
        if (limits->has_hi && limits->has_lo)
        {
            const int32_t d_hi = limits->hi - last;
            const int32_t d_lo = last - limits->lo;

            thr = (d_hi <= d_lo) ? limits->hi : limits->lo;
        }
        else
        {
            thr = limits->has_hi ? limits->hi : limits->lo;
        }
        hi = (thr > hi) ? thr : hi;
        lo = (thr < lo) ? thr : lo;
    }
    int32_t pad = ((hi - lo) * PAD_PERCENT) / 100;

    pad = (pad < limits->scale) ? limits->scale : pad;
    lo -= pad;
    hi += pad;
    const int32_t rng = hi - lo;

    const int32_t thr_row =
        has_thr ? (int32_t) (((int64_t) (thr - lo) * (int32_t) CHART_ROWS) / rng) : -1;

    int32_t level[CHART_COLS];
    uint8_t colour[CHART_COLS];
    bool bold[CHART_COLS];

    for (uint32_t c = 0U; c < CHART_COLS; c++)
    {
        const int64_t num = (int64_t) (data[c] - lo) * (int32_t) (CHART_ROWS * LEVELS_PER_ROW);

        level[c] = (int32_t) (((num * 2) + rng) / (2 * (int64_t) rng));
        const int32_t t = chart_proximity_q8(data[c], limits);

        colour[c] = theme_prox_index(t);
        bold[c] = theme_prox_bold(t);
    }

    for (uint32_t r = 0U; r < CHART_ROWS; r++)
    {
        const int32_t from_bottom = (int32_t) (CHART_ROWS - 1U - r);
        const int32_t base = from_bottom * LEVELS_PER_ROW;
        const bool on_thr_row = has_thr && (thr_row == from_bottom);

        for (uint32_t c = 0U; c < CHART_COLS; c++)
        {
            const int32_t idx = prv_clamp(level[c] - base, 0, CHART_BLK_LEVELS);
            const uint8_t row = (uint8_t) (row0 + r);
            const uint8_t col = (uint8_t) (col0 + c);

            if (idx == 0)
            {
                if (on_thr_row)
                {
                    grid_put_glyph(grid, row, col, (uint8_t) GLYPH_DASH, PAL_THR, PAL_BG, false);
                }
                else
                {
                    grid_put_glyph(grid, row, col, 0U, PAL_BG, PAL_BG, false);
                }
            }
            else
            {
                grid_put_glyph(grid, row, col, (uint8_t) (GLYPH_BLK1 + (uint32_t) idx - 1U),
                               colour[c], PAL_BG, bold[c]);
            }
        }
    }

    chart_range_t range = {lo, hi};

    return range;
}
