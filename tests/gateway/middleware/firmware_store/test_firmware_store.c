/**
 * @file test_firmware_store.c
 * @brief Unit tests for FirmwareStore: handle checks, verification, installation, mutex, format.
 *
 * Test IDs follow docs/lld/middleware/firmware-store.md §22:
 *   TC-FS-001..003   create and handle checks
 *   TC-FS-040..051   verification (the cryptography is the fake; the real SHA-256 and
 *                    ECDSA vectors are in test_fs_crypto_vectors.c)
 *   TC-FS-060..068   installation
 *   TC-FS-095        mutex discipline
 *   TC-FS-098..099   CRC check value and on-flash structure sizes
 * Download and resume are in test_fs_staging.c, boot resolution and boot control in
 * test_fs_metadata.c, power-loss sweeps in test_fs_power_loss.c.
 *
 * Mocking strategy:
 *   - QspiFlashDriver    -> fake_qspi_flash (NOR rules, fault injection)
 *   - InternalFlashDriver-> fake_internal_flash (ECC rules, write guards, operation log)
 *   - mbedTLS            -> fake_ota_crypto (deterministic stand-in)
 *   - Logger             -> logger_log() spy in fs_test_util.c
 *   - FreeRTOS           -> freertos_mock.h
 * Build defines: STM32L475xx, BOARD_GATEWAY, TEST (:test_firmware_store:).
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "freertos_mock.h" /* causes Ceedling to auto-link freertos_mock.c */

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
#define IMAGE_ID (7U)
#define VERSION_NEW (0x01020003UL)
#define CHUNK (1000U)

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
/* Arrangements                                                            */
/* ---------------------------------------------------------------------- */

/** @brief A capable device running Slot A with an image built for Slot B in hand. */
static void arrange_capable(uint32_t body_len)
{
    fsu_seed_running_a();
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    s_len = fsu_build_image(s_file, body_len, FIRMWARE_SLOT_B_BASE, VERSION_NEW);
}

static void arrange_received(uint32_t body_len)
{
    arrange_capable(body_len);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_download(s_file, s_len, IMAGE_ID, CHUNK));
}

static void arrange_verified(uint32_t body_len)
{
    firmware_image_info_t info;

    arrange_received(body_len);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->verify(fsu_fs, &info));
}

/** @brief Stage a (possibly altered) file and return the verify() result. */
static firmware_store_err_t stage_and_verify(void)
{
    firmware_image_info_t info;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_download(s_file, s_len, IMAGE_ID, CHUNK));

    return IMG->verify(fsu_fs, &info);
}

/* ====================================================================== */
/* Create and handle checks                                                */
/* ====================================================================== */

