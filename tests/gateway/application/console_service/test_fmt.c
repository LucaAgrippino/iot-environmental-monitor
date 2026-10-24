/**
 * @file test_fmt.c
 * @brief Unity tests for the console's fixed-point text helpers.
 */

#include <string.h>

#include "fmt.h"
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static const char *fx(int32_t v, int32_t scale, uint8_t dec)
{
    static char buf[24];

    (void) fmt_fixed(buf, sizeof(buf), v, scale, dec);
    return buf;
}

void test_fixed_prints_decimals_with_rounding_half_away_from_zero(void)
{
    TEST_ASSERT_EQUAL_STRING("24.3", fx(2430, 100, 1));
    TEST_ASSERT_EQUAL_STRING("24.4", fx(2435, 100, 1)); /* .35 rounds up */
    TEST_ASSERT_EQUAL_STRING("24.30", fx(2430, 100, 2));
    TEST_ASSERT_EQUAL_STRING("1013", fx(10130, 10, 0));
    TEST_ASSERT_EQUAL_STRING("1013.2", fx(10132, 10, 1));
    TEST_ASSERT_EQUAL_STRING("0.0", fx(0, 100, 1));
    TEST_ASSERT_EQUAL_STRING("100", fx(100, 1, 0));
}

void test_fixed_handles_negatives_and_the_minus_zero_case(void)
{
    TEST_ASSERT_EQUAL_STRING("-3.5", fx(-350, 100, 1));
    TEST_ASSERT_EQUAL_STRING("-40.0", fx(-4000, 100, 1));
    TEST_ASSERT_EQUAL_STRING("-0.1", fx(-10, 100, 1));
    TEST_ASSERT_EQUAL_STRING("0.0", fx(-2, 100, 1)); /* rounds to zero: no "-0.0" */
}

void test_fixed_clamps_decimals_to_what_the_scale_holds(void)
{
    TEST_ASSERT_EQUAL_STRING("1013.0", fx(10130, 10, 2)); /* scale 10 holds one decimal, so 2 clamps to 1 */
    TEST_ASSERT_EQUAL_STRING("1013.0", fx(10130, 10, 1));
}

void test_parse_accepts_plain_signed_and_decimal_numbers(void)
{
    int32_t v;

    TEST_ASSERT_TRUE(fmt_parse_fixed("24", 100, &v));
    TEST_ASSERT_EQUAL_INT32(2400, v);
    TEST_ASSERT_TRUE(fmt_parse_fixed("24.5", 100, &v));
    TEST_ASSERT_EQUAL_INT32(2450, v);
    TEST_ASSERT_TRUE(fmt_parse_fixed("24.55", 100, &v));
    TEST_ASSERT_EQUAL_INT32(2455, v);
    TEST_ASSERT_TRUE(fmt_parse_fixed("-3.5", 100, &v));
    TEST_ASSERT_EQUAL_INT32(-350, v);
    TEST_ASSERT_TRUE(fmt_parse_fixed("+7", 10, &v));
    TEST_ASSERT_EQUAL_INT32(70, v);
    TEST_ASSERT_TRUE(fmt_parse_fixed("1013.2", 10, &v));
    TEST_ASSERT_EQUAL_INT32(10132, v);
    TEST_ASSERT_TRUE(fmt_parse_fixed("500", 1, &v));
    TEST_ASSERT_EQUAL_INT32(500, v);
}

void test_parse_rejects_malformed_input(void)
{
    int32_t v = 77;

    TEST_ASSERT_FALSE(fmt_parse_fixed("", 100, &v));
    TEST_ASSERT_FALSE(fmt_parse_fixed("abc", 100, &v));
    TEST_ASSERT_FALSE(fmt_parse_fixed("1.2.3", 100, &v));
    TEST_ASSERT_FALSE(fmt_parse_fixed("5.", 100, &v));
    TEST_ASSERT_FALSE(fmt_parse_fixed(".5", 100, &v));
    TEST_ASSERT_FALSE(fmt_parse_fixed("24x", 100, &v));
    TEST_ASSERT_FALSE(fmt_parse_fixed("-", 100, &v));
    TEST_ASSERT_FALSE(fmt_parse_fixed(NULL, 100, &v));
    TEST_ASSERT_FALSE(fmt_parse_fixed("1", 100, NULL));
    TEST_ASSERT_EQUAL_INT32(77, v); /* untouched on failure */
}

void test_parse_rejects_more_decimals_than_the_scale_holds_and_overflow(void)
{
    int32_t v;

    TEST_ASSERT_FALSE(fmt_parse_fixed("1.234", 100, &v));
    TEST_ASSERT_FALSE(fmt_parse_fixed("1.25", 10, &v));
    TEST_ASSERT_FALSE(fmt_parse_fixed("1.5", 1, &v));
    TEST_ASSERT_FALSE(fmt_parse_fixed("99999999999", 100, &v));
    TEST_ASSERT_FALSE(fmt_parse_fixed("999999999", 100, &v)); /* x100 exceeds int32 */
}

void test_parse_u32_takes_up_to_nine_digits(void)
{
    uint32_t v;

    TEST_ASSERT_TRUE(fmt_parse_u32("0", &v));
    TEST_ASSERT_EQUAL_UINT32(0U, v);
    TEST_ASSERT_TRUE(fmt_parse_u32("123456789", &v));
    TEST_ASSERT_EQUAL_UINT32(123456789U, v);
    TEST_ASSERT_FALSE(fmt_parse_u32("1234567890", &v));
    TEST_ASSERT_FALSE(fmt_parse_u32("", &v));
    TEST_ASSERT_FALSE(fmt_parse_u32("-1", &v));
    TEST_ASSERT_FALSE(fmt_parse_u32("12a", &v));
    TEST_ASSERT_FALSE(fmt_parse_u32(NULL, &v));
}

void test_padding_counts_columns_not_bytes(void)
{
    char buf[32];

    TEST_ASSERT_EQUAL_size_t(1U, fmt_cols("a"));
    TEST_ASSERT_EQUAL_size_t(4U, fmt_cols("● ok"));
    TEST_ASSERT_EQUAL_size_t(2U, fmt_cols("°C"));

    (void) fmt_pad(buf, sizeof(buf), "● ok", 8U);
    TEST_ASSERT_EQUAL_size_t(8U, fmt_cols(buf));
    TEST_ASSERT_EQUAL_STRING("● ok    ", buf);

    (void) fmt_pad(buf, sizeof(buf), "longer than width", 4U);
    TEST_ASSERT_EQUAL_STRING("longer than width", buf); /* never truncated */

    (void) fmt_pad(buf, 6U, "abcdefghij", 20U); /* buffer too small: terminated, not overrun */
    TEST_ASSERT_EQUAL_size_t(5U, strlen(buf));
}
