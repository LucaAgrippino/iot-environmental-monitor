/**
 * @file iinternal_flash.h
 * @brief IInternalFlash — contract for the on-chip flash driver (Gateway).
 *
 * Interface header of the InternalFlashDriver (docs/lld/drivers/internal-flash-driver.md),
 * which implements it. FirmwareStore (firmware-store.md §4.2) is its consumer.
 *
 * The STM32L475VG on-chip flash is two 512 KB hardware banks (bank 2 starts at
 * 0x0808_0000). Read-while-write is possible only across banks, so the driver
 * selects the bank itself and the caller never sees it.
 *
 * @see docs/lld/middleware/firmware-store.md §4.2
 */

#ifndef IINTERNAL_FLASH_H
#define IINTERNAL_FLASH_H

#include <stdint.h>

/** Erase granularity of the on-chip flash (one page). */
#define INTERNAL_FLASH_PAGE_SIZE_BYTES (2048U)

/** Program granularity: one double word, written with ECC. */
#define INTERNAL_FLASH_DWORD_SIZE_BYTES (8U)

/** @brief Error codes returned by every InternalFlashDriver operation. */
typedef enum
{
    INTERNAL_FLASH_OK = 0,               /**< Operation succeeded.                          */
    INTERNAL_FLASH_ERR_NULL_POINTER = 1, /**< A required pointer argument was NULL.       */
    INTERNAL_FLASH_ERR_ADDR = 2,         /**< Address (or address + length) outside flash.  */
    INTERNAL_FLASH_ERR_ALIGN = 3,        /**< Address or length violates the alignment rule. */
    INTERNAL_FLASH_ERR_ERASE = 4,        /**< The page erase failed.                        */
    INTERNAL_FLASH_ERR_PROGRAM =
        5, /**< The programming failed (including a target that was not erased). */
    INTERNAL_FLASH_ERR_NOT_INITIALISED = 6, /**< internal_flash_init() has not succeeded yet. */
    INTERNAL_FLASH_ERR_BANK_MODE = 7        /**< The option bytes are not in dual-bank mode.  */
} internal_flash_err_t;

/**
 * @brief Vtable exposing the InternalFlashDriver operational surface.
 *
 * erase_page(addr): addr is 2 KB-aligned and inside on-chip flash; the driver
 *   selects the hardware bank.
 * program(addr, data, len): addr is 8-aligned, len a multiple of 8 and the
 *   target is erased. Double-word programming; leaves the instruction and data
 *   caches coherent for subsequent reads.
 * read(addr, buf, len): copies from mapped flash. Exists so that middleware
 *   never dereferences a flash address directly (and so it is host-testable).
 */
typedef struct
{
    internal_flash_err_t (*erase_page)(uint32_t addr);
    internal_flash_err_t (*program)(uint32_t addr, const uint8_t *data, uint32_t len);
    internal_flash_err_t (*read)(uint32_t addr, uint8_t *buf, uint32_t len);
} iinternal_flash_t;

#endif /* IINTERNAL_FLASH_H */
