/**
 * @file test_internal_flash.c
 * @brief Unit tests for the InternalFlashDriver, Gateway build (host, Unity).
 *
 * Test IDs follow docs/lld/drivers/internal-flash-driver.md §7 (TC-IFD-001..031).
 *
 * Mocking strategy: the whole register and flash-bus seam (internal_flash_hw.h) is a model of
 * the STM32L475 flash controller, fake_flash_ctrl. It implements the unlock sequence,
 * PG/PER/START, BSY, EOP and the error flags, programs a double word only onto erased
 * flash, and records every register write, so the tests assert the real programming
 * sequence and not only return codes.
 *
 * Build defines: STM32L475xx, BOARD_GATEWAY, TEST (:test_internal_flash:).
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "fake_flash_ctrl.h" /* auto-links fake_flash_ctrl.c */
#include "internal_flash.h"
#include "internal_flash_hw.h" /* the IFLASH_* register masks; no .c of this name */

#define PAGE (INTERNAL_FLASH_PAGE_SIZE_BYTES)
#define BANK1_PAGE20 (INTERNAL_FLASH_BASE_ADDR + (20UL * PAGE))
#define BANK2_FIRST (INTERNAL_FLASH_BANK2_ADDR)
#define FLASH_END (INTERNAL_FLASH_BASE_ADDR + INTERNAL_FLASH_SIZE_BYTES)

void setUp(void)
{
    fake_flash_reset();
    internal_flash_reset_for_test();
}

void tearDown(void)
{
}

static void start(void)
{
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_init());
}

/** @brief Number of recorded accesses of one kind. */
static uint32_t count_kind(fake_flash_event_kind_t kind)
{
    uint32_t n = 0U;

    for (uint32_t i = 0U; i < fake_flash_event_count(); i++)
    {
        if (fake_flash_event_at(i)->kind == kind)
        {
            n++;
        }
    }

    return n;
}

/** @brief The n-th recorded access of one kind. */
static const fake_flash_event_t *nth_of_kind(fake_flash_event_kind_t kind, uint32_t n)
{
    for (uint32_t i = 0U; i < fake_flash_event_count(); i++)
    {
        if ((fake_flash_event_at(i)->kind == kind) && (n-- == 0U))
        {
            return fake_flash_event_at(i);
        }
    }

    TEST_FAIL_MESSAGE("expected register access not recorded");

    return NULL;
}

static void fill(uint32_t addr, uint32_t len, uint8_t value)
{
    (void) memset(fake_flash_mem(addr), value, len);
}

static void assert_all(uint32_t addr, uint32_t len, uint8_t value)
{
    const uint8_t *p = fake_flash_mem(addr);

    for (uint32_t i = 0U; i < len; i++)
    {
        TEST_ASSERT_EQUAL_HEX8(value, p[i]);
    }
}

static void assert_clean_exit(void)
{
    TEST_ASSERT_TRUE(fake_flash_is_locked());
    TEST_ASSERT_EQUAL_HEX32(0U, fake_flash_sr_value() & (IFLASH_SR_ERRORS | IFLASH_SR_EOP));
    TEST_ASSERT_EQUAL_HEX32(IFLASH_CR_LOCK, fake_flash_cr_value());
}

/* ====================================================================== */
/* init, state and read                                                    */
/* ====================================================================== */

void test_TC_IFD_001_init_on_a_dual_bank_device_leaves_the_controller_locked(void)
{
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_init());
    TEST_ASSERT_TRUE(fake_flash_is_locked());

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_init());
    TEST_ASSERT_TRUE(fake_flash_is_locked());
    TEST_ASSERT_EQUAL_UINT32(0U, count_kind(FAKE_EVT_KEYR));
}

