/**
 * @file bootloader_sim.h
 * @brief Host-side simulator of the bootloader rows of firmware-store.md §13.2.
 *
 * The bootloader does not exist yet (FS-O7). This reproduces its documented
 * behaviour directly against the on-chip flash fake, independently of
 * FirmwareStore's own metadata code, so the two halves of the contract are
 * checked against each other: trial promotion, rollback of an unconfirmed
 * trial, first-boot initialisation and the descriptor fallback.
 *
 * Including this header makes Ceedling auto-link bootloader_sim.c.
 */

#ifndef BOOTLOADER_SIM_H
#define BOOTLOADER_SIM_H

#include <stdint.h>

#include "firmware_store_types.h"

/**
 * @brief Run one simulated boot.
 *
 * @return The slot the bootloader jumps to, or FIRMWARE_SLOT_NONE if it halts
 *         because neither slot holds a valid image.
 */
firmware_slot_t bootloader_sim_boot(void);

/** @brief Number of metadata records the simulator has written. */
uint32_t bootloader_sim_meta_writes(void);

#endif /* BOOTLOADER_SIM_H */
