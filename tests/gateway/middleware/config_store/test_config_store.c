/**
 * @file test_config_store.c
 * @brief Unit tests for ConfigStore, Gateway build (host, Unity).
 *
 * Test IDs follow docs/lld/middleware/config-store-lld.md §16 (CS-T01..T29,
 * T33..T35; the CRC cases T30..T32 are in test_config_store_crc.c). CS-T40
 * onward cover guards the plan does not name.
 *
 * Mocking strategy:
 *   - QspiFlashDriver -> an iqspi_flash_t vtable over a 64 KB RAM array that
 *     obeys the real driver's rules (256-byte pages, no page crossing, AND-only
 *     programming, 4 KB sector erase) with fault injection by address
 *   - HealthMonitor   -> health_monitor_push_event() is a spy defined below; the
 *                        test never includes health_monitor.h directly, so the
 *                        real module is not linked
 *   - FreeRTOS        -> freertos_mock.h
 *   - CRC             -> the real config_store_crc.c; slot images in the tests
 *                        are built independently of config_store_save()
 *
 * Runs under tests/project_gateway.yml because the Gateway and Field Device
 * ConfigStore share basenames. Build defines: STM32L475xx, BOARD_GATEWAY, TEST
 * (:test_config_store:).
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "freertos_mock.h" /* causes Ceedling to auto-link freertos_mock.c */

#include "config_store.h"
#include "config_store_config.h"
#include "config_store_crc.h" /* causes Ceedling to auto-link config_store_crc.c */

/* ====================================================================== */
/* RAM flash behind the iqspi_flash_t vtable                              */
/* ====================================================================== */

#define SIM_SIZE (CONFIG_STORE_PARTITION_SIZE)
#define NO_ADDR (0xFFFFFFFFUL)

static uint8_t s_flash_sim[SIM_SIZE];

static uint32_t s_read_calls;
static uint32_t s_write_calls;
static uint32_t s_erase_calls;
static uint32_t s_rule_violations; /* a call that the real driver would reject */

static uint32_t s_fail_read_addr; /* a read STARTING here fails              */
static uint32_t s_fail_read_lo;   /* every read overlapping [lo, hi) fails   */
static uint32_t s_fail_read_hi;
static uint32_t s_fail_write_addr; /* a program starting here fails           */
static uint32_t s_fail_erase_addr; /* an erase of this sector fails           */
static bool s_fail_all_reads;

static qspi_flash_err_t fake_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    s_read_calls++;

    if (s_fail_all_reads || (addr == s_fail_read_addr) ||
        ((addr < s_fail_read_hi) && ((addr + len) > s_fail_read_lo)))
    {
        return QSPI_FLASH_ERR_TIMEOUT;
    }

    if ((len == 0U) || ((addr + len) > SIM_SIZE))
    {
        s_rule_violations++;
        return QSPI_FLASH_ERR_ADDR;
    }

    (void) memcpy(buf, &s_flash_sim[addr], len);

    return QSPI_FLASH_OK;
}

static qspi_flash_err_t fake_write_page(uint32_t addr, const uint8_t *data, uint16_t len)
{
    s_write_calls++;

    if (addr == s_fail_write_addr)
    {
        return QSPI_FLASH_ERR_TIMEOUT;
    }

    /* The real driver: 1..256 bytes, all within one 256-byte page. */
    if ((len == 0U) || (len > QSPI_FLASH_PAGE_SIZE_BYTES) ||
        ((addr / QSPI_FLASH_PAGE_SIZE_BYTES) != ((addr + len - 1U) / QSPI_FLASH_PAGE_SIZE_BYTES)) ||
        ((addr + len) > SIM_SIZE))
    {
        s_rule_violations++;
        return QSPI_FLASH_ERR_LEN;
    }

    for (uint16_t i = 0U; i < len; i++)
    {
        s_flash_sim[addr + i] &= data[i]; /* NOR: programming only clears bits */
    }

    return QSPI_FLASH_OK;
}

