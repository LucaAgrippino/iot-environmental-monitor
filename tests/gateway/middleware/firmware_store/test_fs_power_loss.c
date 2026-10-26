/**
 * @file test_fs_power_loss.c
 * @brief Power-loss sweeps for FirmwareStore (REQ-NF-214).
 *
 * Test IDs follow docs/lld/middleware/firmware-store.md §22 "Power loss" (TC-FS-085..088).
 * Each sweep runs one operation repeatedly, cutting power at mutating flash operation
 * 0, 1, 2 ... in turn. The cut leaves a torn write behind (half a program, half an
 * erase) and every later operation fails until power returns. After each cut the
 * module is re-created over the same flash, as after a reset, and the invariants of
 * §14 are checked. The sweep ends when an operation completes without being cut.
 *
 * Build defines: STM32L475xx, BOARD_GATEWAY, TEST (:test_fs_power_loss:).
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

#define MAX_CUTS (600U)
#define IMAGE_ID (0x42U)
#define BODY_LEN (6000U)
#define CHUNK (900U)
#define VERSION_NEW (0x03000000UL)

static uint8_t s_file[FSU_MAX_FILE];
static uint8_t s_staged[FSU_MAX_FILE];
static uint32_t s_len;
static uint32_t s_sent; /* bytes handed to write_chunk, including the call that was cut */

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

typedef void (*arrange_fn)(void);
typedef firmware_store_err_t (*op_fn)(void);
typedef void (*check_fn)(bool fired);

/**
 * @brief Run op() with power cut at operation 0, 1, 2 ... until one runs to the end.
 *
 * After each cut the check function sees the flash as the next boot would.
 */
static void sweep(arrange_fn arrange, op_fn op, check_fn check)
{
    for (uint32_t n = 0U; n < MAX_CUTS; n++)
    {
        reset_world();
        arrange();
        power_cut_reset();
        power_cut_arm(n);

        const firmware_store_err_t err = op();
        const bool fired = power_cut_is_dead();

        power_cut_restore();

        if (!fired)
        {
            TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, err);
        }

        check(fired);

        if (!fired)
        {
            return; /* every earlier index was exercised */
        }
    }

    TEST_FAIL_MESSAGE("the sweep never reached an uncut run");
}

/* ---------------------------------------------------------------------- */
/* Raw metadata inspection, independent of fs_metadata.c                   */
/* ---------------------------------------------------------------------- */

/** @brief The record the bootloader would select; false if no copy is valid. */
static bool selected_record(firmware_boot_meta_t *out)
{
    bool found = false;

    for (uint8_t c = 0U; c < 2U; c++)
    {
        firmware_boot_meta_t r;

        (void) memcpy(&r, fake_iflash_ptr((c == 0U) ? FIRMWARE_META_A_ADDR : FIRMWARE_META_B_ADDR),
                      sizeof(r));

        const bool ok =
            (r.magic == FIRMWARE_META_MAGIC) &&
            (r.crc32 == config_store_crc32((const uint8_t *) &r, FIRMWARE_META_CRC_SPAN));

        if (ok && (!found || (r.generation > out->generation)))
        {
            *out = r;
            found = true;
        }
    }

    return found;
}

static bool desc_valid(firmware_slot_t slot)
{
    bool valid = false;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fs_desc_read(&fake_iflash_if, slot, &valid, NULL));

    return valid;
}

/* ====================================================================== */
/* TC-FS-085  download and resume                                          */
/* ====================================================================== */

static void arrange_download(void)
{
    fsu_seed_running_a();
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    s_len = fsu_build_image(s_file, BODY_LEN, FIRMWARE_SLOT_B_BASE, VERSION_NEW);
    s_sent = 0U;
}

static firmware_store_err_t op_download(void)
{
    firmware_store_err_t err = IMG->begin(fsu_fs, IMAGE_ID, s_len);

    for (uint32_t pos = 0U; (err == FIRMWARE_STORE_ERR_OK) && (pos < s_len); pos += CHUNK)
    {
        const uint32_t n = ((s_len - pos) < CHUNK) ? (s_len - pos) : CHUNK;

        s_sent = pos + n;
        err = IMG->write_chunk(fsu_fs, pos, &s_file[pos], n);
    }

    return err;
}

