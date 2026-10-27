/**
 * @file boot_core.h
 * @brief Bootloader decision logic: which slot to run, and the boot-record transitions on the way.
 *
 * Hardware-free. Everything it touches goes through the iinternal_flash_t vtable, so the host
 * tests run this code over fake_internal_flash and the target runs it over InternalFlashDriver.
 * It never jumps and never loops: boot_main.c acts on the outcome.
 *
 * @see docs/lld/drivers/bootloader.md §2.2, §3
 * @see docs/hld/flash-partition-layout.md §7
 */

#ifndef BOOT_CORE_H
#define BOOT_CORE_H

#include <stdint.h>

#include "firmware_store/firmware_image_format.h"
#include "internal_flash/iinternal_flash.h"

/**
 * Rollbacks after which the bootloader halts instead of rolling back again. The count it is
 * compared with is the stored (lifetime) one: docs/lld/drivers/bootloader.md BL-D5, BL-O2.
 */
#ifndef BOOT_MAX_ROLLBACKS
#define BOOT_MAX_ROLLBACKS (16U)
#endif

/** @brief What boot_main.c must do next. */
typedef enum
{
    BOOT_OUTCOME_RUN = 0,             /**< Jump to boot_report_t::slot_base.                    */
    BOOT_OUTCOME_HALT_NO_IMAGE,       /**< Neither slot is bootable.                            */
    BOOT_OUTCOME_HALT_ROLLBACK_LIMIT, /**< BOOT_MAX_ROLLBACKS reached.                          */
    BOOT_OUTCOME_HALT_FLASH_FAULT     /**< Flash unusable, or the trial could not be armed.     */
} boot_outcome_t;

/** @name Event bits of boot_report_t::events */
/**@{*/
#define BOOT_EVT_RECORD_INITIALISED (1UL << 0) /**< No valid record: factory record written.    */
#define BOOT_EVT_TRIAL_STARTED (1UL << 1)      /**< TRIAL_PENDING became TRIAL_RUNNING.         */
#define BOOT_EVT_TRIAL_ABANDONED (1UL << 2)    /**< TRIAL_RUNNING found at reset: rolled back.  */
#define BOOT_EVT_ACTIVE_INVALID (1UL << 3)     /**< Active slot failed verification: fell back. */
#define BOOT_EVT_META_WRITE_FAILED (1UL << 4)  /**< A record write failed after one retry.      */
#define BOOT_EVT_COPY_INVALID (1UL << 5)       /**< Exactly one record copy was invalid.        */
/**@}*/

/** @brief Result of one boot decision. */
typedef struct
{
    boot_outcome_t outcome;
    uint32_t events;         /**< BOOT_EVT_* bits.                                       */
    uint32_t slot_base;      /**< Slot to run (RUN only).                                */
    uint32_t image_len;      /**< Its descriptor's length (RUN only).                    */
    uint32_t version;        /**< Its descriptor's version (RUN only).                   */
    uint8_t state_in;        /**< boot_state found at reset; 0 when no valid record.     */
    uint8_t state_out;       /**< boot_state in force at the jump.                       */
    uint8_t active_bank;     /**< FIRMWARE_BANK_A or _B in force.                        */
    uint32_t generation;     /**< Generation of the record in force.                     */
    uint32_t rollback_count; /**< Its rollback count.                                    */
} boot_report_t;

/**
 * @brief Run steps 1 to 3 of the bootloader contract (flash-partition-layout.md §7).
 *
 * Selects the boot record, applies the boot_state rule, verifies the active slot and falls back
 * to the other one when it fails. Writes the boot record when a transition requires it; never
 * writes a descriptor or a slot.
 *
 * @param[in]  fl      Initialised on-chip flash.
 * @param[out] report  Filled on every return.
 * @return The outcome, also stored in report->outcome.
 */
boot_outcome_t boot_core_run(const iinternal_flash_t *fl, boot_report_t *report);

#endif /* BOOT_CORE_H */
