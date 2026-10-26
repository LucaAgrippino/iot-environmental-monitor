/**
 * @file chart.h
 * @brief Block-element area chart with proximity ("fuzzy") colouring.
 *
 * A 46-column by 6-row chart of the last ~92 samples. Each column is coloured from how close
 * that sample is to an alarm threshold: BASE cyan, through WARN amber, to OVER red, with bold as
 * the terminal substitute for the browser's glow. All arithmetic is integer: sensor values are
 * already fixed point, proximity is Q8.
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §8
 */

#ifndef CHART_H
#define CHART_H

#include <stdbool.h>
#include <stdint.h>

#include "termout.h"

#define CHART_COLS (46U)
#define CHART_ROWS (6U)
#define CHART_HISTORY (92U) /* two samples per column */
#define CHART_CHANNELS (3U) /* temperature, humidity, pressure */

/** Block characters, index 0 = space .. 8 = full block (glyph table: GLYPH_BLK1 + idx - 1). */
#define CHART_BLK_LEVELS (8)

/** Sample history for the three chart channels (a ring of CHART_HISTORY samples each). */
typedef struct
{
    int16_t samples[CHART_CHANNELS][CHART_HISTORY];
    uint8_t head;  /**< Next write position.        */
    uint8_t count; /**< Valid samples, 0..CHART_HISTORY. */
} chart_history_t;

/** @brief Alarm limits of one channel, in the channel's fixed-point units. */
typedef struct
{
    bool has_hi;
    bool has_lo;
    int32_t hi;
    int32_t lo;
    int32_t band;  /**< Sensitivity window: proximity starts this far inside a limit. */
    int32_t scale; /**< Units per 1.0 of the channel (1 display unit).                */
} chart_limits_t;

/** @brief Status chip classification (LLD §8.4). */
typedef enum
{
    CHART_ST_OK,
    CHART_ST_NEAR,
    CHART_ST_ALARM,
    CHART_ST_STALE,
} chart_status_t;

/** @brief Display range of a rendered chart (for the y-axis labels). */
typedef struct
{
    int32_t lo;
    int32_t hi;
} chart_range_t;

/** @brief Empty the history. */
void chart_history_reset(chart_history_t *h);

/** @brief Append one sample per channel. */
void chart_history_push(chart_history_t *h, const int16_t values[CHART_CHANNELS]);

/**
 * @brief Resample a channel's history to CHART_COLS values by linear interpolation.
 *
 * With no samples the output is all zeros (the chart starts flat); with one sample it is flat at
 * that value.
 */
void chart_resample(const chart_history_t *h, uint8_t channel, int32_t out[CHART_COLS]);

/**
 * @brief Proximity of @p v to the nearest limit, Q8 (PROX_ONE = at the limit, <= 0 = safe).
 */
int32_t chart_proximity_q8(int32_t v, const chart_limits_t *limits);

/** @brief Classify a proximity: stale wins, then alarm (>= 1.0), near (>= 0.6), else ok. */
chart_status_t chart_status(int32_t t_q8, bool stale);

/**
 * @brief Draw the chart into CHART_ROWS x CHART_COLS cells starting at (row0, col0).
 *
 * Empty cells on the threshold row show a dashed guide; filled columns keep their block.
 *
 * @param[in,out] grid    Target grid.
 * @param[in]     row0    Top row of the chart.
 * @param[in]     col0    Left column of the chart.
 * @param[in]     data    CHART_COLS resampled values.
 * @param[in]     limits  The channel's limits.
 * @return The y-range the chart was scaled to.
 */
chart_range_t chart_render(cell_grid_t *grid, uint8_t row0, uint8_t col0,
                           const int32_t data[CHART_COLS], const chart_limits_t *limits);

#endif /* CHART_H */