static qspi_flash_err_t fake_erase_sector(uint32_t addr)
{
    s_erase_calls++;

    if ((addr & ~(QSPI_FLASH_SECTOR_SIZE_BYTES - 1U)) == s_fail_erase_addr)
    {
        return QSPI_FLASH_ERR_TIMEOUT;
    }

    if (addr >= SIM_SIZE)
    {
        s_rule_violations++;
        return QSPI_FLASH_ERR_ADDR;
    }

    uint32_t base = addr & ~(QSPI_FLASH_SECTOR_SIZE_BYTES - 1U);

    (void) memset(&s_flash_sim[base], 0xFF, QSPI_FLASH_SECTOR_SIZE_BYTES);

    return QSPI_FLASH_OK;
}

static const iqspi_flash_t s_fake_flash = {
    .read = fake_read,
    .write_page = fake_write_page,
    .erase_sector = fake_erase_sector,
};

/* ====================================================================== */
/* HealthMonitor spy                                                      */
/* ====================================================================== */

#define EVENT_SPY_MAX (8U)

static uint32_t s_event_count;
static health_event_t s_event_ids[EVENT_SPY_MAX];
static uint32_t s_event_params[EVENT_SPY_MAX];

health_monitor_err_t health_monitor_push_event(health_monitor_handle_t handle, health_event_t event,
                                               uint32_t param)
{
    (void) handle;

    if (s_event_count < EVENT_SPY_MAX)
    {
        s_event_ids[s_event_count] = event;
        s_event_params[s_event_count] = param;
    }

    s_event_count++;

    return HEALTH_MONITOR_ERR_OK;
}

static void expect_one_event(health_event_t event, uint32_t param)
{
    TEST_ASSERT_EQUAL_UINT32(1U, s_event_count);
    TEST_ASSERT_EQUAL(event, s_event_ids[0]);
    TEST_ASSERT_EQUAL_HEX32(param, s_event_params[0]);
}

/* ====================================================================== */
/* Slot image helpers — independent of config_store_save()                */
/* ====================================================================== */

static void put_u32(uint8_t *dest, uint32_t value)
{
    dest[0] = (uint8_t) (value & 0xFFU);
    dest[1] = (uint8_t) ((value >> 8U) & 0xFFU);
    dest[2] = (uint8_t) ((value >> 16U) & 0xFFU);
    dest[3] = (uint8_t) ((value >> 24U) & 0xFFU);
}

static uint32_t get_u32(const uint8_t *src)
{
    return (uint32_t) src[0] | ((uint32_t) src[1] << 8U) | ((uint32_t) src[2] << 16U) |
           ((uint32_t) src[3] << 24U);
}

static uint8_t *slot_ptr(uint8_t slot)
{
    return &s_flash_sim[(uint32_t) slot * CONFIG_STORE_SLOT_SIZE];
}

/** @brief Lay a complete valid slot into the sim: header, data, 0xFF fill, CRC. */
static void write_valid_slot(uint8_t slot, uint32_t seq, const uint8_t *data, uint32_t len)
{
    uint8_t *p = slot_ptr(slot);

    (void) memset(p, 0xFF, CONFIG_STORE_SLOT_SIZE);
    put_u32(&p[0], CONFIG_STORE_MAGIC);
    put_u32(&p[4], seq);
    put_u32(&p[8], len);
    put_u32(&p[12], 0U);
    (void) memcpy(&p[CONFIG_STORE_HEADER_SIZE], data, len);
    put_u32(&p[CONFIG_STORE_CRC_OFFSET], config_store_crc32(p, CONFIG_STORE_CRC_OFFSET));
}

/** @brief Clear bits in the stored CRC, as an interrupted commit would leave it. */
static void corrupt_slot_crc(uint8_t slot)
{
    slot_ptr(slot)[CONFIG_STORE_CRC_OFFSET] &= 0x0FU;
    slot_ptr(slot)[CONFIG_STORE_CRC_OFFSET + 1U] = 0x00U;
}

