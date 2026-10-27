/**
 * @file test_boot_core.c
 * @brief Unit tests for the bootloader decision logic (host, Unity).
 *
 * Test IDs follow docs/lld/drivers/bootloader.md §7 (TC-BL-001..027). TC-BL-028..030, the contract
 * with FirmwareStore and the simulator, are in tests/gateway/middleware/firmware_store/.
 *
 * boot_core.c runs unchanged over fake_internal_flash (NOR + ECC semantics, power-cut and fault
 * injection). Records and descriptors are seeded as raw bytes from firmware_image_format.h,
 * independently of the code under test. Guards on the bootloader region, the descriptor pages
 * and both slots turn any write outside the boot-record pages into a test failure.
 *
 * Build defines: STM32L475xx, BOARD_GATEWAY, TEST (:test_boot_core:).
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "boot_core.h"
#include "config_store_crc.h" /* auto-links config_store_crc.c */
#include "fake_internal_flash.h"
#include "power_cut.h"

#define SRAM1_TOP (0x20018000UL)
#define BODY_LEN (4000U)
#define VERSION_A (0x01000000UL)
#define VERSION_B (0x02000000UL)

#define A (FIRMWARE_BANK_A)
#define B (FIRMWARE_BANK_B)

#define CONFIRMED (FIRMWARE_META_STATE_CONFIRMED)
#define PENDING (FIRMWARE_META_STATE_TRIAL_PENDING)
#define RUNNING (FIRMWARE_META_STATE_TRIAL_RUNNING)
#define ROLLED_BACK (FIRMWARE_META_STATE_ROLLED_BACK)

static boot_report_t s_rep;

/* ---------------------------------------------------------------------- */
/* Seeding and inspection, straight from the on-flash format                */
/* ---------------------------------------------------------------------- */

static uint32_t base_of(uint8_t bank)
{
    return (bank == B) ? FIRMWARE_SLOT_B_BASE : FIRMWARE_SLOT_A_BASE;
}

static uint32_t desc_of(uint8_t bank)
{
    return (bank == B) ? FIRMWARE_DESC_B_ADDR : FIRMWARE_DESC_A_ADDR;
}

static uint32_t meta_of(uint8_t copy)
{
    return (copy == 0U) ? FIRMWARE_META_A_ADDR : FIRMWARE_META_B_ADDR;
}

static void put_record_raw(uint8_t copy, uint8_t active, uint8_t state, uint8_t previous,
                           uint32_t count, uint32_t generation, bool break_crc)
{
    firmware_boot_meta_t r = {
        .magic = FIRMWARE_META_MAGIC,
        .active_bank = active,
        .boot_state = state,
        .previous_bank = previous,
        .reserved = 0xFFU,
        .rollback_count = count,
        .generation = generation,
        .pad = 0xFFFFFFFFUL,
    };

    r.crc32 = config_store_crc32((const uint8_t *) &r, FIRMWARE_META_CRC_SPAN);

    if (break_crc)
    {
        r.crc32 ^= 1U;
    }

    (void) memcpy(fake_iflash_ptr(meta_of(copy)), &r, sizeof(r));
}

static void put_record(uint8_t copy, uint8_t active, uint8_t state, uint8_t previous,
                       uint32_t count, uint32_t generation)
{
    put_record_raw(copy, active, state, previous, count, generation, false);
}

/** @brief A body with a legal vector pair, then a pattern; and a descriptor for it. */
static void put_slot_ex(uint8_t bank, uint32_t len, uint32_t version, uint32_t sp, uint32_t pc)
{
    uint8_t *const body = fake_iflash_ptr(base_of(bank));
    const uint32_t vectors[2] = {sp, pc};

    for (uint32_t i = 0U; i < len; i++)
    {
        body[i] = (uint8_t) ((i * 7U) + bank);
    }

    (void) memcpy(body, vectors, sizeof(vectors));

    firmware_slot_descriptor_t d = {
        .magic = FIRMWARE_DESC_MAGIC,
        .image_len = len,
        .version = version,
        .load_addr = base_of(bank),
        .body_crc32 = config_store_crc32(body, len),
        .reserved = 0xFFFFFFFFUL,
        .pad = 0xFFFFFFFFUL,
    };

    d.desc_crc32 = config_store_crc32((const uint8_t *) &d, FIRMWARE_DESC_CRC_SPAN);
    (void) memcpy(fake_iflash_ptr(desc_of(bank)), &d, sizeof(d));
}

