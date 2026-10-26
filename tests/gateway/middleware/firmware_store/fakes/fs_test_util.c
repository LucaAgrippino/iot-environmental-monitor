/**
 * @file fs_test_util.c
 * @brief Shared helpers for the FirmwareStore unit tests.
 */

#include "fs_test_util.h"

#include <string.h>

#include "FreeRTOS.h"
#include "config_store/config_store_crc.h"
#include "fake_internal_flash.h"
#include "fake_qspi_flash.h"
#include "logger_stub.h"
#include "power_cut.h"
#include "unity.h"

#define LOG_LEVELS (4U)
#define LOG_KEEP (24U)
#define LOG_TEXT_MAX (160U)

firmware_store_t *fsu_fs;

static uint32_t s_level_counts[LOG_LEVELS];
static char s_messages[LOG_KEEP][LOG_TEXT_MAX];
static uint32_t s_message_count;

/* ---------------------------------------------------------------------- */
/* Logger spy                                                              */
/* ---------------------------------------------------------------------- */

void logger_log(log_level_t level, const char *module, const char *msg)
{
    (void) module;

    if ((uint32_t) level < LOG_LEVELS)
    {
        s_level_counts[level]++;
    }

    if (s_message_count < LOG_KEEP)
    {
        (void) strncpy(s_messages[s_message_count], msg, LOG_TEXT_MAX - 1U);
        s_messages[s_message_count][LOG_TEXT_MAX - 1U] = '\0';
        s_message_count++;
    }
}

uint32_t fsu_log_count(uint32_t level)
{
    return (level < LOG_LEVELS) ? s_level_counts[level] : 0U;
}

bool fsu_log_contains(const char *text)
{
    for (uint32_t i = 0U; i < s_message_count; i++)
    {
        if (strstr(s_messages[i], text) != NULL)
        {
            return true;
        }
    }

    return false;
}

/* ---------------------------------------------------------------------- */
/* World                                                                   */
/* ---------------------------------------------------------------------- */

void fsu_reset_world(void)
{
    mock_freertos_reset();
    power_cut_reset();
    fake_qspi_reset();
    fake_iflash_reset();
    firmware_store_reset_for_test();

    (void) memset(s_level_counts, 0, sizeof(s_level_counts));
    s_message_count = 0U;
    fsu_fs = NULL;
}

uint8_t fsu_pattern(uint32_t index, uint32_t seed)
{
    return (uint8_t) (((index * 131U) + seed) ^ (index >> 8));
}

void fsu_seed_meta(uint8_t copy, uint8_t active, uint8_t state, uint8_t previous,
                   uint32_t rollback_count, uint32_t generation)
{
    firmware_boot_meta_t rec = {
        .magic = FIRMWARE_META_MAGIC,
        .active_bank = active,
        .boot_state = state,
        .previous_bank = previous,
        .reserved = 0xFFU,
        .rollback_count = rollback_count,
        .generation = generation,
        .crc32 = 0U,
        .pad = 0xFFFFFFFFUL,
    };

    rec.crc32 = config_store_crc32((const uint8_t *) &rec, FIRMWARE_META_CRC_SPAN);
    (void) memcpy(fake_iflash_ptr((copy == 0U) ? FIRMWARE_META_A_ADDR : FIRMWARE_META_B_ADDR), &rec,
                  sizeof(rec));
}

void fsu_seed_slot(firmware_slot_t slot, uint32_t body_len, uint32_t version)
{
    const uint32_t base = (slot == FIRMWARE_SLOT_B) ? FIRMWARE_SLOT_B_BASE : FIRMWARE_SLOT_A_BASE;
    const uint32_t desc = (slot == FIRMWARE_SLOT_B) ? FIRMWARE_DESC_B_ADDR : FIRMWARE_DESC_A_ADDR;
    uint8_t *body = fake_iflash_ptr(base);

    for (uint32_t i = 0U; i < body_len; i++)
    {
        body[i] = fsu_pattern(i, (slot == FIRMWARE_SLOT_B) ? 0x5BU : 0xA1U);
    }

    firmware_slot_descriptor_t d = {
        .magic = FIRMWARE_DESC_MAGIC,
        .image_len = body_len,
        .version = version,
        .load_addr = base,
        .body_crc32 = config_store_crc32(body, body_len),
        .reserved = 0xFFFFFFFFUL,
        .desc_crc32 = 0U,
        .pad = 0xFFFFFFFFUL,
    };

    d.desc_crc32 = config_store_crc32((const uint8_t *) &d, FIRMWARE_DESC_CRC_SPAN);
    (void) memcpy(fake_iflash_ptr(desc), &d, sizeof(d));
}

void fsu_seed_running_a(void)
{
    fsu_seed_meta(0U, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 0U, 1U);
    fsu_seed_slot(FIRMWARE_SLOT_A, FSU_RUNNING_BODY_LEN, FSU_RUNNING_VERSION);
}

firmware_store_t *fsu_create(uint32_t running_image_base)
{
    const firmware_store_config_t cfg = {
        .qspi = &fake_qspi_flash_if,
        .iflash = &fake_iflash_if,
        .running_image_base = running_image_base,
    };

    firmware_store_reset_for_test();
    fsu_fs = NULL;
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, firmware_store_create(&cfg, &fsu_fs));
    TEST_ASSERT_NOT_NULL(fsu_fs);

    return fsu_fs;
}

firmware_store_err_t fsu_download(const uint8_t *file, uint32_t len, uint32_t image_id,
                                  uint32_t chunk)
{
    firmware_store_err_t err = firmware_store_image_ops.begin(fsu_fs, image_id, len);

    for (uint32_t pos = 0U; (err == FIRMWARE_STORE_ERR_OK) && (pos < len); pos += chunk)
    {
        const uint32_t n = ((len - pos) < chunk) ? (len - pos) : chunk;

        err = firmware_store_image_ops.write_chunk(fsu_fs, pos, &file[pos], n);
    }

    return err;
}

firmware_store_err_t fsu_install_all(uint32_t *steps_out)
{
    bool done = false;
    uint32_t steps = 0U;
    firmware_store_err_t err = firmware_store_image_ops.install_begin(fsu_fs);

    while ((err == FIRMWARE_STORE_ERR_OK) && !done)
    {
        err = firmware_store_image_ops.install_step(fsu_fs, &done);
        steps++;
    }

    if (steps_out != NULL)
    {
        *steps_out = steps;
    }

    return err;
}

void fsu_read_staged(uint8_t *out, uint32_t len)
{
    (void) memcpy(
        out, &fake_qspi_mem()[FIRMWARE_STORE_STAGING_BASE + FIRMWARE_STAGING_IMAGE_OFFSET], len);
}
