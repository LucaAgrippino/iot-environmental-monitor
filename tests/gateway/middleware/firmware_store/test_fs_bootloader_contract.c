/**
 * @file test_fs_bootloader_contract.c
 * @brief The contract between FirmwareStore and the real bootloader logic (host, Unity).
 *
 * Test IDs follow docs/lld/drivers/bootloader.md §7 (TC-BL-028..030). The bootloader side is
 * boot_core.c, unmodified; the application side is the real FirmwareStore (install, activate,
 * confirm, rollback, acknowledge) over the same fake flash. Neither side is seeded from the
 * other's code: a record or descriptor written by one has to be accepted by the other.
 *
 * TC-FS-090..092 run the same flows against the older simulator (sim/bootloader_sim.c);
 * TC-BL-030 pins the two together.
 *
 * Build defines: STM32L475xx, BOARD_GATEWAY, TEST (:test_fs_bootloader_contract:).
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "freertos_mock.h" /* causes Ceedling to auto-link freertos_mock.c */

#include "boot_core.h"
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
static boot_report_t s_rep;

/* ---------------------------------------------------------------------- */
/* The two sides                                                           */
/* ---------------------------------------------------------------------- */

/** @brief One reset of the real bootloader logic. @return the slot it jumps to. */
static firmware_slot_t real_boot(void)
{
    if (boot_core_run(&fake_iflash_if, &s_rep) != BOOT_OUTCOME_RUN)
    {
        return FIRMWARE_SLOT_NONE;
    }

    return (s_rep.slot_base == FIRMWARE_SLOT_B_BASE) ? FIRMWARE_SLOT_B : FIRMWARE_SLOT_A;
}

static firmware_boot_info_t boot_info(void)
{
    firmware_boot_info_t info;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->get_boot_info(fsu_fs, &info));

    return info;
}

/** @brief Give a seeded slot the vector pair a linked image has, and a descriptor to match. */
static void patch_vectors(firmware_slot_t slot)
{
    const uint32_t base = (slot == FIRMWARE_SLOT_B) ? FIRMWARE_SLOT_B_BASE : FIRMWARE_SLOT_A_BASE;
    const uint32_t desc = (slot == FIRMWARE_SLOT_B) ? FIRMWARE_DESC_B_ADDR : FIRMWARE_DESC_A_ADDR;
    const uint32_t vectors[2] = {0x20018000UL, base + 0x101UL};
    firmware_slot_descriptor_t d;

    (void) memcpy(&d, fake_iflash_ptr(desc), sizeof(d));
    (void) memcpy(fake_iflash_ptr(base), vectors, sizeof(vectors));

    d.body_crc32 = config_store_crc32(fake_iflash_ptr(base), d.image_len);
    d.desc_crc32 = config_store_crc32((const uint8_t *) &d, FIRMWARE_DESC_CRC_SPAN);
    (void) memcpy(fake_iflash_ptr(desc), &d, sizeof(d));
}

/** @brief Running Slot A, confirmed, with a Slot B image staged, verified and installed. */
static void arrange_installed(void)
{
    firmware_image_info_t info;

    fsu_seed_running_a();
    patch_vectors(FIRMWARE_SLOT_A);
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);

    const uint32_t len = fsu_build_image(s_file, BODY_LEN, FIRMWARE_SLOT_B_BASE, VERSION_NEW);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_download(s_file, len, IMAGE_ID, CHUNK));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->verify(fsu_fs, &info));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fsu_install_all(NULL));
}

static bool desc_valid(firmware_slot_t slot)
{
    bool valid = false;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, fs_desc_read(&fake_iflash_if, slot, &valid, NULL));

    return valid;
}

void setUp(void)
{
    fsu_reset_world();
    fake_crypto_reset();
}

void tearDown(void)
{
}

/* ---------------------------------------------------------------------- */
/* TC-BL-028: both directions of the on-flash contract                     */
/* ---------------------------------------------------------------------- */