static void put_slot(uint8_t bank, uint32_t version)
{
    put_slot_ex(bank, BODY_LEN, version, SRAM1_TOP, base_of(bank) + 0x101UL);
}

static void kill_desc(uint8_t bank)
{
    (void) memset(fake_iflash_ptr(desc_of(bank)), 0xFF, sizeof(firmware_slot_descriptor_t));
}

static bool record_ok(uint8_t copy, firmware_boot_meta_t *out)
{
    firmware_boot_meta_t r;

    (void) memcpy(&r, fake_iflash_ptr(meta_of(copy)), sizeof(r));

    const bool ok = (r.magic == FIRMWARE_META_MAGIC) &&
                    (r.crc32 == config_store_crc32((const uint8_t *) &r, FIRMWARE_META_CRC_SPAN));

    if (ok && (out != NULL))
    {
        *out = r;
    }

    return ok;
}

/** @brief The record an independent reader would select; false if none is valid. */
static bool current(firmware_boot_meta_t *out)
{
    firmware_boot_meta_t a;
    firmware_boot_meta_t b;
    const bool ok_a = record_ok(0U, &a);
    const bool ok_b = record_ok(1U, &b);

    if (ok_a && (!ok_b || (a.generation >= b.generation)))
    {
        *out = a;
        return true;
    }

    if (ok_b)
    {
        *out = b;
        return true;
    }

    return false;
}

static uint32_t valid_records(void)
{
    return (uint32_t) record_ok(0U, NULL) + (uint32_t) record_ok(1U, NULL);
}

static boot_outcome_t run(void)
{
    return boot_core_run(&fake_iflash_if, &s_rep);
}

/** @brief Both slots hold a valid image, A confirmed on copy A at generation 5. */
static void world_confirmed_a(void)
{
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, A, CONFIRMED, B, 0U, 5U);
}

void setUp(void)
{
    fake_iflash_reset();
    power_cut_reset();
    fake_iflash_guard_add(FIRMWARE_BOOTLOADER_BASE,
                          FIRMWARE_BOOTLOADER_BASE + FIRMWARE_BOOTLOADER_SIZE);
    fake_iflash_guard_add(FIRMWARE_DESC_A_ADDR, FIRMWARE_DESC_B_ADDR + FIRMWARE_PAGE_SIZE);
    fake_iflash_guard_add(FIRMWARE_SLOT_A_BASE, FIRMWARE_SLOT_A_BASE + FIRMWARE_SLOT_SIZE);
    fake_iflash_guard_add(FIRMWARE_SLOT_B_BASE, FIRMWARE_SLOT_B_BASE + FIRMWARE_SLOT_SIZE);
}

void tearDown(void)
{
    /* TC-BL-027, for every test: only the two record pages are ever written. */
    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_guard_violations());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_rule_violations());
}

/* ---------------------------------------------------------------------- */
/* First boot                                                              */
/* ---------------------------------------------------------------------- */

void test_BL001_blank_flash_writes_factory_record_and_halts_without_image(void)
{
    TEST_ASSERT_EQUAL(BOOT_OUTCOME_HALT_NO_IMAGE, run());

    firmware_boot_meta_t r;
    TEST_ASSERT_TRUE(current(&r));
    TEST_ASSERT_EQUAL_UINT8(A, r.active_bank);
    TEST_ASSERT_EQUAL_UINT8(B, r.previous_bank);
    TEST_ASSERT_EQUAL_UINT8(CONFIRMED, r.boot_state);
    TEST_ASSERT_EQUAL_UINT32(1U, r.generation);
    TEST_ASSERT_EQUAL_UINT32(0U, r.rollback_count);
    TEST_ASSERT_TRUE((s_rep.events & BOOT_EVT_RECORD_INITIALISED) != 0U);
}

