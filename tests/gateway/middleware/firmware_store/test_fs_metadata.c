/**
 * @file test_fs_metadata.c
 * @brief Unit tests for FirmwareStore boot-state resolution, the A/B metadata write
 *        protocol, boot control, and the bootloader contract (host simulator).
 *
 * Test IDs follow docs/lld/middleware/firmware-store.md §22:
 *   TC-FS-004..011   create and boot resolution
 *   TC-FS-070..079   boot control
 *   TC-FS-090..092   bootloader contract, against sim/bootloader_sim.c
 * plus TC-FS-M01.. for the metadata write protocol of fs_metadata.c (§6.3).
 *
 * Records are seeded and inspected as raw bytes built from firmware_image_format.h,
 * independently of fs_metadata.c.
 *
 * Build defines: STM32L475xx, BOARD_GATEWAY, TEST (:test_fs_metadata:).
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "freertos_mock.h" /* causes Ceedling to auto-link freertos_mock.c */

#include "bootloader_sim.h"
#include "config_store_crc.h" /* auto-links config_store_crc.c */
#include "fake_internal_flash.h"
#include "fake_ota_crypto.h"
#include "fake_qspi_flash.h"
#include "firmware_store.h"
#include "fs_metadata.h"
#include "fs_staging.h"
#include "fs_test_image.h"
#include "fs_test_util.h"
#include "power_cut.h"

#define IMG (&firmware_store_image_ops)
#define BOOT (&firmware_store_boot_ops)

#define BODY_LEN (4200U)
#define IMAGE_ID (9U)
#define CHUNK (1500U)
#define VERSION_NEW (0x02000000UL)

static uint8_t s_file[FSU_MAX_FILE];
static uint32_t s_len;

/** @brief Everything back to power-on: the shared world plus the fake crypto. */
static void reset_world(void)
{
    fsu_reset_world();
    fake_crypto_reset();
}

void setUp(void)
{
    reset_world();
}

void tearDown(void)
{
}

/* ---------------------------------------------------------------------- */
/* Raw inspection                                                          */
/* ---------------------------------------------------------------------- */

static firmware_boot_meta_t raw_meta(uint8_t copy)
{
    firmware_boot_meta_t r;

    (void) memcpy(&r, fake_iflash_ptr((copy == 0U) ? FIRMWARE_META_A_ADDR : FIRMWARE_META_B_ADDR),
                  sizeof(r));

    return r;
}

static bool raw_meta_valid(uint8_t copy)
{
    const firmware_boot_meta_t r = raw_meta(copy);

    return (r.magic == FIRMWARE_META_MAGIC) &&
           (r.crc32 == config_store_crc32((const uint8_t *) &r, FIRMWARE_META_CRC_SPAN));
}

static bool desc_valid(firmware_slot_t slot)
{
    bool valid = false;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fs_desc_read(&fake_iflash_if, slot, &valid, NULL));

    return valid;
}

static firmware_boot_info_t boot_info(void)
{
    firmware_boot_info_t info;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->get_boot_info(fsu_fs, &info));

    return info;
}

/** @brief Total mutating operations on both devices. */
static uint32_t mutations(void)
{
    return fake_iflash_erase_calls() + fake_iflash_program_calls() + fake_qspi_erase_calls() +
           fake_qspi_program_calls();
}

/* ---------------------------------------------------------------------- */
/* Arrangements                                                            */
/* ---------------------------------------------------------------------- */

/** @brief Running Slot A, confirmed, with a Slot B image built, staged, verified and installed. */
static void arrange_installed(void)
{
    firmware_image_info_t info;

    fsu_seed_running_a();
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    s_len = fsu_build_image(s_file, BODY_LEN, FIRMWARE_SLOT_B_BASE, VERSION_NEW);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_download(s_file, s_len, IMAGE_ID, CHUNK));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->verify(fsu_fs, &info));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_install_all(NULL));
}