void test_BL028a_an_installed_update_is_trialled_by_the_bootloader_and_confirmed(void)
{
    arrange_installed();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->activate(fsu_fs));

    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, real_boot());
    TEST_ASSERT_EQUAL_UINT32(VERSION_NEW, s_rep.version);
    TEST_ASSERT_EQUAL_UINT32(BODY_LEN, s_rep.image_len);

    (void) fsu_create(FIRMWARE_SLOT_B_BASE);
    TEST_ASSERT_TRUE(boot_info().ota_capable);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_TRIAL_RUNNING, boot_info().state);
    TEST_ASSERT_EQUAL_HEX32(VERSION_NEW, boot_info().running_version);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->confirm(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_CONFIRMED, boot_info().state);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, real_boot());
    TEST_ASSERT_EQUAL_UINT32(0U, s_rep.events);
    TEST_ASSERT_FALSE(desc_valid(FIRMWARE_SLOT_A)); /* the old image is gone after confirm */
}

void test_BL028b_a_failed_self_check_rolls_back_and_is_acknowledged(void)
{
    arrange_installed();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->activate(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, real_boot());

    (void) fsu_create(FIRMWARE_SLOT_B_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->rollback(fsu_fs));

    /* The application's own rollback is already ROLLED_BACK: the bootloader just boots it. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, real_boot());
    TEST_ASSERT_EQUAL_UINT32(0U, s_rep.events);

    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_ROLLED_BACK, boot_info().state);
    TEST_ASSERT_EQUAL_UINT32(1U, boot_info().rollback_count);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->acknowledge_rollback(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_CONFIRMED, boot_info().state);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, real_boot());
    TEST_ASSERT_FALSE(desc_valid(FIRMWARE_SLOT_B)); /* the failed image can never be booted */
}

void test_BL028c_a_crash_during_the_trial_rolls_back_at_the_second_boot(void)
{
    arrange_installed();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->activate(fsu_fs));

    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, real_boot());
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, real_boot());
    TEST_ASSERT_TRUE((s_rep.events & BOOT_EVT_TRIAL_ABANDONED) != 0U);

    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_ROLLED_BACK, boot_info().state);
    TEST_ASSERT_EQUAL_UINT32(1U, boot_info().rollback_count);
    TEST_ASSERT_EQUAL_HEX32(FSU_RUNNING_VERSION, boot_info().running_version);

    /* The application can still acknowledge it. */
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->acknowledge_rollback(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_CONFIRMED, boot_info().state);
}

void test_BL028d_a_bit_rotted_new_image_is_never_run(void)
{
    arrange_installed();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->activate(fsu_fs));
    fake_iflash_ptr(FIRMWARE_SLOT_B_BASE)[1500] &= 0xFEU;

    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, real_boot());
    TEST_ASSERT_TRUE((s_rep.events & BOOT_EVT_ACTIVE_INVALID) != 0U);

    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_ROLLED_BACK, boot_info().state);
}

void test_BL028e_a_device_the_bootloader_initialised_is_ota_capable(void)
{
    /* Blank metadata, Slot A holds the factory image: the bootloader writes the first record. */
    fsu_seed_slot(FIRMWARE_SLOT_A, FSU_RUNNING_BODY_LEN, FSU_RUNNING_VERSION);
    patch_vectors(FIRMWARE_SLOT_A);

    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, real_boot());
    TEST_ASSERT_TRUE((s_rep.events & BOOT_EVT_RECORD_INITIALISED) != 0U);

    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    TEST_ASSERT_TRUE(boot_info().ota_capable);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_BOOT_CONFIRMED, boot_info().state);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, boot_info().running_slot);
}

/* ---------------------------------------------------------------------- */
/* TC-BL-029: the two rollback routes converge on the same record          */
/* ---------------------------------------------------------------------- */