static void check_download(bool fired)
{
    firmware_image_info_t info;
    uint32_t offset = 0U;

    (void) fsu_create(FIRMWARE_SLOT_A_BASE); /* the reset */

    const firmware_store_err_t r = IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset);

    if (r == FIRMWARE_STORE_ERR_NO_SESSION)
    {
        TEST_ASSERT_TRUE(fired); /* only a cut inside begin() can lose the session */
        TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_download(s_file, s_len, IMAGE_ID, CHUNK));
    }
    else
    {
        TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, r);

        /* The checkpoint is aligned, and never claims more than was sent. */
        TEST_ASSERT_TRUE(((offset % FIRMWARE_CHECKPOINT_STEP) == 0U) || (offset == s_len));
        TEST_ASSERT_TRUE(offset <= s_sent);

        /* Everything below it really is in flash, byte for byte. */
        if (offset > 0U) /* a session cut before its first checkpoint resumes at zero */
        {
            fsu_read_staged(s_staged, offset);
            TEST_ASSERT_EQUAL_MEMORY(s_file, s_staged, offset);
        }

        if (offset < s_len)
        {
            TEST_ASSERT_EQUAL_INT(
                FIRMWARE_STORE_ERR_OK,
                IMG->write_chunk(fsu_fs, offset, &s_file[offset], s_len - offset));
        }
    }

    fsu_read_staged(s_staged, s_len);
    TEST_ASSERT_EQUAL_MEMORY(s_file, s_staged, s_len);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->verify(fsu_fs, &info));
}

void test_TC_FS_085_a_power_cut_at_any_point_of_a_download_is_resumable(void)
{
    sweep(arrange_download, op_download, check_download);
}

/* ====================================================================== */
/* TC-FS-086  install                                                      */
/* ====================================================================== */

static firmware_boot_meta_t s_meta_before;

static void arrange_verified(void)
{
    firmware_image_info_t info;

    arrange_download();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_download(s_file, s_len, IMAGE_ID, CHUNK));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->verify(fsu_fs, &info));
    TEST_ASSERT_TRUE(selected_record(&s_meta_before));
}

static firmware_store_err_t op_install(void)
{
    return fsu_install_all(NULL);
}

static void check_install(bool fired)
{
    firmware_boot_meta_t now;
    firmware_image_info_t info;
    uint32_t offset = 0U;

    /* The metadata never changes during an install. */
    TEST_ASSERT_TRUE(selected_record(&now));
    TEST_ASSERT_EQUAL_MEMORY(&s_meta_before, &now, sizeof(now));

    /* The running image is untouched. */
    for (uint32_t i = 0U; i < FSU_RUNNING_BODY_LEN; i++)
    {
        TEST_ASSERT_EQUAL_HEX8(fsu_pattern(i, 0xA1U), *fake_iflash_ptr(FIRMWARE_SLOT_A_BASE + i));
    }

    /* The target descriptor is valid only if the install ran to completion. */
    TEST_ASSERT_EQUAL(!fired, desc_valid(FIRMWARE_SLOT_B));

    /* The reset boots the current image. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, bootloader_sim_boot());
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);

    /* Recovery: staging is still complete, so resume -> RECEIVED -> verify -> install again. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset));
    TEST_ASSERT_EQUAL_UINT32(s_len, offset);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->verify(fsu_fs, &info));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_install_all(NULL));
    TEST_ASSERT_TRUE(desc_valid(FIRMWARE_SLOT_B));
    TEST_ASSERT_EQUAL_MEMORY(&s_file[FIRMWARE_HEADER_SIZE], fake_iflash_ptr(FIRMWARE_SLOT_B_BASE),
                             BODY_LEN);
}

void test_TC_FS_086_a_power_cut_at_any_point_of_an_install_leaves_the_current_image_bootable(void)
{
    sweep(arrange_verified, op_install, check_install);
}

/* ====================================================================== */
/* TC-FS-087  activate                                                     */
/* ====================================================================== */

static void arrange_installed(void)
{
    arrange_verified();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_install_all(NULL));
}