static uint32_t slot_seq(uint8_t slot)
{
    return get_u32(&slot_ptr(slot)[4]);
}

/** @brief True if the slot's stored CRC matches its contents. */
static bool slot_crc_ok(uint8_t slot)
{
    const uint8_t *p = slot_ptr(slot);

    return get_u32(&p[CONFIG_STORE_CRC_OFFSET]) == config_store_crc32(p, CONFIG_STORE_CRC_OFFSET);
}

static void fill_pattern(uint8_t *buf, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0U; i < len; i++)
    {
        buf[i] = (uint8_t) (seed + (uint8_t) (i * 3U));
    }
}

/* ====================================================================== */
/* Fixture                                                                */
/* ====================================================================== */

static config_store_handle_t s_h;
static config_store_config_t s_cfg;
static uint8_t s_blob1[600];
static uint8_t s_blob2[1500];
static uint8_t s_out[CONFIG_STORE_MAX_DATA_BYTES];

void setUp(void)
{
    mock_freertos_reset();
    config_store_reset_for_test();

    (void) memset(s_flash_sim, 0xFF, sizeof(s_flash_sim));
    s_read_calls = 0U;
    s_write_calls = 0U;
    s_erase_calls = 0U;
    s_rule_violations = 0U;
    s_fail_read_addr = NO_ADDR;
    s_fail_read_lo = 0U;
    s_fail_read_hi = 0U;
    s_fail_write_addr = NO_ADDR;
    s_fail_erase_addr = NO_ADDR;
    s_fail_all_reads = false;

    s_event_count = 0U;
    (void) memset(s_event_ids, 0, sizeof(s_event_ids));
    (void) memset(s_event_params, 0, sizeof(s_event_params));

    s_h = NULL;
    s_cfg.flash = &s_fake_flash;
    s_cfg.health = (health_monitor_handle_t) 1;

    fill_pattern(s_blob1, sizeof(s_blob1), 0x11U);
    fill_pattern(s_blob2, sizeof(s_blob2), 0x55U);
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, s_rule_violations);
}

static void create_ok(void)
{
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_create(&s_cfg, &s_h));
    TEST_ASSERT_NOT_NULL(s_h);
}

static void load_expect(const uint8_t *expected, uint32_t len)
{
    uint32_t got = 0U;

    (void) memset(s_out, 0, 64U);
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_load(s_h, s_out, &got, sizeof(s_out)));
    TEST_ASSERT_EQUAL_UINT32(len, got);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, s_out, len);
}

/* ====================================================================== */
/* Group 1 — create / lifecycle                                           */
/* ====================================================================== */

void test_CS_T01_create_succeeds_without_touching_flash(void)
{
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_create(&s_cfg, &s_h));
    TEST_ASSERT_NOT_NULL(s_h);
    TEST_ASSERT_EQUAL_UINT32(0U, s_read_calls + s_write_calls + s_erase_calls);
}

void test_CS_T02_create_null_config_is_rejected(void)
{
    config_store_handle_t untouched = (config_store_handle_t) 0x1234;

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NULL_PTR, config_store_create(NULL, &untouched));
    TEST_ASSERT_EQUAL_PTR(0x1234, untouched);
}

void test_CS_T03_create_null_handle_is_rejected(void)
{
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NULL_PTR, config_store_create(&s_cfg, NULL));
}

void test_CS_T04_second_create_exhausts_the_pool(void)
{
    config_store_handle_t second = NULL;

    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NO_RESOURCE, config_store_create(&s_cfg, &second));
    TEST_ASSERT_NULL(second);
}

void test_CS_T05_load_null_handle_is_rejected(void)
{
    uint32_t len = 0U;

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NULL_PTR, config_store_load(NULL, s_out, &len, 16U));
}