void test_TC_FS_001_create_rejects_null_arguments(void)
{
    firmware_store_t *h = NULL;
    firmware_store_config_t good = {
        .qspi = &fake_qspi_flash_if,
        .iflash = &fake_iflash_if,
        .running_image_base = FIRMWARE_SLOT_A_BASE,
    };
    firmware_store_config_t bad;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, firmware_store_create(NULL, &h));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, firmware_store_create(&good, NULL));

    bad = good;
    bad.qspi = NULL;
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, firmware_store_create(&bad, &h));

    bad = good;
    bad.iflash = NULL;
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, firmware_store_create(&bad, &h));

    /* None of the failures consumed the single instance. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, firmware_store_create(&good, &h));
}

void test_TC_FS_002_second_create_is_refused(void)
{
    const firmware_store_config_t cfg = {
        .qspi = &fake_qspi_flash_if,
        .iflash = &fake_iflash_if,
        .running_image_base = FIRMWARE_SLOT_A_BASE,
    };
    firmware_store_t *first = NULL;
    firmware_store_t *second = NULL;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, firmware_store_create(&cfg, &first));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NO_INSTANCE, firmware_store_create(&cfg, &second));
    TEST_ASSERT_NULL(second);
    TEST_ASSERT_TRUE(fsu_log_count(0U) >= 1U);
}

void test_TC_FS_002b_mutex_creation_failure_is_reported(void)
{
    const firmware_store_config_t cfg = {
        .qspi = &fake_qspi_flash_if,
        .iflash = &fake_iflash_if,
        .running_image_base = FIRMWARE_SLOT_A_BASE,
    };
    firmware_store_t *h = NULL;

    g_mock_xSemaphoreCreateMutexStatic_return = NULL;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NO_INSTANCE, firmware_store_create(&cfg, &h));
    TEST_ASSERT_NULL(h);
}

void test_TC_FS_003_every_operation_checks_its_handle(void)
{
    static int dummy;
    firmware_store_t *bogus = (firmware_store_t *) (void *) &dummy;
    uint32_t offset = 0U;
    uint8_t byte = 0U;
    bool done = false;
    firmware_image_info_t info;
    firmware_boot_info_t boot;

    /* A handle that did not come from create(). */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_INITIALISED, IMG->begin(bogus, 1U, 1000U));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_INITIALISED,
                          IMG->resume(bogus, 1U, 1000U, &offset));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_INITIALISED,
                          IMG->write_chunk(bogus, 0U, &byte, 1U));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_INITIALISED, IMG->verify(bogus, &info));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_INITIALISED, IMG->install_begin(bogus));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_INITIALISED, IMG->install_step(bogus, &done));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_INITIALISED, IMG->activate(bogus));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_INITIALISED, IMG->discard(bogus));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_INITIALISED, BOOT->get_boot_info(bogus, &boot));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_INITIALISED, BOOT->confirm(bogus));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_INITIALISED, BOOT->rollback(bogus));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_INITIALISED, BOOT->acknowledge_rollback(bogus));

    arrange_capable(BODY_LEN);

    /* A bogus handle is still refused once a real instance exists. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_INITIALISED, IMG->begin(bogus, 1U, 1000U));

    /* NULL self. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, IMG->begin(NULL, 1U, 1000U));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, IMG->resume(NULL, 1U, 1000U, &offset));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, IMG->write_chunk(NULL, 0U, &byte, 1U));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, IMG->verify(NULL, &info));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, IMG->install_begin(NULL));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, IMG->install_step(NULL, &done));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, IMG->activate(NULL));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, IMG->discard(NULL));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, BOOT->get_boot_info(NULL, &boot));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, BOOT->confirm(NULL));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, BOOT->rollback(NULL));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, BOOT->acknowledge_rollback(NULL));

    /* NULL out-parameters and data. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, IMG->resume(fsu_fs, 1U, 1000U, NULL));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, IMG->write_chunk(fsu_fs, 0U, NULL, 1U));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, IMG->verify(fsu_fs, NULL));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, IMG->install_step(fsu_fs, NULL));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, BOOT->get_boot_info(fsu_fs, NULL));
}

/* ====================================================================== */
/* Verification                                                            */
/* ====================================================================== */

void test_TC_FS_040_verify_before_the_last_byte_is_incomplete(void)
{
    firmware_image_info_t info;

    arrange_capable(BODY_LEN);

    /* No session at all: wrong state. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->verify(fsu_fs, &info));

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->write_chunk(fsu_fs, 0U, s_file, 500U));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_INCOMPLETE, IMG->verify(fsu_fs, &info));
}

void test_TC_FS_041_valid_image_verifies_with_the_fake_crypto(void)
{
    firmware_image_info_t info = {0};

    arrange_received(BODY_LEN);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->verify(fsu_fs, &info));
    TEST_ASSERT_EQUAL_UINT32(VERSION_NEW, info.version);
    TEST_ASSERT_EQUAL_UINT32(BODY_LEN, info.image_len);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, info.target_slot);
    TEST_ASSERT_EQUAL_UINT32(2U, fake_crypto_verify_calls() + 1U); /* exactly one signature check */

    /* VERIFIED is what install_begin needs. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->install_begin(fsu_fs));
}

void test_TC_FS_042_bad_magic_version_or_size_is_a_bad_header(void)
{
    firmware_image_header_t *h = (firmware_image_header_t *) (void *) s_file;

    arrange_capable(BODY_LEN);
    h->magic ^= 1U;
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_HEADER, stage_and_verify());

    reset_world();
    arrange_capable(BODY_LEN);
    h->header_version = 2U;
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_HEADER, stage_and_verify());

    reset_world();
    arrange_capable(BODY_LEN);
    h->header_size = 64U;
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_HEADER, stage_and_verify());

    /* Nothing cryptographic ran: the structural checks come first. */
    TEST_ASSERT_EQUAL_UINT32(0U, fake_crypto_verify_calls());
}

