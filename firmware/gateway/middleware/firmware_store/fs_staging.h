/**
 * @file fs_staging.h
 * @brief FirmwareStore private: QSPI staging area, resume journal and lazy sector erase.
 *
 * Every failing flash call is logged here, once, with the offset involved.
 *
 * @see docs/lld/middleware/firmware-store.md §6.4, §10
 */

#ifndef FS_STAGING_H
#define FS_STAGING_H

#include <stdbool.h>
#include <stdint.h>

#include "firmware_store_types.h"
#include "qspi_flash/qspi_flash.h"

/** Scratch bytes the journal routines need from the caller. */
#define FS_STAGING_SCRATCH_LEN (256U)

/** @brief Read from the staged image file. @param pos Byte position within the file. */
firmware_store_err_t fs_stage_read(const iqspi_flash_t *q, uint32_t pos, uint8_t *buf,
                                   uint32_t len);

/**
 * @brief Stage the next bytes of the file.
 *
 * Splits at 256 B page boundaries. A 4 KB sector is erased the first time the
 * write position enters it (lazy erase). A checkpoint is appended to the journal
 * each time the position crosses a 4 KB boundary and once when it reaches
 * @p file_len.
 *
 * @param[in]     pos       File position of data[0].
 * @param[in]     file_len  Total length of the file being staged.
 * @param[in,out] next      Byte offset of the next free journal entry.
 * @return OK; FLASH_ERASE; FLASH_WRITE.
 */
firmware_store_err_t fs_stage_write(const iqspi_flash_t *q, uint32_t pos, const uint8_t *data,
                                    uint32_t len, uint32_t file_len, uint32_t *next);

/** @brief Erase the journal sector and write a fresh session record. @param[out] next First free
 * entry. */
firmware_store_err_t fs_journal_begin(const iqspi_flash_t *q, uint32_t image_id, uint32_t file_len,
                                      uint8_t *scratch, uint32_t *next);

/**
 * @brief Read the journal back for a session.
 *
 * @param[out] session_ok   False when the session record is absent, torn or for another file.
 * @param[out] checkpoint   Bytes of the file safely staged (0 if no valid entry).
 * @param[out] next         Byte offset of the first erased entry.
 * @return OK; FLASH_READ.
 */
firmware_store_err_t fs_journal_scan(const iqspi_flash_t *q, uint32_t image_id, uint32_t file_len,
                                     uint8_t *scratch, bool *session_ok, uint32_t *checkpoint,
                                     uint32_t *next);

/** @brief Erase the journal sector (image sectors are left; lazy erase handles their reuse). */
firmware_store_err_t fs_journal_erase(const iqspi_flash_t *q);

#endif /* FS_STAGING_H */
