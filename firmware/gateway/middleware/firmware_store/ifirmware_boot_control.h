/**
 * @file ifirmware_boot_control.h
 * @brief IFirmwareBootControl — boot state, confirm and rollback (LifecycleController,
 * UpdateService).
 *
 * Vtable and types only. FirmwareStore never resets the MCU: the caller resets
 * after activate() and rollback().
 *
 * @see docs/lld/middleware/firmware-store.md §7.3
 */

#ifndef IFIRMWARE_BOOT_CONTROL_H
#define IFIRMWARE_BOOT_CONTROL_H

#include "firmware_store_types.h"

typedef struct
{
    /** Report the cached boot state; performs no flash access. */
    firmware_store_err_t (*get_boot_info)(const firmware_store_t *self,
                                          firmware_boot_info_t *info_out);

    /** Self-check passed: TRIAL_RUNNING -> CONFIRMED; the previous slot's descriptor is erased. */
    firmware_store_err_t (*confirm)(firmware_store_t *self);

    /** Self-check failed: active = previous, ROLLED_BACK, rollback_count + 1. */
    firmware_store_err_t (*rollback)(firmware_store_t *self);

    /** Rollback reported to the cloud: ROLLED_BACK -> CONFIRMED; the failed slot's descriptor is
     * erased. */
    firmware_store_err_t (*acknowledge_rollback)(firmware_store_t *self);
} ifirmware_boot_control_t;

#endif /* IFIRMWARE_BOOT_CONTROL_H */
