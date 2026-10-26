/**
 * @file test_fs_staging.c
 * @brief Unit tests for FirmwareStore download, resume and the staging journal.
 *
 * Test IDs follow docs/lld/middleware/firmware-store.md §22 "Download and resume"
 * (TC-FS-020..037). The journal is inspected directly in the QSPI fake so the
 * on-flash format is checked independently of the code that writes it.
 *
 * Build defines: STM32L475xx, BOARD_GATEWAY, TEST (:test_fs_staging:).
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

#define IMAGE_ID (0x1234U)
#define FILE_LEN (10000U)
#define VERSION_NEW (0x01020003UL)

#define JOURNAL_BASE (FIRMWARE_STORE_STAGING_BASE)
#define IMAGE_BASE (FIRMWARE_STORE_STAGING_BASE + FIRMWARE_STAGING_IMAGE_OFFSET)

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

static void arrange_capable(void)
{
    fsu_seed_running_a();
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
}

/** @brief A capable device and a built file of roughly FILE_LEN bytes. */
static void arrange_file(void)
{
    arrange_capable();
    s_len =
        fsu_build_image(s_file, FILE_LEN - FIRMWARE_HEADER_SIZE, FIRMWARE_SLOT_B_BASE, VERSION_NEW);
}

/** @brief Simulate a reboot: a fresh module instance over the same flash. */
static void reboot(void)
{
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
}

/** @brief Journal checkpoint values written so far; returns how many entries are programmed. */
static uint32_t journal_values(uint32_t *values, uint32_t max)
{
    const uint8_t *j = &fake_qspi_mem()[JOURNAL_BASE];
    uint32_t count = 0U;

    for (uint32_t i = 0U; i < FIRMWARE_JOURNAL_ENTRY_COUNT; i++)
    {
        firmware_journal_entry_t e;

        (void) memcpy(&e, &j[FIRMWARE_JOURNAL_ENTRIES_OFFSET + (i * sizeof(e))], sizeof(e));

        if ((e.value == 0xFFFFFFFFUL) && (e.inverse == 0xFFFFFFFFUL))
        {
            break;
        }

        TEST_ASSERT_EQUAL_HEX32(~e.value, e.inverse);

        if (count < max)
        {
            values[count] = e.value;
        }

        count++;
    }

    return count;
}

/* ====================================================================== */
/* begin                                                                   */
/* ====================================================================== */

void test_TC_FS_020_begin_when_not_ota_capable(void)
{
    /* Blank metadata. */
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_OTA_CAPABLE,
                          IMG->begin(fsu_fs, IMAGE_ID, FILE_LEN));

    /* A development image (not in a slot). */
    reset_world();
    fsu_seed_running_a();
    (void) fsu_create(0x08000000UL);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_OTA_CAPABLE,
                          IMG->begin(fsu_fs, IMAGE_ID, FILE_LEN));

    TEST_ASSERT_EQUAL_UINT32(0U, fake_qspi_erase_calls());
}

void test_TC_FS_021_begin_during_a_trial_or_after_a_rollback_is_a_bad_state(void)
{
    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_TRIAL_RUNNING, FIRMWARE_BANK_B, 0U, 2U);
    fsu_seed_slot(FIRMWARE_SLOT_A, FSU_RUNNING_BODY_LEN, FSU_RUNNING_VERSION);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->begin(fsu_fs, IMAGE_ID, FILE_LEN));

    reset_world();
    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_ROLLED_BACK, FIRMWARE_BANK_B, 1U, 2U);
    fsu_seed_slot(FIRMWARE_SLOT_A, FSU_RUNNING_BODY_LEN, FSU_RUNNING_VERSION);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->begin(fsu_fs, IMAGE_ID, FILE_LEN));

    TEST_ASSERT_EQUAL_UINT32(0U, fake_qspi_erase_calls());
}