void test_TC_FS_043_image_len_must_match_the_file_length(void)
{
    firmware_image_header_t *h = (firmware_image_header_t *) (void *) s_file;

    arrange_capable(BODY_LEN);
    h->image_len = BODY_LEN - 1U;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_HEADER, stage_and_verify());
}

void test_TC_FS_043b_image_shorter_than_a_vector_pair_is_a_bad_header(void)
{
    fsu_seed_running_a();
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    s_len = FIRMWARE_HEADER_SIZE + 4U;
    (void) memset(s_file, 0, sizeof(s_file));
    ((firmware_image_header_t *) (void *) s_file)->magic = FIRMWARE_IMAGE_MAGIC;
    ((firmware_image_header_t *) (void *) s_file)->header_version = FIRMWARE_IMAGE_HEADER_VERSION;
    ((firmware_image_header_t *) (void *) s_file)->header_size = FIRMWARE_HEADER_SIZE;
    ((firmware_image_header_t *) (void *) s_file)->image_len = 4U;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_HEADER, stage_and_verify());
}

void test_TC_FS_044_an_image_linked_for_the_running_slot_is_refused(void)
{
    fsu_seed_running_a();
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    s_len = fsu_build_image(s_file, BODY_LEN, FIRMWARE_SLOT_A_BASE, VERSION_NEW);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_WRONG_SLOT, stage_and_verify());
}

void test_TC_FS_044b_running_from_slot_b_wants_a_slot_a_image(void)
{
    firmware_image_info_t info;

    fsu_seed_meta(0U, FIRMWARE_BANK_B, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_A, 0U, 4U);
    fsu_seed_slot(FIRMWARE_SLOT_B, FSU_RUNNING_BODY_LEN, FSU_RUNNING_VERSION);
    (void) fsu_create(FIRMWARE_SLOT_B_BASE);
    s_len = fsu_build_image(s_file, BODY_LEN, FIRMWARE_SLOT_A_BASE, VERSION_NEW);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_download(s_file, s_len, IMAGE_ID, CHUNK));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->verify(fsu_fs, &info));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, info.target_slot);
}

void test_TC_FS_045_a_forged_signature_is_sig_invalid(void)
{
    arrange_capable(BODY_LEN);
    fake_crypto_set_verify_mode(FAKE_VERIFY_FORCE_INVALID);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_SIG_INVALID, stage_and_verify());
}

void test_TC_FS_045b_a_changed_header_field_breaks_the_fake_signature(void)
{
    arrange_capable(BODY_LEN);
    ((firmware_image_header_t *) (void *) s_file)->version ^=
        1U; /* signed field, signature stale */

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_SIG_INVALID, stage_and_verify());
}

void test_TC_FS_046_a_changed_body_byte_is_a_hash_mismatch(void)
{
    arrange_capable(BODY_LEN);
    s_file[FIRMWARE_HEADER_SIZE + 1000U] ^= 0x40U; /* header still describes the original body */

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_HASH_MISMATCH, stage_and_verify());
}

