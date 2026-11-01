/**
 * @file test_chart.c
 * @brief Unity tests for the proximity and status maths (companion §22.2).
 *
 * TC-GCS-022..029. Temperature is used throughout: values are centi-degC, limits HI 40.00 /
 * LO 0.00, band 2.00.
 */

#include "chart.h"
#include "unity.h"

static chart_limits_t s_lim;

void setUp(void)
{
    s_lim.has_hi = true;
    s_lim.has_lo = true;
    s_lim.hi = 4000;
    s_lim.lo = 0;
    s_lim.band = 200;
    s_lim.scale = 100;
}

void tearDown(void)
{
}

/* ---------------------------------------------------------------------- */

/* TC-GCS-022 */
void test_proximity_is_one_exactly_at_the_limit(void)
{
    TEST_ASSERT_EQUAL_INT32(PROX_ONE, chart_proximity_q8(4000, &s_lim));
    TEST_ASSERT_EQUAL_INT32(PROX_ONE, chart_proximity_q8(0, &s_lim));
}

/* TC-GCS-023 */
void test_proximity_is_zero_or_less_well_inside_the_limits(void)
{
    TEST_ASSERT_LESS_OR_EQUAL_INT32(0, chart_proximity_q8(2200, &s_lim));
}

/* TC-GCS-024 */
void test_proximity_goes_past_one_beyond_the_limit_and_reaches_red(void)
{
    const int32_t t = chart_proximity_q8(4400, &s_lim);

    TEST_ASSERT_GREATER_THAN_INT32(PROX_ONE, t);
}

/* TC-GCS-026 */
void test_a_low_limit_alone_drives_the_proximity(void)
{
    s_lim.has_hi = false;

    TEST_ASSERT_EQUAL_INT32(0, chart_proximity_q8(3900, &s_lim)); /* far from LO, HI ignored */
    TEST_ASSERT_GREATER_THAN_INT32(0, chart_proximity_q8(100, &s_lim));
    TEST_ASSERT_GREATER_OR_EQUAL_INT32(PROX_ONE, chart_proximity_q8(-100, &s_lim));
}

void test_with_no_limits_the_proximity_is_always_zero(void)
{
    s_lim.has_hi = false;
    s_lim.has_lo = false;

    TEST_ASSERT_EQUAL_INT32(0, chart_proximity_q8(99999, &s_lim));
}

/* TC-GCS-029 */
void test_status_chip_classification(void)
{
    TEST_ASSERT_EQUAL(CHART_ST_OK, chart_status(0, false));
    TEST_ASSERT_EQUAL(CHART_ST_OK, chart_status(150, false));
    TEST_ASSERT_EQUAL(CHART_ST_NEAR, chart_status(154, false)); /* 0.6 */
    TEST_ASSERT_EQUAL(CHART_ST_NEAR, chart_status(255, false));
    TEST_ASSERT_EQUAL(CHART_ST_ALARM, chart_status(PROX_ONE, false));
    TEST_ASSERT_EQUAL(CHART_ST_ALARM, chart_status(400, false));
    TEST_ASSERT_EQUAL(CHART_ST_STALE, chart_status(400, true)); /* stale wins */
}
