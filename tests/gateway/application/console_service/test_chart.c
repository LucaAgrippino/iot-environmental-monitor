/**
 * @file test_chart.c
 * @brief Unity tests for the console chart and colour theme (companion §22.2).
 *
 * TC-GCS-020..031 plus history, resampling and palette cases. Temperature is used throughout:
 * values are centi-degC, limits HI 40.00 / LO 0.00, band 2.00.
 */

#include <string.h>

#include "chart.h"
#include "termout.h"
#include "theme.h"
#include "unity.h"

static chart_limits_t s_lim;
static cell_grid_t s_grid;

#define ROW0 (3U)
#define COL0 (5U)

void setUp(void)
{
    s_lim.has_hi = true;
    s_lim.has_lo = true;
    s_lim.hi = 4000;
    s_lim.lo = 0;
    s_lim.band = 200;
    s_lim.scale = 100;
    grid_clear(&s_grid);
}

void tearDown(void)
{
}

static void fill(int32_t data[CHART_COLS], int32_t v)
{
    for (uint32_t i = 0U; i < CHART_COLS; i++)
    {
        data[i] = v;
    }
}

/* Colour of the topmost filled cell of column c, or 255 if the column is empty. */
static uint8_t top_colour(uint32_t c)
{
    for (uint32_t r = 0U; r < CHART_ROWS; r++)
    {
        const cell_t *cell = &s_grid.cells[ROW0 + r][COL0 + c];

        if ((cell->glyph >= GLYPH_BLK1) && (cell->glyph <= GLYPH_BLK8))
        {
            return cell->fg;
        }
    }
    return 255U;
}

static uint8_t top_glyph(uint32_t c)
{
    for (uint32_t r = 0U; r < CHART_ROWS; r++)
    {
        const cell_t *cell = &s_grid.cells[ROW0 + r][COL0 + c];

        if ((cell->glyph >= GLYPH_BLK1) && (cell->glyph <= GLYPH_BLK8))
        {
            return cell->glyph;
        }
    }
    return 0U;
}

/* ---------------------------------------------------------------------- */
/* History and resampling                                                  */
/* ---------------------------------------------------------------------- */

void test_history_starts_empty_and_wraps_at_its_capacity(void)
{
    chart_history_t h;
    int16_t v[CHART_CHANNELS];
    int32_t out[CHART_COLS];

    chart_history_reset(&h);
    TEST_ASSERT_EQUAL_UINT8(0U, h.count);
    for (int16_t i = 0; i < (int16_t) (CHART_HISTORY + 10U); i++)
    {
        v[0] = i;
        v[1] = (int16_t) (i * 2);
        v[2] = (int16_t) (i * 3);
        chart_history_push(&h, v);
    }
    TEST_ASSERT_EQUAL_UINT8(CHART_HISTORY, h.count);

    chart_resample(&h, 0U, out);
    TEST_ASSERT_EQUAL_INT32(10, out[0]);                          /* oldest kept sample */
    TEST_ASSERT_EQUAL_INT32(CHART_HISTORY + 9, out[CHART_COLS - 1U]); /* newest sample */
    chart_resample(&h, 2U, out);
    TEST_ASSERT_EQUAL_INT32(30, out[0]);
}

/* TC-GCS-027 */
void test_resample_92_to_46_columns_interpolates_linearly(void)
{
    chart_history_t h;
    int16_t v[CHART_CHANNELS] = {0, 0, 0};
    int32_t out[CHART_COLS];

    chart_history_reset(&h);
    for (int16_t i = 0; i < (int16_t) CHART_HISTORY; i++)
    {
        v[0] = (int16_t) (i * 10);
        chart_history_push(&h, v);
    }
    chart_resample(&h, 0U, out);

    TEST_ASSERT_EQUAL_INT32(0, out[0]);
    TEST_ASSERT_EQUAL_INT32(910, out[CHART_COLS - 1U]);
    /* A straight ramp stays a straight ramp: roughly 910 / 45 per column. */
    for (uint32_t c = 1U; c < CHART_COLS; c++)
    {
        const int32_t step = out[c] - out[c - 1U];

        TEST_ASSERT_INT32_WITHIN(1, 20, step);
    }
}

void test_resample_of_empty_and_single_sample_histories(void)
{
    chart_history_t h;
    int16_t v[CHART_CHANNELS] = {1234, 0, 0};
    int32_t out[CHART_COLS];

    chart_history_reset(&h);
    chart_resample(&h, 0U, out);
    for (uint32_t c = 0U; c < CHART_COLS; c++)
    {
        TEST_ASSERT_EQUAL_INT32(0, out[c]);
    }
    chart_history_push(&h, v);
    chart_resample(&h, 0U, out);
    for (uint32_t c = 0U; c < CHART_COLS; c++)
    {
        TEST_ASSERT_EQUAL_INT32(1234, out[c]);
    }
}

/* ---------------------------------------------------------------------- */
/* Proximity and status                                                    */
/* ---------------------------------------------------------------------- */

