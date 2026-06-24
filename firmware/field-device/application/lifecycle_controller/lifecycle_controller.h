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

/* ======================================================================= */
/* Dependency headers — real in production, stubs in TEST                  */
/* ======================================================================= */

#ifndef TEST

#include "config_store/config_store.h"
#include "config_service/config_service.h"
#include "sensor_service/sensor_service.h"
#include "alarm_service/alarm_service.h"
#include "console_service/console_service.h"
#include "health_monitor/health_monitor.h"
#ifdef BOARD_FIELD_DEVICE
#include "graphics_library/graphics_library.h"
#include "lcd_ui/lcd_ui.h"
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
#ifdef BOARD_FIELD_DEVICE
#include "graphics_library_stub.h"
#include "lcd_ui_stub.h"
#include "modbus_slave_iface_stub.h"
#else /* BOARD_GATEWAY */
#include "lifecycle_gw_deps.h"
#endif /* BOARD_FIELD_DEVICE */
#include "FreeRTOS.h"

#endif /* TEST */

/* ======================================================================= */
/* Data types                                                               */
/* ======================================================================= */

/** Lifecycle state machine states (§5.1). */
#ifndef LIFECYCLE_STATE_DEFINED
#define LIFECYCLE_STATE_DEFINED
typedef enum
{
    LIFECYCLE_STATE_INIT = 0,
    LIFECYCLE_STATE_OPERATIONAL = 1,
    LIFECYCLE_STATE_EDITING_CONFIG = 2,
    LIFECYCLE_STATE_RESTARTING = 3,  /* GW only */
    LIFECYCLE_STATE_UPDATING_FW = 4, /* GW only */
    LIFECYCLE_STATE_FAULTED = 5,
} lifecycle_state_t;
#endif /* LIFECYCLE_STATE_DEFINED */

/** Cause of the most recent hardware reset (§5.2). */
#ifndef LIFECYCLE_RESET_CAUSE_DEFINED
#define LIFECYCLE_RESET_CAUSE_DEFINED
typedef enum
{
    LIFECYCLE_RESET_POWER_ON = 0,
    LIFECYCLE_RESET_SOFT = 1,
    LIFECYCLE_RESET_WATCHDOG = 2,
    LIFECYCLE_RESET_UNKNOWN = 3,
} lifecycle_reset_cause_t;
#endif /* LIFECYCLE_RESET_CAUSE_DEFINED */

/** Error codes (§5.3). */
typedef enum
{
    LIFECYCLE_ERR_OK = 0,
    LIFECYCLE_ERR_NULL_ARG = 1,
    LIFECYCLE_ERR_NOT_INIT = 2,
    LIFECYCLE_ERR_QUEUE_FULL = 3,
    LIFECYCLE_ERR_UNKNOWN_CMD = 4,
    LIFECYCLE_ERR_BAD_STATE = 5,
} lifecycle_err_t;

/** Runtime event types (§5.4). */
typedef enum
{
    LC_EVENT_CONFIG_EDIT_ENTER = 0,
    LC_EVENT_CONFIG_EDIT_APPLY = 1,
    LC_EVENT_CONFIG_EDIT_CANCEL = 2,
    LC_EVENT_CONFIG_EDIT_TIMEOUT = 3,
    LC_EVENT_RESTART_REQUESTED = 4, /* GW */
    LC_EVENT_RESTART_CONFIRMED = 5, /* GW */
    LC_EVENT_RESTART_TIMEOUT = 6,   /* GW */
    LC_EVENT_OTA_REQUESTED = 7,     /* GW */
    LC_EVENT_SELF_CHECK_PASS = 8,   /* GW */
    LC_EVENT_SELF_CHECK_FAIL = 9,   /* GW */
    LC_EVENT_UNRECOVERABLE_FAULT = 10,
} lifecycle_event_type_t;

/** Event payload (§5.4). */
typedef struct
{
    lifecycle_event_type_t type;
    uint32_t param; /* fault code, OTA image size, etc. */
} lifecycle_event_t;

/** Remote commands dispatched through handle_remote_command (§5.5). */
typedef enum
{
    LC_REMOTE_CMD_SOFT_RESTART = 0,
    LC_REMOTE_CMD_RESET_METRICS = 1,
} lifecycle_remote_cmd_t;

/** Fault param sent with LC_EVENT_UNRECOVERABLE_FAULT on init timeout. */
#define LC_FAULT_INIT_TIMEOUT (0xFFF0U)

/* ======================================================================= */
/* Provided vtable — ILifecycle (§4.2)                                     */
/* ======================================================================= */

typedef struct
{
    lifecycle_state_t (*get_state)(void);
    lifecycle_reset_cause_t (*get_reset_cause)(void);
    bool (*post_event)(lifecycle_event_t event);
    lifecycle_err_t (*handle_remote_command)(lifecycle_remote_cmd_t cmd);
} ilifecycle_t;

extern const ilifecycle_t *const lifecycle_controller;

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
                          const igraphics_library_t *graphics, const ilcd_ui_t *lcd_ui,
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
