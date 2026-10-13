/**
 * @file device_profile_registry.h
 * @brief DeviceProfileRegistry read side as ModbusPoller consumes it (Gateway placeholder).
 *
 * The real DeviceProfileRegistry (docs/lld/application/device-profile-registry-lld.md)
 * is a Draft with no code.  This header carries the two functions that
 * modbus-poller-lld.md §2.2 specifies, so ModbusPoller can be built and
 * tested now; device_profile_registry.c supplies weak defaults that serve one
 * hard-coded Field Device profile.  The real module replaces that file.
 *
 * @note Companion: docs/lld/application/modbus-poller-lld.md §2.2
 */

#ifndef DEVICE_PROFILE_REGISTRY_H
#define DEVICE_PROFILE_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

/** @brief One provisioned Modbus slave and the input-register span to poll. */
typedef struct
{
    uint8_t slave_addr;       /**< Slave address, 1..247. */
    uint16_t input_reg_start; /**< FC04 range start. */
    uint16_t input_reg_count; /**< FC04 range length, 1..125. */
} device_profile_t;

/**
 * @brief Iterate the registered device profiles.
 *
 * Set *index to 0 and *profile to NULL before the first call.  Each call that
 * returns true points *profile at the profile stored at the incoming *index and
 * advances *index by one.
 *
 * @param[in,out] profile  Receives the next profile.
 * @param[in,out] index    Iterator position.
 * @return true if a profile was returned, false once the registry is exhausted
 *         or an argument is NULL.
 * @note Threading: callable from ModbusPollerTask.
 */
bool device_profile_get_next(const device_profile_t **profile, uint8_t *index);

/**
 * @brief Check whether a slave address is provisioned.
 *
 * @param[in] slave_addr  Address to look up.
 * @return true if the address is in the registry.
 * @note Threading: callable from any task.
 */
bool device_profile_is_registered(uint8_t slave_addr);

#endif /* DEVICE_PROFILE_REGISTRY_H */