void test_TC_FS_022_begin_validates_its_parameters(void)
{
    arrange_capable();

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_PARAM, IMG->begin(fsu_fs, IMAGE_ID, 0U));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_PARAM,
                          IMG->begin(fsu_fs, IMAGE_ID, FIRMWARE_HEADER_SIZE));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_PARAM, IMG->begin(fsu_fs, 0xFFFFFFFFUL, FILE_LEN));
    TEST_ASSERT_EQUAL_INT(
        FIRMWARE_STORE_ERR_TOO_LARGE,
        IMG->begin(fsu_fs, IMAGE_ID, FIRMWARE_HEADER_SIZE + FIRMWARE_MAX_IMAGE_LEN + 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_qspi_erase_calls());

    /* The largest legal file is accepted. */
    TEST_ASSERT_EQUAL_INT(
        FIRMWARE_STORE_ERR_OK,
        IMG->begin(fsu_fs, IMAGE_ID, FIRMWARE_HEADER_SIZE + FIRMWARE_MAX_IMAGE_LEN));
    /* ... and so is a file with the smallest legal body. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK,
                          IMG->begin(fsu_fs, IMAGE_ID, FIRMWARE_HEADER_SIZE + 1U));
}

void test_TC_FS_023_begin_writes_a_valid_session_record(void)
{
    firmware_journal_session_t s;
    uint8_t data[4] = {1U, 2U, 3U, 4U};

    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));

    (void) memcpy(&s, &fake_qspi_mem()[JOURNAL_BASE], sizeof(s));
    TEST_ASSERT_EQUAL_HEX32(FIRMWARE_JOURNAL_MAGIC, s.magic);
    TEST_ASSERT_EQUAL_HEX32(IMAGE_ID, s.image_id);
    TEST_ASSERT_EQUAL_UINT32(s_len, s.file_len);
    TEST_ASSERT_EQUAL_HEX32(config_store_crc32((const uint8_t *) &s, 12U), s.crc32);

    /* The write position is zero: offset 4 is refused, offset 0 accepted. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_PARAM, IMG->write_chunk(fsu_fs, 4U, data, 4U));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->write_chunk(fsu_fs, 0U, data, 4U));
}

void test_TC_FS_023b_begin_flash_failures_are_reported_and_leave_the_store_idle(void)
{
    uint8_t data[4] = {0U};

    arrange_file();

    fake_qspi_fail_erase_at(JOURNAL_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_ERASE, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->write_chunk(fsu_fs, 0U, data, 4U));
    fake_qspi_fail_erase_at(FAKE_QSPI_NO_ADDR);

    fake_qspi_fail_program_at(JOURNAL_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_WRITE, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    fake_qspi_fail_program_at(FAKE_QSPI_NO_ADDR);

    fake_qspi_fail_all_reads(true);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_READ, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    fake_qspi_fail_all_reads(false);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    TEST_ASSERT_TRUE(fsu_log_count(0U) >= 3U);
}

/* ====================================================================== */
/* write_chunk                                                             */
/* ====================================================================== */

void test_TC_FS_024_a_non_contiguous_chunk_writes_nothing(void)
{
    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->write_chunk(fsu_fs, 0U, s_file, 100U));

    const uint32_t before = fake_qspi_program_calls();

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_PARAM,
                          IMG->write_chunk(fsu_fs, 101U, &s_file[101], 50U));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_PARAM,
                          IMG->write_chunk(fsu_fs, 50U, &s_file[50], 50U));
    TEST_ASSERT_EQUAL_UINT32(before, fake_qspi_program_calls());

    /* The write position did not move. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->write_chunk(fsu_fs, 100U, &s_file[100], 50U));
}

void test_TC_FS_025_a_chunk_past_the_end_or_empty_is_refused(void)
{
    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));

    const uint32_t before = fake_qspi_program_calls(); /* the session record */

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_PARAM,
                          IMG->write_chunk(fsu_fs, 0U, s_file, s_len + 1U));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_PARAM, IMG->write_chunk(fsu_fs, 0U, s_file, 0U));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_PARAM,
                          IMG->write_chunk(fsu_fs, 0U, s_file, 0xFFFFFFFFUL));
    TEST_ASSERT_EQUAL_UINT32(before, fake_qspi_program_calls());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_qspi_rule_violations());
}