/* ====================================================================== */
/* Group 2 — load, happy path                                             */
/* ====================================================================== */

void test_CS_T06_load_takes_slot_a_when_slot_b_is_erased(void)
{
    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    create_ok();

    load_expect(s_blob1, sizeof(s_blob1));
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
}

void test_CS_T07_load_prefers_the_higher_sequence_in_slot_b(void)
{
    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    write_valid_slot(1U, 2U, s_blob2, sizeof(s_blob2));
    create_ok();

    load_expect(s_blob2, sizeof(s_blob2));
}

void test_CS_T08_load_prefers_the_higher_sequence_in_slot_a(void)
{
    write_valid_slot(0U, 3U, s_blob1, sizeof(s_blob1));
    write_valid_slot(1U, 2U, s_blob2, sizeof(s_blob2));
    create_ok();

    load_expect(s_blob1, sizeof(s_blob1));
}

/* ====================================================================== */
/* Group 3 — load, error paths                                            */
/* ====================================================================== */

void test_CS_T09_load_with_both_slots_erased_reports_never_written(void)
{
    uint32_t len = 0U;

    create_ok();

    /* A fresh device is not a fault: its own code, no health event (LC-O14, D-1). */
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NEVER_WRITTEN, config_store_load(s_h, s_out, &len, 64U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
}

void test_CS_T10_load_falls_back_to_the_valid_slot_when_the_other_crc_is_bad(void)
{
    write_valid_slot(0U, 2U, s_blob1, sizeof(s_blob1));
    write_valid_slot(1U, 1U, s_blob2, sizeof(s_blob2));
    corrupt_slot_crc(0U);
    create_ok();

    load_expect(s_blob2, sizeof(s_blob2));
    /* A single bad slot is expected after an interrupted write: no event. */
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
}

void test_CS_T11_load_with_both_crcs_bad_reports_no_valid_slot(void)
{
    uint32_t len = 0U;

    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    write_valid_slot(1U, 2U, s_blob2, sizeof(s_blob2));
    corrupt_slot_crc(0U);
    corrupt_slot_crc(1U);
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NO_VALID_SLOT, config_store_load(s_h, s_out, &len, 4096U));
    expect_one_event(HEALTH_EVENT_FAULT, FAULT_CONFIG_NO_VALID_SLOT);
}

void test_CS_T12_load_into_a_too_small_buffer_is_rejected_without_an_event(void)
{
    uint32_t len = 0U;

    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_TOO_LARGE, config_store_load(s_h, s_out, &len, 16U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
}

void test_CS_T13_load_when_no_slot_can_be_read_reports_a_read_failure(void)
{
    uint32_t len = 0U;

    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    create_ok();
    s_fail_all_reads = true;

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_FLASH_READ, config_store_load(s_h, s_out, &len, 4096U));
    expect_one_event(HEALTH_EVENT_FAULT, FAULT_CONFIG_READ_FAIL);
}

void test_CS_T14_load_null_data_buffer_is_rejected(void)
{
    uint32_t len = 0U;

    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NULL_PTR, config_store_load(s_h, NULL, &len, 64U));
}

void test_CS_T15_load_null_length_is_rejected(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NULL_PTR, config_store_load(s_h, s_out, NULL, 64U));
}

/* ====================================================================== */
/* Group 4 — save, happy path                                             */
/* ====================================================================== */

