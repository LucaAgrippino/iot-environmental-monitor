/**
 * @file ilifecycle.h
 * @brief ILifecycle — LifecycleController interface types and vtable.
 *
 * Provides only the vtable and supporting enums. Consumers that need
 * lifecycle state or event types include this header rather than the
 * full lifecycle_controller.h, avoiding include-graph cycles.
 *
 * @see lifecycle_controller.h for the full API (init, task body, start-gate).
 * @see docs/lld/application/lifecycle-controller.md
 */

#ifndef ILIFECYCLE_H
#define ILIFECYCLE_H

#include <stdint.h>
#include <stdbool.h>

/* ======================================================================= */
/* Data types                                                               */
/* ======================================================================= */

/** Lifecycle state machine states (§5.1). */
#ifndef LIFECYCLE_STATE_DEFINED
#define LIFECYCLE_STATE_DEFINED
typedef enum
{
    LIFECYCLE_STATE_INIT = 0,
    LIFECYCLE_STATE_OPERATIONAL = 1,
    LIFECYCLE_STATE_EDITING_CONFIG = 2,
    LIFECYCLE_STATE_RESTARTING = 3,  /* GW only */
    LIFECYCLE_STATE_UPDATING_FW = 4, /* GW only */
    LIFECYCLE_STATE_FAULTED = 5,
} lifecycle_state_t;
#endif /* LIFECYCLE_STATE_DEFINED */

/** Cause of the most recent hardware reset (§5.2). */
#ifndef LIFECYCLE_RESET_CAUSE_DEFINED
#define LIFECYCLE_RESET_CAUSE_DEFINED
typedef enum
{
    LIFECYCLE_RESET_POWER_ON = 0,
    LIFECYCLE_RESET_SOFT = 1,
    LIFECYCLE_RESET_WATCHDOG = 2,
    LIFECYCLE_RESET_UNKNOWN = 3,
} lifecycle_reset_cause_t;
#endif /* LIFECYCLE_RESET_CAUSE_DEFINED */

/** Error codes (§5.3). */
typedef enum
{
    LIFECYCLE_ERR_OK = 0,
    LIFECYCLE_ERR_NULL_ARG = 1,
    LIFECYCLE_ERR_NOT_INIT = 2,
    LIFECYCLE_ERR_QUEUE_FULL = 3,
    LIFECYCLE_ERR_UNKNOWN_CMD = 4,
    LIFECYCLE_ERR_BAD_STATE = 5,
} lifecycle_err_t;

/** Runtime event types (§5.4). */
typedef enum
{
    LC_EVENT_CONFIG_EDIT_ENTER = 0,
    LC_EVENT_CONFIG_EDIT_APPLY = 1,
    LC_EVENT_CONFIG_EDIT_CANCEL = 2,
    LC_EVENT_CONFIG_EDIT_TIMEOUT = 3,
    LC_EVENT_RESTART_REQUESTED = 4, /* GW */
    LC_EVENT_RESTART_CONFIRMED = 5, /* GW */
    LC_EVENT_RESTART_TIMEOUT = 6,   /* GW */
    LC_EVENT_OTA_REQUESTED = 7,     /* GW */
    LC_EVENT_SELF_CHECK_PASS = 8,   /* GW */
    LC_EVENT_SELF_CHECK_FAIL = 9,   /* GW */
    LC_EVENT_UNRECOVERABLE_FAULT = 10,
} lifecycle_event_type_t;

/** Event payload (§5.4). */
typedef struct
{
    lifecycle_event_type_t type;
    uint32_t param; /* fault code, OTA image size, etc. */
} lifecycle_event_t;

/** Remote commands dispatched through handle_remote_command (§5.5). */
typedef enum
{
    LC_REMOTE_CMD_SOFT_RESTART = 0,
    LC_REMOTE_CMD_RESET_METRICS = 1,
} lifecycle_remote_cmd_t;

/** Fault param sent with LC_EVENT_UNRECOVERABLE_FAULT on init timeout. */
#define LC_FAULT_INIT_TIMEOUT (0xFFF0U)

/* ======================================================================= */
/* Provided vtable — ILifecycle (§4.2)                                     */
/* ======================================================================= */

typedef struct
{
    lifecycle_state_t (*get_state)(void);
    lifecycle_reset_cause_t (*get_reset_cause)(void);
    bool (*post_event)(lifecycle_event_t event);
    lifecycle_err_t (*handle_remote_command)(lifecycle_remote_cmd_t cmd);
} ilifecycle_t;

extern const ilifecycle_t *const lifecycle_controller;

#endif /* ILIFECYCLE_H */
