/**
 * @file fake_qspi_flash.h
 * @brief RAM QSPI NOR flash with the real driver's rules (FirmwareStore tests).
 *
 * Erase sets a 4 KB sector to 0xFF; programming only clears bits; a program is
 * 1..256 bytes inside one 256 B page. Calls the real driver would reject are
 * counted as rule violations. Fault injection by address, and power-cut
 * injection through power_cut.h.
 *
 * Including this header makes Ceedling auto-link fake_qspi_flash.c.
 */

#ifndef FAKE_QSPI_FLASH_H
#define FAKE_QSPI_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "qspi_flash/qspi_flash.h"

/** Device bytes simulated: covers the whole OTA staging partition used here. */
#define FAKE_QSPI_SIZE (0x200000UL)

/** Sectors in the simulated device. */
#define FAKE_QSPI_SECTORS (FAKE_QSPI_SIZE / QSPI_FLASH_SECTOR_SIZE_BYTES)

/** Marker for "no fault armed". */
#define FAKE_QSPI_NO_ADDR (0xFFFFFFFFUL)

extern const iqspi_flash_t fake_qspi_flash_if;

/** @brief Erase everything to 0xFF, clear counters and faults. */
void fake_qspi_reset(void);

/** @brief Direct access to the simulated device (for seeding and inspection). */
uint8_t *fake_qspi_mem(void);

uint32_t fake_qspi_read_calls(void);
uint32_t fake_qspi_program_calls(void);
uint32_t fake_qspi_erase_calls(void);

/** @brief Calls the real driver would have rejected (bad page crossing, out of range ...). */
uint32_t fake_qspi_rule_violations(void);

/** @brief How many times a sector has been erased since the last reset. */
uint32_t fake_qspi_sector_erases(uint32_t sector);

/** @brief Every read fails. */
void fake_qspi_fail_all_reads(bool fail);

/** @brief A read that starts at this device offset fails. */
void fake_qspi_fail_read_at(uint32_t addr);

/** @brief A program that starts at this device offset fails (nothing is written). */
void fake_qspi_fail_program_at(uint32_t addr);

/** @brief An erase of the sector holding this device offset fails. */
void fake_qspi_fail_erase_at(uint32_t addr);

#endif /* FAKE_QSPI_FLASH_H */