/* TC-GCS-022 */
void test_proximity_is_one_exactly_at_the_limit(void)
{
    TEST_ASSERT_EQUAL_INT32(PROX_ONE, chart_proximity_q8(4000, &s_lim));
    TEST_ASSERT_EQUAL_INT32(PROX_ONE, chart_proximity_q8(0, &s_lim));
    TEST_ASSERT_EQUAL_UINT8(PAL_GRAD_FIRST + PAL_GRAD_HALF, theme_prox_index(PROX_ONE));
}

/* TC-GCS-023 */
void test_proximity_is_zero_or_less_well_inside_the_limits(void)
{
    TEST_ASSERT_LESS_OR_EQUAL_INT32(0, chart_proximity_q8(2200, &s_lim));
    TEST_ASSERT_EQUAL_UINT8(PAL_GRAD_FIRST, theme_prox_index(chart_proximity_q8(2200, &s_lim)));
    TEST_ASSERT_FALSE(theme_prox_bold(chart_proximity_q8(2200, &s_lim)));
}

/* TC-GCS-024 */
void test_proximity_goes_past_one_beyond_the_limit_and_reaches_red(void)
{
    const int32_t t = chart_proximity_q8(4400, &s_lim);

    TEST_ASSERT_GREATER_THAN_INT32(PROX_ONE, t);
    TEST_ASSERT_EQUAL_UINT8(PAL_COUNT - 1U, theme_prox_index(t)); /* OVER, fully red */
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

/* ---------------------------------------------------------------------- */
/* Theme                                                                   */
/* ---------------------------------------------------------------------- */

void test_gradient_runs_from_cyan_through_amber_to_red(void)
{
    uint8_t r;
    uint8_t g;
    uint8_t b;

    theme_rgb(PAL_GRAD_FIRST, &r, &g, &b);
    TEST_ASSERT_EQUAL_UINT8(123, r);
    TEST_ASSERT_EQUAL_UINT8(175, g);
    TEST_ASSERT_EQUAL_UINT8(212, b);

    theme_rgb(PAL_GRAD_FIRST + PAL_GRAD_HALF, &r, &g, &b); /* WARN plus its glow nudge */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT8(224, r);
    TEST_ASSERT_UINT8_WITHIN(20, 178, g);

    theme_rgb(PAL_COUNT - 1U, &r, &g, &b); /* OVER plus the full glow nudge */
    TEST_ASSERT_UINT8_WITHIN(35, 204, r);
    TEST_ASSERT_UINT8_WITHIN(35, 102, g);
}

void test_out_of_range_palette_index_falls_back_to_ink(void)
{
    uint8_t r;
    uint8_t g;
    uint8_t b;

    theme_rgb(PAL_COUNT, &r, &g, &b);
    TEST_ASSERT_EQUAL_UINT8(199, r);
    TEST_ASSERT_EQUAL_UINT8(211, g);
    TEST_ASSERT_EQUAL_UINT8(223, b);
}

void test_sgr_always_resets_first_and_omits_the_default_background(void)
{
    char buf[THEME_SGR_MAX];

    size_t n = theme_sgr(buf, PAL_OK, PAL_BG, false);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("\x1b[0;38;2;111;191;142m", buf);

    n = theme_sgr(buf, PAL_SEL_FG, PAL_SEL_BG, true);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("\x1b[0;1;38;2;8;18;26;48;2;123;175;212m", buf);
    TEST_ASSERT_LESS_THAN_size_t(THEME_SGR_MAX, n);
}

/* ---------------------------------------------------------------------- */
/* Rendering                                                               */
/* ---------------------------------------------------------------------- */

/* TC-GCS-020 */
void test_flat_data_gives_equal_columns_all_in_the_base_colour(void)
{
    int32_t data[CHART_COLS];

    fill(data, 2200);
    (void) chart_render(&s_grid, ROW0, COL0, data, &s_lim);

    for (uint32_t c = 0U; c < CHART_COLS; c++)
    {
        TEST_ASSERT_EQUAL_UINT8(top_glyph(0U), top_glyph(c));
        TEST_ASSERT_EQUAL_UINT8(PAL_GRAD_FIRST, top_colour(c));
    }
}

/* TC-GCS-021 */
void test_rising_data_heats_along_its_length(void)
{
    int32_t data[CHART_COLS];

    for (uint32_t c = 0U; c < CHART_COLS; c++)
    {
        data[c] = 3000 + (int32_t) (c * 32U); /* 30.00 .. 44.40, crossing HI 40.00 */
    }
    (void) chart_render(&s_grid, ROW0, COL0, data, &s_lim);

    TEST_ASSERT_EQUAL_UINT8(PAL_GRAD_FIRST, top_colour(0U));
    TEST_ASSERT_EQUAL_UINT8(PAL_COUNT - 1U, top_colour(CHART_COLS - 1U));
    for (uint32_t c = 1U; c < CHART_COLS; c++)
    {
        TEST_ASSERT_GREATER_OR_EQUAL_UINT8(top_colour(c - 1U), top_colour(c)); /* never cools down */
    }
    TEST_ASSERT_EQUAL_UINT8(PAL_GRAD_FIRST, top_colour(20U));            /* 36.40: outside the band */
    TEST_ASSERT_GREATER_THAN_UINT8(PAL_GRAD_FIRST, top_colour(30U));     /* 39.60: inside it, warming */
}

/* TC-GCS-025 */
void test_threshold_row_shows_a_dashed_guide_only_in_empty_cells(void)
{
    int32_t data[CHART_COLS];
    bool saw_dash = false;
    bool dash_on_filled = false;

    fill(data, 1000);
    data[10] = 3000;
    s_lim.has_lo = false; /* the guide follows HI 40.00 */
    (void) chart_render(&s_grid, ROW0, COL0, data, &s_lim);

    for (uint32_t r = 0U; r < CHART_ROWS; r++)
    {
        for (uint32_t c = 0U; c < CHART_COLS; c++)
        {
            const cell_t *cell = &s_grid.cells[ROW0 + r][COL0 + c];

            if (cell->glyph == GLYPH_DASH)
            {
                saw_dash = true;
                TEST_ASSERT_EQUAL_UINT8(PAL_THR, cell->fg);
            }
        }
    }
    /* Where a column is tall enough to reach the guide row, its block must win over the dash. */
    for (uint32_t r = 0U; r < CHART_ROWS; r++)
    {
        const cell_t *cell = &s_grid.cells[ROW0 + r][COL0 + 10U];

        if ((cell->glyph == GLYPH_DASH) && (top_glyph(10U) != 0U))
        {
            dash_on_filled = (r > 0U) && (s_grid.cells[ROW0 + r - 1U][COL0 + 10U].glyph == 0U);
        }
    }
    TEST_ASSERT_TRUE(saw_dash);
    TEST_ASSERT_FALSE(dash_on_filled);
}

/* TC-GCS-028 */
void test_every_cell_is_a_space_a_dash_or_a_valid_block(void)
{
    int32_t data[CHART_COLS];

    for (uint32_t c = 0U; c < CHART_COLS; c++)
    {
        data[c] = (int32_t) ((c * 97U) % 4800U); /* wild data, partly past both limits */
    }
    (void) chart_render(&s_grid, ROW0, COL0, data, &s_lim);

    for (uint32_t r = 0U; r < CHART_ROWS; r++)
    {
        for (uint32_t c = 0U; c < CHART_COLS; c++)
        {
            const uint8_t gl = s_grid.cells[ROW0 + r][COL0 + c].glyph;

            TEST_ASSERT_TRUE((gl == 0U) || (gl == GLYPH_DASH) || ((gl >= GLYPH_BLK1) && (gl <= GLYPH_BLK8)));
        }
    }
}

/* TC-GCS-030 */
void test_cells_past_the_limit_are_bold_and_safe_cells_are_not(void)
{
    int32_t data[CHART_COLS];

    fill(data, 2200);
    data[CHART_COLS - 1U] = 4400;
    (void) chart_render(&s_grid, ROW0, COL0, data, &s_lim);

    bool safe_bold = false;
    bool hot_bold = false;

    for (uint32_t r = 0U; r < CHART_ROWS; r++)
    {
        safe_bold = safe_bold || ((s_grid.cells[ROW0 + r][COL0].glyph != 0U) &&
                                  ((s_grid.cells[ROW0 + r][COL0].bg_attr & CELL_BOLD) != 0U));
        hot_bold = hot_bold || ((s_grid.cells[ROW0 + r][COL0 + CHART_COLS - 1U].glyph != 0U) &&
                                ((s_grid.cells[ROW0 + r][COL0 + CHART_COLS - 1U].bg_attr & CELL_BOLD) != 0U));
    }
    TEST_ASSERT_FALSE(safe_bold);
    TEST_ASSERT_TRUE(hot_bold);
}

void test_display_range_includes_the_limit_and_a_margin(void)
{
    int32_t data[CHART_COLS];

    fill(data, 2200);
    const chart_range_t r = chart_render(&s_grid, ROW0, COL0, data, &s_lim);

    TEST_ASSERT_LESS_THAN_INT32(2200, r.lo);    /* padding below the data */
    TEST_ASSERT_GREATER_THAN_INT32(4000, r.hi); /* the nearer limit, HI 40.00, is inside with padding */
    TEST_ASSERT_GREATER_THAN_INT32(0, r.lo);    /* the far limit LO 0.00 does not stretch the range */
}

void test_a_flat_chart_with_no_limits_still_renders_without_dividing_by_zero(void)
{
    int32_t data[CHART_COLS];

    s_lim.has_hi = false;
    s_lim.has_lo = false;
    fill(data, 500);
    const chart_range_t r = chart_render(&s_grid, ROW0, COL0, data, &s_lim);

    TEST_ASSERT_GREATER_THAN_INT32(r.lo, r.hi);
}
