/**
 * @file device_profile_registry.h
 * @brief DeviceProfileRegistry — the configurable registry of Field Device profiles (Gateway).
 *
 * Each profile binds a Modbus slave address to the identity the Gateway expects from that
 * slave (map version and vendor code) and a description.  Read access (ModbusPoller,
 * ConsoleService) and write access (ConfigService, ConsoleService) are separate vtables.
 * Persistence is a callback pair, so the module depends on neither ConfigService nor
 * ConfigStore.
 *
 * @note Companion: docs/lld/application/device-profile-registry-lld.md v0.2
 */

#ifndef DEVICE_PROFILE_REGISTRY_H
#define DEVICE_PROFILE_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

/** Profiles the registry holds. */
#define DEVICE_PROFILE_MAX (8U)

/** Size of a description including its terminating NUL. */
#define DEVICE_PROFILE_DESC_MAX (32U)

/** The profile seeded when nothing is stored (LLD §6.1). */
#define DEVICE_PROFILE_DEFAULT_ADDR (1U)
#define DEVICE_PROFILE_DEFAULT_MAP_VERSION (1U)
#define DEVICE_PROFILE_DEFAULT_VENDOR_CODE (0x1A45U)
#define DEVICE_PROFILE_DEFAULT_DESCRIPTION "Field Device 1 (default)"

/** @brief One provisioned Modbus slave and the identity it must present. */
typedef struct
{
    uint8_t slave_addr;   /**< Slave address, 1..247.                                    */
    uint16_t map_version; /**< MAP_VERSION (register 0x0000) accepted; not 0.            */
    uint16_t vendor_code; /**< VENDOR_CODE (register 0x0007) required; not 0.            */
    char description[DEVICE_PROFILE_DESC_MAX]; /**< NUL-terminated label.                */
} device_profile_t;

typedef enum
{
    DEVICE_PROFILE_REGISTRY_OK = 0,
    DEVICE_PROFILE_REGISTRY_ERR_NULL_ARG,
    DEVICE_PROFILE_REGISTRY_ERR_NOT_INIT,
    DEVICE_PROFILE_REGISTRY_ERR_NOT_FOUND,
    DEVICE_PROFILE_REGISTRY_ERR_FULL,
    DEVICE_PROFILE_REGISTRY_ERR_INVALID,
    DEVICE_PROFILE_REGISTRY_ERR_PERSIST,
    DEVICE_PROFILE_REGISTRY_ERR_NO_RESOURCE
} device_profile_registry_err_t;

/** @brief Where the profiles are kept across reboots.  Bound to ConfigService by main(). */
typedef struct
{
    /** Read the stored profiles.  Return false if there are none or they are unreadable. */
    bool (*load)(device_profile_t *out, uint8_t max, uint8_t *count_out);
    /** Store the profiles.  Return false if the write failed.  May block for about 0.8 s. */
    bool (*save)(const device_profile_t *in, uint8_t count);
} device_profile_storage_t;

/** @brief Read side. */
typedef struct
{
    /**
     * Iterate the profiles: set *index to 0 first; each true return copies the profile at the
     * incoming *index into *out and advances *index.  The copy is taken under the registry
     * lock, so it is consistent even if a writer commits meanwhile.
     */
    bool (*get_next)(device_profile_t *out, uint8_t *index);
    device_profile_registry_err_t (*get_by_addr)(uint8_t slave_addr, device_profile_t *out);
    bool (*is_registered)(uint8_t slave_addr);
} idevice_profile_provider_t;

/** @brief Write side. */
typedef struct
{
    device_profile_registry_err_t (*add_or_update)(const device_profile_t *profile);
    device_profile_registry_err_t (*remove)(uint8_t slave_addr);
} idevice_profile_manager_t;

extern const idevice_profile_provider_t *const device_profile_provider;
extern const idevice_profile_manager_t *const device_profile_manager;

/**
 * @brief Create the mutexes and bind the storage.  The registry starts empty.
 * @param[in] storage  Both callbacks must be non-NULL.
 * @return OK; ERR_NULL_ARG; ERR_NO_RESOURCE if a mutex could not be created.
 *         A second call after success returns OK and changes nothing.
 * @note Threading: boot, single-threaded.
 */
device_profile_registry_err_t device_profile_registry_init(const device_profile_storage_t *storage);

/**
 * @brief Fill the registry from storage; seed the default profile if there is none.
 *
 * A failed or empty load, or a stored profile that fails validation, leaves the default
 * profile in memory; nothing is written until the first change (LLD §6.1).
 * @return OK; ERR_NOT_INIT.
 * @note Threading: boot, single-threaded.
 */
device_profile_registry_err_t device_profile_registry_load(void);

/** @brief Number of registered profiles; 0 before init. */
uint8_t device_profile_registry_count(void);

/** @brief Test hook: return to the un-initialised state. */
void device_profile_registry_reset_for_test(void);

/* Free-function form of the read side, kept for consumers that have not moved to the vtable. */
bool device_profile_get_next(device_profile_t *out, uint8_t *index);
bool device_profile_is_registered(uint8_t slave_addr);

#endif /* DEVICE_PROFILE_REGISTRY_H */
