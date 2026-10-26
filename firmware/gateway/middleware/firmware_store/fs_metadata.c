/**
 * @file fs_metadata.c
 * @brief FirmwareStore private: on-chip flash access, A/B boot metadata and slot descriptors.
 *
 * @see docs/lld/middleware/firmware-store.md §6.2, §6.3, §13
 */

#include "fs_metadata.h"

#include <string.h>

#include "config_store/config_store_crc.h"
#include "logger/logger.h"

#define FS_MODULE_TAG "FS"

/** Bytes of a metadata record, and of its CRC double word. */
#define FS_META_RECORD_LEN (24U)
#define FS_META_BODY_LEN (16U)
#define FS_DWORD_LEN (8U)

/** Bytes of a slot descriptor, and of its body before the CRC double word. */
#define FS_DESC_RECORD_LEN (32U)
#define FS_DESC_BODY_LEN (24U)

#define FS_META_RESERVED (0xFFU)

uint32_t fs_slot_base(firmware_slot_t slot)
{
    return (slot == FIRMWARE_SLOT_B) ? FIRMWARE_SLOT_B_BASE : FIRMWARE_SLOT_A_BASE;
}

uint32_t fs_desc_addr(firmware_slot_t slot)
{
    return (slot == FIRMWARE_SLOT_B) ? FIRMWARE_DESC_B_ADDR : FIRMWARE_DESC_A_ADDR;
}

firmware_slot_t fs_slot_from_bank(uint8_t bank)
{
    if (bank == FIRMWARE_BANK_A)
    {
        return FIRMWARE_SLOT_A;
    }

    return (bank == FIRMWARE_BANK_B) ? FIRMWARE_SLOT_B : FIRMWARE_SLOT_NONE;
}

uint8_t fs_bank_from_slot(firmware_slot_t slot)
{
    return (slot == FIRMWARE_SLOT_B) ? (uint8_t) FIRMWARE_BANK_B : (uint8_t) FIRMWARE_BANK_A;
}

/* ====================================================================== */
/* On-chip flash access                                                   */
/* ====================================================================== */

firmware_store_err_t fs_if_read(const iinternal_flash_t *fl, uint32_t addr, uint8_t *buf,
                                uint32_t len)
{
    if (fl->read(addr, buf, len) != INTERNAL_FLASH_OK)
    {
        LOG_ERROR(FS_MODULE_TAG, "on-chip read failed addr=0x%08lX", (unsigned long) addr);
        return FIRMWARE_STORE_ERR_FLASH_READ;
    }

    return FIRMWARE_STORE_ERR_OK;
}

firmware_store_err_t fs_if_erase_page(const iinternal_flash_t *fl, uint32_t addr)
{
    if (fl->erase_page(addr) != INTERNAL_FLASH_OK)
    {
        LOG_ERROR(FS_MODULE_TAG, "on-chip erase failed addr=0x%08lX", (unsigned long) addr);
        return FIRMWARE_STORE_ERR_FLASH_ERASE;
    }

    return FIRMWARE_STORE_ERR_OK;
}

firmware_store_err_t fs_if_program(const iinternal_flash_t *fl, uint32_t addr, const uint8_t *data,
                                   uint32_t len)
{
    if (fl->program(addr, data, len) != INTERNAL_FLASH_OK)
    {
        LOG_ERROR(FS_MODULE_TAG, "on-chip program failed addr=0x%08lX", (unsigned long) addr);
        return FIRMWARE_STORE_ERR_FLASH_WRITE;
    }

    return FIRMWARE_STORE_ERR_OK;
}

/* ====================================================================== */
/* Boot metadata                                                          */
/* ====================================================================== */

static bool meta_bank_legal(uint8_t bank)
{
    return (bank == FIRMWARE_BANK_A) || (bank == FIRMWARE_BANK_B);
}