void test_TC_FS_048_a_crypto_failure_is_crypto_never_sig_invalid(void)
{
    firmware_image_info_t info;

    arrange_received(BODY_LEN);
    fake_crypto_set_verify_mode(FAKE_VERIFY_FORCE_FAILED);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_CRYPTO, IMG->verify(fsu_fs, &info));

    /* The image stays RECEIVED, so a retry after the fault clears succeeds. */
    fake_crypto_set_verify_mode(FAKE_VERIFY_NORMAL);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->verify(fsu_fs, &info));
}

void test_TC_FS_049_a_reset_vector_outside_the_image_is_a_bad_image(void)
{
    uint32_t vector;

    /* Outside the image. */
    arrange_capable(BODY_LEN);
    vector = FIRMWARE_SLOT_B_BASE + BODY_LEN + 0x101U;
    (void) memcpy(&s_file[FIRMWARE_HEADER_SIZE + 4U], &vector, sizeof(vector));
    fsu_refresh_header(s_file);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_IMAGE, stage_and_verify());

    /* Thumb bit clear. */
    reset_world();
    arrange_capable(BODY_LEN);
    vector = FIRMWARE_SLOT_B_BASE + 0x100U;
    (void) memcpy(&s_file[FIRMWARE_HEADER_SIZE + 4U], &vector, sizeof(vector));
    fsu_refresh_header(s_file);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_IMAGE, stage_and_verify());

    /* Below the slot base. */
    reset_world();
    arrange_capable(BODY_LEN);
    vector = FIRMWARE_SLOT_A_BASE + 0x101U;
    (void) memcpy(&s_file[FIRMWARE_HEADER_SIZE + 4U], &vector, sizeof(vector));
    fsu_refresh_header(s_file);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_IMAGE, stage_and_verify());
}

void test_TC_FS_050_a_qspi_read_failure_is_flash_read(void)
{
    firmware_image_info_t info;

    arrange_received(BODY_LEN);
    fake_qspi_fail_read_at(FIRMWARE_STORE_STAGING_BASE + FIRMWARE_STAGING_IMAGE_OFFSET);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_READ, IMG->verify(fsu_fs, &info));

    /* A read fault in the body is reported the same way. */
    fake_qspi_fail_read_at(FAKE_QSPI_NO_ADDR);
    fake_qspi_fail_all_reads(true);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_READ, IMG->verify(fsu_fs, &info));
    TEST_ASSERT_TRUE(fsu_log_count(0U) >= 1U);

    /* The image is still RECEIVED once the fault clears. */
    fake_qspi_fail_all_reads(false);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->verify(fsu_fs, &info));
}

void test_TC_FS_051_beginning_again_discards_a_verification(void)
{
    arrange_verified(BODY_LEN);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID + 1U, s_len));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->install_begin(fsu_fs));

    /* So does resume of a complete session: it comes back RECEIVED, not VERIFIED. */
    reset_world();
    arrange_verified(BODY_LEN);
    uint32_t offset = 0U;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset));
    TEST_ASSERT_EQUAL_UINT32(s_len, offset);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->install_begin(fsu_fs));
}

/* ====================================================================== */
/* Installation                                                            */
/* ====================================================================== */

void test_TC_FS_060_install_needs_a_verified_image(void)
{
    bool done = false;

    arrange_received(BODY_LEN);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->install_begin(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->install_step(fsu_fs, &done));
    TEST_ASSERT_FALSE(done);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_erase_calls() + fake_iflash_program_calls());
}