void test_BL002_blank_metadata_with_slot_a_runs_slot_a(void)
{
    put_slot(A, VERSION_A);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_A_BASE, s_rep.slot_base);
    TEST_ASSERT_EQUAL_UINT32(BODY_LEN, s_rep.image_len);
    TEST_ASSERT_EQUAL_UINT32(VERSION_A, s_rep.version);
    TEST_ASSERT_EQUAL_UINT8(0U, s_rep.state_in);
    TEST_ASSERT_EQUAL_UINT8(CONFIRMED, s_rep.state_out);
    TEST_ASSERT_EQUAL_UINT32(1U, s_rep.generation);
    TEST_ASSERT_TRUE((s_rep.events & BOOT_EVT_RECORD_INITIALISED) != 0U);
}

/* ---------------------------------------------------------------------- */
/* Steady state: nothing to write                                          */
/* ---------------------------------------------------------------------- */

void test_BL003_confirmed_a_runs_a_without_writing(void)
{
    world_confirmed_a();

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_A_BASE, s_rep.slot_base);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rep.events);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_erase_calls());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_program_calls());
}

void test_BL004_confirmed_b_runs_b_without_writing(void)
{
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(1U, B, CONFIRMED, A, 0U, 7U);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_B_BASE, s_rep.slot_base);
    TEST_ASSERT_EQUAL_UINT32(VERSION_B, s_rep.version);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_program_calls());
}

void test_BL005_rolled_back_runs_active_without_writing(void)
{
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, A, ROLLED_BACK, B, 1U, 9U);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_A_BASE, s_rep.slot_base);
    TEST_ASSERT_EQUAL_UINT8(ROLLED_BACK, s_rep.state_out);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_program_calls());
}

/* ---------------------------------------------------------------------- */
/* Trial                                                                   */
/* ---------------------------------------------------------------------- */

void test_BL006_pending_is_armed_and_the_new_slot_runs(void)
{
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, B, PENDING, A, 2U, 5U);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_B_BASE, s_rep.slot_base);
    TEST_ASSERT_EQUAL_UINT8(PENDING, s_rep.state_in);
    TEST_ASSERT_EQUAL_UINT8(RUNNING, s_rep.state_out);
    TEST_ASSERT_TRUE((s_rep.events & BOOT_EVT_TRIAL_STARTED) != 0U);

    firmware_boot_meta_t r;
    TEST_ASSERT_TRUE(current(&r));
    TEST_ASSERT_EQUAL_UINT8(RUNNING, r.boot_state);
    TEST_ASSERT_EQUAL_UINT8(B, r.active_bank);
    TEST_ASSERT_EQUAL_UINT8(A, r.previous_bank);
    TEST_ASSERT_EQUAL_UINT32(2U, r.rollback_count);
    TEST_ASSERT_EQUAL_UINT32(6U, r.generation);
    /* It went to the copy that was not selected, and the old one is intact. */
    TEST_ASSERT_TRUE(record_ok(1U, NULL));
    TEST_ASSERT_TRUE(record_ok(0U, NULL));
}

void test_BL007_running_at_reset_rolls_back_to_the_previous_slot(void)
{
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, B, RUNNING, A, 0U, 6U);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_A_BASE, s_rep.slot_base);
    TEST_ASSERT_TRUE((s_rep.events & BOOT_EVT_TRIAL_ABANDONED) != 0U);

    firmware_boot_meta_t r;
    TEST_ASSERT_TRUE(current(&r));
    TEST_ASSERT_EQUAL_UINT8(A, r.active_bank);
    TEST_ASSERT_EQUAL_UINT8(B, r.previous_bank);
    TEST_ASSERT_EQUAL_UINT8(ROLLED_BACK, r.boot_state);
    TEST_ASSERT_EQUAL_UINT32(1U, r.rollback_count);
    TEST_ASSERT_EQUAL_UINT32(7U, r.generation);
}

void test_BL008_a_second_reset_without_confirm_rolls_back(void)
{
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, B, PENDING, A, 0U, 5U);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run()); /* arms; B runs */
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_B_BASE, s_rep.slot_base);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run()); /* never confirmed */
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_A_BASE, s_rep.slot_base);
    TEST_ASSERT_EQUAL_UINT8(ROLLED_BACK, s_rep.state_out);
    TEST_ASSERT_EQUAL_UINT32(1U, s_rep.rollback_count);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run()); /* ROLLED_BACK is stable */
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_A_BASE, s_rep.slot_base);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rep.events);
}

