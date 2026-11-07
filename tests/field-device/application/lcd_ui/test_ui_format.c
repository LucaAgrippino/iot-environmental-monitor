/**
 * @file test_ui_format.c
 * @brief Unit tests for ui_format: fixed-point text, deltas, clock and uptime.
 */

#include "unity.h"

#include <string.h>

#include "lcd_ui/ui_format.h"

void setUp(void) {}
void tearDown(void) {}

static char s_buf[24];

void test_temperature_shows_one_decimal(void)
{
    ui_fmt_value(s_buf, sizeof(s_buf), UI_CH_TEMPERATURE, 2340);
    TEST_ASSERT_EQUAL_STRING("23.4", s_buf);
}

void test_humidity_shows_one_decimal(void)
{
    ui_fmt_value(s_buf, sizeof(s_buf), UI_CH_HUMIDITY, 5820);
    TEST_ASSERT_EQUAL_STRING("58.2", s_buf);
}

void test_pressure_shows_an_integer(void)
{
    ui_fmt_value(s_buf, sizeof(s_buf), UI_CH_PRESSURE, 10132);
    TEST_ASSERT_EQUAL_STRING("1013", s_buf);
}

void test_rounding_is_half_away_from_zero(void)
{
    ui_fmt_value(s_buf, sizeof(s_buf), UI_CH_TEMPERATURE, 2345); /* 23.45 -> 23.5 */
    TEST_ASSERT_EQUAL_STRING("23.5", s_buf);
    ui_fmt_value(s_buf, sizeof(s_buf), UI_CH_TEMPERATURE, -2345);
    TEST_ASSERT_EQUAL_STRING(UI_MINUS "23.5", s_buf);
    ui_fmt_value(s_buf, sizeof(s_buf), UI_CH_PRESSURE, 10135); /* 1013.5 -> 1014 */
    TEST_ASSERT_EQUAL_STRING("1014", s_buf);
}

void test_negative_values_use_the_true_minus_sign(void)
{
    ui_fmt_value(s_buf, sizeof(s_buf), UI_CH_TEMPERATURE, -500);
    TEST_ASSERT_EQUAL_STRING(UI_MINUS "5.0", s_buf);
    TEST_ASSERT_EQUAL_UINT8(0xE2U, (uint8_t) s_buf[0]);
}

void test_a_negative_value_that_rounds_to_zero_has_no_sign(void)
{
    ui_fmt_value(s_buf, sizeof(s_buf), UI_CH_TEMPERATURE, -4);
    TEST_ASSERT_EQUAL_STRING("0.0", s_buf);
}

void test_extreme_values_do_not_overflow(void)
{
    ui_fmt_value(s_buf, sizeof(s_buf), UI_CH_TEMPERATURE, INT32_MIN);
    TEST_ASSERT_TRUE(strlen(s_buf) > 0U);
    ui_fmt_value(s_buf, sizeof(s_buf), UI_CH_PRESSURE, INT32_MAX);
    TEST_ASSERT_TRUE(strlen(s_buf) > 0U);
}

void test_delta_positive_has_a_plus_sign(void)
{
    ui_fmt_delta(s_buf, sizeof(s_buf), UI_CH_TEMPERATURE, 30, 5);
    TEST_ASSERT_EQUAL_STRING("+0.3 / 5m", s_buf);
}

void test_delta_negative_uses_the_true_minus(void)
{
    ui_fmt_delta(s_buf, sizeof(s_buf), UI_CH_HUMIDITY, -40, 5);
    TEST_ASSERT_EQUAL_STRING(UI_MINUS "0.4 / 5m", s_buf);
}

void test_delta_zero_is_unsigned_and_pressure_keeps_a_decimal(void)
{
    ui_fmt_delta(s_buf, sizeof(s_buf), UI_CH_TEMPERATURE, 0, 2);
    TEST_ASSERT_EQUAL_STRING("0.0 / 2m", s_buf);
    ui_fmt_delta(s_buf, sizeof(s_buf), UI_CH_PRESSURE, 1, 5);
    TEST_ASSERT_EQUAL_STRING("+0.1 / 5m", s_buf);
}

void test_clock_formats_time_of_day(void)
{
    ui_fmt_clock(s_buf, sizeof(s_buf), (12U * 3600U) + (34U * 60U) + 56U);
    TEST_ASSERT_EQUAL_STRING("12:34:56", s_buf);
}

