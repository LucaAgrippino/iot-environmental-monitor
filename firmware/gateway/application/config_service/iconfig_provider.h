/**
 * @file iconfig_provider.h
 * @brief IConfigProvider — read side of ConfigService (P3 interface split).
 *
 * Consumed by SensorService, AlarmService, ModbusPoller, TimeService and ConsoleService.
 */

#ifndef ICONFIG_PROVIDER_H
#define ICONFIG_PROVIDER_H

#include "config_params.h"

typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by the read-side consumers */
    const config_params_t *(*get_params)(void *ctx);
} iconfig_provider_t;

#endif /* ICONFIG_PROVIDER_H */