void test_BL009_a_confirmed_trial_stays_on_the_new_slot(void)
{
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, B, PENDING, A, 0U, 5U);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());

    /* The application confirms: a CONFIRMED record on the copy that is not selected (copy A,
     * since the bootloader armed the trial on copy B). */
    firmware_boot_meta_t r;
    TEST_ASSERT_TRUE(current(&r));
    TEST_ASSERT_EQUAL_UINT32(6U, r.generation);
    (void) memset(fake_iflash_ptr(FIRMWARE_META_A_ADDR), 0xFF, FIRMWARE_PAGE_SIZE);
    put_record(0U, B, CONFIRMED, A, 0U, 7U);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_B_BASE, s_rep.slot_base);
}

/* ---------------------------------------------------------------------- */
/* Record selection                                                        */
/* ---------------------------------------------------------------------- */

void test_BL010_higher_generation_wins_and_a_tie_goes_to_copy_a(void)
{
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, A, CONFIRMED, B, 0U, 4U);
    put_record(1U, B, CONFIRMED, A, 0U, 5U);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_B_BASE, s_rep.slot_base);
    TEST_ASSERT_EQUAL_UINT32(5U, s_rep.generation);

    fake_iflash_reset();
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, A, CONFIRMED, B, 0U, 5U);
    put_record(1U, B, CONFIRMED, A, 0U, 5U);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_A_BASE, s_rep.slot_base);
}

void test_BL011_a_torn_copy_is_ignored_and_the_next_record_lands_on_it(void)
{
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, A, CONFIRMED, B, 0U, 4U);
    put_record_raw(1U, B, PENDING, A, 0U, 5U, true); /* newer, but its CRC is bad */

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_A_BASE, s_rep.slot_base);
    TEST_ASSERT_TRUE((s_rep.events & BOOT_EVT_COPY_INVALID) != 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_program_calls());

    /* A transition now writes to copy B, the copy that is not selected. */
    fake_iflash_reset();
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, A, PENDING, B, 0U, 4U);
    put_record_raw(1U, B, PENDING, A, 0U, 5U, true);
    (void) run();
    TEST_ASSERT_TRUE(record_ok(1U, NULL));
}

void test_BL012_illegal_records_are_treated_as_invalid(void)
{
    uint8_t bad_banks[4][3] = {
        {0x0CU, CONFIRMED, B}, {A, CONFIRMED, 0x00U}, {A, 0x00U, B}, {A, 0x05U, B}};

    for (uint32_t i = 0U; i < 4U; i++)
    {
        fake_iflash_reset();
        put_slot(A, VERSION_A);
        put_record(0U, bad_banks[i][0], bad_banks[i][1], bad_banks[i][2], 0U, 9U);

        TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
        TEST_ASSERT_TRUE((s_rep.events & BOOT_EVT_RECORD_INITIALISED) != 0U);
        TEST_ASSERT_EQUAL_UINT32(1U, s_rep.generation);
    }

    /* Bad magic. */
    fake_iflash_reset();
    put_slot(A, VERSION_A);
    put_record(0U, A, CONFIRMED, B, 0U, 9U);
    fake_iflash_ptr(FIRMWARE_META_A_ADDR)[0] ^= 0x01U;

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_TRUE((s_rep.events & BOOT_EVT_RECORD_INITIALISED) != 0U);
}

/* ---------------------------------------------------------------------- */
/* Verification and fall-back                                              */
/* ---------------------------------------------------------------------- */

static void expect_fell_back_from_a_to_b(void)
{
    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_B_BASE, s_rep.slot_base);
    TEST_ASSERT_TRUE((s_rep.events & BOOT_EVT_ACTIVE_INVALID) != 0U);
    TEST_ASSERT_EQUAL_UINT8(ROLLED_BACK, s_rep.state_out);
    TEST_ASSERT_EQUAL_UINT32(1U, s_rep.rollback_count);

    firmware_boot_meta_t r;
    TEST_ASSERT_TRUE(current(&r));
    TEST_ASSERT_EQUAL_UINT8(B, r.active_bank);
    TEST_ASSERT_EQUAL_UINT8(A, r.previous_bank);
    TEST_ASSERT_EQUAL_UINT8(ROLLED_BACK, r.boot_state);
    TEST_ASSERT_EQUAL_UINT32(6U, r.generation);
}

