/**
 * @file lifecycle_wiring.h
 * @brief Binds the real Gateway modules to LifecycleController's interfaces.
 *
 * LifecycleController reaches every collaborator through an lc_*_if_t vtable (lifecycle_deps.h).
 * This module supplies the implementations: thin adapters over the real ConfigStore,
 * ConfigService, SensorService, AlarmService, ModbusPoller, TimeService, CloudPublisher,
 * HealthMonitor and ResetDriver. The Gateway main() calls lifecycle_wiring_bind() once and hands
 * the result to lifecycle_controller_init().
 *
 * What each adapter does is deliberately not always "call the module's init":
 *  - SensorService and AlarmService are initialised by sensor_task_create() before the scheduler
 *    (SS-D9), because the sensor drivers require it. Their adapters verify that and apply the
 *    loaded configuration.
 *  - CloudPublisher and ConsoleService are created by main() before the scheduler. Their adapters
 *    verify the instance exists (the console has nothing left to finish).
 *  - ConfigStore reports "no valid slot" on a never-programmed device. By default that is a first
 *    boot and the defaults apply; set fault_on_no_config to treat it as fatal (LC-D16).
 *  - UpdateService and FirmwareStore are bound through their interface vtables (iupdate_control_t,
 *    ifirmware_boot_control_t), so this module links neither. Leave all four handles NULL and the
 *    stand-ins apply instead: nothing pending, every update refused, so a Gateway without OTA
 *    boots and never enters UpdatingFirmware (LC-O6).
 *
 * @note See docs/lld/application/lifecycle-controller-lld.md section 26.
 */

#ifndef LIFECYCLE_WIRING_H
#define LIFECYCLE_WIRING_H

#include <stdbool.h>

#include "cloud_publisher/cloud_publisher.h"
#include "config_store/config_store.h"
#include "health_monitor/health_monitor.h"
#include "lifecycle_controller/lifecycle_controller.h"
#include "ntp_client/ntp_client.h"
#include "firmware_store/ifirmware_boot_control.h"
#include "update_service/iupdate_control.h"

/** @brief The handles the adapters need; all are created by main() before the scheduler. */
typedef struct
{
    health_monitor_handle_t health;     /**< HealthMonitor.                               */
    config_store_handle_t config_store; /**< ConfigStore.                                 */
    cloud_publisher_handle_t cloud;     /**< CloudPublisher, already created.             */
    ntp_client_handle_t ntp;            /**< NtpClient, handed to TimeService on init.    */
    bool fault_on_no_config;            /**< true: "no valid slot" is fatal, not a first boot. */

    /** @name OTA (all four set, or all four NULL for a Gateway without OTA) */
    /**@{*/
    const iupdate_control_t *update_ops;          /**< &update_control_ops.                  */
    update_service_t *update;                     /**< UpdateService, already created.       */
    const ifirmware_boot_control_t *firmware_ops; /**< &firmware_store_boot_ops.             */
    firmware_store_t *firmware;                   /**< FirmwareStore, already created.       */
    /**@}*/
} lifecycle_wiring_config_t;

/**
 * @brief Build the dependency bundle for lifecycle_controller_init().
 *
 * Copies the configuration and fills @p deps_out with pointers to this module's static adapter
 * tables. The watchdog member is left NULL: the Gateway has no watchdog driver yet (LC-O8).
 *
 * @param[in]  config    Handles; every handle must be non-NULL except the four OTA members, which
 *                       are set together or left NULL together.
 * @param[out] deps_out  Receives the bundle.
 * @return LIFECYCLE_ERR_OK; LIFECYCLE_ERR_NULL_ARG if @p config, @p deps_out or a handle is NULL.
 * @note Threading: call once, before the scheduler starts.
 */
lifecycle_err_t lifecycle_wiring_bind(const lifecycle_wiring_config_t *config,
                                      lifecycle_deps_t *deps_out);

/** @name Remote control commands (LC-D17)
 *  The /control topic carries {"command":"<name>"}. The restart is two-step (REQ-DM-020): the
 *  first request moves the Gateway to Restarting, the confirmation resets it.
 *  @{ */
#define LIFECYCLE_REMOTE_CMD_RESTART "restart"
#define LIFECYCLE_REMOTE_CMD_RESTART_CONFIRM "restart_confirm"
#define LIFECYCLE_REMOTE_CMD_RESET_METRICS "reset_metrics"
/** @} */

/**
 * @brief Dispatch a /control message from CloudPublisher to LifecycleController.
 *
 * This is the symbol CloudPublisher already calls (its ILifecycle placeholder). The payload is
 * matched for "command":"restart", "restart_confirm" or "reset_metrics"; anything else is logged
 * and ignored, and only the first 63 bytes are examined. Results are not reported back yet
 * (CloudPublisher's call returns void).
 *
 * @param[in] handle   Unused; LifecycleController is a singleton.
 * @param[in] payload  Message body, not NUL-terminated; NULL or empty is ignored.
 * @param[in] len      Byte count.
 * @note Threading: CloudPublisherTask. Not ISR-safe.
 */
void lifecycle_handle_remote(lifecycle_handle_t handle, const uint8_t *payload, uint32_t len);

/**
 * @brief ModbusPollerTask entry: wait for the start gate, then run the poller.
 *
 * ModbusPoller's own task body asserts that init has run, and init is done by LifecycleController
 * after the scheduler starts, so the poller task must not begin before the gate opens (companion
 * section 17).
 *
 * @param[in] arg  Unused.
 * @note Threading: ModbusPollerTask only. Does not return.
 */
void lifecycle_wiring_poller_task(void *arg);

#ifdef TEST
/** @brief Forget the bound configuration. */
void lifecycle_wiring_reset_for_test(void);
#endif /* TEST */

#endif /* LIFECYCLE_WIRING_H */
