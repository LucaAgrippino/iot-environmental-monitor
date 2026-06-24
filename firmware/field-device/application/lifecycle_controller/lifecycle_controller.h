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
 * @see docs/lld/application/lifecycle-controller.md
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
#ifdef USE_GUI
/* GUI module includes reinstated here when graphics_library and lcd_ui land. */
#endif /* USE_GUI */
#ifdef BOARD_FIELD_DEVICE
#include "modbus_slave/modbus_slave.h"
#else  /* BOARD_GATEWAY */
/* GW-specific headers — forward declarations until those modules land. */
typedef struct icloud_publisher_s icloud_publisher_t;
typedef struct imodbus_poller_s imodbus_poller_t;
typedef struct iupdate_service_s iupdate_service_t;
typedef struct itime_service_s itime_service_t;
typedef struct ifirmware_store_s ifirmware_store_t;
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
#ifdef USE_GUI
#include "graphics_library_stub.h"
#include "lcd_ui_stub.h"
#endif /* USE_GUI */
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
 * @return LIFECYCLE_ERR_OK or LIFECYCLE_ERR_NULL_ARG on any null pointer.
 */
lifecycle_err_t
lifecycle_controller_init(lifecycle_reset_cause_t reset_cause, const iconfig_store_t *config_store,
                          const iconfig_provider_t *cfg_read, const iconfig_manager_t *cfg_write,
                          const isensor_service_t *sensors, const ialarm_service_t *alarms,
                          const iconsole_service_t *console, const ihealth_report_t *health_report,
#ifdef BOARD_FIELD_DEVICE
#ifdef USE_GUI
                          const igraphics_library_t *graphics, const ilcd_ui_t *lcd_ui,
#endif /* USE_GUI */
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
 *  in production; in TEST mode processes one event and returns (LC-O5). */
void lifecycle_task_body(void *arg);

/* ======================================================================= */
/* Start-gate (§4.5)                                                       */
/* ======================================================================= */

/** Bit set by LifecycleController in the start-gate event group on
 *  Operational entry. Other tasks block on this bit before their work loop. */
#define LIFECYCLE_START_GATE_BIT (1U << 0)

/** Returns the start-gate event group handle. */
EventGroupHandle_t lifecycle_get_start_gate(void);

/* ======================================================================= */
/* Test-only hooks (§LC-O5)                                                */
/* ======================================================================= */

#ifdef TEST
/** Zero all file-scope statics and recreate RTOS primitives. Call in setUp(). */
void lifecycle_controller_reset_for_test(void);
#endif /* TEST */

#endif /* LIFECYCLE_CONTROLLER_H */
