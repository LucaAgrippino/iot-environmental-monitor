/**
 * @file console_service.h
 * @brief ConsoleService — operator serial console (FD + GW application layer).
 *
 * Owns the debug-UART console. Reads lines from IDebugUart, parses them into
 * command tokens, and dispatches to a static command table. Responses are
 * written back through IDebugUart. All diagnostic data, config reads/writes,
 * and self-test evidence are obtained through injected interfaces.
 *
 * Boards: Field Device (STM32F469I-DISCO) and Gateway (B-L475E-IOT01A).
 *
 * @see docs/lld/application/console-service-lld.md
 */

#ifndef CONSOLE_SERVICE_H
#define CONSOLE_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ======================================================================= */
/* Dependency headers — real in production, narrow stubs in test builds    */
/* ======================================================================= */

#ifndef TEST
#include "debug-uart/debug_uart_driver.h"
#include "sensor_service/sensor_service.h"
#include "config_service/config_service.h"
#else
#include "debug_uart_stub.h"
#include "sensor_service_stub.h"
#include "config_service_stub.h"
#endif /* TEST */

/* ihealth_snapshot.h is a pure-type interface header — safe to include
 * unconditionally in both production and TEST builds. */
#include "health_monitor/ihealth_snapshot.h"

/* ======================================================================= */
/* IDeviceProfileManager (GW-only — not yet implemented)                   */
/* TODO: replace forward declaration with #include "device_profile_mgr/..."*/
/*       once that module lands.                                            */
/* ======================================================================= */

#if defined(BOARD_GATEWAY)
struct idevice_profile_mgr_s;
typedef struct idevice_profile_mgr_s idevice_profile_mgr_t;
#endif /* BOARD_GATEWAY */

/* ======================================================================= */
/* Error codes                                                             */
/* ======================================================================= */

typedef enum
{
    CONSOLE_SERVICE_ERR_OK = 0,
    CONSOLE_SERVICE_ERR_NULL_ARG = 1,
    CONSOLE_SERVICE_ERR_NOT_INIT = 2,
    CONSOLE_SERVICE_ERR_VALIDATION = 3,
    CONSOLE_SERVICE_ERR_UNKNOWN_KEY = 4,
    CONSOLE_SERVICE_ERR_APPLY_FAILED = 5,
    CONSOLE_SERVICE_ERR_TIMEOUT = 6,
    CONSOLE_SERVICE_ERR_LINE_OVERFLOW = 7,
} console_service_err_t;

/* ======================================================================= */
/* Provided vtable — IConsoleService                                       */
/* ======================================================================= */

typedef struct iconsole_service
{
    /**
     * @brief Finalise ConsoleService after all other services are operational.
     *
     * Called once by LifecycleController in sub-state StartingMiddleware,
     * just before the start-gate bit is set. Emits the "System ready" banner
     * and first command prompt. Must not be called before console_service_init().
     *
     * @return CONSOLE_SERVICE_ERR_OK on success.
     */
    console_service_err_t (*init_finalise)(void);

    /**
     * @brief Drain one line from the UART buffer and dispatch it.
     *
     * Called by ConsoleTask in its loop after the RX line-ready notification.
     * Returns immediately if no line is available (spurious wakeup).
     *
     * @return CONSOLE_SERVICE_ERR_OK on success or empty line;
     *         CONSOLE_SERVICE_ERR_LINE_OVERFLOW if the received line exceeded
     *         DEBUG_UART_LINE_MAX_LEN; other non-zero on command error.
     *         The prompt is always printed before returning.
     */
    console_service_err_t (*run_once)(void);
} iconsole_service_t;

extern const iconsole_service_t *const console_service;

/* ======================================================================= */
/* Initialisation                                                          */
/* ======================================================================= */

/**
 * @brief Initialise ConsoleService.
 *
 * Stores all injected interface pointers, zero-initialises staging structs,
 * and emits the boot prompt. Called once by LifecycleController before the
 * FreeRTOS scheduler starts.
 *
 * @param uart      IDebugUart — must not be NULL.
 * @param sensors   ISensorService — must not be NULL.
 * @param cfg_read  IConfigProvider — must not be NULL.
 * @param cfg_write IConfigManager — must not be NULL.
 * @param health    IHealthSnapshot — must not be NULL.
 * @param profiles  IDeviceProfileManager (GW only). Must not be NULL on GW;
 *                  ignored / pass NULL on FD.
 *
 * @return CONSOLE_SERVICE_ERR_OK on success; CONSOLE_SERVICE_ERR_NULL_ARG if
 *         any required pointer is NULL.
 */
console_service_err_t console_service_init(const idebug_uart_t *uart,
                                           const isensor_service_t *sensors,
                                           const iconfig_provider_t *cfg_read,
                                           const iconfig_manager_t *cfg_write,
                                           const ihealth_snapshot_t *health
#if defined(BOARD_GATEWAY)
                                           ,
                                           const idevice_profile_mgr_t *profiles
#endif
);

/* ======================================================================= */
/* Task entry point                                                        */
/* ======================================================================= */

/**
 * @brief FreeRTOS task body for ConsoleTask.
 *
 * Registers the UART RX callback, then loops: waits on the direct-to-task
 * line-ready notification and calls console_service->run_once(). Never returns.
 *
 * @param arg Ignored (pass NULL to xTaskCreateStatic).
 */
void console_task_body(void *arg);

/* ======================================================================= */
/* Test-only hooks                                                         */
/* ======================================================================= */

#ifdef TEST
/** Reset all file-scope statics to post-BSS state. Call from setUp(). */
void console_service_reset_for_test(void);
#endif /* TEST */

#endif /* CONSOLE_SERVICE_H */