static bool meta_state_legal(uint8_t state)
{
    return (state >= FIRMWARE_META_STATE_CONFIRMED) && (state <= FIRMWARE_META_STATE_ROLLED_BACK);
}

/** @brief Decode a record; true if the magic, CRC, banks and state are all legal. */
static bool meta_parse(const uint8_t raw[FS_META_RECORD_LEN], firmware_boot_meta_t *rec)
{
    (void) memcpy(rec, raw, sizeof(*rec));

    return (rec->magic == FIRMWARE_META_MAGIC) &&
           (rec->crc32 == config_store_crc32(raw, FIRMWARE_META_CRC_SPAN)) &&
           meta_bank_legal(rec->active_bank) && meta_bank_legal(rec->previous_bank) &&
           meta_state_legal(rec->boot_state);
}

firmware_store_err_t fs_meta_load(const iinternal_flash_t *fl, fs_meta_t *out)
{
    static const uint32_t k_copy_addr[2] = {FIRMWARE_META_A_ADDR, FIRMWARE_META_B_ADDR};
    firmware_boot_meta_t rec[2];
    bool valid[2] = {false, false};

    for (uint8_t i = 0U; i < 2U; i++)
    {
        uint8_t raw[FS_META_RECORD_LEN];
        firmware_store_err_t err = fs_if_read(fl, k_copy_addr[i], raw, sizeof(raw));

        if (err != FIRMWARE_STORE_ERR_OK)
        {
            return err;
        }

        valid[i] = meta_parse(raw, &rec[i]);
    }

    (void) memset(out, 0, sizeof(*out));
    out->selected = FS_META_NONE;

    if (valid[0] && (!valid[1] || (rec[0].generation >= rec[1].generation)))
    {
        out->selected = 0U;
    }
    else if (valid[1])
    {
        out->selected = 1U;
    }
    else
    {
        return FIRMWARE_STORE_ERR_OK;
    }

    out->active_bank = rec[out->selected].active_bank;
    out->boot_state = rec[out->selected].boot_state;
    out->previous_bank = rec[out->selected].previous_bank;
    out->rollback_count = rec[out->selected].rollback_count;
    out->generation = rec[out->selected].generation;

    return FIRMWARE_STORE_ERR_OK;
}

firmware_store_err_t fs_meta_write(const iinternal_flash_t *fl, fs_meta_t *meta,
                                   uint8_t active_bank, uint8_t boot_state, uint8_t previous_bank,
                                   uint32_t rollback_count)
{
    /* The copy that is not selected; copy A when neither is valid. */
    const uint8_t target = (meta->selected == 0U) ? 1U : 0U;
    const uint32_t addr = (target == 0U) ? FIRMWARE_META_A_ADDR : FIRMWARE_META_B_ADDR;
    const uint32_t generation = (meta->selected == FS_META_NONE) ? 1U : (meta->generation + 1U);

    firmware_boot_meta_t rec = {
        .magic = FIRMWARE_META_MAGIC,
        .active_bank = active_bank,
        .boot_state = boot_state,
        .previous_bank = previous_bank,
        .reserved = FS_META_RESERVED,
        .rollback_count = rollback_count,
        .generation = generation,
        .crc32 = 0U,
        .pad = FIRMWARE_ERASED_WORD,
    };
    uint8_t raw[FS_META_RECORD_LEN];
    uint8_t back[FS_META_RECORD_LEN];
    firmware_boot_meta_t check;

    (void) memcpy(raw, &rec, sizeof(rec));
    rec.crc32 = config_store_crc32(raw, FIRMWARE_META_CRC_SPAN);
    (void) memcpy(raw, &rec, sizeof(rec));

    firmware_store_err_t err = fs_if_erase_page(fl, addr);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        /* Two double words first, the double word holding the CRC last. */
        err = fs_if_program(fl, addr, raw, FS_META_BODY_LEN);
    }

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        err = fs_if_program(fl, addr + FS_META_BODY_LEN, &raw[FS_META_BODY_LEN], FS_DWORD_LEN);
    }

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        err = fs_if_read(fl, addr, back, sizeof(back));
    }

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        if ((memcmp(back, raw, sizeof(raw)) != 0) || !meta_parse(back, &check))
        {
            LOG_ERROR(FS_MODULE_TAG, "metadata read-back mismatch addr=0x%08lX",
                      (unsigned long) addr);
            err = FIRMWARE_STORE_ERR_FLASH_WRITE;
        }
    }

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        meta->selected = target;
        meta->active_bank = active_bank;
        meta->boot_state = boot_state;
        meta->previous_bank = previous_bank;
        meta->rollback_count = rollback_count;
        meta->generation = generation;
    }

    return err;
}