void test_TC_FS_025b_write_chunk_without_a_session_is_a_bad_state(void)
{
    arrange_file();

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->write_chunk(fsu_fs, 0U, s_file, 10U));

    /* And once the file is complete there is nothing left to write. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_download(s_file, s_len, IMAGE_ID, 4096U));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE,
                          IMG->write_chunk(fsu_fs, s_len, s_file, 1U));
}

void test_TC_FS_026_chunks_are_split_at_256_byte_pages(void)
{
    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));

    const uint32_t base = fake_qspi_program_calls(); /* the session record */

    /* 700 bytes from offset 0: 256 + 256 + 188. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->write_chunk(fsu_fs, 0U, s_file, 700U));
    TEST_ASSERT_EQUAL_UINT32(3U, fake_qspi_program_calls() - base);

    /* 100 bytes then 300 bytes: the second starts mid-page and crosses one boundary. */
    reset_world();
    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    const uint32_t base2 = fake_qspi_program_calls();

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->write_chunk(fsu_fs, 0U, s_file, 100U));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK,
                          IMG->write_chunk(fsu_fs, 100U, &s_file[100], 300U));
    TEST_ASSERT_EQUAL_UINT32(3U, fake_qspi_program_calls() - base2);

    TEST_ASSERT_EQUAL_MEMORY(s_file, &fake_qspi_mem()[IMAGE_BASE], 400U);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_qspi_rule_violations());
}

void test_TC_FS_027_each_sector_is_erased_once_on_first_entry(void)
{
    const uint32_t first_sector = IMAGE_BASE / QSPI_FLASH_SECTOR_SIZE_BYTES;

    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_download(s_file, s_len, IMAGE_ID, 777U));

    /* 10 000 bytes span three sectors; the journal sector was erased once by begin. */
    TEST_ASSERT_EQUAL_UINT32(1U, fake_qspi_sector_erases(first_sector));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_qspi_sector_erases(first_sector + 1U));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_qspi_sector_erases(first_sector + 2U));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_qspi_sector_erases(first_sector + 3U));
    TEST_ASSERT_EQUAL_UINT32(1U,
                             fake_qspi_sector_erases(JOURNAL_BASE / QSPI_FLASH_SECTOR_SIZE_BYTES));
    TEST_ASSERT_EQUAL_UINT32(4U, fake_qspi_erase_calls());
}

void test_TC_FS_028_a_checkpoint_is_appended_at_each_4kb_boundary(void)
{
    uint32_t values[8] = {0U};

    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_download(s_file, s_len, IMAGE_ID, 1000U));

    /* Boundaries 4096 and 8192, then the end of the file. */
    TEST_ASSERT_EQUAL_UINT32(3U, journal_values(values, 8U));
    TEST_ASSERT_EQUAL_UINT32(4096U, values[0]);
    TEST_ASSERT_EQUAL_UINT32(8192U, values[1]);
    TEST_ASSERT_EQUAL_UINT32(s_len, values[2]);
}