void test_TC_FS_061_the_target_descriptor_is_erased_before_any_slot_page(void)
{
    firmware_slot_descriptor_t d;

    arrange_verified(BODY_LEN);
    fsu_seed_slot(FIRMWARE_SLOT_B, 1000U, 0x00090000UL); /* a stale, valid descriptor for B */

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->install_begin(fsu_fs));

    /* Its very first on-chip operation: erase the descriptor page. */
    TEST_ASSERT_EQUAL_UINT32(1U, fake_iflash_log_count());
    TEST_ASSERT_EQUAL_INT(FAKE_IFLASH_OP_ERASE, fake_iflash_log_at(0U)->op);
    TEST_ASSERT_EQUAL_HEX32(FIRMWARE_DESC_B_ADDR, fake_iflash_log_at(0U)->addr);

    (void) memcpy(&d, fake_iflash_ptr(FIRMWARE_DESC_B_ADDR), sizeof(d));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFUL, d.magic);

    /* Everything after that is slot pages, then the descriptor itself as the last two programs
     * (three double words, then the CRC double word). */
    bool done = false;

    while (!done)
    {
        TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->install_step(fsu_fs, &done));
    }

    const uint32_t count = fake_iflash_log_count();

    for (uint32_t i = 1U; i < (count - 2U); i++)
    {
        TEST_ASSERT_TRUE(fake_iflash_log_at(i)->addr >= FIRMWARE_SLOT_B_BASE);
    }

    TEST_ASSERT_EQUAL_INT(FAKE_IFLASH_OP_PROGRAM, fake_iflash_log_at(count - 2U)->op);
    TEST_ASSERT_EQUAL_HEX32(FIRMWARE_DESC_B_ADDR, fake_iflash_log_at(count - 2U)->addr);
    TEST_ASSERT_EQUAL_UINT32(24U, fake_iflash_log_at(count - 2U)->len);
    TEST_ASSERT_EQUAL_HEX32(FIRMWARE_DESC_B_ADDR + 24U, fake_iflash_log_at(count - 1U)->addr);
    TEST_ASSERT_EQUAL_UINT32(8U, fake_iflash_log_at(count - 1U)->len);
}