/** @brief A device running Slot B in the given state, with the previous image still in Slot A. */
static void arrange_running_b(uint8_t state, uint32_t rollback_count)
{
    fsu_seed_meta(0U, FIRMWARE_BANK_B, state, FIRMWARE_BANK_A, rollback_count, 3U);
    fsu_seed_slot(FIRMWARE_SLOT_B, FSU_RUNNING_BODY_LEN, VERSION_NEW);
    fsu_seed_slot(FIRMWARE_SLOT_A, FSU_RUNNING_BODY_LEN, FSU_RUNNING_VERSION);
    (void) fsu_create(FIRMWARE_SLOT_B_BASE);
}

/** @brief A device running Slot A in the given state; Slot B holds the other image. */
static void arrange_running_a(uint8_t state, uint32_t rollback_count)
{
    fsu_seed_meta(0U, FIRMWARE_BANK_A, state, FIRMWARE_BANK_B, rollback_count, 3U);
    fsu_seed_slot(FIRMWARE_SLOT_A, FSU_RUNNING_BODY_LEN, FSU_RUNNING_VERSION);
    fsu_seed_slot(FIRMWARE_SLOT_B, FSU_RUNNING_BODY_LEN, VERSION_NEW);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
}

/* ====================================================================== */
/* Create and boot resolution                                              */
/* ====================================================================== */

void test_TC_FS_004_blank_metadata_in_a_slot_is_not_ota_capable(void)
{
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);

    const firmware_boot_info_t info = boot_info();

    TEST_ASSERT_FALSE(info.ota_capable);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, info.running_slot);
    TEST_ASSERT_EQUAL_UINT32(0U, info.rollback_count);
    TEST_ASSERT_EQUAL_UINT32(0U, info.running_version);

    /* The bootloader should have written metadata: that is an ERROR, logged once. */
    TEST_ASSERT_EQUAL_UINT32(1U, fsu_log_count(0U));
    TEST_ASSERT_EQUAL_UINT32(0U, fsu_log_count(1U));
}

void test_TC_FS_005_a_development_image_is_not_ota_capable(void)
{
    fsu_seed_running_a();

    /* Booted from the start of flash, and from an address that is no slot base. */
    (void) fsu_create(0x08000000UL);
    TEST_ASSERT_FALSE(boot_info().ota_capable);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_NONE, boot_info().running_slot);

    /* A WARN, not an ERROR: a development image is expected. */
    TEST_ASSERT_EQUAL_UINT32(1U, fsu_log_count(1U));
    TEST_ASSERT_EQUAL_UINT32(0U, fsu_log_count(0U));

    (void) fsu_create(FIRMWARE_SLOT_A_BASE + 0x200U);
    TEST_ASSERT_FALSE(boot_info().ota_capable);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_NONE, boot_info().running_slot);
}

void test_TC_FS_006_confirmed_metadata_matching_the_running_slot_is_capable(void)
{
    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 4U, 6U);
    fsu_seed_slot(FIRMWARE_SLOT_A, FSU_RUNNING_BODY_LEN, FSU_RUNNING_VERSION);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);

    const firmware_boot_info_t info = boot_info();

    TEST_ASSERT_TRUE(info.ota_capable);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, info.running_slot);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_CONFIRMED, info.state);
    TEST_ASSERT_EQUAL_UINT32(4U, info.rollback_count);
    TEST_ASSERT_EQUAL_HEX32(FSU_RUNNING_VERSION, info.running_version);

    /* Nothing to report. */
    TEST_ASSERT_EQUAL_UINT32(0U, fsu_log_count(0U) + fsu_log_count(1U));
}

void test_TC_FS_006b_running_from_slot_b_and_a_missing_descriptor(void)
{
    arrange_running_b(FIRMWARE_META_STATE_TRIAL_RUNNING, 0U);
    TEST_ASSERT_TRUE(boot_info().ota_capable);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, boot_info().running_slot);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_TRIAL_RUNNING, boot_info().state);
    TEST_ASSERT_EQUAL_HEX32(VERSION_NEW, boot_info().running_version);

    /* No valid descriptor for the running slot: still capable, version unknown. */
    reset_world();
    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 0U, 1U);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_TRUE(boot_info().ota_capable);
    TEST_ASSERT_EQUAL_UINT32(0U, boot_info().running_version);
}