void test_CS_T16_first_save_goes_to_slot_a_with_sequence_one_and_a_valid_crc(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_save(s_h, s_blob1, sizeof(s_blob1)));

    TEST_ASSERT_EQUAL_HEX32(CONFIG_STORE_MAGIC, get_u32(&slot_ptr(0U)[0]));
    TEST_ASSERT_EQUAL_UINT32(1U, slot_seq(0U));
    TEST_ASSERT_EQUAL_UINT32(sizeof(s_blob1), get_u32(&slot_ptr(0U)[8]));
    TEST_ASSERT_EQUAL_UINT32(0U, get_u32(&slot_ptr(0U)[12]));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_blob1, &slot_ptr(0U)[CONFIG_STORE_HEADER_SIZE],
                                  sizeof(s_blob1));
    TEST_ASSERT_TRUE(slot_crc_ok(0U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
}

void test_CS_T17_second_save_goes_to_slot_b_and_leaves_slot_a_alone(void)
{
    uint8_t before[CONFIG_STORE_SLOT_SIZE];

    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    (void) memcpy(before, slot_ptr(0U), sizeof(before));
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_save(s_h, s_blob2, sizeof(s_blob2)));

    TEST_ASSERT_EQUAL_UINT32(2U, slot_seq(1U));
    TEST_ASSERT_TRUE(slot_crc_ok(1U));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(before, slot_ptr(0U), sizeof(before));
}

void test_CS_T18_load_after_the_second_save_returns_the_new_blob(void)
{
    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    create_ok();
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_save(s_h, s_blob2, sizeof(s_blob2)));

    load_expect(s_blob2, sizeof(s_blob2));
}

/* ====================================================================== */
/* Group 5 — save, error paths                                            */
/* ====================================================================== */

void test_CS_T19_save_null_data_is_rejected(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NULL_PTR, config_store_save(s_h, NULL, 16U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_erase_calls);
}

void test_CS_T20_save_over_the_maximum_is_rejected_before_flash(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_TOO_LARGE,
                      config_store_save(s_h, s_out, CONFIG_STORE_MAX_DATA_BYTES + 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_erase_calls + s_write_calls);
}

void test_CS_T21_erase_failure_keeps_the_active_slot_and_raises_write_fail(void)
{
    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    create_ok();
    s_fail_erase_addr = CONFIG_STORE_SLOT_SIZE; /* first sector of slot B */

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_FLASH_ERASE,
                      config_store_save(s_h, s_blob2, sizeof(s_blob2)));

    expect_one_event(HEALTH_EVENT_CONFIG_WRITE_FAIL, 0U);
    s_fail_erase_addr = NO_ADDR;
    load_expect(s_blob1, sizeof(s_blob1));
}

void test_CS_T22_header_program_failure_keeps_the_active_slot(void)
{
    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    create_ok();
    s_fail_write_addr = CONFIG_STORE_SLOT_SIZE; /* slot B header */

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_FLASH_WRITE,
                      config_store_save(s_h, s_blob2, sizeof(s_blob2)));

    expect_one_event(HEALTH_EVENT_CONFIG_WRITE_FAIL, 0U);
    s_fail_write_addr = NO_ADDR;
    load_expect(s_blob1, sizeof(s_blob1));
}

void test_CS_T23_crc_commit_failure_leaves_a_slot_that_is_never_chosen(void)
{
    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    create_ok();
    s_fail_write_addr = CONFIG_STORE_SLOT_SIZE + CONFIG_STORE_CRC_OFFSET;

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_FLASH_WRITE,
                      config_store_save(s_h, s_blob2, sizeof(s_blob2)));

    expect_one_event(HEALTH_EVENT_CONFIG_WRITE_FAIL, 0U);

    /* Slot B has a newer header and all its data, but no CRC: it must lose. */
    TEST_ASSERT_EQUAL_UINT32(2U, slot_seq(1U));
    TEST_ASSERT_FALSE(slot_crc_ok(1U));
    s_fail_write_addr = NO_ADDR;
    load_expect(s_blob1, sizeof(s_blob1));
}

/* ====================================================================== */
/* Group 6 — check integrity                                              */
/* ====================================================================== */

void test_CS_T24_integrity_is_ok_with_one_valid_slot(void)
{
    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_check_integrity(s_h));
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
}

