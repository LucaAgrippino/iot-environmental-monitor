/**
 * @file firmware_store.h
 * @brief FirmwareStore — OTA image staging, authentication, installation and boot control
 * (Gateway).
 *
 * Owns the mechanics of firmware storage: staging the signed image on QSPI with
 * a power-safe resume journal, authenticating it, copying it into the inactive
 * on-chip slot, and keeping the A/B boot metadata that is shared with the
 * bootloader. UpdateService owns the policy (when to start, retry, discard,
 * activate, confirm and roll back).
 *
 * Provides IFirmwareImageStore (UpdateService) and IFirmwareBootControl
 * (UpdateService, LifecycleController). Called from task context only; one
 * priority-inheritance mutex serialises callers. The module has no task.
 *
 * Gated by the prerequisites in firmware-store.md §4: until the InternalFlash
 * driver and the bootloader exist the module is exercised on the host against
 * fakes only.
 *
 * @see docs/lld/middleware/firmware-store.md
 */

#ifndef FIRMWARE_STORE_H
#define FIRMWARE_STORE_H

#include <stdint.h>

#include "internal_flash/iinternal_flash.h"
#include "ifirmware_boot_control.h"
#include "ifirmware_image_store.h"
#include "firmware_store_types.h"
#include "qspi_flash/qspi_flash.h"

/** @brief Dependencies injected at creation. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember */
    const iqspi_flash_t *qspi; /**< Staging: QspiFlashDriver vtable.                   */
    /* cppcheck-suppress unusedStructMember */
    const iinternal_flash_t *iflash; /**< Slots and metadata: InternalFlashDriver vtable. */
    /* cppcheck-suppress unusedStructMember */
    uint32_t running_image_base; /**< Vector-table base the image booted with (FS-O6). */
} firmware_store_config_t;

/**
 * @brief Create the FirmwareStore instance and resolve the boot state.
 *
 * Reads the metadata and descriptors only. Never erases or programs flash. An
 * image that is not OTA-capable (a development build, or inconsistent
 * metadata) is not an error: the cause is logged once and get_boot_info()
 * reports ota_capable = false.
 *
 * @param[in]  cfg  Dependencies and the running image base.
 * @param[out] out  Instance handle.
 * @return FIRMWARE_STORE_ERR_OK; ERR_NULL_POINTER on any NULL in cfg or out;
 *         ERR_NO_INSTANCE if already created or the mutex cannot be created;
 *         ERR_FLASH_READ on a read failure.
 * @note Threading: call once, before the scheduler starts.
 */
firmware_store_err_t firmware_store_create(const firmware_store_config_t *cfg,
                                           firmware_store_t **out);

/** IFirmwareImageStore implementation — consumer: UpdateService. */
extern const ifirmware_image_store_t firmware_store_image_ops;

/** IFirmwareBootControl implementation — consumers: UpdateService, LifecycleController. */
extern const ifirmware_boot_control_t firmware_store_boot_ops;

#ifdef TEST
/** @brief Test hook — release the instance and return the pool to its post-bss state. */
void firmware_store_reset_for_test(void);
#endif

#endif /* FIRMWARE_STORE_H */