void test_TC_FS_062_the_step_loop_copies_the_body_and_seals_the_slot(void)
{
    firmware_slot_descriptor_t d;
    uint32_t steps = 0U;
    bool valid = false;

    arrange_verified(BODY_LEN);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_install_all(&steps));
    TEST_ASSERT_EQUAL_UINT32(3U, steps);
    TEST_ASSERT_EQUAL_MEMORY(&s_file[FIRMWARE_HEADER_SIZE], fake_iflash_ptr(FIRMWARE_SLOT_B_BASE),
                             BODY_LEN);

    (void) memcpy(&d, fake_iflash_ptr(FIRMWARE_DESC_B_ADDR), sizeof(d));
    TEST_ASSERT_EQUAL_HEX32(FIRMWARE_DESC_MAGIC, d.magic);
    TEST_ASSERT_EQUAL_UINT32(BODY_LEN, d.image_len);
    TEST_ASSERT_EQUAL_HEX32(VERSION_NEW, d.version);
    TEST_ASSERT_EQUAL_HEX32(FIRMWARE_SLOT_B_BASE, d.load_addr);
    TEST_ASSERT_EQUAL_HEX32(config_store_crc32(&s_file[FIRMWARE_HEADER_SIZE], BODY_LEN),
                            d.body_crc32);
    TEST_ASSERT_EQUAL_HEX32(config_store_crc32((const uint8_t *) &d, FIRMWARE_DESC_CRC_SPAN),
                            d.desc_crc32);

    /* The module itself agrees. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK,
                          fs_desc_read(&fake_iflash_if, FIRMWARE_SLOT_B, &valid, NULL));
    TEST_ASSERT_TRUE(valid);
}

void test_TC_FS_063_nothing_outside_the_target_slot_and_descriptor_is_written(void)
{
    arrange_verified(BODY_LEN);

    fake_iflash_guard_add(FIRMWARE_BOOTLOADER_BASE,
                          FIRMWARE_BOOTLOADER_BASE + FIRMWARE_BOOTLOADER_SIZE);
    fake_iflash_guard_add(FIRMWARE_META_A_ADDR, FIRMWARE_META_B_ADDR + FIRMWARE_PAGE_SIZE);
    fake_iflash_guard_add(FIRMWARE_DESC_A_ADDR, FIRMWARE_DESC_A_ADDR + FIRMWARE_PAGE_SIZE);
    fake_iflash_guard_add(FIRMWARE_SLOT_A_BASE, FIRMWARE_SLOT_A_BASE + FIRMWARE_SLOT_SIZE);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_install_all(NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_guard_violations());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_rule_violations());
}

void test_TC_FS_064_the_step_count_is_the_page_count(void)
{
    static const uint32_t k_body_len[] = {8U, 2047U, 2048U, 2049U, 4096U, 8191U, 8192U, 8193U};

    for (uint32_t i = 0U; i < (sizeof(k_body_len) / sizeof(k_body_len[0])); i++)
    {
        uint32_t steps = 0U;

        reset_world();
        arrange_verified(k_body_len[i]);

        TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_install_all(&steps));
        TEST_ASSERT_EQUAL_UINT32((k_body_len[i] + 2047U) / 2048U, steps);
        TEST_ASSERT_EQUAL_MEMORY(&s_file[FIRMWARE_HEADER_SIZE],
                                 fake_iflash_ptr(FIRMWARE_SLOT_B_BASE), k_body_len[i]);
    }
}

void test_TC_FS_065_the_last_page_is_padded_and_the_crc_covers_the_image_only(void)
{
    const uint32_t body_len = 4195U; /* last page holds 99 bytes, padded to 104 */
    firmware_slot_descriptor_t d;
    const uint8_t *slot;

    arrange_verified(body_len);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_install_all(NULL));

    slot = fake_iflash_ptr(FIRMWARE_SLOT_B_BASE);
    TEST_ASSERT_EQUAL_MEMORY(&s_file[FIRMWARE_HEADER_SIZE], slot, body_len);

    for (uint32_t i = body_len; i < (4096U + 104U); i++)
    {
        TEST_ASSERT_EQUAL_HEX8(0xFFU, slot[i]); /* the pad, then the erased remainder */
    }

    (void) memcpy(&d, fake_iflash_ptr(FIRMWARE_DESC_B_ADDR), sizeof(d));
    TEST_ASSERT_EQUAL_UINT32(body_len, d.image_len);
    TEST_ASSERT_EQUAL_HEX32(config_store_crc32(&s_file[FIRMWARE_HEADER_SIZE], body_len),
                            d.body_crc32);
}

void test_TC_FS_066_a_readback_bit_flip_leaves_no_descriptor(void)
{
    bool valid = true;
    bool done = false;

    arrange_verified(BODY_LEN);
    fake_iflash_corrupt_after_program(FIRMWARE_SLOT_B_BASE + 10U);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->install_begin(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_INSTALL_VERIFY, IMG->install_step(fsu_fs, &done));
    TEST_ASSERT_FALSE(done);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK,
                          fs_desc_read(&fake_iflash_if, FIRMWARE_SLOT_B, &valid, NULL));
    TEST_ASSERT_FALSE(valid);
    TEST_ASSERT_TRUE(fsu_log_count(0U) >= 1U);
}

void test_TC_FS_067_staging_altered_after_verify_is_caught_at_the_end(void)
{
    bool valid = true;

    arrange_verified(BODY_LEN);

    /* A QSPI bit flips in the last page after verify() passed. */
    fake_qspi_mem()[FIRMWARE_STORE_STAGING_BASE + FIRMWARE_STAGING_IMAGE_OFFSET +
                    FIRMWARE_HEADER_SIZE + 4100U] ^= 0x01U;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_INSTALL_VERIFY, fsu_install_all(NULL));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK,
                          fs_desc_read(&fake_iflash_if, FIRMWARE_SLOT_B, &valid, NULL));
    TEST_ASSERT_FALSE(valid);
}