void test_TC_FS_007_metadata_naming_another_slot_is_not_capable(void)
{
    fsu_seed_meta(0U, FIRMWARE_BANK_B, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_A, 0U, 3U);
    fsu_seed_slot(FIRMWARE_SLOT_A, FSU_RUNNING_BODY_LEN, FSU_RUNNING_VERSION);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);

    TEST_ASSERT_FALSE(boot_info().ota_capable);
    TEST_ASSERT_EQUAL_UINT32(1U, fsu_log_count(0U));
}

void test_TC_FS_008_trial_pending_seen_by_the_application_is_not_capable(void)
{
    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_TRIAL_PENDING, FIRMWARE_BANK_B, 0U, 3U);
    fsu_seed_slot(FIRMWARE_SLOT_A, FSU_RUNNING_BODY_LEN, FSU_RUNNING_VERSION);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);

    /* The bootloader did not advance the state: treated as inconsistent. */
    TEST_ASSERT_FALSE(boot_info().ota_capable);
    TEST_ASSERT_EQUAL_UINT32(1U, fsu_log_count(0U));
}

void test_TC_FS_009_the_higher_generation_copy_is_selected(void)
{
    /* Copy B is newer. */
    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 3U, 2U);
    fsu_seed_meta(1U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 7U, 5U);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_EQUAL_UINT32(7U, boot_info().rollback_count);

    /* Copy A is newer. */
    reset_world();
    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 3U, 9U);
    fsu_seed_meta(1U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 7U, 4U);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_EQUAL_UINT32(3U, boot_info().rollback_count);

    /* Equal generations: copy A wins. */
    reset_world();
    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 1U, 3U);
    fsu_seed_meta(1U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 2U, 3U);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_EQUAL_UINT32(1U, boot_info().rollback_count);
}

void test_TC_FS_010_a_torn_newer_copy_falls_back_to_the_older(void)
{
    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 3U, 2U);
    fsu_seed_meta(1U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 7U, 5U);
    fake_iflash_ptr(FIRMWARE_META_B_ADDR)[0x10] ^= 0x01U; /* a CRC bit flipped */
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);

    TEST_ASSERT_TRUE(boot_info().ota_capable);
    TEST_ASSERT_EQUAL_UINT32(3U, boot_info().rollback_count);
}

void test_TC_FS_010b_illegal_field_values_invalidate_a_copy(void)
{
    /* A good record with a bad state byte, then a bad bank byte, then a bad magic. */
    fsu_seed_meta(0U, FIRMWARE_BANK_A, 9U, FIRMWARE_BANK_B, 0U, 3U);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_FALSE(boot_info().ota_capable);

    reset_world();
    fsu_seed_meta(0U, 0x0CU, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 0U, 3U);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_FALSE(boot_info().ota_capable);

    reset_world();
    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, 0x00U, 0U, 3U);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_FALSE(boot_info().ota_capable);

    reset_world();
    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 0U, 3U);
    fake_iflash_ptr(FIRMWARE_META_A_ADDR)[0] ^= 0xFFU; /* magic */
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_FALSE(boot_info().ota_capable);
}

void test_TC_FS_011_create_never_erases_or_programs(void)
{
    /* Blank, development image, capable, trial pending, torn, and a record with a stale descriptor.
     */
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_EQUAL_UINT32(0U, mutations());

    fsu_seed_running_a();
    (void) fsu_create(0x08000000UL);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_EQUAL_UINT32(0U, mutations());

    fsu_seed_meta(1U, FIRMWARE_BANK_B, FIRMWARE_META_STATE_TRIAL_PENDING, FIRMWARE_BANK_A, 0U, 8U);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    (void) fsu_create(FIRMWARE_SLOT_B_BASE);
    fake_iflash_ptr(FIRMWARE_META_B_ADDR)[0x11] ^= 0x01U;
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_EQUAL_UINT32(0U, mutations());
}