void test_clock_wraps_at_one_day(void)
{
    ui_fmt_clock(s_buf, sizeof(s_buf), 86400U + 61U);
    TEST_ASSERT_EQUAL_STRING("00:01:01", s_buf);
}

void test_uptime_under_a_day_is_hh_mm_ss(void)
{
    ui_fmt_uptime(s_buf, sizeof(s_buf), 3725U);
    TEST_ASSERT_EQUAL_STRING("01:02:05", s_buf);
}

void test_uptime_over_a_day_shows_days_hours_minutes(void)
{
    ui_fmt_uptime(s_buf, sizeof(s_buf), (3U * 86400U) + (14U * 3600U) + (22U * 60U) + 9U);
    TEST_ASSERT_EQUAL_STRING("3d 14:22", s_buf);
}

void test_a_short_buffer_is_never_overrun(void)
{
    char tiny[4] = {'x', 'x', 'x', 'x'};
    ui_fmt_value(tiny, 3U, UI_CH_TEMPERATURE, 2340);
    TEST_ASSERT_EQUAL_UINT8('\0', (uint8_t) tiny[2]);
    TEST_ASSERT_EQUAL_UINT8('x', (uint8_t) tiny[3]);
}

void test_null_and_empty_destinations_are_ignored(void)
{
    ui_fmt_value(NULL, 8U, UI_CH_TEMPERATURE, 1);
    ui_fmt_clock(s_buf, 0U, 1U);
    ui_fmt_uptime(NULL, 0U, 1U);
    TEST_PASS();
}

void test_ago_scales_from_seconds_to_days(void)
{
    ui_fmt_ago(s_buf, sizeof(s_buf), 20U);
    TEST_ASSERT_EQUAL_STRING("just now", s_buf);
    ui_fmt_ago(s_buf, sizeof(s_buf), 5U * 60U);
    TEST_ASSERT_EQUAL_STRING("5m ago", s_buf);
    ui_fmt_ago(s_buf, sizeof(s_buf), (3U * 3600U) + (12U * 60U));
    TEST_ASSERT_EQUAL_STRING("3h 12m ago", s_buf);
    ui_fmt_ago(s_buf, sizeof(s_buf), (2U * 86400U) + (4U * 3600U));
    TEST_ASSERT_EQUAL_STRING("2d 4h ago", s_buf);
}

void test_parse_accepts_whole_and_one_decimal(void)
{
    int32_t t = 0;
    TEST_ASSERT_TRUE(ui_parse_tenths("30.0", &t));
    TEST_ASSERT_EQUAL_INT32(300, t);
    TEST_ASSERT_TRUE(ui_parse_tenths("5", &t));
    TEST_ASSERT_EQUAL_INT32(50, t);
    TEST_ASSERT_TRUE(ui_parse_tenths("-5.5", &t));
    TEST_ASSERT_EQUAL_INT32(-55, t);
    TEST_ASSERT_TRUE(ui_parse_tenths("7.", &t));
    TEST_ASSERT_EQUAL_INT32(70, t);
    TEST_ASSERT_TRUE(ui_parse_tenths(".5", &t));
    TEST_ASSERT_EQUAL_INT32(5, t);
    TEST_ASSERT_TRUE(ui_parse_tenths("-0.0", &t));
    TEST_ASSERT_EQUAL_INT32(0, t);
}

void test_parse_rejects_incomplete_or_malformed_entries(void)
{
    int32_t t = 99;
    TEST_ASSERT_FALSE(ui_parse_tenths("", &t));
    TEST_ASSERT_FALSE(ui_parse_tenths("-", &t));
    TEST_ASSERT_FALSE(ui_parse_tenths(".", &t));
    TEST_ASSERT_FALSE(ui_parse_tenths("-.", &t));
    TEST_ASSERT_FALSE(ui_parse_tenths("1.25", &t)); /* two decimals do not fit the fields */
    TEST_ASSERT_FALSE(ui_parse_tenths("1a", &t));
    TEST_ASSERT_FALSE(ui_parse_tenths("--1", &t));
    TEST_ASSERT_FALSE(ui_parse_tenths(NULL, &t));
    TEST_ASSERT_FALSE(ui_parse_tenths("1", NULL));
    TEST_ASSERT_EQUAL_INT32(99, t); /* untouched on failure */
}

void test_parse_rejects_a_number_too_large_for_int32(void)
{
    int32_t t = 0;
    TEST_ASSERT_FALSE(ui_parse_tenths("99999999999", &t));
}