void test_TC_FS_028b_chunking_does_not_change_the_checkpoints(void)
{
    uint32_t values[8] = {0U};

    arrange_file();

    /* One call for the lot: the boundaries inside it are still recorded. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_download(s_file, s_len, IMAGE_ID, s_len));
    TEST_ASSERT_EQUAL_UINT32(3U, journal_values(values, 8U));
    TEST_ASSERT_EQUAL_UINT32(4096U, values[0]);
    TEST_ASSERT_EQUAL_UINT32(8192U, values[1]);
    TEST_ASSERT_EQUAL_UINT32(s_len, values[2]);
}

void test_TC_FS_029_the_final_byte_completes_the_file(void)
{
    uint32_t values[8] = {0U};
    uint32_t offset = 0U;
    firmware_image_info_t info;

    /* A file whose length is itself a 4 KB multiple gets one entry for the end, not two. */
    arrange_capable();
    s_len =
        fsu_build_image(s_file, 8192U - FIRMWARE_HEADER_SIZE, FIRMWARE_SLOT_B_BASE, VERSION_NEW);
    TEST_ASSERT_EQUAL_UINT32(8192U, s_len);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_download(s_file, s_len, IMAGE_ID, 1000U));
    TEST_ASSERT_EQUAL_UINT32(2U, journal_values(values, 8U));
    TEST_ASSERT_EQUAL_UINT32(8192U, values[1]);

    /* RECEIVED: verify() no longer says INCOMPLETE, and a resume sees a complete file. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->verify(fsu_fs, &info));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset));
    TEST_ASSERT_EQUAL_UINT32(s_len, offset);
}

void test_TC_FS_030_a_flash_error_mid_chunk_returns_to_idle(void)
{
    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));

    fake_qspi_fail_program_at(IMAGE_BASE + 256U);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_WRITE,
                          IMG->write_chunk(fsu_fs, 0U, s_file, 600U));
    fake_qspi_fail_program_at(FAKE_QSPI_NO_ADDR);

    /* IDLE: the caller must resume. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->write_chunk(fsu_fs, 0U, s_file, 600U));
    TEST_ASSERT_TRUE(fsu_log_count(0U) >= 1U);

    /* An erase failure is reported as such. */
    reset_world();
    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    fake_qspi_fail_erase_at(IMAGE_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_ERASE,
                          IMG->write_chunk(fsu_fs, 0U, s_file, 100U));
}

void test_TC_FS_030b_a_full_journal_is_a_write_error(void)
{
    /* Drive the staging layer directly with a journal that has no free entry. */
    uint32_t next = FIRMWARE_JOURNAL_SIZE;
    uint8_t chunk[4096];

    (void) memset(chunk, 0x5A, sizeof(chunk));
    arrange_capable();

    TEST_ASSERT_EQUAL_INT(
        FIRMWARE_STORE_ERR_FLASH_WRITE,
        fs_stage_write(&fake_qspi_flash_if, 0U, chunk, sizeof(chunk), 100000U, &next));
}

/* ====================================================================== */
/* resume and discard                                                      */
/* ====================================================================== */

void test_TC_FS_031_resume_returns_the_last_checkpoint(void)
{
    uint32_t offset = 0xDEADU;

    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->write_chunk(fsu_fs, 0U, s_file, 6000U));

    reboot(); /* the RAM state is gone; the flash is not */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset));

    /* At most 4 KB is downloaded again. */
    TEST_ASSERT_EQUAL_UINT32(4096U, offset);
}

void test_TC_FS_031b_a_session_with_no_checkpoint_resumes_at_zero(void)
{
    uint32_t offset = 0xDEADU;

    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->write_chunk(fsu_fs, 0U, s_file, 3000U));

    reboot();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset));
    TEST_ASSERT_EQUAL_UINT32(0U, offset);
}