static firmware_store_err_t op_activate(void)
{
    return IMG->activate(fsu_fs);
}

static void check_activate(bool fired)
{
    firmware_boot_meta_t r;

    TEST_ASSERT_TRUE(selected_record(&r)); /* at least one copy is always valid */

    if (r.active_bank == FIRMWARE_BANK_A)
    {
        /* Not applied: the old record, CONFIRMED on the running slot. */
        TEST_ASSERT_TRUE(fired);
        TEST_ASSERT_EQUAL_UINT8(FIRMWARE_META_STATE_CONFIRMED, r.boot_state);
        TEST_ASSERT_EQUAL_UINT32(1U, r.generation);
        TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, bootloader_sim_boot());
    }
    else
    {
        /* Applied: the new record, a trial of Slot B whose descriptor is valid. */
        TEST_ASSERT_EQUAL_UINT8(FIRMWARE_BANK_B, r.active_bank);
        TEST_ASSERT_EQUAL_UINT8(FIRMWARE_META_STATE_TRIAL_PENDING, r.boot_state);
        TEST_ASSERT_EQUAL_UINT8(FIRMWARE_BANK_A, r.previous_bank);
        TEST_ASSERT_EQUAL_UINT32(2U, r.generation);
        TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, bootloader_sim_boot());
    }

    if (!fired)
    {
        TEST_ASSERT_EQUAL_UINT8(FIRMWARE_BANK_B, r.active_bank);
    }
}

void test_TC_FS_087_a_power_cut_at_any_point_of_activate_is_all_or_nothing(void)
{
    sweep(arrange_installed, op_activate, check_activate);
}

/* ====================================================================== */
/* TC-FS-088  confirm, rollback, acknowledge                               */
/* ====================================================================== */

#define GENERATION_BEFORE (3U)
#define ROLLBACKS_BEFORE (2U)

static void arrange_trial_on_b(void)
{
    fsu_seed_meta(0U, FIRMWARE_BANK_B, FIRMWARE_META_STATE_TRIAL_RUNNING, FIRMWARE_BANK_A,
                  ROLLBACKS_BEFORE, GENERATION_BEFORE);
    fsu_seed_slot(FIRMWARE_SLOT_B, FSU_RUNNING_BODY_LEN, VERSION_NEW);
    fsu_seed_slot(FIRMWARE_SLOT_A, FSU_RUNNING_BODY_LEN, FSU_RUNNING_VERSION);
    (void) fsu_create(FIRMWARE_SLOT_B_BASE);
}

static void arrange_rolled_back_on_a(void)
{
    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_ROLLED_BACK, FIRMWARE_BANK_B,
                  ROLLBACKS_BEFORE, GENERATION_BEFORE);
    fsu_seed_slot(FIRMWARE_SLOT_A, FSU_RUNNING_BODY_LEN, FSU_RUNNING_VERSION);
    fsu_seed_slot(FIRMWARE_SLOT_B, FSU_RUNNING_BODY_LEN, VERSION_NEW);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
}

static firmware_store_err_t op_confirm(void)
{
    return BOOT->confirm(fsu_fs);
}

static firmware_store_err_t op_rollback(void)
{
    return BOOT->rollback(fsu_fs);
}

static firmware_store_err_t op_acknowledge(void)
{
    return BOOT->acknowledge_rollback(fsu_fs);
}

/** @brief The selected record is exactly the state before or the state after, never anything else.
 */