void test_BL013_a_bad_descriptor_falls_back_to_the_other_slot(void)
{
    world_confirmed_a();
    fake_iflash_ptr(FIRMWARE_DESC_A_ADDR)[0x18] ^= 0x01U; /* desc_crc32 */

    expect_fell_back_from_a_to_b();
}

void test_BL014_a_flipped_body_byte_falls_back_to_the_other_slot(void)
{
    world_confirmed_a();
    fake_iflash_ptr(FIRMWARE_SLOT_A_BASE + BODY_LEN - 1U)[0] ^= 0x80U;

    expect_fell_back_from_a_to_b();
}

void test_BL015_a_descriptor_for_the_other_slot_is_not_bootable(void)
{
    world_confirmed_a();
    /* Slot A's descriptor claims it is linked for Slot B. */
    firmware_slot_descriptor_t d;
    (void) memcpy(&d, fake_iflash_ptr(FIRMWARE_DESC_A_ADDR), sizeof(d));
    d.load_addr = FIRMWARE_SLOT_B_BASE;
    d.desc_crc32 = config_store_crc32((const uint8_t *) &d, FIRMWARE_DESC_CRC_SPAN);
    (void) memcpy(fake_iflash_ptr(FIRMWARE_DESC_A_ADDR), &d, sizeof(d));

    expect_fell_back_from_a_to_b();
}

void test_BL016_a_zero_or_oversized_length_is_not_bootable(void)
{
    const uint32_t lengths[2] = {0U, FIRMWARE_SLOT_SIZE + 1U};

    for (uint32_t i = 0U; i < 2U; i++)
    {
        fake_iflash_reset();
        world_confirmed_a();

        firmware_slot_descriptor_t d;
        (void) memcpy(&d, fake_iflash_ptr(FIRMWARE_DESC_A_ADDR), sizeof(d));
        d.image_len = lengths[i];
        d.desc_crc32 = config_store_crc32((const uint8_t *) &d, FIRMWARE_DESC_CRC_SPAN);
        (void) memcpy(fake_iflash_ptr(FIRMWARE_DESC_A_ADDR), &d, sizeof(d));

        expect_fell_back_from_a_to_b();
    }
}

void test_BL017_an_implausible_vector_pair_is_not_bootable(void)
{
    const uint32_t a_base = FIRMWARE_SLOT_A_BASE;
    const struct
    {
        uint32_t sp;
        uint32_t pc;
    } cases[] = {
        {0x40000000UL, a_base + 0x101UL},          /* SP not in SRAM        */
        {SRAM1_TOP - 4UL + 1UL, a_base + 0x101UL}, /* SP not aligned   */
        {SRAM1_TOP, a_base + 0x100UL},             /* reset vector not Thumb */
        {SRAM1_TOP, FIRMWARE_SLOT_B_BASE + 1UL},   /* reset vector in the other slot */
        {SRAM1_TOP, a_base + BODY_LEN + 0x101UL},  /* reset vector beyond the body */
        {0U, 0U},
    };

    for (uint32_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++)
    {
        fake_iflash_reset();
        put_slot(B, VERSION_B);
        put_slot_ex(A, BODY_LEN, VERSION_A, cases[i].sp, cases[i].pc);
        put_record(0U, A, CONFIRMED, B, 0U, 5U);

        expect_fell_back_from_a_to_b();
    }
}

void test_BL018_two_bad_slots_halt_with_no_image(void)
{
    world_confirmed_a();
    kill_desc(A);
    kill_desc(B);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_HALT_NO_IMAGE, run());
    TEST_ASSERT_TRUE((s_rep.events & BOOT_EVT_ACTIVE_INVALID) != 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rep.slot_base);
    /* Nothing was changed on the way to halting. */
    firmware_boot_meta_t r;
    TEST_ASSERT_TRUE(current(&r));
    TEST_ASSERT_EQUAL_UINT32(5U, r.generation);
}