void test_TC_FS_031c_resume_obeys_the_same_preconditions_as_begin(void)
{
    uint32_t offset = 0U;

    (void) fsu_create(FIRMWARE_SLOT_A_BASE); /* blank metadata: not capable */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NOT_OTA_CAPABLE,
                          IMG->resume(fsu_fs, IMAGE_ID, FILE_LEN, &offset));

    reset_world();
    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_TRIAL_RUNNING, FIRMWARE_BANK_B, 0U, 2U);
    fsu_seed_slot(FIRMWARE_SLOT_A, FSU_RUNNING_BODY_LEN, FSU_RUNNING_VERSION);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE,
                          IMG->resume(fsu_fs, IMAGE_ID, FILE_LEN, &offset));

    reset_world();
    arrange_capable();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_PARAM,
                          IMG->resume(fsu_fs, 0xFFFFFFFFUL, FILE_LEN, &offset));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_PARAM,
                          IMG->resume(fsu_fs, IMAGE_ID, 100U, &offset));
    TEST_ASSERT_EQUAL_INT(
        FIRMWARE_STORE_ERR_TOO_LARGE,
        IMG->resume(fsu_fs, IMAGE_ID, FIRMWARE_HEADER_SIZE + FIRMWARE_MAX_IMAGE_LEN + 1U, &offset));
}

void test_TC_FS_032_resume_for_another_file_or_none_is_no_session(void)
{
    uint32_t offset = 0xDEADU;

    arrange_file();

    /* No session at all: a blank journal. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NO_SESSION,
                          IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset));

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->write_chunk(fsu_fs, 0U, s_file, 5000U));
    reboot();

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NO_SESSION,
                          IMG->resume(fsu_fs, IMAGE_ID + 1U, s_len, &offset));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NO_SESSION,
                          IMG->resume(fsu_fs, IMAGE_ID, s_len + 1U, &offset));
    TEST_ASSERT_EQUAL_UINT32(0xDEADU, offset); /* untouched on failure */

    /* The matching session is still there. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset));
}

void test_TC_FS_032b_a_torn_session_record_is_no_session(void)
{
    uint32_t offset = 0U;

    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    reboot();

    /* Damage one byte of the stored session record: its CRC no longer matches. */
    fake_qspi_mem()[JOURNAL_BASE + 5U] &= 0xEFU; /* image_id byte 0x12 -> 0x02 */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NO_SESSION,
                          IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset));
}

void test_TC_FS_033_a_torn_journal_entry_is_skipped(void)
{
    uint32_t offset = 0U;
    const uint32_t torn_slot = JOURNAL_BASE + FIRMWARE_JOURNAL_ENTRIES_OFFSET + 8U; /* 2nd entry */

    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->write_chunk(fsu_fs, 0U, s_file, 5000U));

    /* The power failed halfway through appending the 8192 checkpoint: value in, inverse missing. */
    const uint32_t v = 8192U;

    (void) memcpy(&fake_qspi_mem()[torn_slot], &v, sizeof(v));

    reboot();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset));
    TEST_ASSERT_EQUAL_UINT32(4096U, offset);

    /* The torn slot is skipped, not reused: later checkpoints go after it. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK,
                          IMG->write_chunk(fsu_fs, offset, &s_file[offset], s_len - offset));
    reboot();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset));
    TEST_ASSERT_EQUAL_UINT32(s_len, offset);
}

void test_TC_FS_033b_an_implausible_checkpoint_is_ignored(void)
{
    uint32_t offset = 0U;
    uint32_t values[4] = {0U};

    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->write_chunk(fsu_fs, 0U, s_file, 5000U));

    /* A pair that checks out but is neither 4 KB aligned nor the file length. */
    const firmware_journal_entry_t odd = {.value = 4097U, .inverse = ~4097U};

    (void) memcpy(&fake_qspi_mem()[JOURNAL_BASE + FIRMWARE_JOURNAL_ENTRIES_OFFSET + 8U], &odd,
                  sizeof(odd));
    TEST_ASSERT_EQUAL_UINT32(2U, journal_values(values, 4U));

    reboot();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset));
    TEST_ASSERT_EQUAL_UINT32(4096U, offset);
}

