/**
 * @file fake_flash_ctrl.h
 * @brief Host model of the STM32L475 flash controller (InternalFlashDriver tests).
 *
 * Implements the register-level behaviour the driver relies on: the KEYR unlock sequence
 * (a wrong key locks the controller until reset), CR.PG / CR.PER / CR.START, BSY for a
 * configurable number of SR reads, EOP, the error flags (PGAERR, PGSERR, PROGERR, WRPERR),
 * double-word programming that fails on a non-erased target, and the ACR cache-reset bits.
 * Every register write is recorded so a test can assert the exact sequence.
 *
 * Including this header makes Ceedling auto-link fake_flash_ctrl.c.
 */

#ifndef FAKE_FLASH_CTRL_H
#define FAKE_FLASH_CTRL_H

#include <stdbool.h>
#include <stdint.h>

/** Simulated main flash. */
#define FAKE_FLASH_BASE (0x08000000UL)
#define FAKE_FLASH_SIZE (0x100000UL)

/** @brief Kind of a recorded access. */
typedef enum
{
    FAKE_EVT_KEYR = 0, /**< KEYR write.                         */
    FAKE_EVT_CR,       /**< CR write.                           */
    FAKE_EVT_SR,       /**< SR write (clear flags).             */
    FAKE_EVT_ACR,      /**< ACR write.                          */
    FAKE_EVT_STORE     /**< 32-bit store to a flash address.    */
} fake_flash_event_kind_t;

/** @brief One recorded access. */
typedef struct
{
    fake_flash_event_kind_t kind;
    uint32_t addr; /**< Flash address for FAKE_EVT_STORE, else 0.      */
    uint32_t value;
    bool irq_masked; /**< Interrupts were masked when it happened.       */
} fake_flash_event_t;

/** @brief Everything back to power-on: locked, dual-bank, erased, no faults. */
void fake_flash_reset(void);

/* ---- setup and fault injection ---------------------------------------- */

void fake_flash_set_dual_bank(bool dual);

/** @brief SR reads that report BSY after each started operation (default 2). */
void fake_flash_set_busy_polls(uint32_t polls);

/** @brief BSY is already set when the driver is called, for this many SR reads. */
void fake_flash_set_initial_busy(uint32_t polls);

/** @brief BSY never clears. */
void fake_flash_set_stuck_busy(bool stuck);

/** @brief The controller ignores the unlock sequence. */
void fake_flash_set_stuck_lock(bool stuck);

/** @brief Erasing this page (0..511) sets WRPERR instead of erasing. */
void fake_flash_set_protected_page(uint32_t page_index);

/** @brief Flags already set in SR before the driver runs (stale errors). */
void fake_flash_preset_sr(uint32_t flags);

/* ---- observation ------------------------------------------------------- */

uint8_t *fake_flash_mem(uint32_t addr);
bool fake_flash_is_locked(void);
uint32_t fake_flash_cr_value(void);
uint32_t fake_flash_sr_value(void);
uint32_t fake_flash_event_count(void);
const fake_flash_event_t *fake_flash_event_at(uint32_t index);

/** @brief Number of ACR writes that pulsed ICRST or DCRST. */
uint32_t fake_flash_cache_flushes(void);

/** @brief Cache enables as left by the last ACR write. */
uint32_t fake_flash_acr_value(void);

/** @brief Interrupt mask depth now, and how many save/restore pairs ran. */
bool fake_flash_irq_is_masked(void);
uint32_t fake_flash_irq_pairs(void);

/** @brief Stores executed with interrupts masked / enabled. */
uint32_t fake_flash_stores_masked(void);
uint32_t fake_flash_stores_unmasked(void);

#endif /* FAKE_FLASH_CTRL_H */