void test_CS_T25_integrity_is_ok_with_both_slots_valid(void)
{
    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    write_valid_slot(1U, 2U, s_blob2, sizeof(s_blob2));
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_check_integrity(s_h));
}

void test_CS_T26_integrity_with_no_valid_slot_reports_a_fault(void)
{
    /* Written once and damaged: "something is there", so a fault. */
    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    corrupt_slot_crc(0U);
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NO_VALID_SLOT, config_store_check_integrity(s_h));
    expect_one_event(HEALTH_EVENT_FAULT, FAULT_CONFIG_NO_VALID_SLOT);
}

void test_CS_T26b_integrity_on_a_fresh_store_is_never_written_without_a_fault(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NEVER_WRITTEN, config_store_check_integrity(s_h));
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
}

void test_CS_T26c_one_erased_and_one_damaged_slot_is_damaged_not_never_written(void)
{
    /* D-2: an interrupted first write (A damaged, B still erased) reads as damaged. */
    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    corrupt_slot_crc(0U);
    create_ok();

    uint32_t len = 0U;
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NO_VALID_SLOT, config_store_load(s_h, s_out, &len, 64U));
}

/* ====================================================================== */
/* Group 7 — erase (factory reset)                                        */
/* ====================================================================== */

void test_CS_T27_erase_clears_all_sixteen_sectors(void)
{
    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    write_valid_slot(1U, 2U, s_blob2, sizeof(s_blob2));
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_erase(s_h));

    TEST_ASSERT_EQUAL_UINT32(16U, s_erase_calls);
    for (uint32_t i = 0U; i < SIM_SIZE; i++)
    {
        if (s_flash_sim[i] != 0xFFU)
        {
            TEST_FAIL_MESSAGE("flash not fully erased");
        }
    }
}

void test_CS_T28_load_after_erase_reports_never_written(void)
{
    uint32_t len = 0U;

    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    create_ok();
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_erase(s_h));

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NEVER_WRITTEN, config_store_load(s_h, s_out, &len, 4096U));
}

void test_CS_T29_erase_failure_is_reported(void)
{
    create_ok();
    s_fail_erase_addr = 5U * CONFIG_STORE_SECTOR_SIZE;

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_FLASH_ERASE, config_store_erase(s_h));
    expect_one_event(HEALTH_EVENT_CONFIG_WRITE_FAIL, 0U);
}

/* ====================================================================== */
/* Group 9 — round trips                                                  */
/* ====================================================================== */

void test_CS_T33_save_load_save_load_rotates_the_slots(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_save(s_h, s_blob1, sizeof(s_blob1)));
    load_expect(s_blob1, sizeof(s_blob1));
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_save(s_h, s_blob2, sizeof(s_blob2)));
    load_expect(s_blob2, sizeof(s_blob2));

    TEST_ASSERT_EQUAL_UINT32(1U, slot_seq(0U));
    TEST_ASSERT_EQUAL_UINT32(2U, slot_seq(1U));
}

void test_CS_T34_a_corrupted_new_slot_falls_back_to_the_previous_blob(void)
{
    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    create_ok();
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_save(s_h, s_blob2, sizeof(s_blob2)));

    corrupt_slot_crc(1U);

    load_expect(s_blob1, sizeof(s_blob1));
}

/* ====================================================================== */
/* Group 10 — reset for test                                              */
/* ====================================================================== */

void test_CS_T35_reset_for_test_frees_the_pool(void)
{
    config_store_handle_t again = NULL;

    create_ok();
    config_store_reset_for_test();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_create(&s_cfg, &again));
}

/* ====================================================================== */
/* Added during implementation                                            */
/* ====================================================================== */

