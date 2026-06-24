/**
 * @file ihealth_admin.h
 * @brief IHealthAdmin — control-side vtable for HealthMonitor.
 *
 * Consumed only by LifecycleController (LLD-D15). Provides a minimal header
 * so lifecycle_controller.h can include just the admin interface without
 * pulling in the full health_monitor.h.
 *
 * @see health_monitor.h for the full API.
 * @see docs/lld/application/health-monitor.md
 */

#ifndef IHEALTH_ADMIN_H
#define IHEALTH_ADMIN_H

#include <stdint.h>

/* ======================================================================= */
/* Error codes                                                             */
/* ======================================================================= */

typedef enum
{
    HEALTH_ADMIN_ERR_OK = 0,
    HEALTH_ADMIN_ERR_NOT_INIT = 1,
} health_admin_err_t;

/* ======================================================================= */
/* IHealthAdmin vtable — control side                                      */
/* ======================================================================= */

typedef struct ihealth_admin_s
{
    health_admin_err_t (*reset_metrics)(void);
} ihealth_admin_t;

/** Singleton pointer — control side. Consumed only by LifecycleController. */
extern const ihealth_admin_t *const health_admin;

#endif /* IHEALTH_ADMIN_H */