/* ---------------------------------------------------------------------- */
/* Rollback limit                                                          */
/* ---------------------------------------------------------------------- */

void test_BL019_at_the_limit_a_due_rollback_halts(void)
{
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, B, RUNNING, A, BOOT_MAX_ROLLBACKS, 6U);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_HALT_ROLLBACK_LIMIT, run());

    firmware_boot_meta_t r;
    TEST_ASSERT_TRUE(current(&r));
    TEST_ASSERT_EQUAL_UINT32(6U, r.generation);
    TEST_ASSERT_EQUAL_UINT8(RUNNING, r.boot_state);
}

void test_BL019b_at_the_limit_a_due_fallback_halts(void)
{
    world_confirmed_a();
    put_record(1U, A, CONFIRMED, B, BOOT_MAX_ROLLBACKS, 6U);
    kill_desc(A);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_HALT_ROLLBACK_LIMIT, run());
}

void test_BL020_one_below_the_limit_still_rolls_back(void)
{
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, B, RUNNING, A, BOOT_MAX_ROLLBACKS - 1U, 6U);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_A_BASE, s_rep.slot_base);
    TEST_ASSERT_EQUAL_UINT32(BOOT_MAX_ROLLBACKS, s_rep.rollback_count);
}

/* ---------------------------------------------------------------------- */
/* Flash faults                                                            */
/* ---------------------------------------------------------------------- */

void test_BL021_a_trial_that_cannot_be_armed_halts(void)
{
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, B, PENDING, A, 0U, 5U);
    fake_iflash_fail_program_at(FIRMWARE_META_B_ADDR);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_HALT_FLASH_FAULT, run());
    TEST_ASSERT_TRUE((s_rep.events & BOOT_EVT_META_WRITE_FAILED) != 0U);

    firmware_boot_meta_t r;
    TEST_ASSERT_TRUE(current(&r));
    TEST_ASSERT_EQUAL_UINT8(PENDING, r.boot_state); /* unchanged: the next reset tries again */
}

void test_BL022_a_failed_write_is_retried_once(void)
{
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, B, PENDING, A, 0U, 5U);
    fake_iflash_fail_program_once_at(FIRMWARE_META_B_ADDR);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_EQUAL_UINT8(RUNNING, s_rep.state_out);
    TEST_ASSERT_EQUAL_UINT32(2U, fake_iflash_erase_calls());
}

void test_BL023_a_rollback_that_cannot_be_recorded_still_runs_the_previous_slot(void)
{
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, B, RUNNING, A, 0U, 6U);
    fake_iflash_fail_program_at(FIRMWARE_META_B_ADDR);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_A_BASE, s_rep.slot_base);
    TEST_ASSERT_TRUE((s_rep.events & BOOT_EVT_META_WRITE_FAILED) != 0U);

    firmware_boot_meta_t r;
    TEST_ASSERT_TRUE(current(&r));
    TEST_ASSERT_EQUAL_UINT8(RUNNING, r.boot_state); /* unchanged: the next reset repeats it */
    TEST_ASSERT_EQUAL_UINT32(6U, r.generation);
}

void test_BL024_an_unreadable_descriptor_is_not_bootable(void)
{
    world_confirmed_a();
    fake_iflash_fail_read_at(FIRMWARE_DESC_A_ADDR);

    expect_fell_back_from_a_to_b();
}

void test_BL024b_unreadable_records_are_a_flash_fault_and_write_nothing(void)
{
    world_confirmed_a();
    fake_iflash_fail_all_reads(true);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_HALT_FLASH_FAULT, run());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_erase_calls());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_program_calls());
}

void test_BL026_a_corrupt_read_back_is_detected_and_fails_the_write(void)
{
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, B, PENDING, A, 0U, 5U);
    fake_iflash_corrupt_after_program(FIRMWARE_META_B_ADDR + 4U);

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_HALT_FLASH_FAULT, run());
    TEST_ASSERT_TRUE((s_rep.events & BOOT_EVT_META_WRITE_FAILED) != 0U);
    TEST_ASSERT_EQUAL_UINT32(2U, fake_iflash_erase_calls()); /* it did try twice */
}

