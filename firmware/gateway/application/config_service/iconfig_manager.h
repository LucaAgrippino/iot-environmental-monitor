/**
 * @file iconfig_manager.h
 * @brief IConfigManager — write side of ConfigService (P3 interface split).
 *
 * Consumed by ConsoleService and CloudPublisher.
 */

#ifndef ICONFIG_MANAGER_H
#define ICONFIG_MANAGER_H

#include "config_service_types.h"

typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by the write-side consumers */
    config_service_err_t (*set_param)(void *ctx, config_param_id_t id, const void *value);
    /* cppcheck-suppress unusedStructMember -- called via vtable by the write-side consumers */
    config_service_err_t (*validate_param)(void *ctx, config_param_id_t id, const void *value);
} iconfig_manager_t;

#endif /* ICONFIG_MANAGER_H */
