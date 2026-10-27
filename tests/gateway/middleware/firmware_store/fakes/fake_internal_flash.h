/**
 * @file fake_internal_flash.h
 * @brief RAM on-chip flash with ECC semantics (FirmwareStore tests).
 *
 * 1 MB at 0x0800_0000. Erase sets a 2 KB page to 0xFF. A program must be
 * 8-aligned, a multiple of 8 bytes, and every target double word must still be
 * erased (ECC: a second program of the same double word fails). Write guards mark
 * address ranges that must never be erased or programmed (the running slot, the
 * bootloader, the metadata); touching one is counted. Every mutating call is
 * logged for ordering checks, and power-cut injection comes from power_cut.h.
 *
 * Including this header makes Ceedling auto-link fake_internal_flash.c.
 */

#ifndef FAKE_INTERNAL_FLASH_H
#define FAKE_INTERNAL_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "internal_flash/iinternal_flash.h"

/** First address and size of the simulated on-chip flash. */
#define FAKE_IFLASH_BASE (0x08000000UL)
#define FAKE_IFLASH_SIZE (0x100000UL)

/** Marker for "no fault armed". */
#define FAKE_IFLASH_NO_ADDR (0xFFFFFFFFUL)

/** Capacity of the operation log. */
#define FAKE_IFLASH_LOG_MAX (4096U)

/** @brief Kind of a logged operation. */
typedef enum
{
    FAKE_IFLASH_OP_ERASE = 0,
    FAKE_IFLASH_OP_PROGRAM = 1
} fake_iflash_op_t;

/** @brief One logged mutating operation. */
typedef struct
{
    fake_iflash_op_t op;
    uint32_t addr;
    uint32_t len;
} fake_iflash_log_entry_t;

extern const iinternal_flash_t fake_iflash_if;

/** @brief Erase everything to 0xFF; clear counters, guards, faults and the log. */
void fake_iflash_reset(void);

/** @brief Direct access to the byte at an absolute address (seeding and inspection). */
uint8_t *fake_iflash_ptr(uint32_t addr);

uint32_t fake_iflash_erase_calls(void);
uint32_t fake_iflash_program_calls(void);
uint32_t fake_iflash_read_calls(void);

/** @brief Calls the real driver would have rejected (alignment, range, program-over-data). */
uint32_t fake_iflash_rule_violations(void);

/** @brief Forbid erasing or programming [lo, hi). */
void fake_iflash_guard_add(uint32_t lo, uint32_t hi);

/** @brief Number of erases or programs that touched a guarded range. */
uint32_t fake_iflash_guard_violations(void);

uint32_t fake_iflash_log_count(void);
const fake_iflash_log_entry_t *fake_iflash_log_at(uint32_t index);

/** @brief The next erase of the page holding this address fails. */
void fake_iflash_fail_erase_at(uint32_t addr);

/** @brief A program starting at this address fails (nothing is written). */
void fake_iflash_fail_program_at(uint32_t addr);

/** @brief The next program starting at this address fails (nothing is written); later ones pass. */
void fake_iflash_fail_program_once_at(uint32_t addr);

/** @brief Every read fails. */
void fake_iflash_fail_all_reads(bool fail);

/** @brief A read starting at this address fails. */
void fake_iflash_fail_read_at(uint32_t addr);

/** @brief After a program covers this address, flip bit 0 of that byte (read-back corruption). */
void fake_iflash_corrupt_after_program(uint32_t addr);

#endif /* FAKE_INTERNAL_FLASH_H */