void test_TC_FS_011b_create_reports_a_read_failure_and_can_be_retried(void)
{
    const firmware_store_config_t cfg = {
        .qspi = &fake_qspi_flash_if,
        .iflash = &fake_iflash_if,
        .running_image_base = FIRMWARE_SLOT_A_BASE,
    };
    firmware_store_t *h = NULL;

    fsu_seed_running_a();

    fake_iflash_fail_all_reads(true);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_READ, firmware_store_create(&cfg, &h));
    TEST_ASSERT_NULL(h);

    /* Metadata readable, descriptor not. */
    fake_iflash_fail_all_reads(false);
    fake_iflash_fail_read_at(FIRMWARE_DESC_A_ADDR);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_READ, firmware_store_create(&cfg, &h));

    /* A failed create does not consume the instance. */
    fake_iflash_fail_read_at(FAKE_IFLASH_NO_ADDR);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, firmware_store_create(&cfg, &h));
    TEST_ASSERT_NOT_NULL(h);
}

/* ====================================================================== */
/* Metadata write protocol (fs_metadata.c)                                 */
/* ====================================================================== */

void test_TC_FS_M01_the_write_goes_to_the_copy_that_is_not_selected(void)
{
    fs_meta_t m;

    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 0U, 1U);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fs_meta_load(&fake_iflash_if, &m));
    TEST_ASSERT_EQUAL_UINT8(0U, m.selected);

    /* A is selected, so B is written and then selected. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK,
                          fs_meta_write(&fake_iflash_if, &m, FIRMWARE_BANK_B,
                                        FIRMWARE_META_STATE_TRIAL_PENDING, FIRMWARE_BANK_A, 0U));
    TEST_ASSERT_EQUAL_UINT8(1U, m.selected);
    TEST_ASSERT_EQUAL_UINT32(2U, m.generation);
    TEST_ASSERT_TRUE(raw_meta_valid(1U));
    TEST_ASSERT_EQUAL_UINT32(2U, raw_meta(1U).generation);
    TEST_ASSERT_EQUAL_UINT32(1U, raw_meta(0U).generation); /* A untouched */

    /* Then back to A. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK,
                          fs_meta_write(&fake_iflash_if, &m, FIRMWARE_BANK_B,
                                        FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_A, 0U));
    TEST_ASSERT_EQUAL_UINT8(0U, m.selected);
    TEST_ASSERT_EQUAL_UINT32(3U, raw_meta(0U).generation);

    /* The reload agrees with the in-RAM copy. */
    fs_meta_t reloaded;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fs_meta_load(&fake_iflash_if, &reloaded));
    TEST_ASSERT_EQUAL_UINT8(m.selected, reloaded.selected);
    TEST_ASSERT_EQUAL_UINT32(m.generation, reloaded.generation);
    TEST_ASSERT_EQUAL_UINT8(FIRMWARE_BANK_B, reloaded.active_bank);
}

void test_TC_FS_M02_with_no_valid_copy_the_first_write_is_copy_a_generation_1(void)
{
    fs_meta_t m;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fs_meta_load(&fake_iflash_if, &m));
    TEST_ASSERT_EQUAL_UINT8(FS_META_NONE, m.selected);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK,
                          fs_meta_write(&fake_iflash_if, &m, FIRMWARE_BANK_A,
                                        FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 0U));
    TEST_ASSERT_EQUAL_UINT8(0U, m.selected);
    TEST_ASSERT_EQUAL_UINT32(1U, raw_meta(0U).generation);
    TEST_ASSERT_FALSE(raw_meta_valid(1U));
}

void test_TC_FS_M03_erase_then_two_programs_with_the_crc_double_word_last(void)
{
    fs_meta_t m;

    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 0U, 1U);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fs_meta_load(&fake_iflash_if, &m));

    const uint32_t first = fake_iflash_log_count();

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK,
                          fs_meta_write(&fake_iflash_if, &m, FIRMWARE_BANK_B,
                                        FIRMWARE_META_STATE_TRIAL_PENDING, FIRMWARE_BANK_A, 0U));

    TEST_ASSERT_EQUAL_UINT32(3U, fake_iflash_log_count() - first);
    TEST_ASSERT_EQUAL_INT(FAKE_IFLASH_OP_ERASE, fake_iflash_log_at(first)->op);
    TEST_ASSERT_EQUAL_HEX32(FIRMWARE_META_B_ADDR, fake_iflash_log_at(first)->addr);
    TEST_ASSERT_EQUAL_INT(FAKE_IFLASH_OP_PROGRAM, fake_iflash_log_at(first + 1U)->op);
    TEST_ASSERT_EQUAL_HEX32(FIRMWARE_META_B_ADDR, fake_iflash_log_at(first + 1U)->addr);
    TEST_ASSERT_EQUAL_UINT32(16U, fake_iflash_log_at(first + 1U)->len);
    TEST_ASSERT_EQUAL_HEX32(FIRMWARE_META_B_ADDR + 16U, fake_iflash_log_at(first + 2U)->addr);
    TEST_ASSERT_EQUAL_UINT32(8U, fake_iflash_log_at(first + 2U)->len);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_rule_violations());
}

