/**
 * @file config_service.h
 * @brief ConfigService — application-layer configuration management.
 *
 * Implements IConfigProvider (read side, consumed by SensorService / AlarmService)
 * and IConfigManager (write side, consumed by LifecycleController / ConsoleService /
 * ModbusRegisterMap / CloudPublisher).
 *
 * Owns the validate → apply → persist pipeline for every parameter change
 * regardless of source (CLI, Modbus, MQTT).
 *
 * REQ traces: REQ-DM-000, REQ-DM-001, REQ-DM-002, REQ-DM-090;
 *             REQ-SA-000, REQ-SA-010, REQ-SA-020, REQ-SA-050;
 *             REQ-LI-030–LI-130; REQ-MB-100
 *
 * @see docs/lld/application/config-service.md
 */

#ifndef CONFIG_SERVICE_H
#define CONFIG_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

#include "config_service/config_params.h"

#ifndef TEST
#include "middleware/config_store/config_store.h"
#else
#include "config_store_stub.h"
#endif /* TEST */

/* ========================================================================= */
/* Constants                                                                 */
/* ========================================================================= */

#define CONFIG_SERVICE_MAX_CALLBACKS 4U /**< Max registered change callbacks.     */
#define CONFIG_SCHEMA_VERSION 1U        /**< Incremented when config_params_t changes (CS-O1). */

/* ========================================================================= */
/* Error codes                                                               */
/* ========================================================================= */

typedef enum
{
    CONFIG_SERVICE_ERR_OK = 0,
    CONFIG_SERVICE_ERR_NOT_INIT = 1,
    CONFIG_SERVICE_ERR_NULL_ARG = 2,
    CONFIG_SERVICE_ERR_INVALID = 3, /**< Validation failed (REQ-DM-001). */
    CONFIG_SERVICE_ERR_PERSIST = 4, /**< ConfigStore write failed.       */
} config_service_err_t;

/* ========================================================================= */
/* Parameter identifiers                                                     */
/* ========================================================================= */

typedef enum
{
    CONFIG_PARAM_POLL_INTERVAL = 0,
    CONFIG_PARAM_FILTER_ALPHA = 1,
    CONFIG_PARAM_TEMP_RANGE_MIN = 2,
    CONFIG_PARAM_TEMP_RANGE_MAX = 3,
    CONFIG_PARAM_HUMIDITY_RANGE_MIN = 4,
    CONFIG_PARAM_HUMIDITY_RANGE_MAX = 5,
    CONFIG_PARAM_PRESSURE_RANGE_MIN = 6,
    CONFIG_PARAM_PRESSURE_RANGE_MAX = 7,
    CONFIG_PARAM_TEMP_ALARM_HIGH = 8,
    CONFIG_PARAM_TEMP_ALARM_LOW = 9,
    CONFIG_PARAM_TEMP_HYSTERESIS = 10,
    CONFIG_PARAM_HUMIDITY_ALARM_HIGH = 11,
    CONFIG_PARAM_HUMIDITY_ALARM_LOW = 12,
    CONFIG_PARAM_HUMIDITY_HYSTERESIS = 13,
    CONFIG_PARAM_PRESSURE_ALARM_HIGH = 14,
    CONFIG_PARAM_PRESSURE_ALARM_LOW = 15,
    CONFIG_PARAM_PRESSURE_HYSTERESIS = 16,
    CONFIG_PARAM_MODBUS_SLAVE_ADDR = 17,
    CONFIG_PARAM_MODBUS_POLL_PERIOD = 18,
#if defined(BOARD_GATEWAY)
    CONFIG_PARAM_MQTT_BROKER = 19,
    CONFIG_PARAM_MQTT_PORT = 20,
    CONFIG_PARAM_TELEMETRY_INTERVAL = 21,
    CONFIG_PARAM_HEALTH_INTERVAL = 22,
    CONFIG_PARAM_NTP_SERVER_COUNT = 23,
#endif /* BOARD_GATEWAY */
    CONFIG_PARAM_COUNT,
} config_param_id_t;

/* ========================================================================= */
/* Change callback                                                           */
/* ========================================================================= */

/**
 * Callback type fired by config_service_set_param() after a successful write.
 * Called outside the mutex — callback may safely call config_service_get_params().
 */