void test_TC_FS_034_interrupt_resume_finish_yields_the_identical_file(void)
{
    static uint8_t staged[FSU_MAX_FILE];
    firmware_image_info_t info;
    uint32_t offset = 0U;

    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->write_chunk(fsu_fs, 0U, s_file, 6000U));

    reboot();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset));
    TEST_ASSERT_EQUAL_UINT32(4096U, offset);

    /* The session is continued from the checkpoint, re-sending bytes 4096..5999. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK,
                          IMG->write_chunk(fsu_fs, offset, &s_file[offset], s_len - offset));

    fsu_read_staged(staged, s_len);
    TEST_ASSERT_EQUAL_MEMORY(s_file, staged, s_len);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->verify(fsu_fs, &info));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_qspi_rule_violations());
}

void test_TC_FS_034b_a_resumed_sector_is_erased_again(void)
{
    uint32_t offset = 0U;
    const uint32_t sector = (IMAGE_BASE + 4096U) / QSPI_FLASH_SECTOR_SIZE_BYTES;

    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->write_chunk(fsu_fs, 0U, s_file, 6000U));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_qspi_sector_erases(sector)); /* half written */

    reboot();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK,
                          IMG->write_chunk(fsu_fs, offset, &s_file[offset], 100U));

    /* Entering the sector at its aligned start erased it again: NOR cannot overwrite. */
    TEST_ASSERT_EQUAL_UINT32(2U, fake_qspi_sector_erases(sector));
}

void test_TC_FS_035_resume_of_a_complete_file_is_received(void)
{
    firmware_image_info_t info;
    uint32_t offset = 0U;

    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_download(s_file, s_len, IMAGE_ID, 3000U));

    reboot();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset));
    TEST_ASSERT_EQUAL_UINT32(s_len, offset);

    /* RECEIVED, so verification may run straight away (and must be repeated after a reboot). */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->verify(fsu_fs, &info));
}

void test_TC_FS_036_discard_erases_the_journal_and_leaves_the_slots_alone(void)
{
    uint32_t offset = 0U;

    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_download(s_file, s_len, IMAGE_ID, 3000U));

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->discard(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_NO_SESSION,
                          IMG->resume(fsu_fs, IMAGE_ID, s_len, &offset));

    for (uint32_t i = 0U; i < FIRMWARE_JOURNAL_SIZE; i++)
    {
        TEST_ASSERT_EQUAL_HEX8(0xFFU, fake_qspi_mem()[JOURNAL_BASE + i]);
    }

    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_erase_calls());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_iflash_program_calls());

    /* Discard is harmless when there is nothing to discard. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->discard(fsu_fs));
}

void test_TC_FS_036b_discard_reports_an_erase_failure_but_still_forgets_the_session(void)
{
    uint8_t data[4] = {0U};

    arrange_file();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, s_len));

    fake_qspi_fail_erase_at(JOURNAL_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_FLASH_ERASE, IMG->discard(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_BAD_STATE, IMG->write_chunk(fsu_fs, 0U, data, 4U));
}

void test_TC_FS_037_a_maximum_size_file_uses_at_most_122_journal_entries(void)
{
    static uint8_t block[FIRMWARE_CHECKPOINT_STEP];
    const uint32_t file_len = FIRMWARE_HEADER_SIZE + FIRMWARE_MAX_IMAGE_LEN;
    uint32_t values[130] = {0U};

    arrange_capable();
    (void) memset(block, 0xA5, sizeof(block));

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->begin(fsu_fs, IMAGE_ID, file_len));

    for (uint32_t pos = 0U; pos < file_len; pos += sizeof(block))
    {
        const uint32_t n = ((file_len - pos) < sizeof(block)) ? (file_len - pos) : sizeof(block);

        TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->write_chunk(fsu_fs, pos, block, n));
    }

    const uint32_t entries = journal_values(values, 130U);

    TEST_ASSERT_TRUE(entries <= 122U);
    TEST_ASSERT_EQUAL_UINT32(121U, entries); /* 120 boundaries + the end of the file */
    TEST_ASSERT_EQUAL_UINT32(file_len, values[entries - 1U]);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_qspi_rule_violations());
}