void test_TC_IFD_002_a_single_bank_device_is_refused(void)
{
    uint8_t buf[8];

    fake_flash_set_dual_bank(false);

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_BANK_MODE, internal_flash_init());
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_NOT_INITIALISED,
                          internal_flash_erase_page(BANK1_PAGE20));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_NOT_INITIALISED,
                          internal_flash_read(INTERNAL_FLASH_BASE_ADDR, buf, 8U));

    /* A later init on a corrected device recovers. */
    fake_flash_set_dual_bank(true);
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_init());
}

void test_TC_IFD_003_nothing_works_before_init(void)
{
    uint8_t data[8] = {0};

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_NOT_INITIALISED,
                          internal_flash_erase_page(BANK1_PAGE20));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_NOT_INITIALISED,
                          internal_flash_program(BANK1_PAGE20, data, 8U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_NOT_INITIALISED,
                          internal_flash_read(BANK1_PAGE20, data, 8U));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_flash_event_count());
}

void test_TC_IFD_004_read_copies_bytes_and_checks_its_range(void)
{
    uint8_t buf[16];

    start();

    for (uint32_t i = 0U; i < 16U; i++)
    {
        fake_flash_mem(0x08000100UL)[i] = (uint8_t) (0xA0U + i);
    }

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_read(0x08000100UL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY(fake_flash_mem(0x08000100UL), buf, sizeof(buf));

    /* The last byte of flash is readable; one past is not. */
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_read(FLASH_END - 1U, buf, 1U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ADDR, internal_flash_read(FLASH_END, buf, 1U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ADDR, internal_flash_read(FLASH_END - 1U, buf, 2U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ADDR,
                          internal_flash_read(INTERNAL_FLASH_BASE_ADDR - 1U, buf, 1U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ADDR,
                          internal_flash_read(INTERNAL_FLASH_BASE_ADDR, buf, 0U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ADDR,
                          internal_flash_read(INTERNAL_FLASH_BASE_ADDR, buf, 0xFFFFFFFFUL));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_NULL_POINTER,
                          internal_flash_read(INTERNAL_FLASH_BASE_ADDR, NULL, 1U));
}

/* ====================================================================== */
/* erase                                                                   */
/* ====================================================================== */

void test_TC_IFD_010_erase_follows_the_unlock_select_start_sequence(void)
{
    start();
    fill(BANK1_PAGE20, PAGE, 0x00U);

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_erase_page(BANK1_PAGE20));

    assert_all(BANK1_PAGE20, PAGE, 0xFFU);
    assert_clean_exit();

    /* KEY1 then KEY2. */
    TEST_ASSERT_EQUAL_UINT32(2U, count_kind(FAKE_EVT_KEYR));
    TEST_ASSERT_EQUAL_HEX32(IFLASH_KEY1, nth_of_kind(FAKE_EVT_KEYR, 0U)->value);
    TEST_ASSERT_EQUAL_HEX32(IFLASH_KEY2, nth_of_kind(FAKE_EVT_KEYR, 1U)->value);

    /* Select page 20 of bank 1 with PER, then START, then lock. */
    const uint32_t select = IFLASH_CR_PER | (20UL << IFLASH_CR_PNB_POS);

    TEST_ASSERT_EQUAL_UINT32(3U, count_kind(FAKE_EVT_CR));
    TEST_ASSERT_EQUAL_HEX32(select, nth_of_kind(FAKE_EVT_CR, 0U)->value);
    TEST_ASSERT_EQUAL_HEX32(select | IFLASH_CR_START, nth_of_kind(FAKE_EVT_CR, 1U)->value);
    TEST_ASSERT_EQUAL_HEX32(IFLASH_CR_LOCK, nth_of_kind(FAKE_EVT_CR, 2U)->value);
}

void test_TC_IFD_011_erase_selects_the_bank_from_the_address(void)
{
    static const struct
    {
        uint32_t addr;
        uint32_t cr_select;
    } k_case[] = {
        {INTERNAL_FLASH_WRITABLE_FROM_ADDR,
         IFLASH_CR_PER | (8UL << IFLASH_CR_PNB_POS)},                       /* bank 1, page 8 */
        {BANK2_FIRST - PAGE, IFLASH_CR_PER | (255UL << IFLASH_CR_PNB_POS)}, /* bank 1, page 255 */
        {BANK2_FIRST, IFLASH_CR_PER | IFLASH_CR_BKER},                      /* bank 2, page 0   */
        {FLASH_END - PAGE,
         IFLASH_CR_PER | IFLASH_CR_BKER | (255UL << IFLASH_CR_PNB_POS)}, /* bank 2, page 255 */
    };

    for (uint32_t i = 0U; i < (sizeof(k_case) / sizeof(k_case[0])); i++)
    {
        setUp();
        start();
        fill(k_case[i].addr, PAGE, 0x00U);

        TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_erase_page(k_case[i].addr));
        TEST_ASSERT_EQUAL_HEX32(k_case[i].cr_select, nth_of_kind(FAKE_EVT_CR, 0U)->value);
        assert_all(k_case[i].addr, PAGE, 0xFFU);
    }
}

void test_TC_IFD_012_erase_rejects_a_bad_address_without_touching_a_register(void)
{
    start();

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ALIGN, internal_flash_erase_page(BANK1_PAGE20 + 1U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ALIGN, internal_flash_erase_page(BANK1_PAGE20 + 8U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ADDR, internal_flash_erase_page(FLASH_END));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ADDR,
                          internal_flash_erase_page(INTERNAL_FLASH_BASE_ADDR - PAGE));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ALIGN,
                          internal_flash_erase_page(FLASH_END + 1U)); /* alignment first */
    TEST_ASSERT_EQUAL_UINT32(0U, fake_flash_event_count());
}

void test_TC_IFD_013_a_write_protected_page_is_an_erase_error(void)
{
    start();
    fill(BANK1_PAGE20, PAGE, 0x00U);
    fake_flash_set_protected_page(20U);

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ERASE, internal_flash_erase_page(BANK1_PAGE20));

    assert_clean_exit();
    assert_all(BANK1_PAGE20, PAGE, 0x00U); /* untouched */
}