void test_TC_FS_M04_a_failed_write_leaves_the_selected_copy_and_the_cache_alone(void)
{
    fs_meta_t m;
    firmware_boot_meta_t before;

    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 0U, 1U);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fs_meta_load(&fake_iflash_if, &m));
    before = raw_meta(0U);

    fake_iflash_fail_erase_at(FIRMWARE_META_B_ADDR);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_ERASE,
                          fs_meta_write(&fake_iflash_if, &m, FIRMWARE_BANK_B,
                                        FIRMWARE_META_STATE_TRIAL_PENDING, FIRMWARE_BANK_A, 0U));
    fake_iflash_fail_erase_at(FAKE_IFLASH_NO_ADDR);

    fake_iflash_fail_program_at(FIRMWARE_META_B_ADDR);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_WRITE,
                          fs_meta_write(&fake_iflash_if, &m, FIRMWARE_BANK_B,
                                        FIRMWARE_META_STATE_TRIAL_PENDING, FIRMWARE_BANK_A, 0U));
    fake_iflash_fail_program_at(FAKE_IFLASH_NO_ADDR);

    /* The CRC double word is the second program; fail that one. */
    fake_iflash_fail_program_at(FIRMWARE_META_B_ADDR + 16U);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_WRITE,
                          fs_meta_write(&fake_iflash_if, &m, FIRMWARE_BANK_B,
                                        FIRMWARE_META_STATE_TRIAL_PENDING, FIRMWARE_BANK_A, 0U));
    fake_iflash_fail_program_at(FAKE_IFLASH_NO_ADDR);

    TEST_ASSERT_EQUAL_UINT8(0U, m.selected);
    TEST_ASSERT_EQUAL_UINT32(1U, m.generation);
    TEST_ASSERT_EQUAL_MEMORY(&before, fake_iflash_ptr(FIRMWARE_META_A_ADDR), sizeof(before));
    TEST_ASSERT_FALSE(raw_meta_valid(1U)); /* the half-written copy is not valid */
}

void test_TC_FS_M05_a_read_back_mismatch_is_a_write_failure(void)
{
    fs_meta_t m;

    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 0U, 1U);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fs_meta_load(&fake_iflash_if, &m));

    fake_iflash_corrupt_after_program(FIRMWARE_META_B_ADDR + 2U);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_WRITE,
                          fs_meta_write(&fake_iflash_if, &m, FIRMWARE_BANK_B,
                                        FIRMWARE_META_STATE_TRIAL_PENDING, FIRMWARE_BANK_A, 0U));
    TEST_ASSERT_EQUAL_UINT8(0U, m.selected);
    TEST_ASSERT_TRUE(fsu_log_count(0U) >= 1U);
}

/* ====================================================================== */
/* Boot control                                                            */
/* ====================================================================== */

void test_TC_FS_070_activate_needs_an_installed_image(void)
{
    firmware_image_info_t info;

    fsu_seed_running_a();
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->activate(fsu_fs)); /* IDLE */

    s_len = fsu_build_image(s_file, BODY_LEN, FIRMWARE_SLOT_B_BASE, VERSION_NEW);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_download(s_file, s_len, IMAGE_ID, CHUNK));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->activate(fsu_fs)); /* RECEIVED */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->verify(fsu_fs, &info));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->activate(fsu_fs)); /* VERIFIED */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->install_begin(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->activate(fsu_fs)); /* INSTALLING */

    /* No metadata was written by any of the refusals. */
    TEST_ASSERT_EQUAL_UINT32(1U, raw_meta(0U).generation);
    TEST_ASSERT_FALSE(raw_meta_valid(1U));
}

