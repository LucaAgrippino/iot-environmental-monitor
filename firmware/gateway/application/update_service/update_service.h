/**
 * @file update_service.h
 * @brief UpdateService — OTA update policy of the Gateway (Machine 3).
 *
 * Owns the policy of an update: when to start, how to fetch, when to retry or give up, when to
 * activate, when to confirm, and what to report. FirmwareStore owns the mechanics,
 * LifecycleController the system mode and the reset, CloudPublisher MQTT and serialisation.
 * One instance, one task (UpdateServiceTask); nothing survives a reset in RAM, all
 * non-volatile OTA state is FirmwareStore's.
 *
 * Gateway ADT convention: opaque instance passed as @c self, static pool of size one, plus
 * const ops tables (the FirmwareStore shape).
 *
 * @see docs/lld/application/update-service-lld.md
 */

#ifndef UPDATE_SERVICE_H
#define UPDATE_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "firmware_store/ifirmware_boot_control.h"
#include "firmware_store/ifirmware_image_store.h"
#include "iota_transport.h"
#include "iupdate_control.h"
#include "iupdate_service.h"
#include "lifecycle_controller/ilifecycle.h"
#include "update_service_types.h"

/** @brief Everything UpdateService needs, bound by the wiring code (§7.1). */
typedef struct
{
    const ifirmware_image_store_t *fs_image;
    const ifirmware_boot_control_t *fs_boot;
    firmware_store_t *fs; /**< FirmwareStore handle. */
    const iota_transport_t *transport;
    const us_probe_if_t *probes;
    const ilifecycle_t *lifecycle; /**< Normally the LifecycleController vtable. */
} update_service_config_t;

/**
 * @brief Create the instance, its mutex and UpdateServiceTask; cache the boot info.
 *
 * @param[in]  cfg  Bindings; every member and every function used must be non-NULL.
 * @param[out] out  Receives the instance.
 * @return UPDATE_SERVICE_ERR_OK; ERR_NULL_POINTER if cfg, out or any member (or any function
 *         pointer used) is NULL; ERR_NO_INSTANCE if already created or a FreeRTOS object cannot
 *         be created.
 * @note Threading: once, before the scheduler starts, after firmware_store_create().
 */
update_service_err_t update_service_create(const update_service_config_t *cfg,
                                           update_service_t **out);

/** @brief Wire string of a reason (§8.4), for CloudPublisher; "" for US_REASON_NONE. */
const char *update_service_reason_string(us_reason_t reason);

extern const iupdate_service_t update_service_ops; /**< CloudPublisher.        */
extern const iupdate_control_t update_control_ops; /**< LifecycleController.   */

#ifdef TEST
/** @brief Return to the power-on state; the pool, the mutex and the task are forgotten. */
void update_service_reset_for_test(void);

/** @brief Run the task body once: take the pending notifications, then check the deadlines. */
void update_service_task_step_for_test(void);

/** @brief Machine 3 state of the instance, for assertions. */
int update_service_state_for_test(void);
#endif

#endif /* UPDATE_SERVICE_H */
