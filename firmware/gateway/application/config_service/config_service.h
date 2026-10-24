/**
 * @file config_service.h
 * @brief ConfigService (Gateway) — live configuration and the validate / apply / persist pipeline.
 *
 * Holds the system configuration in RAM, validates every change against the parameter
 * rules, persists it through ConfigStore and notifies registered consumers. It has no
 * task: every call runs in the caller's context. Two mutexes keep readers fast: one guards
 * the in-memory configuration (held for microseconds), the other serialises flash writes
 * (held for the whole ~0.8 s ConfigStore save), so a flash write never blocks a reader.
 * The read side (iconfig_provider_t) and write side (iconfig_manager_t) are separate.
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
typedef struct
{
    uint32_t schema_version;
    config_params_t params;
} config_blob_t;

_Static_assert(sizeof(config_blob_t) == (sizeof(uint32_t) + sizeof(config_params_t)),
               "config blob has unexpected padding");
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
 * The pointer is valid for the system lifetime. Every integer field is naturally aligned and
 * is read in one access, so it needs no lock. The string fields (mqtt_broker, ntp_servers)
 * are not atomic: bracket their reads with config_service_lock() / config_service_unlock()
 * (CFG-D3).
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
 * @note Threading: task context; blocks for the flash write (about 0.8 s). Readers are not
 *       blocked meanwhile. Use config_service_set_param_deferred() where that is too long.
 */
config_service_err_t config_service_set_param(config_param_id_t id, const void *value);

/**
 * @brief Validate and apply one parameter now; persist it later (CFG-D16).
 *
 * Does everything config_service_set_param() does except the flash write, so it returns in
 * microseconds. The value is live and callbacks have fired, but it is lost on a reset until
 * config_service_persist_pending() has run, so a caller that uses this must make sure that
 * is called from a context that may block (for example an idle or housekeeping task).
 *
 * @param[in] id     Parameter identifier.
 * @param[in] value  New value; its type is listed with config_param_id_t.
 * @return CONFIG_SERVICE_OK; _ERR_NOT_INIT; _ERR_NULL_ARG; _ERR_INVALID.
 * @note Threading: any task; takes the data mutex only, never blocks on flash.
 */
config_service_err_t config_service_set_param_deferred(config_param_id_t id, const void *value);

/**
 * @brief Write the configuration to flash if a deferred change is waiting.
 *
 * Does nothing and returns CONFIG_SERVICE_OK when nothing is pending. On a ConfigStore failure
 * the change stays pending so the next call retries.
 *
 * @return CONFIG_SERVICE_OK; CONFIG_SERVICE_ERR_NOT_INIT; CONFIG_SERVICE_ERR_PERSIST.
 * @note Threading: task context that may block for the flash write (about 0.8 s).
 */
config_service_err_t config_service_persist_pending(void);

/**
 * @brief Report whether a deferred change has not been written to flash yet.
 * @return true if config_service_persist_pending() has work to do.
 * @note Threading: any task.
 */
bool config_service_has_pending(void);

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