void test_TC_IFD_014_an_erase_that_never_finishes_times_out(void)
{
    start();
    fake_flash_set_stuck_busy(true);

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ERASE, internal_flash_erase_page(BANK1_PAGE20));
    TEST_ASSERT_TRUE(fake_flash_is_locked() || (count_kind(FAKE_EVT_KEYR) == 0U));

    /* Busy before it even starts. */
    setUp();
    start();
    fake_flash_set_initial_busy(0xFFFFFFFFUL);
    fake_flash_set_stuck_busy(true);
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ERASE, internal_flash_erase_page(BANK1_PAGE20));
    TEST_ASSERT_EQUAL_UINT32(0U, count_kind(FAKE_EVT_KEYR)); /* never unlocked */
}

void test_TC_IFD_015_erase_waits_for_a_controller_that_is_busy_on_entry(void)
{
    start();
    fill(BANK1_PAGE20, PAGE, 0x00U);
    fake_flash_set_initial_busy(7U);

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_erase_page(BANK1_PAGE20));
    assert_all(BANK1_PAGE20, PAGE, 0xFFU);
    assert_clean_exit();
}

void test_TC_IFD_016_erase_flushes_the_caches_and_restores_their_enables(void)
{
    start();
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_erase_page(BANK1_PAGE20));

    TEST_ASSERT_EQUAL_UINT32(1U, fake_flash_cache_flushes());
    TEST_ASSERT_EQUAL_HEX32(IFLASH_ACR_ICEN | IFLASH_ACR_DCEN, fake_flash_acr_value());
}

void test_TC_IFD_017_stale_error_flags_do_not_fail_a_good_erase(void)
{
    start();
    fill(BANK1_PAGE20, PAGE, 0x00U);
    fake_flash_preset_sr(IFLASH_SR_PROGERR | IFLASH_SR_WRPERR | IFLASH_SR_EOP);

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_erase_page(BANK1_PAGE20));
    assert_all(BANK1_PAGE20, PAGE, 0xFFU);
    assert_clean_exit();
}