void test_TC_FS_071_activate_arms_the_new_slot_as_a_trial(void)
{
    firmware_boot_meta_t other_before;

    arrange_installed();
    other_before = raw_meta(0U);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->activate(fsu_fs));

    const firmware_boot_meta_t r = raw_meta(1U); /* the previously unselected copy */

    TEST_ASSERT_TRUE(raw_meta_valid(1U));
    TEST_ASSERT_EQUAL_UINT8(FIRMWARE_BANK_B, r.active_bank);
    TEST_ASSERT_EQUAL_UINT8(FIRMWARE_META_STATE_TRIAL_PENDING, r.boot_state);
    TEST_ASSERT_EQUAL_UINT8(FIRMWARE_BANK_A, r.previous_bank);
    TEST_ASSERT_EQUAL_UINT32(2U, r.generation);
    TEST_ASSERT_EQUAL_UINT32(0U, r.rollback_count);

    /* The other copy is intact, so a power cut here would still boot the current image. */
    TEST_ASSERT_EQUAL_MEMORY(&other_before, fake_iflash_ptr(FIRMWARE_META_A_ADDR),
                             sizeof(other_before));

    /* The cache follows, and a second update cannot start until the trial is settled. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_TRIAL_PENDING, boot_info().state);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->begin(fsu_fs, IMAGE_ID + 1U, s_len));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->activate(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->discard(fsu_fs));
}

void test_TC_FS_071b_a_failed_activate_can_be_retried(void)
{
    arrange_installed();

    fake_iflash_fail_erase_at(FIRMWARE_META_B_ADDR);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_ERASE, IMG->activate(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_CONFIRMED, boot_info().state);
    TEST_ASSERT_EQUAL_UINT32(1U, raw_meta(0U).generation);

    fake_iflash_fail_erase_at(FAKE_IFLASH_NO_ADDR);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->activate(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_TRIAL_PENDING, boot_info().state);
}

void test_TC_FS_072_activate_when_not_capable(void)
{
    (void) fsu_create(FIRMWARE_SLOT_A_BASE); /* blank metadata */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_OTA_CAPABLE, IMG->activate(fsu_fs));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_program_calls());
}

void test_TC_FS_073_confirm_settles_the_trial_and_erases_the_old_descriptor(void)
{
    arrange_running_b(FIRMWARE_META_STATE_TRIAL_RUNNING, 2U);

    TEST_ASSERT_TRUE(desc_valid(FIRMWARE_SLOT_A));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->confirm(fsu_fs));

    /* Copy A was selected (generation 3), so the new record is copy B, generation 4. */
    const firmware_boot_meta_t r = raw_meta(1U);

    TEST_ASSERT_EQUAL_UINT8(FIRMWARE_META_STATE_CONFIRMED, r.boot_state);
    TEST_ASSERT_EQUAL_UINT8(FIRMWARE_BANK_B, r.active_bank);
    TEST_ASSERT_EQUAL_UINT8(FIRMWARE_BANK_A, r.previous_bank);
    TEST_ASSERT_EQUAL_UINT32(4U, r.generation);
    TEST_ASSERT_EQUAL_UINT32(2U, r.rollback_count);

    /* The superseded image can never be booted by the bootloader's fallback. */
    TEST_ASSERT_FALSE(desc_valid(FIRMWARE_SLOT_A));
    TEST_ASSERT_TRUE(desc_valid(FIRMWARE_SLOT_B));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_CONFIRMED, boot_info().state);
}

void test_TC_FS_073b_confirm_reports_a_descriptor_erase_failure_after_the_state_change(void)
{
    arrange_running_b(FIRMWARE_META_STATE_TRIAL_RUNNING, 0U);

    fake_iflash_fail_erase_at(FIRMWARE_DESC_A_ADDR);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_ERASE, BOOT->confirm(fsu_fs));

    /* The metadata write came first and stands: CONFIRMED with a stale, harmless descriptor. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_CONFIRMED, boot_info().state);
    TEST_ASSERT_EQUAL_UINT8(FIRMWARE_META_STATE_CONFIRMED, raw_meta(1U).boot_state);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->confirm(fsu_fs)); /* now a no-op */
}

