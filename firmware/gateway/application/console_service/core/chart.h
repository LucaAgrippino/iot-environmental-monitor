/**
 * @file chart.h
 * @brief Proximity of a reading to its alarm limits (the "near / alarm" status chips).
 *
 * Proximity is Q8 fixed point: 256 means "exactly at the limit". The module used to draw a block
 * chart as well; the three-panel console shows numbers only, so only the maths remains.
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §8
 */

#ifndef CHART_H
#define CHART_H

#include <stdbool.h>
#include <stdint.h>

#define CHART_CHANNELS (3U) /* temperature, humidity, pressure */

/** Proximity of exactly 1.0 (the limit itself) in Q8. */
#define PROX_ONE (256)

/** @brief Alarm limits of one channel, all in the channel's own units. */
typedef struct
{
    bool has_hi;
    bool has_lo;
    int32_t hi;
    int32_t lo;
    int32_t band;  /**< Sensitivity window: proximity starts this far inside a limit. */
    int32_t scale; /**< Units per 1.0 of the channel (1 display unit).                */
} chart_limits_t;

typedef enum
{
    CHART_ST_OK,
    CHART_ST_NEAR,
    CHART_ST_ALARM,
    CHART_ST_STALE,
} chart_status_t;

/**
 * @brief Proximity of @p v to the nearest limit, Q8 (0 when clear of both bands).
 * @param[in] v       Reading, in channel units.
 * @param[in] limits  Limits; band must be non-zero.
 */
int32_t chart_proximity_q8(int32_t v, const chart_limits_t *limits);

/** @brief Status chip for a proximity value: stale wins, then >= 1.0 alarm, >= 0.6 near. */
chart_status_t chart_status(int32_t t_q8, bool stale);

#endif /* CHART_H */