void test_CS_T40_blobs_of_every_awkward_length_round_trip_with_page_aligned_writes(void)
{
    static const uint32_t lens[] = {1U,   15U,  16U,  17U,   239U,  240U,  241U,
                                    255U, 256U, 257U, 4000U, 4080U, 4081U, 32711U};

    create_ok();

    for (uint32_t i = 0U; i < (sizeof(lens) / sizeof(lens[0])); i++)
    {
        fill_pattern(s_out, lens[i], (uint8_t) (i + 1U));
        TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_save(s_h, s_out, lens[i]));

        uint8_t expected[CONFIG_STORE_MAX_DATA_BYTES];
        uint32_t got = 0U;

        fill_pattern(expected, lens[i], (uint8_t) (i + 1U));
        TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_load(s_h, s_out, &got, sizeof(s_out)));
        TEST_ASSERT_EQUAL_UINT32(lens[i], got);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, s_out, lens[i]);
    }
    /* tearDown asserts the fake saw no page-boundary or range violation. */
}

void test_CS_T41_the_maximum_blob_fits_and_its_crc_covers_the_padding(void)
{
    static uint8_t big[CONFIG_STORE_MAX_DATA_BYTES];

    fill_pattern(big, sizeof(big), 0x2AU);
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_save(s_h, big, sizeof(big)));

    TEST_ASSERT_TRUE(slot_crc_ok(0U));
    load_expect(big, sizeof(big));
}

void test_CS_T42_a_header_claiming_an_impossible_length_makes_the_slot_invalid(void)
{
    write_valid_slot(0U, 9U, s_blob1, sizeof(s_blob1));
    put_u32(&slot_ptr(0U)[8], CONFIG_STORE_MAX_DATA_BYTES + 1U);
    put_u32(&slot_ptr(0U)[CONFIG_STORE_CRC_OFFSET],
            config_store_crc32(slot_ptr(0U), CONFIG_STORE_CRC_OFFSET)); /* CRC still valid */
    write_valid_slot(1U, 1U, s_blob2, sizeof(s_blob2));
    create_ok();

    load_expect(s_blob2, sizeof(s_blob2));
}

void test_CS_T43_one_unreadable_slot_does_not_stop_the_other_being_used(void)
{
    write_valid_slot(0U, 2U, s_blob1, sizeof(s_blob1));
    write_valid_slot(1U, 1U, s_blob2, sizeof(s_blob2));
    create_ok();
    s_fail_read_lo = 0U;
    s_fail_read_hi = CONFIG_STORE_SLOT_SIZE; /* every read touching slot A fails */

    load_expect(s_blob2, sizeof(s_blob2));
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
}

void test_CS_T44_a_failed_data_read_after_validation_raises_a_read_fault(void)
{
    uint32_t len = 0U;

    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    create_ok();
    s_fail_read_addr = CONFIG_STORE_HEADER_SIZE; /* only the final data read starts here */

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_FLASH_READ, config_store_load(s_h, s_out, &len, 4096U));
    expect_one_event(HEALTH_EVENT_FAULT, FAULT_CONFIG_READ_FAIL);
}

void test_CS_T45_equal_sequence_numbers_resolve_to_slot_a(void)
{
    write_valid_slot(0U, 5U, s_blob1, sizeof(s_blob1));
    write_valid_slot(1U, 5U, s_blob2, sizeof(s_blob2));
    create_ok();

    load_expect(s_blob1, sizeof(s_blob1));
}

void test_CS_T46_saves_alternate_a_b_a_with_rising_sequence(void)
{
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_save(s_h, s_blob1, 100U));
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_save(s_h, s_blob2, 200U));
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_save(s_h, s_blob1, 300U));

    TEST_ASSERT_EQUAL_UINT32(3U, slot_seq(0U)); /* third save is back in slot A */
    TEST_ASSERT_EQUAL_UINT32(2U, slot_seq(1U));
    load_expect(s_blob1, 300U);
}

void test_CS_T47_save_with_no_valid_slot_restarts_at_sequence_one_in_slot_a(void)
{
    write_valid_slot(0U, 7U, s_blob1, 64U);
    corrupt_slot_crc(0U);
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_save(s_h, s_blob2, 64U));

    TEST_ASSERT_EQUAL_UINT32(1U, slot_seq(0U));
    TEST_ASSERT_TRUE(slot_crc_ok(0U));
}

