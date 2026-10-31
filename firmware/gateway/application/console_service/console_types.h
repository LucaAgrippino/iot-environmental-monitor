/**
 * @file console_types.h
 * @brief Types shared by ConsoleService and its core modules.
 *
 * Gathers the injected service interfaces. Test code includes this header, not the service
 * headers, so Ceedling does not link the real modules the tests replace with spies.
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §4, §5
 */

#ifndef CONSOLE_TYPES_H
#define CONSOLE_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "alarm_service/alarm_service.h"
#include "config_service/config_service.h"
#include "debug_uart/debug_uart.h"
#include "device_profile_registry/device_profile_registry.h"
#include "health_monitor/health_monitor.h"
#include "sensor_service/sensor_service.h"

/** @brief Error codes returned by ConsoleService operations (LLD §5.1). */
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

/** @brief Everything ConsoleService reads from or writes to. All pointers must be non-NULL. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- consumed by the service and the commands */
    const idebug_uart_t *uart;
    /* cppcheck-suppress unusedStructMember -- consumed by the model */
    const isensor_service_t *sensors;
    /* cppcheck-suppress unusedStructMember -- consumed by the model */
    const ialarm_service_t *alarms;
    /* cppcheck-suppress unusedStructMember -- consumed by the model and the commands */
    const iconfig_provider_t *cfg_read;
    /* cppcheck-suppress unusedStructMember -- consumed by the commands */
    const iconfig_manager_t *cfg_write;
    /* cppcheck-suppress unusedStructMember -- consumed by the model and the commands */
    health_monitor_handle_t health;
    /* cppcheck-suppress unusedStructMember -- consumed by `profiles list` */
    const idevice_profile_provider_t *prof_read;
} console_deps_t;

#endif /* CONSOLE_TYPES_H */
