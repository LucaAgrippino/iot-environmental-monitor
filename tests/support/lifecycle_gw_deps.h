/**
 * @file lifecycle_gw_deps.h
 * @brief Gateway-only dependency stubs for LifecycleController GW unit tests.
 *
 * Provides vtable definitions for GW-specific interfaces consumed by
 * lifecycle_controller.c under BOARD_GATEWAY:
 *   ICloudPublisher, IModbusPoller, IUpdateService, ITimeService,
 *   IFirmwareStore, IResetDriver.
 *
 * IHealthAdmin is defined in health_monitor_stub.h — include that header
 * (via lifecycle_controller.h) instead of repeating it here.
 *
 * Basename: lifecycle_gw_deps — no matching .c; Ceedling will not auto-link.
 */

#ifndef LIFECYCLE_GW_DEPS_H
#define LIFECYCLE_GW_DEPS_H

#include <stdint.h>
#include <stdbool.h>

/* ===================================================================== */
/* Generic error type shared by GW stubs                                 */
/* ===================================================================== */

typedef enum
{
    GW_SVC_ERR_OK       = 0,
    GW_SVC_ERR_NOT_INIT = 1,
    GW_SVC_ERR_NULL_ARG = 2,
    GW_SVC_ERR_FAIL     = 3,
} gw_svc_err_t;

/* ===================================================================== */
/* ICloudPublisher                                                        */
/* ===================================================================== */

typedef struct
{
    gw_svc_err_t (*init)(void);
    gw_svc_err_t (*flush)(void);
    bool         (*is_ready)(void);
    gw_svc_err_t (*report_rollback_result)(void);
} icloud_publisher_t;

/* ===================================================================== */
/* IModbusPoller                                                          */
/* ===================================================================== */

typedef struct
{
    gw_svc_err_t (*init)(void);
    bool         (*is_ready)(void);
} imodbus_poller_t;

/* ===================================================================== */
/* IUpdateService                                                         */
/* ===================================================================== */

typedef struct
{
    gw_svc_err_t (*init)(void);
    gw_svc_err_t (*start)(uint32_t image_size);
    gw_svc_err_t (*resume_self_checking)(void);
    gw_svc_err_t (*resume_rollback)(void);
    gw_svc_err_t (*resume_after_rollback)(void);
} iupdate_service_t;

/* ===================================================================== */
/* ITimeService                                                           */
/* ===================================================================== */

typedef struct
{
    gw_svc_err_t (*init)(void);
} itime_service_t;

/* ===================================================================== */
/* IFirmwareStore                                                         */
/* ===================================================================== */

typedef struct
{
    gw_svc_err_t (*get_pending_flags)(bool *self_check_out, bool *rollback_out);
    gw_svc_err_t (*confirm_self_check)(void);
    gw_svc_err_t (*rollback)(void);
} ifirmware_store_t;

/* ===================================================================== */
/* IResetDriver                                                           */
/* ===================================================================== */

typedef struct
{
    void (*soft_reset)(void); /* never returns in production */
} ireset_driver_t;

#endif /* LIFECYCLE_GW_DEPS_H */