void test_CS_T48_without_a_health_handle_failures_are_still_reported_quietly(void)
{
    s_cfg.health = NULL;
    create_ok();
    s_fail_erase_addr = 0U;

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_FLASH_ERASE,
                      config_store_save(s_h, s_blob1, sizeof(s_blob1)));
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
}

void test_CS_T49_every_lock_is_released(void)
{
    uint32_t len = 0U;

    create_ok();
    g_mock_xSemaphoreTake_call_count = 0U;
    g_mock_xSemaphoreGive_call_count = 0U;

    (void) config_store_check_integrity(s_h);
    (void) config_store_load(s_h, s_out, &len, 64U);
    (void) config_store_save(s_h, s_blob1, 64U);
    (void) config_store_save(s_h, s_blob1, CONFIG_STORE_MAX_DATA_BYTES + 1U);
    (void) config_store_load(s_h, s_out, &len, 4U);
    s_fail_erase_addr = 0U;
    (void) config_store_save(s_h, s_blob2, 64U);
    s_fail_erase_addr = NO_ADDR;
    (void) config_store_erase(s_h);

    TEST_ASSERT_TRUE(g_mock_xSemaphoreTake_call_count > 0U);
    TEST_ASSERT_EQUAL_UINT32(g_mock_xSemaphoreTake_call_count, g_mock_xSemaphoreGive_call_count);
}

void test_CS_T50_create_reports_a_mutex_failure_and_keeps_the_pool_free(void)
{
    g_mock_xSemaphoreCreateMutexStatic_return = NULL;

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NO_RESOURCE, config_store_create(&s_cfg, &s_h));
    TEST_ASSERT_NULL(s_h);

    g_mock_xSemaphoreCreateMutexStatic_return = (SemaphoreHandle_t) 1;
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_create(&s_cfg, &s_h));
}

void test_CS_T51_create_needs_a_flash_interface(void)
{
    s_cfg.flash = NULL;

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NULL_PTR, config_store_create(&s_cfg, &s_h));
}

void test_CS_T52_every_operation_rejects_a_stale_handle(void)
{
    uint32_t len = 0U;

    create_ok();
    config_store_handle_t stale = s_h;

    config_store_reset_for_test();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NOT_INIT, config_store_load(stale, s_out, &len, 64U));
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NOT_INIT, config_store_check_integrity(stale));
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NOT_INIT, config_store_save(stale, s_blob1, 16U));
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NOT_INIT, config_store_erase(stale));
}

void test_CS_T53_every_operation_rejects_a_null_handle(void)
{
    uint32_t len = 0U;

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NULL_PTR, config_store_check_integrity(NULL));
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NULL_PTR, config_store_save(NULL, s_blob1, 16U));
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NULL_PTR, config_store_erase(NULL));
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NULL_PTR, config_store_load(NULL, s_out, &len, 16U));
}

void test_CS_T54_a_zero_length_blob_round_trips(void)
{
    uint32_t len = 99U;

    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_save(s_h, s_blob1, 0U));
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_load(s_h, s_out, &len, sizeof(s_out)));
    TEST_ASSERT_EQUAL_UINT32(0U, len);
}

void test_CS_T55_integrity_check_does_not_alter_the_stored_data(void)
{
    uint8_t before[SIM_SIZE];

    write_valid_slot(0U, 1U, s_blob1, sizeof(s_blob1));
    write_valid_slot(1U, 2U, s_blob2, sizeof(s_blob2));
    (void) memcpy(before, s_flash_sim, sizeof(before));
    create_ok();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_OK, config_store_check_integrity(s_h));

    TEST_ASSERT_EQUAL_UINT32(0U, s_write_calls + s_erase_calls);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(before, s_flash_sim, sizeof(before));
}