static void expect_pre_or_post(const firmware_boot_meta_t *pre, uint8_t post_active,
                               uint8_t post_state, uint8_t post_previous, uint32_t post_rollbacks)
{
    firmware_boot_meta_t r;

    TEST_ASSERT_TRUE(selected_record(&r));

    if (r.generation == GENERATION_BEFORE)
    {
        TEST_ASSERT_EQUAL_MEMORY(pre, &r, sizeof(r));
    }
    else
    {
        TEST_ASSERT_EQUAL_UINT32(GENERATION_BEFORE + 1U, r.generation);
        TEST_ASSERT_EQUAL_UINT8(post_active, r.active_bank);
        TEST_ASSERT_EQUAL_UINT8(post_state, r.boot_state);
        TEST_ASSERT_EQUAL_UINT8(post_previous, r.previous_bank);
        TEST_ASSERT_EQUAL_UINT32(post_rollbacks, r.rollback_count);
    }

    /* Whatever happened, the next reset boots something. */
    TEST_ASSERT_TRUE(bootloader_sim_boot() != FIRMWARE_SLOT_NONE);
}

static void check_confirm(bool fired)
{
    firmware_boot_meta_t pre;

    (void) fired;
    (void) memset(&pre, 0, sizeof(pre));
    pre.magic = FIRMWARE_META_MAGIC;
    pre.active_bank = FIRMWARE_BANK_B;
    pre.boot_state = FIRMWARE_META_STATE_TRIAL_RUNNING;
    pre.previous_bank = FIRMWARE_BANK_A;
    pre.reserved = 0xFFU;
    pre.rollback_count = ROLLBACKS_BEFORE;
    pre.generation = GENERATION_BEFORE;
    pre.pad = 0xFFFFFFFFUL;
    pre.crc32 = config_store_crc32((const uint8_t *) &pre, FIRMWARE_META_CRC_SPAN);

    expect_pre_or_post(&pre, FIRMWARE_BANK_B, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_A,
                       ROLLBACKS_BEFORE);
}

static void check_rollback(bool fired)
{
    firmware_boot_meta_t pre;

    (void) fired;
    (void) memset(&pre, 0, sizeof(pre));
    pre.magic = FIRMWARE_META_MAGIC;
    pre.active_bank = FIRMWARE_BANK_B;
    pre.boot_state = FIRMWARE_META_STATE_TRIAL_RUNNING;
    pre.previous_bank = FIRMWARE_BANK_A;
    pre.reserved = 0xFFU;
    pre.rollback_count = ROLLBACKS_BEFORE;
    pre.generation = GENERATION_BEFORE;
    pre.pad = 0xFFFFFFFFUL;
    pre.crc32 = config_store_crc32((const uint8_t *) &pre, FIRMWARE_META_CRC_SPAN);

    expect_pre_or_post(&pre, FIRMWARE_BANK_A, FIRMWARE_META_STATE_ROLLED_BACK, FIRMWARE_BANK_B,
                       ROLLBACKS_BEFORE + 1U);
}

static void check_acknowledge(bool fired)
{
    firmware_boot_meta_t pre;

    (void) fired;
    (void) memset(&pre, 0, sizeof(pre));
    pre.magic = FIRMWARE_META_MAGIC;
    pre.active_bank = FIRMWARE_BANK_A;
    pre.boot_state = FIRMWARE_META_STATE_ROLLED_BACK;
    pre.previous_bank = FIRMWARE_BANK_B;
    pre.reserved = 0xFFU;
    pre.rollback_count = ROLLBACKS_BEFORE;
    pre.generation = GENERATION_BEFORE;
    pre.pad = 0xFFFFFFFFUL;
    pre.crc32 = config_store_crc32((const uint8_t *) &pre, FIRMWARE_META_CRC_SPAN);

    expect_pre_or_post(&pre, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B,
                       ROLLBACKS_BEFORE);
}

void test_TC_FS_088a_a_power_cut_at_any_point_of_confirm_converges(void)
{
    sweep(arrange_trial_on_b, op_confirm, check_confirm);
}

void test_TC_FS_088b_a_power_cut_at_any_point_of_rollback_converges(void)
{
    sweep(arrange_trial_on_b, op_rollback, check_rollback);
}

void test_TC_FS_088c_a_power_cut_at_any_point_of_acknowledge_converges(void)
{
    sweep(arrange_rolled_back_on_a, op_acknowledge, check_acknowledge);
}
