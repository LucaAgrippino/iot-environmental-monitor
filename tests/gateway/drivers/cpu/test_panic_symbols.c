/**
 * @file test_panic_symbols.c
 * @brief Unit tests for panic_symbol_lookup() — Track B, on-device panic
 *        symbol resolution.
 *
 * Deliberately tested against a small, hand-built fixture table, not the
 * real generated one (panic_symbols_data.c, gitignored, machine-generated
 * from a real gateway.map — doesn't exist in a fresh checkout or CI).
 * panic_symbol_lookup() itself is generic over any sorted table, exactly
 * so this is possible without needing a real build first.
 */

#include "unity.h"

#include "panic_symbols.h"

/* Sorted by addr ascending, as panic_symbol_lookup() requires. Gaps are
 * deliberate (mirrors real .text layout, where linker-discarded/aligned
 * space separates functions). */
static const panic_symbol_t s_fixture[] = {
    {0x08000100u, 0x20u, "func_a", 0u}, /* covers [0x08000100, 0x08000120) */
    {0x08000200u, 0x10u, "func_b", 0u}, /* covers [0x08000200, 0x08000210) */
    {0x08000300u, 0x40u, "func_c", 0u}, /* covers [0x08000300, 0x08000340) */
};
static const uint32_t s_fixture_count = sizeof(s_fixture) / sizeof(s_fixture[0]);

void setUp(void) {}
void tearDown(void) {}

void test_lookup_null_table_returns_null(void)
{
    TEST_ASSERT_NULL(panic_symbol_lookup(NULL, s_fixture_count, 0x08000100u));
}

void test_lookup_zero_count_returns_null(void)
{
    TEST_ASSERT_NULL(panic_symbol_lookup(s_fixture, 0u, 0x08000100u));
}

void test_lookup_exact_start_address_matches(void)
{
    const panic_symbol_t *sym = panic_symbol_lookup(s_fixture, s_fixture_count, 0x08000100u);
    TEST_ASSERT_NOT_NULL(sym);
    TEST_ASSERT_EQUAL_STRING("func_a", sym->name);
}

void test_lookup_mid_range_address_matches(void)
{
    const panic_symbol_t *sym = panic_symbol_lookup(s_fixture, s_fixture_count, 0x08000110u);
    TEST_ASSERT_NOT_NULL(sym);
    TEST_ASSERT_EQUAL_STRING("func_a", sym->name);
}

void test_lookup_last_byte_of_function_matches(void)
{
    /* func_a covers [0x100, 0x120) — 0x11F is the last byte still inside. */
    const panic_symbol_t *sym = panic_symbol_lookup(s_fixture, s_fixture_count, 0x0800011Fu);
    TEST_ASSERT_NOT_NULL(sym);
    TEST_ASSERT_EQUAL_STRING("func_a", sym->name);
}

void test_lookup_exclusive_end_boundary_does_not_match(void)
{
    /* 0x120 is one past func_a's last byte — belongs to the gap, not func_a. */
    TEST_ASSERT_NULL(panic_symbol_lookup(s_fixture, s_fixture_count, 0x08000120u));
}

void test_lookup_gap_between_functions_returns_null(void)
{
    TEST_ASSERT_NULL(panic_symbol_lookup(s_fixture, s_fixture_count, 0x08000150u));
}

void test_lookup_before_first_entry_returns_null(void)
{
    TEST_ASSERT_NULL(panic_symbol_lookup(s_fixture, s_fixture_count, 0x08000050u));
}

void test_lookup_last_entry_exact_and_last_byte(void)
{
    const panic_symbol_t *at_start = panic_symbol_lookup(s_fixture, s_fixture_count, 0x08000300u);
    TEST_ASSERT_NOT_NULL(at_start);
    TEST_ASSERT_EQUAL_STRING("func_c", at_start->name);

    const panic_symbol_t *at_last_byte =
        panic_symbol_lookup(s_fixture, s_fixture_count, 0x0800033Fu);
    TEST_ASSERT_NOT_NULL(at_last_byte);
    TEST_ASSERT_EQUAL_STRING("func_c", at_last_byte->name);
}

void test_lookup_past_last_entry_returns_null(void)
{
    TEST_ASSERT_NULL(panic_symbol_lookup(s_fixture, s_fixture_count, 0x08000350u));
}

void test_lookup_garbage_address_returns_null_not_crash(void)
{
    /* The exact class of value this exists for: a genuinely garbage
     * fault-time address (this one is the actual BFAR seen on real
     * hardware during the WIFITASK-O6 investigation this session), which
     * must resolve cleanly to "unknown", not misbehave. */
    TEST_ASSERT_NULL(panic_symbol_lookup(s_fixture, s_fixture_count, 0xFFFFFFEEu));
}

/* ---- panic_symbols_match(): is the generated table the one for the running image? ---- */

void test_match_requires_equal_anchor_ignoring_the_thumb_bit(void)
{
    TEST_ASSERT_TRUE(panic_symbols_match(0x08001230u, 0x08001231u, 0x08003200u, 0x08003200u, 10u));
    TEST_ASSERT_TRUE(panic_symbols_match(0x08001230u, 0x08001230u, 0x08003200u, 0x08003200u, 10u));
}

void test_match_rejects_a_table_from_another_build(void)
{
    TEST_ASSERT_FALSE(panic_symbols_match(0x08001230u, 0x08001330u, 0x08003200u, 0x08003200u, 10u));
}

void test_match_rejects_an_image_whose_code_size_changed(void)
{
    /* cpu_panic did not move but code after it did: the end of the code differs. */
    TEST_ASSERT_FALSE(panic_symbols_match(0x08001230u, 0x08001231u, 0x08003200u, 0x08003240u, 10u));
}

void test_match_rejects_an_empty_table_and_missing_anchors(void)
{
    TEST_ASSERT_FALSE(panic_symbols_match(0x08001230u, 0x08001231u, 0x08003200u, 0x08003200u, 0u));
    TEST_ASSERT_FALSE(panic_symbols_match(0u, 0u, 0x08003200u, 0x08003200u, 10u));
    TEST_ASSERT_FALSE(panic_symbols_match(0x08001230u, 0x08001231u, 0u, 0u, 10u));
}