typedef void (*config_change_cb_t)(config_param_id_t param_id);

/* ========================================================================= */
/* Serialisation blob (§6)                                                   */
/* ========================================================================= */

typedef struct __attribute__((packed))
{
    /* cppcheck-suppress unusedStructMember -- read in config_service_apply_loaded() */
    uint32_t schema_version; /**< Must equal CONFIG_SCHEMA_VERSION (CS-O1). */
    /* cppcheck-suppress unusedStructMember -- copied to/from s_cs.params in persist/load */
    config_params_t params;
} config_blob_t;

/* ========================================================================= */
/* Public API — IConfigProvider (read side)                                 */
/* ========================================================================= */

/**
 * @brief  Get a const pointer to the live config parameters.
 *
 * Returns a pointer to the internal config_params_t.  The pointer is valid
 * for the system lifetime; field values may change on a commit.  Callers
 * must not cache values across yield points when they depend on the latest
 * committed value.
 *
 * Thread-safe for 32-bit scalar fields — reads are atomic on Cortex-M4
 * (natural alignment, 32-bit bus).  String fields require a mutex; see CS-O2.
 *
 * @return Pointer to live config_params_t; NULL if not initialised.
 */
const config_params_t *config_service_get_params(void);

/* ========================================================================= */
/* Public API — IConfigManager (write side)                                 */
/* ========================================================================= */

/**
 * @brief  Initialise ConfigService.
 *
 * Applies compile-time defaults, registers the ConfigStore handle, and
 * creates the internal mutex.  Always leaves ConfigService fully initialised
 * so config_service_get_params() returns valid default data immediately
 * (REQ-SA-010, SA-020, SA-050).
 *
 * config_service_apply_loaded() must be called separately by
 * LifecycleController once the stored blob has been read from ConfigStore.
 *
 * @param  store  IConfigStore handle; must not be NULL.
 * @return CONFIG_SERVICE_ERR_OK on success; CONFIG_SERVICE_ERR_NULL_ARG if
 *         store is NULL.
 * @note   Threading: task-context only, non-blocking.  Call before scheduler.
 */
config_service_err_t config_service_init(const iconfig_store_t *store);

/**
 * @brief  Apply stored config blob loaded by LifecycleController.
 *
 * Checks schema_version; on mismatch applies defaults (CS-O1).  On match,
 * validates each field; applies the compile-time default for any field that
 * fails basic range validation.
 *
 * @param  blob  Raw bytes from config_store_load().
 * @param  len   Blob byte count; must equal sizeof(config_blob_t).
 * @return CONFIG_SERVICE_ERR_OK on success; CONFIG_SERVICE_ERR_INVALID if
 *         len is wrong; CONFIG_SERVICE_ERR_NULL_ARG if blob is NULL.
 * @note   Threading: task-context only, may block (acquires mutex).
 */
config_service_err_t config_service_apply_loaded(const void *blob, uint32_t len);

/**
 * @brief  Validate and apply a single parameter change.
 *
 * On validation failure: returns CONFIG_SERVICE_ERR_INVALID; config unchanged.
 * On success: applies to in-memory config; persists via ConfigStore
 * (REQ-DM-090).  On ConfigStore failure: logs warning; returns
 * CONFIG_SERVICE_ERR_PERSIST; in-memory config is already updated.
 *
 * Thread-safe.  Acquires mutex for the duration of the operation.
 *
 * @param  id     Parameter identifier.
 * @param  value  Pointer to new value; type must match the param's declared type.
 * @return CONFIG_SERVICE_ERR_OK on success; non-zero on failure.
 */
config_service_err_t config_service_set_param(config_param_id_t id, const void *value);

/**
 * @brief  Validate a parameter without applying it.
 *
 * Used by ConsoleService to give immediate feedback before the Field
 * Technician confirms the change.  Does not modify state.
 *
 * @param  id     Parameter identifier.
 * @param  value  Pointer to value to validate.
 * @return CONFIG_SERVICE_ERR_OK if valid; CONFIG_SERVICE_ERR_INVALID otherwise.
 * @note   Threading: task-context only, non-blocking.  Not ISR-safe.
 */
config_service_err_t config_service_validate_param(config_param_id_t id, const void *value);