void test_TC_IFD_018_a_controller_that_will_not_unlock_is_an_erase_error(void)
{
    start();
    fill(BANK1_PAGE20, PAGE, 0x00U);
    fake_flash_set_stuck_lock(true);

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ERASE, internal_flash_erase_page(BANK1_PAGE20));

    TEST_ASSERT_TRUE(fake_flash_is_locked());
    TEST_ASSERT_EQUAL_UINT32(0U, count_kind(FAKE_EVT_CR)); /* no select, no start */
    assert_all(BANK1_PAGE20, PAGE, 0x00U);
}

/* ====================================================================== */
/* program                                                                 */
/* ====================================================================== */

void test_TC_IFD_020_one_double_word_is_two_consecutive_stores_under_pg(void)
{
    static const uint8_t k_data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

    start();

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK,
                          internal_flash_program(BANK1_PAGE20, k_data, sizeof(k_data)));
    TEST_ASSERT_EQUAL_MEMORY(k_data, fake_flash_mem(BANK1_PAGE20), sizeof(k_data));
    assert_clean_exit();

    /* CR: PG, then the lock that drops it. */
    TEST_ASSERT_EQUAL_UINT32(2U, count_kind(FAKE_EVT_CR));
    TEST_ASSERT_EQUAL_HEX32(IFLASH_CR_PG, nth_of_kind(FAKE_EVT_CR, 0U)->value);

    /* The low word first, then the high word at +4. */
    TEST_ASSERT_EQUAL_UINT32(2U, count_kind(FAKE_EVT_STORE));
    TEST_ASSERT_EQUAL_HEX32(BANK1_PAGE20, nth_of_kind(FAKE_EVT_STORE, 0U)->addr);
    TEST_ASSERT_EQUAL_HEX32(0x44332211UL, nth_of_kind(FAKE_EVT_STORE, 0U)->value);
    TEST_ASSERT_EQUAL_HEX32(BANK1_PAGE20 + 4U, nth_of_kind(FAKE_EVT_STORE, 1U)->addr);
    TEST_ASSERT_EQUAL_HEX32(0x88776655UL, nth_of_kind(FAKE_EVT_STORE, 1U)->value);
}

void test_TC_IFD_021_several_double_words_are_stored_in_ascending_order(void)
{
    uint8_t data[24];

    start();

    for (uint32_t i = 0U; i < sizeof(data); i++)
    {
        data[i] = (uint8_t) (i * 7U);
    }

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK,
                          internal_flash_program(BANK1_PAGE20, data, sizeof(data)));
    TEST_ASSERT_EQUAL_MEMORY(data, fake_flash_mem(BANK1_PAGE20), sizeof(data));

    TEST_ASSERT_EQUAL_UINT32(6U, count_kind(FAKE_EVT_STORE));

    for (uint32_t i = 0U; i < 6U; i++)
    {
        TEST_ASSERT_EQUAL_HEX32(BANK1_PAGE20 + (4U * i), nth_of_kind(FAKE_EVT_STORE, i)->addr);
    }
}

void test_TC_IFD_021b_the_data_pointer_need_not_be_aligned(void)
{
    uint8_t backing[16 + 1];

    start();

    for (uint32_t i = 0U; i < sizeof(backing); i++)
    {
        backing[i] = (uint8_t) (0x30U + i);
    }

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK,
                          internal_flash_program(BANK1_PAGE20, &backing[1], 16U));
    TEST_ASSERT_EQUAL_MEMORY(&backing[1], fake_flash_mem(BANK1_PAGE20), 16U);
}