/* ====================================================================== */
/* Slot descriptors                                                       */
/* ====================================================================== */

firmware_store_err_t fs_desc_read(const iinternal_flash_t *fl, firmware_slot_t slot, bool *valid,
                                  firmware_slot_descriptor_t *desc)
{
    uint8_t raw[FS_DESC_RECORD_LEN];
    firmware_slot_descriptor_t d;
    firmware_store_err_t err = fs_if_read(fl, fs_desc_addr(slot), raw, sizeof(raw));

    *valid = false;

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        return err;
    }

    (void) memcpy(&d, raw, sizeof(d));

    *valid = (d.magic == FIRMWARE_DESC_MAGIC) &&
             (d.desc_crc32 == config_store_crc32(raw, FIRMWARE_DESC_CRC_SPAN)) &&
             (d.load_addr == fs_slot_base(slot)) && (d.image_len >= FS_DWORD_LEN) &&
             (d.image_len <= FIRMWARE_MAX_IMAGE_LEN);

    if (desc != NULL)
    {
        *desc = d;
    }

    return FIRMWARE_STORE_ERR_OK;
}

firmware_store_err_t fs_desc_erase(const iinternal_flash_t *fl, firmware_slot_t slot)
{
    return fs_if_erase_page(fl, fs_desc_addr(slot));
}

firmware_store_err_t fs_desc_write(const iinternal_flash_t *fl, firmware_slot_t slot,
                                   uint32_t image_len, uint32_t version, uint32_t body_crc32)
{
    const uint32_t addr = fs_desc_addr(slot);
    firmware_slot_descriptor_t d = {
        .magic = FIRMWARE_DESC_MAGIC,
        .image_len = image_len,
        .version = version,
        .load_addr = fs_slot_base(slot),
        .body_crc32 = body_crc32,
        .reserved = FIRMWARE_ERASED_WORD,
        .desc_crc32 = 0U,
        .pad = FIRMWARE_ERASED_WORD,
    };
    uint8_t raw[FS_DESC_RECORD_LEN];
    uint8_t back[FS_DESC_RECORD_LEN];

    (void) memcpy(raw, &d, sizeof(d));
    d.desc_crc32 = config_store_crc32(raw, FIRMWARE_DESC_CRC_SPAN);
    (void) memcpy(raw, &d, sizeof(d));

    /* Three double words first, the double word holding the CRC last. */
    firmware_store_err_t err = fs_if_program(fl, addr, raw, FS_DESC_BODY_LEN);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        err = fs_if_program(fl, addr + FS_DESC_BODY_LEN, &raw[FS_DESC_BODY_LEN], FS_DWORD_LEN);
    }

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        err = fs_if_read(fl, addr, back, sizeof(back));
    }

    if ((err == FIRMWARE_STORE_ERR_OK) && (memcmp(back, raw, sizeof(raw)) != 0))
    {
        LOG_ERROR(FS_MODULE_TAG, "descriptor read-back mismatch addr=0x%08lX",
                  (unsigned long) addr);
        err = FIRMWARE_STORE_ERR_FLASH_WRITE;
    }

    return err;
}
