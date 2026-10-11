/**
 * @file test_config_store_crc.c
 * @brief Unit tests for the ConfigStore CRC32/ISO-HDLC (Gateway, host, Unity).
 *
 * CS-T30..T32 from docs/lld/middleware/config-store-lld.md §16, plus the
 * incremental form that slot verification relies on.
 *
 * Runs under tests/project_gateway.yml because the Gateway and Field Device
 * ConfigStore share basenames. Build defines: STM32L475xx, BOARD_GATEWAY, TEST.
 */

#include "unity.h"

#include <stdint.h>
#include <string.h>

#include "config_store_crc.h"

void setUp(void)
{
}

void tearDown(void)
{
}

void test_CS_T30_check_value_of_the_standard_test_string(void)
{
    const uint8_t buf[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};

    TEST_ASSERT_EQUAL_HEX32(0xCBF43926UL, config_store_crc32(buf, 9U));
}

void test_CS_T31_all_ff_buffer_matches_the_pc_reference(void)
{
    uint8_t buf[256];

    (void) memset(buf, 0xFF, sizeof(buf));

    /* zlib.crc32(b"\xff" * 256) */
    TEST_ASSERT_EQUAL_HEX32(0xFEA8A821UL, config_store_crc32(buf, sizeof(buf)));
}

void test_CS_T32_null_buffer_with_zero_length_is_graceful(void)
{
    TEST_ASSERT_EQUAL_HEX32(0U, config_store_crc32(NULL, 0U));
}

void test_CS_T36_incremental_feed_equals_the_single_call(void)
{
    uint8_t buf[1000];
    uint32_t running;

    for (uint32_t i = 0U; i < sizeof(buf); i++)
    {
        buf[i] = (uint8_t) (i * 7U);
    }

    running = config_store_crc32_begin();
    running = config_store_crc32_feed(running, &buf[0], 1U);
    running = config_store_crc32_feed(running, &buf[1], 255U);
    running = config_store_crc32_feed(running, &buf[256], 744U);

    TEST_ASSERT_EQUAL_HEX32(config_store_crc32(buf, sizeof(buf)), config_store_crc32_end(running));
}

void test_CS_T37_feeding_nothing_leaves_the_running_value_alone(void)
{
    const uint32_t start = config_store_crc32_begin();

    TEST_ASSERT_EQUAL_HEX32(start, config_store_crc32_feed(start, NULL, 0U));
    TEST_ASSERT_EQUAL_HEX32(start, config_store_crc32_feed(start, NULL, 16U));
}

void test_CS_T38_a_single_flipped_bit_changes_the_crc(void)
{
    uint8_t buf[64];

    (void) memset(buf, 0xA5, sizeof(buf));
    const uint32_t before = config_store_crc32(buf, sizeof(buf));

    buf[31] ^= 0x01U;

    TEST_ASSERT_NOT_EQUAL(before, config_store_crc32(buf, sizeof(buf)));
}
