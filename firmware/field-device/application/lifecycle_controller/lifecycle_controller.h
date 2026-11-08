/**
 * @file lifecycle_controller.h
 * @brief LifecycleController — boot orchestrator and runtime event router.
 *
 * Provides ILifecycle (vtable: get_state, get_reset_cause, post_event,
 * handle_remote_command). Drives the Init sub-state sequence, then routes
 * runtime events (EditingConfig, Restarting[GW], UpdatingFirmware[GW],
 * Faulted) from a FreeRTOS queue.
 *
 * Boards: Field Device (STM32F469I-DISCO) and Gateway (B-L475E-IOT01A).
 *
 * @see docs/lld/application/lifecycle-controller-lld.md
 */

#ifndef LIFECYCLE_CONTROLLER_H
#define LIFECYCLE_CONTROLLER_H

#include <stdint.h>
#include <stdbool.h>

/* ILifecycle types and vtable — included unconditionally; no cycle risk. */
#include "lifecycle_controller/ilifecycle.h"

/* ======================================================================= */
/* Dependency headers — real in production, stubs in TEST                  */
/* ======================================================================= */

#ifndef TEST

#include "config_store/config_store.h"
#include "config_service/config_service.h"
#include "sensor_service/sensor_service.h"
#include "alarm_service/alarm_service.h"
#include "console_service/console_service.h"
#include "health_monitor/ihealth_report.h"
#include "health_monitor/ihealth_admin.h"
#ifdef BOARD_FIELD_DEVICE
#include "modbus_slave/modbus_slave.h"
#else  /* BOARD_GATEWAY */
/** GW-specific headers — forward declarations until those modules land. */
typedef struct icloud_publisher_s icloud_publisher_t;
/** @brief Forward declaration: the Gateway's Modbus poller interface (not used on this board). */
typedef struct imodbus_poller_s imodbus_poller_t;
/** @brief Forward declaration: the Gateway's UpdateService interface (not used on this board). */
typedef struct iupdate_service_s iupdate_service_t;
/** @brief Forward declaration: the Gateway's TimeService interface (not used on this board). */
typedef struct itime_service_s itime_service_t;
/** @brief Forward declaration: the Gateway's FirmwareStore interface (not used on this board). */
typedef struct ifirmware_store_s ifirmware_store_t;
/** @brief Forward declaration: the Gateway's reset driver interface (not used on this board). */
typedef struct ireset_driver_s ireset_driver_t;
#endif /* BOARD_FIELD_DEVICE */
#include "FreeRTOS.h"
#include "event_groups.h"

#else /* TEST */

#include "config_store_stub.h"
#include "config_service_stub.h"
#include "sensor_service_stub.h"
#include "alarm_service_stub.h"
#include "console_service_stub.h"
#include "health_monitor_stub.h"
#ifdef BOARD_FIELD_DEVICE
#include "modbus_slave_iface_stub.h"
#else /* BOARD_GATEWAY */
#include "lifecycle_gw_deps.h"
#endif /* BOARD_FIELD_DEVICE */
#include "FreeRTOS.h"

#endif /* TEST */

/* ======================================================================= */
/* Reset-cause detection (§13)                                             */
/* ======================================================================= */

/**
 * @brief Read RCC->CSR flags, clear them, and return the reset cause.
 *
 * Call from main() before any other initialisation; pass the returned value
 * to lifecycle_controller_init() as its first argument.
 *
 * In TEST builds this function is compiled as a normal (non-static) symbol
 * so TC-LC-010..015 can call it directly with the CMSIS mock.
 *
 * @return The cause of the reset.
 */
lifecycle_reset_cause_t lifecycle_detect_reset_cause(void);

/* ======================================================================= */
/* Initialisation (§4.3)                                                   */
/* ======================================================================= */

/**
 * @brief Initialise LifecycleController.
 *
 * Stores all injected interface pointers, creates the event queue, timers,
 * and start-gate event group. Does not run the state machine.
 *
 * @param reset_cause    Cause of the reset, from lifecycle_detect_reset_cause().
 * @param config_store   ConfigStore access.
 * @param cfg_read       Configuration read side.
 * @param cfg_write      Configuration write side.
 * @param sensors        SensorService read side.
 * @param alarms         AlarmService read side.
 * @param console        Console control.
 * @param health_report  HealthMonitor event sink.
 * @param modbus_slave   Modbus slave (Field Device build).
 * @note The Gateway build takes further interfaces instead of @p modbus_slave: cloud,
 * modbus_poller, update_service, time_service, firmware_store, reset_driver and health_admin.
 * @return LIFECYCLE_ERR_OK or LIFECYCLE_ERR_NULL_ARG on any null pointer.
 */
lifecycle_err_t
lifecycle_controller_init(lifecycle_reset_cause_t reset_cause, const iconfig_store_t *config_store,
                          const iconfig_provider_t *cfg_read, const iconfig_manager_t *cfg_write,
                          const isensor_service_t *sensors, const ialarm_service_t *alarms,
                          const iconsole_service_t *console, const ihealth_report_t *health_report,
#ifdef BOARD_FIELD_DEVICE
                          const imodbus_slave_t *modbus_slave
#else  /* BOARD_GATEWAY */
                          const icloud_publisher_t *cloud, const imodbus_poller_t *modbus_poller,
                          const iupdate_service_t *update_service,
                          const itime_service_t *time_service,
                          const ifirmware_store_t *firmware_store,
                          const ireset_driver_t *reset_driver, const ihealth_admin_t *health_admin
#endif /* BOARD_FIELD_DEVICE */
);

/* ======================================================================= */
/* Task entry (§4.4)                                                       */
/* ======================================================================= */

/** FreeRTOS task body — drives boot sequence then event loop. Never returns
 * in production; in TEST mode processes one event and returns (LC-O5).
 *
 * @param arg Unused.
 */
void lifecycle_task_body(void *arg);

/* ======================================================================= */
/* Start-gate (§4.5)                                                       */
/* ======================================================================= */

/** Bit set by LifecycleController in the start-gate event group on
 *  Operational entry. Other tasks block on this bit before their work loop. */
#define LIFECYCLE_START_GATE_BIT (1U << 0)

/**
 * Returns the start-gate event group handle.
 *
 * @return Handle of the event group.
 */
EventGroupHandle_t lifecycle_get_start_gate(void);

/* ======================================================================= */
/* Test-only hooks (§LC-O5)                                                */
/* ======================================================================= */

#ifdef TEST
/** Zero all file-scope statics and recreate RTOS primitives. Call in setUp(). */
void lifecycle_controller_reset_for_test(void);
#endif /* TEST */

#endif /* LIFECYCLE_CONTROLLER_H */