void test_TC_IFD_022_program_rejects_bad_arguments_without_touching_a_register(void)
{
    uint8_t data[32] = {0};

    start();

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_NULL_POINTER,
                          internal_flash_program(BANK1_PAGE20, NULL, 8U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ALIGN,
                          internal_flash_program(BANK1_PAGE20 + 4U, data, 8U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ALIGN,
                          internal_flash_program(BANK1_PAGE20, data, 12U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ALIGN, internal_flash_program(BANK1_PAGE20, data, 0U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ADDR, internal_flash_program(FLASH_END, data, 8U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ADDR,
                          internal_flash_program(FLASH_END - 8U, data, 16U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ADDR,
                          internal_flash_program(INTERNAL_FLASH_BASE_ADDR - 8U, data, 8U));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_flash_event_count());
}

void test_TC_IFD_023_programming_a_double_word_twice_is_a_program_error(void)
{
    static const uint8_t k_old[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    static const uint8_t k_new[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

    start();
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_program(BANK1_PAGE20, k_old, 8U));

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_PROGRAM,
                          internal_flash_program(BANK1_PAGE20, k_new, 8U));

    TEST_ASSERT_EQUAL_MEMORY(k_old, fake_flash_mem(BANK1_PAGE20), 8U);
    assert_clean_exit();
}

void test_TC_IFD_024_programming_may_cross_the_bank_boundary(void)
{
    uint8_t data[16];

    start();

    for (uint32_t i = 0U; i < sizeof(data); i++)
    {
        data[i] = (uint8_t) (0xC0U + i);
    }

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK,
                          internal_flash_program(BANK2_FIRST - 8U, data, sizeof(data)));
    TEST_ASSERT_EQUAL_MEMORY(data, fake_flash_mem(BANK2_FIRST - 8U), sizeof(data));
}

void test_TC_IFD_025_a_program_that_cannot_start_is_a_program_error(void)
{
    static const uint8_t k_data[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    start();
    fake_flash_set_stuck_busy(true);

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_PROGRAM,
                          internal_flash_program(BANK1_PAGE20, k_data, 8U));
    TEST_ASSERT_EQUAL_UINT32(0U, count_kind(FAKE_EVT_STORE));
    TEST_ASSERT_TRUE(fake_flash_is_locked());
}

void test_TC_IFD_025b_a_controller_that_will_not_unlock_is_a_program_error(void)
{
    static const uint8_t k_data[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    start();
    fake_flash_set_stuck_lock(true);

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_PROGRAM,
                          internal_flash_program(BANK1_PAGE20, k_data, 8U));
    TEST_ASSERT_EQUAL_UINT32(0U, count_kind(FAKE_EVT_CR));
    assert_all(BANK1_PAGE20, 8U, 0xFFU);
}

void test_TC_IFD_026_programming_stops_at_the_first_failing_double_word(void)
{
    uint8_t data[24];

    start();
    (void) memset(data, 0x5A, sizeof(data));
    fill(BANK1_PAGE20 + 8U, 8U, 0x00U); /* the second double word is already programmed */

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_PROGRAM,
                          internal_flash_program(BANK1_PAGE20, data, sizeof(data)));

    assert_all(BANK1_PAGE20, 8U, 0x5AU);       /* written before the failure */
    assert_all(BANK1_PAGE20 + 8U, 8U, 0x00U);  /* the one that failed        */
    assert_all(BANK1_PAGE20 + 16U, 8U, 0xFFU); /* never attempted            */
    assert_clean_exit();
}

void test_TC_IFD_027_interrupts_are_masked_only_around_the_two_stores(void)
{
    uint8_t data[32];

    start();
    (void) memset(data, 0x3C, sizeof(data));

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK,
                          internal_flash_program(BANK1_PAGE20, data, sizeof(data)));

    TEST_ASSERT_EQUAL_UINT32(8U, fake_flash_stores_masked());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_flash_stores_unmasked());
    TEST_ASSERT_EQUAL_UINT32(4U, fake_flash_irq_pairs());
    TEST_ASSERT_FALSE(fake_flash_irq_is_masked());

    /* Erasing needs no masking at all. */
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_erase_page(BANK2_FIRST));
    TEST_ASSERT_EQUAL_UINT32(4U, fake_flash_irq_pairs());
}

