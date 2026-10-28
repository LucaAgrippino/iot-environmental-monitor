/**
 * @file iupdate_control.h
 * @brief IUpdateControl — update control by LifecycleController (LifecycleTask).
 *
 * Vtable only. None of these makes a FirmwareStore call in the caller's context.
 *
 * @see docs/lld/application/update-service-lld.md §7.3
 */

#ifndef IUPDATE_CONTROL_H
#define IUPDATE_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "update_service_types.h"

typedef struct
{
    /** True once update_service_create() has succeeded. */
    bool (*is_created)(const update_service_t *self);

    /** LifecycleController agreed to the update: begin downloading job @p job. */
    update_service_err_t (*start)(update_service_t *self, uint32_t job);

    /** Boot state is TRIAL_RUNNING: run the self-check. */
    update_service_err_t (*resume_self_checking)(update_service_t *self);

    /** The self-check failed: enter ROLLING_BACK. Synchronous, no flash access. */
    update_service_err_t (*resume_rollback)(update_service_t *self);

    /** Boot state is ROLLED_BACK: report it and acknowledge. */
    update_service_err_t (*resume_after_rollback)(update_service_t *self);
} iupdate_control_t;

#endif /* IUPDATE_CONTROL_H */
