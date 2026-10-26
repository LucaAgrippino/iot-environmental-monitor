/**
 * @file firmware_store_types.h
 * @brief Error codes, enumerations and information structures of FirmwareStore.
 *
 * Kept apart from firmware_store.h so that the two interface headers can share
 * these types without including the creation API (and without a circular
 * include).
 *
 * @see docs/lld/middleware/firmware-store.md §8
 */

#ifndef FIRMWARE_STORE_TYPES_H
#define FIRMWARE_STORE_TYPES_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Opaque FirmwareStore instance, the first parameter of every operation. */
typedef struct firmware_store firmware_store_t;

/** @brief FirmwareStore error codes. Codes 0..3 follow the project-wide ADT convention. */
typedef enum
{
    FIRMWARE_STORE_ERR_OK = 0,
    FIRMWARE_STORE_ERR_NULL_POINTER = 1,
    FIRMWARE_STORE_ERR_NOT_INITIALISED = 2,
    FIRMWARE_STORE_ERR_NO_INSTANCE = 3,
    FIRMWARE_STORE_ERR_BAD_PARAM = 4,
    FIRMWARE_STORE_ERR_BAD_STATE = 5,
    FIRMWARE_STORE_ERR_NOT_OTA_CAPABLE = 6,
    FIRMWARE_STORE_ERR_NO_SESSION = 7,
    FIRMWARE_STORE_ERR_INCOMPLETE = 8,
    FIRMWARE_STORE_ERR_TOO_LARGE = 9,
    FIRMWARE_STORE_ERR_BAD_HEADER = 10,
    FIRMWARE_STORE_ERR_WRONG_SLOT = 11,
    FIRMWARE_STORE_ERR_SIG_INVALID = 12,
    FIRMWARE_STORE_ERR_HASH_MISMATCH = 13,
    FIRMWARE_STORE_ERR_BAD_IMAGE = 14,
    FIRMWARE_STORE_ERR_CRYPTO = 15,
    FIRMWARE_STORE_ERR_FLASH_READ = 16,
    FIRMWARE_STORE_ERR_FLASH_ERASE = 17,
    FIRMWARE_STORE_ERR_FLASH_WRITE = 18,
    FIRMWARE_STORE_ERR_INSTALL_VERIFY = 19
} firmware_store_err_t;

/** @brief On-chip firmware slot. */
typedef enum
{
    FIRMWARE_SLOT_A = 0,
    FIRMWARE_SLOT_B = 1,
    FIRMWARE_SLOT_NONE = 2 /**< The running image is not in a slot (development build). */
} firmware_slot_t;

/** @brief Boot state; the values are the on-flash encoding (firmware_image_format.h). */
typedef enum
{
    FIRMWARE_BOOT_CONFIRMED = 0x01U,
    FIRMWARE_BOOT_TRIAL_PENDING = 0x02U,
    FIRMWARE_BOOT_TRIAL_RUNNING = 0x03U,
    FIRMWARE_BOOT_ROLLED_BACK = 0x04U
} firmware_boot_state_t;

/** @brief Cached boot state reported by get_boot_info(). */
typedef struct
{
    bool ota_capable;
    firmware_slot_t running_slot;
    firmware_boot_state_t state; /**< Meaningful only if ota_capable.   */
    uint32_t rollback_count;
    uint32_t running_version; /**< 0 if no valid descriptor.           */
} firmware_boot_info_t;

/** @brief Facts about an authenticated staged image, filled by verify(). */
typedef struct
{
    uint32_t version;
    uint32_t image_len;
    firmware_slot_t target_slot;
} firmware_image_info_t;

#endif /* FIRMWARE_STORE_TYPES_H */