void test_TC_FS_074_confirm_when_already_confirmed_writes_nothing(void)
{
    arrange_running_a(FIRMWARE_META_STATE_CONFIRMED, 0U);

    const uint32_t before = mutations();

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->confirm(fsu_fs));
    TEST_ASSERT_EQUAL_UINT32(before, mutations());
}

void test_TC_FS_075_confirm_in_the_wrong_state_is_a_bad_state(void)
{
    arrange_running_a(FIRMWARE_META_STATE_ROLLED_BACK, 1U);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, BOOT->confirm(fsu_fs));

    /* TRIAL_PENDING in RAM, between activate and the reset. */
    reset_world();
    arrange_installed();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->activate(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, BOOT->confirm(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, BOOT->rollback(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, BOOT->acknowledge_rollback(fsu_fs));
}

void test_TC_FS_076_rollback_points_the_next_boot_at_the_previous_slot(void)
{
    arrange_running_b(FIRMWARE_META_STATE_TRIAL_RUNNING, 2U);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->rollback(fsu_fs));

    const firmware_boot_meta_t r = raw_meta(1U);

    TEST_ASSERT_EQUAL_UINT8(FIRMWARE_BANK_A, r.active_bank);
    TEST_ASSERT_EQUAL_UINT8(FIRMWARE_BANK_B, r.previous_bank);
    TEST_ASSERT_EQUAL_UINT8(FIRMWARE_META_STATE_ROLLED_BACK, r.boot_state);
    TEST_ASSERT_EQUAL_UINT32(3U, r.rollback_count);
    TEST_ASSERT_EQUAL_UINT32(4U, r.generation);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_ROLLED_BACK, boot_info().state);
    TEST_ASSERT_EQUAL_UINT32(3U, boot_info().rollback_count);

    /* The failed image's descriptor is left until the rollback is acknowledged. */
    TEST_ASSERT_TRUE(desc_valid(FIRMWARE_SLOT_A));
    TEST_ASSERT_TRUE(desc_valid(FIRMWARE_SLOT_B));
}

void test_TC_FS_077_rollback_outside_a_trial_is_a_bad_state(void)
{
    arrange_running_a(FIRMWARE_META_STATE_CONFIRMED, 0U);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, BOOT->rollback(fsu_fs));

    reset_world();
    arrange_running_a(FIRMWARE_META_STATE_ROLLED_BACK, 1U);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, BOOT->rollback(fsu_fs));
    TEST_ASSERT_EQUAL_UINT32(0U, mutations());
}

void test_TC_FS_078_acknowledging_a_rollback_confirms_and_erases_the_failed_image(void)
{
    arrange_running_a(FIRMWARE_META_STATE_ROLLED_BACK, 1U); /* previous = B, the failed image */

    TEST_ASSERT_TRUE(desc_valid(FIRMWARE_SLOT_B));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->acknowledge_rollback(fsu_fs));

    const firmware_boot_meta_t r = raw_meta(1U);

    TEST_ASSERT_EQUAL_UINT8(FIRMWARE_META_STATE_CONFIRMED, r.boot_state);
    TEST_ASSERT_EQUAL_UINT8(FIRMWARE_BANK_A, r.active_bank);
    TEST_ASSERT_EQUAL_UINT32(1U, r.rollback_count); /* the count is history, it stays */

    TEST_ASSERT_FALSE(desc_valid(FIRMWARE_SLOT_B));
    TEST_ASSERT_TRUE(desc_valid(FIRMWARE_SLOT_A));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_CONFIRMED, boot_info().state);

    /* An update may start again. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, 1000U));
}

void test_TC_FS_078b_acknowledge_in_other_states(void)
{
    arrange_running_a(FIRMWARE_META_STATE_CONFIRMED, 0U);
    const uint32_t before = mutations();

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->acknowledge_rollback(fsu_fs)); /* no-op */
    TEST_ASSERT_EQUAL_UINT32(before, mutations());

    reset_world();
    arrange_running_b(FIRMWARE_META_STATE_TRIAL_RUNNING, 0U);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, BOOT->acknowledge_rollback(fsu_fs));
}

