/**
 * @file lifecycle_deps.h
 * @brief The interfaces LifecycleController consumes (Gateway).
 *
 * LifecycleController is the last layer of the application stack: it drives every other module's
 * start-up and so would otherwise include all their headers. It does not. Each collaborator is
 * reached through the small vtable declared here, which holds only the operations the controller
 * actually calls (P2, P3).
 *
 * Why the types are not the owners' own (LC-D1): several of the modules the LLD names do not exist
 * yet (UpdateService, FirmwareStore, ResetDriver), and those that do expose handle-based or
 * differently shaped APIs than the LLD assumed (ConfigService has no snapshot / commit,
 * ConsoleService has no init_finalise, CloudPublisher has no flush / is_ready). Defining the
 * contract here, with a
 * @c ctx pointer in the same idiom as iconfig_manager_t, lets the controller be built and tested
 * now; a thin adapter per module binds the real implementation when it is wired in (LC-O6).
 *
 * @see docs/lld/application/lifecycle-controller-lld.md §1, §4.3
 */

#ifndef LIFECYCLE_DEPS_H
#define LIFECYCLE_DEPS_H

#include <stdbool.h>
#include <stdint.h>

#include "ilifecycle.h"

/** @brief Result of every collaborator call. */
typedef enum
{
    LC_DEP_OK = 0,   /**< The operation succeeded.   */
    LC_DEP_FAIL = 1, /**< The operation failed.      */
} lc_dep_status_t;

/* ======================================================================= */
/* Configuration                                                            */
/* ======================================================================= */

/** @brief IConfigStore — what Init needs from the persistent store. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*check_integrity)(void *ctx);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*load)(void *ctx, void *buf, uint32_t *len_out, uint32_t max_len);
} lc_config_store_if_t;

/** @brief IConfigManager — loading, and the EditingConfig snapshot workflow. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*apply_loaded)(void *ctx, const void *buf, uint32_t len);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*snapshot)(void *ctx, void *buf, uint32_t *len_out, uint32_t max_len);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*restore_snapshot)(void *ctx, const void *buf, uint32_t len);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*commit)(void *ctx);
} lc_config_manager_if_t;

/* ======================================================================= */
/* Acquisition                                                              */
/* ======================================================================= */

/** @brief ISensorService. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*init)(void *ctx);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    bool (*is_ready)(void *ctx);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*reconfigure)(void *ctx);
} lc_sensors_if_t;

/** @brief IAlarmService. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*init)(void *ctx);
} lc_alarms_if_t;

/* ======================================================================= */
/* Middleware and services                                                  */
/* ======================================================================= */

/** @brief IConsoleService. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*init_finalise)(void *ctx);
} lc_console_if_t;

/** @brief ICloudPublisher. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*init)(void *ctx);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*flush)(void *ctx);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    bool (*is_ready)(void *ctx);
} lc_cloud_if_t;

/** @brief IModbusPoller. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*init)(void *ctx);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    bool (*is_ready)(void *ctx);
} lc_modbus_poller_if_t;

/** @brief ITimeService. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*init)(void *ctx);
} lc_time_service_if_t;

/** @brief IUpdateService. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*init)(void *ctx);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*start)(void *ctx, uint32_t job);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*resume_self_checking)(void *ctx);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*resume_rollback)(void *ctx);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*resume_after_rollback)(void *ctx);
} lc_update_service_if_t;

/** @brief IFirmwareStore. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*get_pending_flags)(void *ctx, bool *self_check_out, bool *rollback_out);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*confirm_self_check)(void *ctx);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*rollback)(void *ctx);
} lc_firmware_store_if_t;

/** @brief IResetDriver. @c soft_reset does not return on the target. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    void (*soft_reset)(void *ctx);
} lc_reset_driver_if_t;

/* ======================================================================= */
/* Health                                                                   */
/* ======================================================================= */

/** @brief IHealthReport — the write side LifecycleController uses. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*init)(void *ctx);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*push_fault)(void *ctx, uint32_t fault_code);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*update_lifecycle)(void *ctx, lifecycle_state_t state);
} lc_health_report_if_t;

/** @brief IHealthAdmin — the control side, LifecycleController only. */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*reset_metrics)(void *ctx);
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    lc_dep_status_t (*set_led_fault)(void *ctx);
} lc_health_admin_if_t;

/** @brief Watchdog kick (optional). The Gateway has no watchdog driver yet (LC-O8). */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;
    /* cppcheck-suppress unusedStructMember -- called via vtable by LifecycleTask */
    void (*kick)(void *ctx);
} lc_watchdog_if_t;

/* ======================================================================= */
/* Injection bundle                                                         */
/* ======================================================================= */

/**
 * @brief Everything lifecycle_controller_init() stores.
 *
 * Every member except @c watchdog must be non-NULL (and so must each function pointer the
 * controller calls). The struct is copied; the pointed-to vtables must outlive the controller.
 */
typedef struct
{
    const lc_config_store_if_t *config_store;
    const lc_config_manager_if_t *config_manager;
    const lc_sensors_if_t *sensors;
    const lc_alarms_if_t *alarms;
    const lc_console_if_t *console;
    const lc_health_report_if_t *health_report;
    const lc_health_admin_if_t *health_admin;
    const lc_cloud_if_t *cloud;
    const lc_modbus_poller_if_t *modbus_poller;
    const lc_update_service_if_t *update_service;
    const lc_time_service_if_t *time_service;
    const lc_firmware_store_if_t *firmware_store;
    const lc_reset_driver_if_t *reset_driver;
    const lc_watchdog_if_t *watchdog; /**< Optional; NULL = no watchdog to kick. */
} lifecycle_deps_t;

#endif /* LIFECYCLE_DEPS_H */
