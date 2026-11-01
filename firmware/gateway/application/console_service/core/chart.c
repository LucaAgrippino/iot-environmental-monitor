/**
 * @file chart.c
 * @brief Proximity of a reading to its alarm limits, and the status chip derived from it.
 */

#include "chart.h"

#define Q8 (256)
#define NEAR_NUM (3) /* near from 0.6 = 3/5 */
#define NEAR_DEN (5)

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