/**
 * @brief  Save a snapshot of the current config for rollback.
 *
 * Called by LifecycleController on entering EditingConfig.  Stores a copy of
 * config_params_t in the snapshot slot.
 *
 * @return CONFIG_SERVICE_ERR_OK on success; CONFIG_SERVICE_ERR_NOT_INIT if not
 *         initialised.
 * @note   Threading: task-context only, non-blocking.  Not ISR-safe.
 */
config_service_err_t config_service_snapshot(void);

/**
 * @brief  Restore config from the last snapshot.
 *
 * Called by LifecycleController on EditingConfig cancel or timeout.  Overwrites
 * in-memory config with snapshot; persists the restored config via ConfigStore.
 *
 * @return CONFIG_SERVICE_ERR_OK on success; CONFIG_SERVICE_ERR_INVALID if no
 *         snapshot is available; CONFIG_SERVICE_ERR_PERSIST on ConfigStore fail.
 * @note   Threading: task-context only, non-blocking.  Not ISR-safe.
 */
config_service_err_t config_service_restore_snapshot(void);

/**
 * @brief  Persist the current in-memory config to ConfigStore.
 *
 * Explicit flush — used by LifecycleController in the Restarting state to
 * ensure config is persisted before a soft reset.
 *
 * @return CONFIG_SERVICE_ERR_OK on success; CONFIG_SERVICE_ERR_PERSIST on failure.
 * @note   Threading: task-context only, may block.  Not ISR-safe.
 */
config_service_err_t config_service_flush(void);

/**
 * @brief  Register a parameter-change notification callback.
 *
 * Callbacks are invoked from config_service_set_param() after a successful
 * write, outside the mutex, to prevent deadlock when the callback itself
 * calls config_service_get_params().
 *
 * @param  cb  Callback; must remain valid for the system lifetime.
 * @return CONFIG_SERVICE_ERR_OK on success; CONFIG_SERVICE_ERR_INVALID if
 *         the table is full (> CONFIG_SERVICE_MAX_CALLBACKS).
 */
config_service_err_t config_service_register_change_callback(config_change_cb_t cb);

/* ========================================================================= */
/* Singleton vtables — IConfigProvider / IConfigManager (LLD-D10 pattern)  */
/* ========================================================================= */

typedef struct
{
    /* cppcheck-suppress unusedStructMember -- called via vtable by SensorService, AlarmService */
    const config_params_t *(*get_params)(void);
} iconfig_provider_t;

typedef struct
{
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleController */
    config_service_err_t (*init)(const iconfig_store_t *store);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleController */
    config_service_err_t (*apply_loaded)(const void *blob, uint32_t len);
    /* cppcheck-suppress unusedStructMember -- called via vtable by ConsoleService,
     * ModbusRegisterMap */
    config_service_err_t (*set_param)(config_param_id_t id, const void *value);
    /* cppcheck-suppress unusedStructMember -- called via vtable by ConsoleService */
    config_service_err_t (*validate_param)(config_param_id_t id, const void *value);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleController */
    config_service_err_t (*snapshot)(void);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleController */
    config_service_err_t (*restore_snapshot)(void);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleController */
    config_service_err_t (*flush)(void);
    /* cppcheck-suppress unusedStructMember -- called via vtable by ModbusSlave, ModbusPoller,
     * CloudPublisher */
    config_service_err_t (*register_change_callback)(config_change_cb_t cb);
} iconfig_manager_t;

/** Singleton — read interface (SensorService, AlarmService, LcdUi). */
extern const iconfig_provider_t *const config_provider;

/** Singleton — write interface (LifecycleController, ConsoleService, etc.). */
extern const iconfig_manager_t *const config_manager;

/* ========================================================================= */
/* Test visibility macro                                                     */
/* ========================================================================= */

#ifdef CONFIG_SERVICE_TEST_VISIBLE
#undef CONFIG_SERVICE_TEST_VISIBLE
#endif

#ifdef TEST
#define CONFIG_SERVICE_TEST_VISIBLE
#else
#define CONFIG_SERVICE_TEST_VISIBLE static
#endif

/* ========================================================================= */
/* Test-only hooks                                                           */
/* ========================================================================= */

#ifdef TEST
/**
 * @brief Reset module state to post-BSS values (call from setUp()).
 */
void config_service_reset_for_test(void);
#endif /* TEST */

#endif /* CONFIG_SERVICE_H */
