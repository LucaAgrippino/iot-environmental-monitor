/**
 * @file device_profile_registry_stub.h
 * @brief Narrow stub for DeviceProfileRegistry in the ModbusPoller unit tests.
 *
 * Delegates to the canonical leaf header so device_profile_t is shared with
 * production.  A test that includes this file instead of
 * device_profile_registry.h keeps Ceedling from auto-linking
 * device_profile_registry.c (the weak placeholder); the test supplies its own
 * device_profile_get_next() / device_profile_is_registered().
 */

#ifndef DEVICE_PROFILE_REGISTRY_STUB_H
#define DEVICE_PROFILE_REGISTRY_STUB_H

#include "device_profile_registry/device_profile_registry.h"

#endif /* DEVICE_PROFILE_REGISTRY_STUB_H */