void test_TC_FS_078c_boot_control_when_not_capable(void)
{
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_OTA_CAPABLE, BOOT->confirm(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_OTA_CAPABLE, BOOT->rollback(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_OTA_CAPABLE, BOOT->acknowledge_rollback(fsu_fs));
    TEST_ASSERT_EQUAL_UINT32(0U, mutations());
}

void test_TC_FS_079_get_boot_info_touches_no_flash(void)
{
    arrange_running_b(FIRMWARE_META_STATE_TRIAL_RUNNING, 5U);

    const uint32_t i_reads = fake_iflash_read_calls();
    const uint32_t q_reads = fake_qspi_read_calls();
    const firmware_boot_info_t info = boot_info();

    TEST_ASSERT_EQUAL_UINT32(i_reads, fake_iflash_read_calls());
    TEST_ASSERT_EQUAL_UINT32(q_reads, fake_qspi_read_calls());
    TEST_ASSERT_EQUAL_UINT32(0U, mutations());
    TEST_ASSERT_EQUAL_UINT32(5U, info.rollback_count);
}

/* ====================================================================== */
/* Bootloader contract (simulator)                                         */
/* ====================================================================== */

void test_TC_FS_090_an_update_is_activated_trialled_and_confirmed(void)
{
    arrange_installed();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->activate(fsu_fs));

    /* Reset: the bootloader promotes TRIAL_PENDING to TRIAL_RUNNING and jumps to Slot B. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, bootloader_sim_boot());

    (void) fsu_create(FIRMWARE_SLOT_B_BASE);
    TEST_ASSERT_TRUE(boot_info().ota_capable);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_TRIAL_RUNNING, boot_info().state);
    TEST_ASSERT_EQUAL_HEX32(VERSION_NEW, boot_info().running_version);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->confirm(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_CONFIRMED, boot_info().state);

    /* The next reset stays on Slot B and the old image is gone. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, bootloader_sim_boot());
    TEST_ASSERT_FALSE(desc_valid(FIRMWARE_SLOT_A));
}

void test_TC_FS_091_a_failed_self_check_rolls_back_and_is_acknowledged(void)
{
    arrange_installed();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->activate(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, bootloader_sim_boot());

    (void) fsu_create(FIRMWARE_SLOT_B_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->rollback(fsu_fs));

    /* Reset: the bootloader boots the previous slot, ROLLED_BACK. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, bootloader_sim_boot());
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_TRUE(boot_info().ota_capable);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_ROLLED_BACK, boot_info().state);
    TEST_ASSERT_EQUAL_UINT32(1U, boot_info().rollback_count);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->acknowledge_rollback(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_CONFIRMED, boot_info().state);
    TEST_ASSERT_FALSE(desc_valid(FIRMWARE_SLOT_B)); /* the failed image can never be booted again */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, bootloader_sim_boot());
}

void test_TC_FS_092_a_crash_during_the_trial_rolls_back_at_the_second_boot(void)
{
    arrange_installed();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->activate(fsu_fs));

    /* Boot 1 enters the trial; the new image crashes before confirming. Boot 2 rolls back. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, bootloader_sim_boot());
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, bootloader_sim_boot());

    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_TRUE(boot_info().ota_capable);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_ROLLED_BACK, boot_info().state);
    TEST_ASSERT_EQUAL_UINT32(1U, boot_info().rollback_count);
    TEST_ASSERT_EQUAL_HEX32(FSU_RUNNING_VERSION, boot_info().running_version);

    /* A further reset changes nothing. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, bootloader_sim_boot());
}

void test_TC_FS_092b_the_simulator_falls_back_when_the_active_image_is_corrupt(void)
{
    arrange_installed();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->activate(fsu_fs));

    /* One body bit of the new image rots in flash before the first boot. */
    fake_iflash_ptr(FIRMWARE_SLOT_B_BASE)[1500] &= 0xFEU;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, bootloader_sim_boot());
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_ROLLED_BACK, boot_info().state);
}
