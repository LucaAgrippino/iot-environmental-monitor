/**
 * @file test_ui_history.c
 * @brief Unit tests for ui_history: the five-minute ring buffer.
 */

#include "unity.h"

#include "lcd_ui/ui_history.h"

void setUp(void)
{
    ui_history_reset();
}

void tearDown(void) {}

static void push_all(int32_t t, int32_t h, int32_t p, bool ok)
{
    const int32_t v[UI_CH_COUNT] = {t, h, p};
    const bool valid[UI_CH_COUNT] = {ok, ok, ok};
    ui_history_push(v, valid);
}

void test_starts_empty(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, ui_history_count());
    int32_t v;
    TEST_ASSERT_FALSE(ui_history_get(UI_CH_TEMPERATURE, 0U, &v));
}

void test_push_and_read_back_in_order(void)
{
    push_all(100, 200, 300, true);
    push_all(110, 210, 310, true);

    int32_t v;
    TEST_ASSERT_EQUAL_UINT32(2U, ui_history_count());
    TEST_ASSERT_TRUE(ui_history_get(UI_CH_TEMPERATURE, 0U, &v));
    TEST_ASSERT_EQUAL_INT32(100, v);
    TEST_ASSERT_TRUE(ui_history_get(UI_CH_PRESSURE, 1U, &v));
    TEST_ASSERT_EQUAL_INT32(310, v);
}

void test_the_oldest_sample_is_dropped_after_sixty(void)
{
    for (int32_t i = 0; i < 65; i++)
    {
        push_all(i, 0, 0, true);
    }
    int32_t v;
    TEST_ASSERT_EQUAL_UINT32(UI_HISTORY_LEN, ui_history_count());
    TEST_ASSERT_TRUE(ui_history_get(UI_CH_TEMPERATURE, 0U, &v));
    TEST_ASSERT_EQUAL_INT32(5, v); /* samples 0..4 were overwritten */
    TEST_ASSERT_TRUE(ui_history_get(UI_CH_TEMPERATURE, 59U, &v));
    TEST_ASSERT_EQUAL_INT32(64, v);
}

void test_an_invalid_sample_reads_as_a_gap(void)
{
    push_all(1, 1, 1, true);
    push_all(2, 2, 2, false);
    push_all(3, 3, 3, true);

    int32_t v;
    TEST_ASSERT_TRUE(ui_history_get(UI_CH_HUMIDITY, 0U, &v));
    TEST_ASSERT_FALSE(ui_history_get(UI_CH_HUMIDITY, 1U, &v));
    TEST_ASSERT_TRUE(ui_history_get(UI_CH_HUMIDITY, 2U, &v));
}

void test_out_of_range_reads_fail(void)
{
    push_all(1, 1, 1, true);
    int32_t v;
    TEST_ASSERT_FALSE(ui_history_get(UI_CH_TEMPERATURE, 1U, &v));
    TEST_ASSERT_FALSE(ui_history_get((ui_channel_t) 9, 0U, &v));
    TEST_ASSERT_FALSE(ui_history_get(UI_CH_TEMPERATURE, 0U, NULL));
}

void test_tick_samples_every_five_seconds(void)
{
    const int32_t v[UI_CH_COUNT] = {1, 2, 3};
    const bool ok[UI_CH_COUNT] = {true, true, true};

    TEST_ASSERT_TRUE(ui_history_tick(1000U, v, ok));  /* first call always stores */
    TEST_ASSERT_FALSE(ui_history_tick(3000U, v, ok)); /* 2 s later: too soon */
    TEST_ASSERT_FALSE(ui_history_tick(5999U, v, ok));
    TEST_ASSERT_TRUE(ui_history_tick(6000U, v, ok));
    TEST_ASSERT_EQUAL_UINT32(2U, ui_history_count());
}

void test_tick_handles_the_millisecond_counter_wrapping(void)
{
    const int32_t v[UI_CH_COUNT] = {1, 2, 3};
    const bool ok[UI_CH_COUNT] = {true, true, true};

    TEST_ASSERT_TRUE(ui_history_tick(0xFFFFFC00U, v, ok));
    TEST_ASSERT_TRUE(ui_history_tick(0x00001000U, v, ok)); /* 5.1 s later across the wrap */
}

void test_delta_needs_two_valid_samples(void)
{
    int32_t d;
    uint32_t m;
    TEST_ASSERT_FALSE(ui_history_delta(UI_CH_TEMPERATURE, &d, &m));
    push_all(100, 0, 0, true);
    TEST_ASSERT_FALSE(ui_history_delta(UI_CH_TEMPERATURE, &d, &m));
}

void test_delta_is_newest_minus_oldest_valid(void)
{
    push_all(2340, 0, 0, false); /* an invalid first sample is skipped */
    push_all(2340, 0, 0, true);
    for (int i = 0; i < 10; i++)
    {
        push_all(2350, 0, 0, true);
    }
    push_all(2370, 0, 0, true);

    int32_t d;
    uint32_t m;
    TEST_ASSERT_TRUE(ui_history_delta(UI_CH_TEMPERATURE, &d, &m));
    TEST_ASSERT_EQUAL_INT32(30, d);
    TEST_ASSERT_EQUAL_UINT32(1U, m); /* 11 samples * 5 s = 55 s -> 1 min */
}

void test_delta_window_is_at_most_five_minutes(void)
{
    for (int i = 0; i < 60; i++)
    {
        push_all(i, 0, 0, true);
    }
    int32_t d;
    uint32_t m;
    TEST_ASSERT_TRUE(ui_history_delta(UI_CH_TEMPERATURE, &d, &m));
    TEST_ASSERT_EQUAL_INT32(59, d);
    TEST_ASSERT_EQUAL_UINT32(5U, m); /* 59 intervals * 5 s = 295 s -> 5 min */
}

void test_delta_rejects_null_outputs(void)
{
    push_all(1, 0, 0, true);
    push_all(2, 0, 0, true);
    int32_t d;
    uint32_t m;
    TEST_ASSERT_FALSE(ui_history_delta(UI_CH_TEMPERATURE, NULL, &m));
    TEST_ASSERT_FALSE(ui_history_delta(UI_CH_TEMPERATURE, &d, NULL));
}