void test_BL026b_null_arguments_are_a_flash_fault(void)
{
    TEST_ASSERT_EQUAL(BOOT_OUTCOME_HALT_FLASH_FAULT, boot_core_run(NULL, &s_rep));
    TEST_ASSERT_EQUAL(BOOT_OUTCOME_HALT_FLASH_FAULT, boot_core_run(&fake_iflash_if, NULL));
}

/* ---------------------------------------------------------------------- */
/* Power loss (REQ-NF-214)                                                 */
/* ---------------------------------------------------------------------- */

/**
 * Cut power at operation @p cut of the first run, restore it, and run again. At every point at
 * least one valid record must exist, and the second run must reach a verified slot.
 */
static void power_loss_case(uint8_t state, uint8_t active, uint8_t previous, uint32_t cut)
{
    fake_iflash_reset();
    power_cut_reset();
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, active, state, previous, 0U, 5U);

    power_cut_arm(cut);
    (void) run();
    TEST_ASSERT_TRUE_MESSAGE(valid_records() >= 1U, "no valid record after the cut");

    power_cut_restore();
    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_TRUE(valid_records() >= 1U);
    TEST_ASSERT_TRUE((s_rep.slot_base == FIRMWARE_SLOT_A_BASE) ||
                     (s_rep.slot_base == FIRMWARE_SLOT_B_BASE));

    /* Whatever was cut, the state settles: an unconfirmed trial rolls back on the next reset,
     * after which nothing changes any more. */
    if (s_rep.state_out == RUNNING)
    {
        TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
        TEST_ASSERT_EQUAL_UINT8(ROLLED_BACK, s_rep.state_out);
    }

    const uint32_t gen = s_rep.generation;
    const uint32_t base = s_rep.slot_base;

    TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
    TEST_ASSERT_EQUAL_UINT32(base, s_rep.slot_base);
    TEST_ASSERT_EQUAL_UINT32(gen, s_rep.generation);
    TEST_ASSERT_EQUAL_UINT32(0U, s_rep.events & ~BOOT_EVT_COPY_INVALID);
}

void test_BL025_power_loss_at_every_write_of_the_arming_converges(void)
{
    for (uint32_t cut = 0U; cut < 8U; cut++)
    {
        power_loss_case(PENDING, B, A, cut);
    }
}

void test_BL025b_power_loss_at_every_write_of_the_rollback_converges(void)
{
    for (uint32_t cut = 0U; cut < 8U; cut++)
    {
        power_loss_case(RUNNING, B, A, cut);
    }
}

void test_BL025c_power_loss_during_the_factory_record_converges(void)
{
    for (uint32_t cut = 0U; cut < 4U; cut++)
    {
        fake_iflash_reset();
        power_cut_reset();
        put_slot(A, VERSION_A);

        power_cut_arm(cut);
        (void) run();
        TEST_ASSERT_TRUE(valid_records() <= 1U);

        power_cut_restore();
        TEST_ASSERT_EQUAL(BOOT_OUTCOME_RUN, run());
        TEST_ASSERT_EQUAL_UINT32(FIRMWARE_SLOT_A_BASE, s_rep.slot_base);
        TEST_ASSERT_EQUAL_UINT8(CONFIRMED, s_rep.state_out);
    }
}

/* ---------------------------------------------------------------------- */
/* TC-BL-027: what is never written                                        */
/* ---------------------------------------------------------------------- */

void test_BL027_every_transition_writes_only_record_pages(void)
{
    /* The guards turn any other write into a violation, checked in tearDown(). */
    put_slot(A, VERSION_A);
    put_slot(B, VERSION_B);
    put_record(0U, B, PENDING, A, 0U, 5U);

    (void) run(); /* arm */
    (void) run(); /* roll back */
    kill_desc(A);
    (void) run(); /* fall back */

    for (uint32_t i = 0U; i < fake_iflash_log_count(); i++)
    {
        const fake_iflash_log_entry_t *e = fake_iflash_log_at(i);
        TEST_ASSERT_TRUE((e->addr >= FIRMWARE_META_A_ADDR) &&
                         (e->addr < (FIRMWARE_META_B_ADDR + FIRMWARE_PAGE_SIZE)));
    }
}
