/**
 * @file fs_metadata.h
 * @brief FirmwareStore private: on-chip flash access, A/B boot metadata and slot descriptors.
 *
 * Every failing flash call is logged here, once, with the address involved, so
 * callers only map the returned code.
 *
 * @see docs/lld/middleware/firmware-store.md §6.2, §6.3, §13
 */

#ifndef FS_METADATA_H
#define FS_METADATA_H

#include <stdbool.h>
#include <stdint.h>

#include "firmware_image_format.h"
#include "firmware_store_types.h"
#include "internal_flash/iinternal_flash.h"

/** Marker for "no valid metadata copy". */
#define FS_META_NONE (0xFFU)

/** @brief The selected boot metadata record, held in RAM. */
typedef struct
{
    uint8_t selected;      /**< 0 = copy A, 1 = copy B, FS_META_NONE = no valid copy. */
    uint8_t active_bank;   /**< FIRMWARE_BANK_A or _B.                                */
    uint8_t boot_state;    /**< FIRMWARE_META_STATE_*.                                */
    uint8_t previous_bank; /**< FIRMWARE_BANK_A or _B.                                */
    uint32_t rollback_count;
    uint32_t generation;
} fs_meta_t;

/** @brief Base address of a slot. */
uint32_t fs_slot_base(firmware_slot_t slot);

/** @brief Address of a slot's descriptor page. */
uint32_t fs_desc_addr(firmware_slot_t slot);

/** @brief Slot for a bank value; FIRMWARE_SLOT_NONE for anything else. */
firmware_slot_t fs_slot_from_bank(uint8_t bank);

/** @brief Bank value for a slot (FIRMWARE_BANK_A or _B). */
uint8_t fs_bank_from_slot(firmware_slot_t slot);

/** @brief Read on-chip flash. @return FLASH_READ on failure (logged). */
firmware_store_err_t fs_if_read(const iinternal_flash_t *fl, uint32_t addr, uint8_t *buf,
                                uint32_t len);

/** @brief Erase one on-chip page. @return FLASH_ERASE on failure (logged). */
firmware_store_err_t fs_if_erase_page(const iinternal_flash_t *fl, uint32_t addr);

/** @brief Program on-chip flash. @return FLASH_WRITE on failure (logged). */
firmware_store_err_t fs_if_program(const iinternal_flash_t *fl, uint32_t addr, const uint8_t *data,
                                   uint32_t len);

/**
 * @brief Read both metadata copies and select the valid one with the higher generation.
 *
 * Read-only. If both are valid with equal generation, copy A wins. If neither is
 * valid, out->selected is FS_META_NONE.
 *
 * @return OK; FLASH_READ if a copy cannot be read.
 */
firmware_store_err_t fs_meta_load(const iinternal_flash_t *fl, fs_meta_t *out);

/**
 * @brief Write a new metadata record to the copy that is not selected.
 *
 * Erase, program (CRC double word last), read back and validate; only then is
 * @p meta updated. A failure leaves the selected copy, and @p meta, untouched.
 * The new generation is the selected one plus one (1 when none was valid).
 *
 * @return OK; FLASH_ERASE; FLASH_WRITE (also for a failed read-back).
 */
firmware_store_err_t fs_meta_write(const iinternal_flash_t *fl, fs_meta_t *meta,
                                   uint8_t active_bank, uint8_t boot_state, uint8_t previous_bank,
                                   uint32_t rollback_count);

/**
 * @brief Read a slot descriptor and check it.
 *
 * @param[out] valid  True if the magic, CRC, load address and length are legal.
 * @param[out] desc   The descriptor as read (may be NULL).
 * @return OK; FLASH_READ.
 */
firmware_store_err_t fs_desc_read(const iinternal_flash_t *fl, firmware_slot_t slot, bool *valid,
                                  firmware_slot_descriptor_t *desc);

/** @brief Erase a slot's descriptor page. */
firmware_store_err_t fs_desc_erase(const iinternal_flash_t *fl, firmware_slot_t slot);

/**
 * @brief Program a slot's descriptor (CRC double word last) and read it back.
 *
 * @return OK; FLASH_WRITE.
 */
firmware_store_err_t fs_desc_write(const iinternal_flash_t *fl, firmware_slot_t slot,
                                   uint32_t image_len, uint32_t version, uint32_t body_crc32);

#endif /* FS_METADATA_H */
