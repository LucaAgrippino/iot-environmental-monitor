/**
 * @file bootloader_sim.c
 * @brief Host-side simulator of the bootloader rows of firmware-store.md §13.2.
 */

#include "bootloader_sim.h"

#include <stdbool.h>
#include <string.h>

#include "config_store/config_store_crc.h"
#include "fake_internal_flash.h"
#include "firmware_image_format.h"

typedef struct
{
    bool valid;
    uint8_t copy;
    firmware_boot_meta_t rec;
} sim_meta_t;

static uint32_t s_writes;

static bool bank_ok(uint8_t b)
{
    return (b == FIRMWARE_BANK_A) || (b == FIRMWARE_BANK_B);
}

static sim_meta_t read_meta(void)
{
    sim_meta_t best = {.valid = false, .copy = 0U};
    static const uint32_t k_addr[2] = {FIRMWARE_META_A_ADDR, FIRMWARE_META_B_ADDR};

    for (uint8_t c = 0U; c < 2U; c++)
    {
        firmware_boot_meta_t r;

        (void) memcpy(&r, fake_iflash_ptr(k_addr[c]), sizeof(r));

        const bool ok =
            (r.magic == FIRMWARE_META_MAGIC) &&
            (r.crc32 == config_store_crc32((const uint8_t *) &r, FIRMWARE_META_CRC_SPAN)) &&
            bank_ok(r.active_bank) && bank_ok(r.previous_bank) && (r.boot_state >= 1U) &&
            (r.boot_state <= 4U);

        if (ok && (!best.valid || (r.generation > best.rec.generation)))
        {
            best.valid = true;
            best.copy = c;
            best.rec = r;
        }
    }

    return best;
}

/** @brief Write a record to the copy that is not selected (copy A if none), as the bootloader
 * would. */
static void write_meta(sim_meta_t *cur, uint8_t active, uint8_t state, uint8_t previous,
                       uint32_t count)
{
    const uint8_t target = (cur->valid && (cur->copy == 0U)) ? 1U : 0U;
    const uint32_t addr = (target == 0U) ? FIRMWARE_META_A_ADDR : FIRMWARE_META_B_ADDR;
    firmware_boot_meta_t r = {
        .magic = FIRMWARE_META_MAGIC,
        .active_bank = active,
        .boot_state = state,
        .previous_bank = previous,
        .reserved = 0xFFU,
        .rollback_count = count,
        .generation = cur->valid ? (cur->rec.generation + 1U) : 1U,
        .crc32 = 0U,
        .pad = 0xFFFFFFFFUL,
    };

    r.crc32 = config_store_crc32((const uint8_t *) &r, FIRMWARE_META_CRC_SPAN);
    (void) memset(fake_iflash_ptr(addr), 0xFF, INTERNAL_FLASH_PAGE_SIZE_BYTES);
    (void) memcpy(fake_iflash_ptr(addr), &r, sizeof(r));

    cur->valid = true;
    cur->copy = target;
    cur->rec = r;
    s_writes++;
}

static uint32_t base_of(uint8_t bank)
{
    return (bank == FIRMWARE_BANK_B) ? FIRMWARE_SLOT_B_BASE : FIRMWARE_SLOT_A_BASE;
}

static uint32_t desc_of(uint8_t bank)
{
    return (bank == FIRMWARE_BANK_B) ? FIRMWARE_DESC_B_ADDR : FIRMWARE_DESC_A_ADDR;
}

/** @brief A slot is bootable when its descriptor and its body CRC both check out. */
static bool slot_bootable(uint8_t bank)
{
    firmware_slot_descriptor_t d;

    (void) memcpy(&d, fake_iflash_ptr(desc_of(bank)), sizeof(d));

    if ((d.magic != FIRMWARE_DESC_MAGIC) ||
        (d.desc_crc32 != config_store_crc32((const uint8_t *) &d, FIRMWARE_DESC_CRC_SPAN)) ||
        (d.load_addr != base_of(bank)) || (d.image_len == 0U) || (d.image_len > FIRMWARE_SLOT_SIZE))
    {
        return false;
    }

    return d.body_crc32 == config_store_crc32(fake_iflash_ptr(base_of(bank)), d.image_len);
}

static uint8_t other_bank(uint8_t bank)
{
    return (bank == FIRMWARE_BANK_A) ? (uint8_t) FIRMWARE_BANK_B : (uint8_t) FIRMWARE_BANK_A;
}

firmware_slot_t bootloader_sim_boot(void)
{
    sim_meta_t m = read_meta();

    /* No valid metadata: first boot, Slot A, confirmed, generation 1. */
    if (!m.valid)
    {
        write_meta(&m, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B, 0U);
    }
    else if (m.rec.boot_state == FIRMWARE_META_STATE_TRIAL_PENDING)
    {
        write_meta(&m, m.rec.active_bank, FIRMWARE_META_STATE_TRIAL_RUNNING, m.rec.previous_bank,
                   m.rec.rollback_count);
    }
    else if (m.rec.boot_state == FIRMWARE_META_STATE_TRIAL_RUNNING)
    {
        /* The application never confirmed: crash, watchdog or power loss. Roll back. */
        write_meta(&m, m.rec.previous_bank, FIRMWARE_META_STATE_ROLLED_BACK, m.rec.active_bank,
                   m.rec.rollback_count + 1U);
    }
    else
    {
        /* CONFIRMED or ROLLED_BACK: boot the active slot as it stands. */
    }

    if (slot_bootable(m.rec.active_bank))
    {
        return (m.rec.active_bank == FIRMWARE_BANK_B) ? FIRMWARE_SLOT_B : FIRMWARE_SLOT_A;
    }

    /* Active image invalid: fall back to the other slot if it is valid, else halt safely. */
    const uint8_t other = other_bank(m.rec.active_bank);

    if (slot_bootable(other))
    {
        write_meta(&m, other, FIRMWARE_META_STATE_ROLLED_BACK, m.rec.active_bank,
                   m.rec.rollback_count + 1U);
        return (other == FIRMWARE_BANK_B) ? FIRMWARE_SLOT_B : FIRMWARE_SLOT_A;
    }

    return FIRMWARE_SLOT_NONE;
}

uint32_t bootloader_sim_meta_writes(void)
{
    return s_writes;
}