void test_TC_IFD_028_program_flushes_the_caches(void)
{
    static const uint8_t k_data[8] = {0};

    start();
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_program(BANK1_PAGE20, k_data, 8U));

    TEST_ASSERT_EQUAL_UINT32(1U, fake_flash_cache_flushes());
    TEST_ASSERT_EQUAL_HEX32(IFLASH_ACR_ICEN | IFLASH_ACR_DCEN, fake_flash_acr_value());
}

void test_TC_IFD_029_all_zero_data_programs_onto_erased_flash(void)
{
    uint8_t zeros[16] = {0};

    start();

    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK,
                          internal_flash_program(BANK1_PAGE20, zeros, sizeof(zeros)));
    assert_all(BANK1_PAGE20, sizeof(zeros), 0x00U);
}

void test_TC_IFD_029b_an_erase_makes_a_programmed_page_programmable_again(void)
{
    static const uint8_t k_a[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    static const uint8_t k_b[8] = {2, 2, 2, 2, 2, 2, 2, 2};

    start();
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_program(BANK1_PAGE20, k_a, 8U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_erase_page(BANK1_PAGE20));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_program(BANK1_PAGE20, k_b, 8U));
    TEST_ASSERT_EQUAL_MEMORY(k_b, fake_flash_mem(BANK1_PAGE20), 8U);
}

/* ====================================================================== */
/* vtable and test hook                                                    */
/* ====================================================================== */

void test_TC_IFD_030_the_vtable_points_at_the_three_operations(void)
{
    TEST_ASSERT_NOT_NULL(internal_flash_driver);
    TEST_ASSERT_TRUE(internal_flash_driver->erase_page == internal_flash_erase_page);
    TEST_ASSERT_TRUE(internal_flash_driver->program == internal_flash_program);
    TEST_ASSERT_TRUE(internal_flash_driver->read == internal_flash_read);
}

void test_TC_IFD_031_the_reset_hook_returns_to_uninitialised(void)
{
    start();
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK, internal_flash_erase_page(BANK1_PAGE20));

    internal_flash_reset_for_test();
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_NOT_INITIALISED,
                          internal_flash_erase_page(BANK1_PAGE20));
}

void test_TC_IFD_032_erase_and_program_refuse_the_bootloader_pages(void)
{
    const uint8_t data[8] = {0};
    const uint32_t last_protected = INTERNAL_FLASH_WRITABLE_FROM_ADDR - PAGE;

    start();

    fill(INTERNAL_FLASH_BASE_ADDR, 8U, 0xFFU);
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ADDR,
                          internal_flash_erase_page(INTERNAL_FLASH_BASE_ADDR));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ADDR, internal_flash_erase_page(last_protected));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ADDR,
                          internal_flash_program(INTERNAL_FLASH_BASE_ADDR, data, 8U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_ERR_ADDR,
                          internal_flash_program(INTERNAL_FLASH_WRITABLE_FROM_ADDR - 8U, data, 8U));
    /* A write that starts below and ends above the boundary is refused too. */
    TEST_ASSERT_EQUAL_INT(
        INTERNAL_FLASH_ERR_ADDR,
        internal_flash_program(INTERNAL_FLASH_WRITABLE_FROM_ADDR - 8U, data, 16U));
    assert_all(INTERNAL_FLASH_BASE_ADDR, 8U, 0xFFU);
}

void test_TC_IFD_033_the_first_writable_page_is_accepted_and_reads_are_not_restricted(void)
{
    uint8_t buf[8];

    start();
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK,
                          internal_flash_erase_page(INTERNAL_FLASH_WRITABLE_FROM_ADDR));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK,
                          internal_flash_program(INTERNAL_FLASH_WRITABLE_FROM_ADDR, buf, 8U));
    TEST_ASSERT_EQUAL_INT(INTERNAL_FLASH_OK,
                          internal_flash_read(INTERNAL_FLASH_BASE_ADDR, buf, 8U));
}