void test_TC_FS_068_an_erase_failure_is_flash_erase_and_install_can_be_retried(void)
{
    uint32_t steps = 0U;
    bool done = false;

    arrange_verified(BODY_LEN);
    fake_iflash_fail_erase_at(FIRMWARE_SLOT_B_BASE + FIRMWARE_PAGE_SIZE);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->install_begin(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->install_step(fsu_fs, &done));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_ERASE, IMG->install_step(fsu_fs, &done));

    /* Back to VERIFIED: install_step is refused, install_begin works again. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->install_step(fsu_fs, &done));
    fake_iflash_fail_erase_at(FAKE_IFLASH_NO_ADDR);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_install_all(&steps));
    TEST_ASSERT_EQUAL_UINT32(3U, steps);
    TEST_ASSERT_EQUAL_MEMORY(&s_file[FIRMWARE_HEADER_SIZE], fake_iflash_ptr(FIRMWARE_SLOT_B_BASE),
                             BODY_LEN);
}

void test_TC_FS_068b_program_and_read_failures_are_mapped(void)
{
    bool done = false;

    arrange_verified(BODY_LEN);

    fake_iflash_fail_program_at(FIRMWARE_SLOT_B_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->install_begin(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_WRITE, IMG->install_step(fsu_fs, &done));
    fake_iflash_fail_program_at(FAKE_IFLASH_NO_ADDR);

    fake_qspi_fail_all_reads(true);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->install_begin(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_READ, IMG->install_step(fsu_fs, &done));
    fake_qspi_fail_all_reads(false);

    fake_iflash_fail_all_reads(true);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->install_begin(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_READ, IMG->install_step(fsu_fs, &done));
}

/* ====================================================================== */
/* Mutex, CRC and format                                                   */
/* ====================================================================== */

void test_TC_FS_095_the_mutex_is_taken_and_given_once_around_each_operation(void)
{
    firmware_boot_info_t boot;
    uint32_t operations = 0U;

    arrange_capable(BODY_LEN);
    g_mock_xSemaphoreTake_call_count = 0U;
    g_mock_xSemaphoreGive_call_count = 0U;

    /* Success and error paths alike. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    operations++;
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_PARAM, IMG->begin(fsu_fs, 0xFFFFFFFFU, s_len));
    operations++;
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->write_chunk(fsu_fs, 0U, s_file, 100U));
    operations++;
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_PARAM, IMG->write_chunk(fsu_fs, 7U, s_file, 100U));
    operations++;
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->get_boot_info(fsu_fs, &boot));
    operations++;
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, BOOT->rollback(fsu_fs));
    operations++;
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->discard(fsu_fs));
    operations++;

    /* A call refused before the lock (NULL self) neither takes nor gives. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NULL_POINTER, IMG->discard(NULL));

    TEST_ASSERT_EQUAL_UINT32(operations, g_mock_xSemaphoreTake_call_count);
    TEST_ASSERT_EQUAL_UINT32(operations, g_mock_xSemaphoreGive_call_count);
}

void test_TC_FS_098_the_crc_check_value(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926UL, config_store_crc32((const uint8_t *) "123456789", 9U));
}

void test_TC_FS_099_the_on_flash_structures_have_the_specified_sizes(void)
{
    /* Also enforced by _Static_assert in firmware_image_format.h; listed for traceability. */
    TEST_ASSERT_EQUAL_UINT32(128U, sizeof(firmware_image_header_t));
    TEST_ASSERT_EQUAL_UINT32(32U, sizeof(firmware_slot_descriptor_t));
    TEST_ASSERT_EQUAL_UINT32(24U, sizeof(firmware_boot_meta_t));
    TEST_ASSERT_EQUAL_UINT32(16U, sizeof(firmware_journal_session_t));
    TEST_ASSERT_EQUAL_UINT32(8U, sizeof(firmware_journal_entry_t));
}
