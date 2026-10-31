/**
 * @file internal_flash.h
 * @brief InternalFlashDriver — erase, program and read the on-chip flash (Gateway).
 *
 * STM32L475VG: 1 MB in two 512 KB banks, 2 KB pages, 64-bit programming with ECC. A
 * singleton with module-prefixed free functions; dependency inversion goes through the
 * iinternal_flash_t vtable (internal_flash_driver). The flash controller is locked again
 * after every call.
 *
 * A program or erase stalls instruction and data fetch from the SAME bank until it
 * finishes (a page erase takes ~22 ms); the other bank keeps running. Callers that need
 * the CPU to keep going (FirmwareStore) put the target in the other bank.
 *
 * Not reentrant and not ISR-safe: one caller at a time (FirmwareStore holds a mutex).
 *
 * @see docs/lld/drivers/internal-flash-driver.md
 */

#ifndef INTERNAL_FLASH_H
#define INTERNAL_FLASH_H

#include <stdint.h>

#include "iinternal_flash.h"

/** First address and size of the main flash. */
#define INTERNAL_FLASH_BASE_ADDR (0x08000000UL)
#define INTERNAL_FLASH_SIZE_BYTES (0x100000UL)

/**
 * First address erase and program accept: the bootloader (pages 0 to 7, 16 KB) is below it and
 * can only be changed with a debug probe (WP11 D-3, BL-O4). Reads are not restricted.
 */
#define INTERNAL_FLASH_WRITABLE_FROM_ADDR (0x08004000UL)

/** First address of bank 2. */
#define INTERNAL_FLASH_BANK2_ADDR (0x08080000UL)

/**
 * @brief Check the option bytes and make the driver usable.
 *
 * Leaves the flash controller locked. Safe to call again.
 *
 * @return INTERNAL_FLASH_OK; INTERNAL_FLASH_ERR_BANK_MODE if the device is not in
 *         dual-bank mode (the driver then stays uninitialised).
 * @note Threading: call once at start-up, before the scheduler.
 */
internal_flash_err_t internal_flash_init(void);

/**
 * @brief Erase one 2 KB page to 0xFF.
 *
 * @param[in] addr  2 KB-aligned address of the page, inside the main flash. The bank is
 *                  selected from the address.
 * @return OK; NOT_INITIALISED; ALIGN; ADDR; ERASE (controller error flag, a controller
 *         that stays busy, or one that will not unlock).
 * @note Threading: blocks for the erase (~22 ms); the CPU stalls on fetches from the same bank.
 */
internal_flash_err_t internal_flash_erase_page(uint32_t addr);

/**
 * @brief Program erased flash.
 *
 * @param[in] addr  8-byte-aligned start address.
 * @param[in] data  Bytes to program; need not be aligned.
 * @param[in] len   A non-zero multiple of 8.
 * @return OK; NOT_INITIALISED; NULL_POINTER; ALIGN; ADDR; PROGRAM (including a target that
 *         is not erased). Stops at the first failing double word; earlier ones are written.
 * @note Threading: ~85 µs per double word; interrupts are masked only for the two stores of
 *       one double word.
 */
internal_flash_err_t internal_flash_program(uint32_t addr, const uint8_t *data, uint32_t len);

/**
 * @brief Copy from mapped flash.
 *
 * @param[in]  addr  Start address.
 * @param[out] buf   Destination.
 * @param[in]  len   At least 1.
 * @return OK; NOT_INITIALISED; NULL_POINTER; ADDR (outside the main flash, or empty).
 */
internal_flash_err_t internal_flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

/** Singleton pointer to the InternalFlashDriver vtable instance. */
extern const iinternal_flash_t *const internal_flash_driver;

#ifdef TEST
/** @brief Test hook — return the driver to its un-initialised, post-bss state. */
void internal_flash_reset_for_test(void);
#endif

#endif /* INTERNAL_FLASH_H */
