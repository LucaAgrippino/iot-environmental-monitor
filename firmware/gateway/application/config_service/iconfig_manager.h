/**
 * @file iconfig_manager.h
 * @brief IConfigManager — write side of ConfigService (P3 interface split).
 *
 * Consumed by ConsoleService and CloudPublisher. set_param persists before it returns
 * (about 0.8 s on the Gateway); set_param_deferred applies at once and leaves the flash
 * write to persist_pending(), which the caller must run from a context that may block.
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
    /* cppcheck-suppress unusedStructMember -- called via vtable by latency-sensitive callers */
    config_service_err_t (*set_param_deferred)(void *ctx, config_param_id_t id, const void *value);
    /* cppcheck-suppress unusedStructMember -- called via vtable by whoever owns the flush */
    config_service_err_t (*persist_pending)(void *ctx);
} iconfig_manager_t;

#endif /* ICONFIG_MANAGER_H */