void test_BL029_the_application_and_bootloader_rollbacks_write_the_same_record(void)
{
    firmware_boot_meta_t app_side;
    firmware_boot_meta_t boot_side;

    /* Route 1: the application rolls back, then the bootloader boots the result. */
    arrange_installed();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->activate(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, real_boot());
    (void) fsu_create(FIRMWARE_SLOT_B_BASE);
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, BOOT->rollback(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, real_boot());
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    app_side.active_bank =
        (uint8_t) ((boot_info().running_slot == FIRMWARE_SLOT_A) ? FIRMWARE_BANK_A
                                                                 : FIRMWARE_BANK_B);
    app_side.boot_state = FIRMWARE_META_STATE_ROLLED_BACK;
    app_side.rollback_count = boot_info().rollback_count;

    /* Route 2: the application crashes; the bootloader rolls back at the next reset. */
    setUp();
    arrange_installed();
    TEST_ASSERT_EQUAL_INT(FIRMWARE_STORE_ERR_OK, IMG->activate(fsu_fs));
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_B, real_boot());
    TEST_ASSERT_EQUAL_INT(FIRMWARE_SLOT_A, real_boot());
    (void) fsu_create(FIRMWARE_SLOT_A_BASE);
    boot_side.active_bank =
        (uint8_t) ((boot_info().running_slot == FIRMWARE_SLOT_A) ? FIRMWARE_BANK_A
                                                                 : FIRMWARE_BANK_B);
    boot_side.boot_state = (uint8_t) ((boot_info().state == FIRMWARE_BOOT_ROLLED_BACK)
                                          ? FIRMWARE_META_STATE_ROLLED_BACK
                                          : 0U);
    boot_side.rollback_count = boot_info().rollback_count;

    TEST_ASSERT_EQUAL_UINT8(app_side.active_bank, boot_side.active_bank);
    TEST_ASSERT_EQUAL_UINT8(app_side.boot_state, boot_side.boot_state);
    TEST_ASSERT_EQUAL_UINT32(app_side.rollback_count, boot_side.rollback_count);
}

/* ---------------------------------------------------------------------- */
/* TC-BL-030: the bootloader and the simulator agree on every combination  */
/* ---------------------------------------------------------------------- */

/** @brief Seed one combination: record state, active bank, and the health of each slot. */
static void seed_combo(uint8_t state, uint8_t active, bool a_ok, bool b_ok)
{
    const uint8_t previous = (active == FIRMWARE_BANK_A) ? FIRMWARE_BANK_B : FIRMWARE_BANK_A;

    fsu_reset_world();
    fsu_seed_meta(0U, active, state, previous, 0U, 3U);
    fsu_seed_slot(FIRMWARE_SLOT_A, FSU_RUNNING_BODY_LEN, 0x01000000UL);
    fsu_seed_slot(FIRMWARE_SLOT_B, FSU_RUNNING_BODY_LEN, 0x02000000UL);
    patch_vectors(FIRMWARE_SLOT_A);
    patch_vectors(FIRMWARE_SLOT_B);

    if (!a_ok)
    {
        fake_iflash_ptr(FIRMWARE_SLOT_A_BASE)[100] ^= 0x01U;
    }

    if (!b_ok)
    {
        fake_iflash_ptr(FIRMWARE_SLOT_B_BASE)[100] ^= 0x01U;
    }
}

void test_BL030_the_bootloader_and_the_simulator_choose_the_same_slot(void)
{
    const uint8_t states[4] = {FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_META_STATE_TRIAL_PENDING,
                               FIRMWARE_META_STATE_TRIAL_RUNNING, FIRMWARE_META_STATE_ROLLED_BACK};
    const uint8_t banks[2] = {FIRMWARE_BANK_A, FIRMWARE_BANK_B};
    uint32_t combos = 0U;

    for (uint32_t s = 0U; s < 4U; s++)
    {
        for (uint32_t b = 0U; b < 2U; b++)
        {
            for (uint32_t health = 0U; health < 4U; health++)
            {
                const bool a_ok = (health & 1U) != 0U;
                const bool b_ok = (health & 2U) != 0U;

                seed_combo(states[s], banks[b], a_ok, b_ok);
                const firmware_slot_t from_sim = bootloader_sim_boot();

                seed_combo(states[s], banks[b], a_ok, b_ok);
                const firmware_slot_t from_real = real_boot();

                char msg[64];
                (void) snprintf(msg, sizeof(msg), "state %u active %02X A %d B %d",
                                (unsigned) states[s], (unsigned) banks[b], (int) a_ok, (int) b_ok);
                TEST_ASSERT_EQUAL_INT_MESSAGE(from_sim, from_real, msg);
                combos++;
            }
        }
    }

    TEST_ASSERT_EQUAL_UINT32(32U, combos);
}
