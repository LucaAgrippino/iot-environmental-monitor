/**
 * @file config_service.h
 * @brief ConfigService (Gateway) — live configuration and the validate / apply / persist pipeline.
 *
 * Holds the system configuration in RAM, validates every change against the parameter
 * rules, persists it through ConfigStore and notifies registered consumers. It has no
 * task: every call runs in the caller's context, serialised by a mutex. The read side
 * (iconfig_provider_t) and write side (iconfig_manager_t) are separate interfaces.
 *
 * LifecycleController is meant to drive load, snapshot and restore; the Gateway has no
 * LifecycleController yet, so callers do it directly.
 *
 * @note Companion: docs/lld/application/config-service-lld.md
 */

#ifndef CONFIG_SERVICE_H
#define CONFIG_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "config_params.h"
#include "config_service_types.h"
#include "config_store/config_store.h"
#include "iconfig_manager.h"
#include "iconfig_provider.h"

/** Version of the stored blob layout; a mismatch discards the stored configuration (CFG-D5). */
#define CONFIG_SCHEMA_VERSION (1U)

/** @brief Stored form of the configuration: schema version followed by the parameters. */
typedef struct __attribute__((packed))
{
    uint32_t schema_version;
    config_params_t params;
} config_blob_t;

_Static_assert(sizeof(config_blob_t) <= CONFIG_STORE_MAX_DATA_BYTES,
               "config blob exceeds ConfigStore capacity");

/**
 * @brief Initialise ConfigService with the default configuration.
 *
 * Creates the mutex and applies the defaults. It does not read ConfigStore: the stored
 * configuration is loaded afterwards with config_store_load() and config_service_apply_loaded(),
 * so get_params() is valid at once. A second call is a no-op returning CONFIG_SERVICE_OK.
 *
 * @param[in] store  ConfigStore instance used by every persist; must not be NULL.
 * @return CONFIG_SERVICE_OK; CONFIG_SERVICE_ERR_NULL_ARG if @p store is NULL.
 * @note Threading: call once before the scheduler starts, after ConfigStore is created.
 */
config_service_err_t config_service_init(config_store_handle_t store);

/**
 * @brief Get the read-side interface.
 * @return The static iconfig_provider_t, or NULL until init has succeeded.
 */
const iconfig_provider_t *config_service_get_provider(void);

/**
 * @brief Get the write-side interface.
 * @return The static iconfig_manager_t, or NULL until init has succeeded.
 */
const iconfig_manager_t *config_service_get_manager(void);

/**
 * @brief Apply a configuration blob read from ConfigStore.
 *
 * Every field is validated; a field that fails keeps its default (corrupt stored value).
 * A wrong length or schema version discards the whole blob and applies the defaults.
 * Change callbacks are not fired.
 *
 * @param[in] blob  Bytes returned by config_store_load().
 * @param[in] len   Byte count; must equal sizeof(config_blob_t).
 * @return CONFIG_SERVICE_OK; CONFIG_SERVICE_ERR_NOT_INIT; CONFIG_SERVICE_ERR_NULL_ARG;
 *         CONFIG_SERVICE_ERR_INVALID if the length or schema version is wrong (defaults applied).
 * @note Threading: task context; takes the mutex.
 */
config_service_err_t config_service_apply_loaded(const void *blob, uint32_t len);

/**
 * @brief Get the live configuration.
 *
 * The pointer is valid for the system lifetime. Naturally aligned fields are read in one
 * access; misaligned 32-bit fields and the string fields are not atomic, so readers of
 * those must bracket the read with config_service_lock() / config_service_unlock()
 * (CFG-D3, CFG-O5).
 *
 * @return The live parameters, or NULL until init has succeeded.
 * @note Threading: any task.
 */
const config_params_t *config_service_get_params(void);

/**
 * @brief Validate, apply and persist one parameter.
 *
 * On a validation failure nothing changes. Otherwise the value is applied in memory and
 * persisted; if persisting fails the value stays applied and CONFIG_SERVICE_ERR_PERSIST
 * is returned. Callbacks fire after any in-memory change, with the mutex released.
 *
 * @param[in] id     Parameter identifier.
 * @param[in] value  New value; its type is listed with config_param_id_t.
 * @return CONFIG_SERVICE_OK; _ERR_NOT_INIT; _ERR_NULL_ARG; _ERR_INVALID (bad id or value);
 *         _ERR_PERSIST.
 * @note Threading: task context; takes the mutex and may block on a flash write.
 */
config_service_err_t config_service_set_param(config_param_id_t id, const void *value);

/**
 * @brief Check a proposed value against the rules and the current configuration.
 *
 * Does not change anything and does not take the mutex.
 *
 * @param[in] id     Parameter identifier.
 * @param[in] value  Proposed value.
 * @return CONFIG_SERVICE_OK; _ERR_NOT_INIT; _ERR_NULL_ARG; _ERR_INVALID.
 * @note Threading: any task.
 */
config_service_err_t config_service_validate_param(config_param_id_t id, const void *value);

/**
 * @brief Save a copy of the configuration for a later rollback.
 * @return CONFIG_SERVICE_OK; CONFIG_SERVICE_ERR_NOT_INIT.
 * @note Threading: task context; takes the mutex.
 */
config_service_err_t config_service_snapshot(void);

/**
 * @brief Restore the configuration saved by config_service_snapshot() and persist it.
 *
 * Callbacks are not fired (CFG-O6).
 *
 * @return CONFIG_SERVICE_OK; _ERR_NOT_INIT; _ERR_INVALID if no snapshot exists;
 *         _ERR_PERSIST (the restored values are applied in memory regardless).
 * @note Threading: task context; takes the mutex and may block on a flash write.
 */
config_service_err_t config_service_restore_snapshot(void);

/**
 * @brief Persist the current configuration (for example before a soft reset).
 * @return CONFIG_SERVICE_OK; CONFIG_SERVICE_ERR_NOT_INIT; CONFIG_SERVICE_ERR_PERSIST.
 * @note Threading: task context; takes the mutex and may block on a flash write.
 */
config_service_err_t config_service_flush(void);

/**
 * @brief Register a callback fired after a parameter changes.
 *
 * @param[in] cb  Callback; must stay valid for the system lifetime.
 * @return CONFIG_SERVICE_OK; _ERR_NOT_INIT; _ERR_NULL_ARG; _ERR_INVALID if
 *         CONFIG_MAX_CHANGE_CALLBACKS are already registered.
 * @note Threading: call before the scheduler starts.
 */
config_service_err_t config_service_register_change_cb(config_change_cb_t cb);

/**
 * @brief Take the configuration mutex around a read of a non-atomic field.
 * @note Threading: task context. Pair with config_service_unlock().
 */
void config_service_lock(void);

/** @brief Release the configuration mutex. */
void config_service_unlock(void);

#ifdef TEST
/** @brief Reset all module state (unit tests only). */
void config_service_reset_for_test(void);
#endif /* TEST */

#endif /* CONFIG_SERVICE_H */
