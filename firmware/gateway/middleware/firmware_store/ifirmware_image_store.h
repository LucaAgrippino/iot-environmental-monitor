/**
 * @file ifirmware_image_store.h
 * @brief IFirmwareImageStore — staging, authentication and installation (UpdateService).
 *
 * Vtable and types only. Consumers receive a pointer to the vtable and the
 * FirmwareStore handle at wiring time and pass the handle as @c self.
 *
 * @see docs/lld/middleware/firmware-store.md §7.2
 */

#ifndef IFIRMWARE_IMAGE_STORE_H
#define IFIRMWARE_IMAGE_STORE_H

#include <stdint.h>

#include "firmware_store_types.h"

typedef struct
{
    /** Start a fresh staging session. */
    firmware_store_err_t (*begin)(firmware_store_t *self, uint32_t image_id, uint32_t file_len);

    /** Continue a session after a disconnect or reboot; *offset_out = bytes already staged. */
    firmware_store_err_t (*resume)(firmware_store_t *self, uint32_t image_id, uint32_t file_len,
                                   uint32_t *offset_out);

    /** Stage the next bytes of the file; offset must equal the current write position. */
    firmware_store_err_t (*write_chunk)(firmware_store_t *self, uint32_t offset,
                                        const uint8_t *data, uint32_t len);

    /** Authenticate the staged image (header, signature, body hash, reset vector). */
    firmware_store_err_t (*verify)(firmware_store_t *self, firmware_image_info_t *info_out);

    /** Invalidate the target descriptor and arm the copy. */
    firmware_store_err_t (*install_begin)(firmware_store_t *self);

    /** Copy and check one 2 KB page; *done_out is true after the last page. */
    firmware_store_err_t (*install_step)(firmware_store_t *self, bool *done_out);

    /** Point the next boot at the new slot (TRIAL_PENDING). The caller resets. */
    firmware_store_err_t (*activate)(firmware_store_t *self);

    /** Erase the journal and forget the session. */
    firmware_store_err_t (*discard)(firmware_store_t *self);
} ifirmware_image_store_t;

#endif /* IFIRMWARE_IMAGE_STORE_H */
