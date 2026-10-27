/**
 * @file ilifecycle.h
 * @brief ILifecycle — LifecycleController interface types and vtable (Gateway).
 *
 * Only the vtable and its supporting enums, so a consumer that needs the lifecycle state or the
 * event types can include this header without pulling in the controller's dependency list.
 *
 * The enumerators and include guards match the placeholder blocks in health_monitor.h, so the two
 * headers can be included together.
 *
 * @see docs/lld/application/lifecycle-controller-lld.md §4.2, §5
 */

#ifndef ILIFECYCLE_H
#define ILIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

/* ======================================================================= */
/* Data types                                                               */
/* ======================================================================= */

/** @brief Lifecycle state machine states (§5.1). */
#ifndef LIFECYCLE_STATE_DEFINED
#define LIFECYCLE_STATE_DEFINED
typedef enum
{
    LIFECYCLE_STATE_INIT = 0,
    LIFECYCLE_STATE_OPERATIONAL = 1,
    LIFECYCLE_STATE_EDITING_CONFIG = 2,
    LIFECYCLE_STATE_RESTARTING = 3,
    LIFECYCLE_STATE_UPDATING_FW = 4,
    LIFECYCLE_STATE_FAULTED = 5,
} lifecycle_state_t;
#endif /* LIFECYCLE_STATE_DEFINED */

/** @brief Cause of the most recent hardware reset (§5.2). */
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

/** @brief Error codes (§5.3). */
typedef enum
{
    LIFECYCLE_ERR_OK = 0,
    LIFECYCLE_ERR_NULL_ARG = 1,
    LIFECYCLE_ERR_NOT_INIT = 2,
    LIFECYCLE_ERR_QUEUE_FULL = 3,
    LIFECYCLE_ERR_UNKNOWN_CMD = 4,
    LIFECYCLE_ERR_BAD_STATE = 5,
} lifecycle_err_t;

/** @brief Runtime event types (§5.4). */
typedef enum
{
    LC_EVENT_CONFIG_EDIT_ENTER = 0,
    LC_EVENT_CONFIG_EDIT_APPLY = 1,
    LC_EVENT_CONFIG_EDIT_CANCEL = 2,
    LC_EVENT_CONFIG_EDIT_TIMEOUT = 3,
    LC_EVENT_RESTART_REQUESTED = 4,
    LC_EVENT_RESTART_CONFIRMED = 5,
    LC_EVENT_RESTART_TIMEOUT = 6,
    LC_EVENT_OTA_REQUESTED = 7,
    LC_EVENT_SELF_CHECK_PASS = 8,
    LC_EVENT_SELF_CHECK_FAIL = 9,
    LC_EVENT_UNRECOVERABLE_FAULT = 10,
    LC_EVENT_OTA_RESET_REQUESTED = 11, /**< UpdateService: image activated, reset (LCC-1). */
    LC_EVENT_OTA_FINISHED = 12,        /**< UpdateService: update ended, leave UPDATING_FW. */
} lifecycle_event_type_t;

/** @brief Event payload (§5.4). */
typedef struct
{
    lifecycle_event_type_t type;
    uint32_t param; /**< Fault code, OTA image size, ... */
} lifecycle_event_t;

/** @brief Remote commands dispatched through handle_remote_command (§5.5). */
#ifndef LC_REMOTE_CMD_DEFINED
#define LC_REMOTE_CMD_DEFINED
typedef enum
{
    LC_REMOTE_CMD_SOFT_RESTART = 0,  /**< Modbus 0x0202; two-step (REQ-DM-020).  */
    LC_REMOTE_CMD_RESET_METRICS = 1, /**< Modbus 0x0201; direct dispatch.        */
} lifecycle_remote_cmd_t;
#endif /* LC_REMOTE_CMD_DEFINED */

/**
 * @brief Fault codes carried in lifecycle_event_t.param with LC_EVENT_UNRECOVERABLE_FAULT.
 *
 * One code per Init failure, so the cause survives in the log and in the health snapshot.
 * Codes from other tasks (UpdateService, ...) use their own values above LC_FAULT_EXTERNAL_BASE.
 */
typedef enum
{
    LC_FAULT_INTEGRITY = 1U,     /**< ConfigStore integrity check failed.        */
    LC_FAULT_CONFIG_LOAD = 2U,   /**< ConfigStore load failed.                   */
    LC_FAULT_CONFIG_APPLY = 3U,  /**< ConfigService rejected the loaded blob.    */
    LC_FAULT_SENSORS_INIT = 4U,  /**< SensorService init failed.                 */
    LC_FAULT_ALARMS_INIT = 5U,   /**< AlarmService init failed.                  */
    LC_FAULT_POLLER_INIT = 6U,   /**< ModbusPoller init failed.                  */
    LC_FAULT_CLOUD_INIT = 7U,    /**< CloudPublisher init failed.                */
    LC_FAULT_TIME_INIT = 8U,     /**< TimeService init failed.                   */
    LC_FAULT_UPDATE_INIT = 9U,   /**< UpdateService init failed.                 */
    LC_FAULT_CONSOLE_INIT = 10U, /**< ConsoleService init_finalise failed.       */
    LC_FAULT_HEALTH_INIT = 11U,  /**< HealthMonitor init failed.                 */
    LC_FAULT_SELF_CHECK = 12U,   /**< A SelfChecking readiness probe failed.     */
    LC_FAULT_OTA_START = 13U,    /**< UpdateService refused to start an update.  */
    LC_FAULT_EXTERNAL_BASE = 0x100U,
    LC_FAULT_INIT_TIMEOUT = 0xFFF0U, /**< Init exceeded its time budget (LC-O2).   */
} lifecycle_fault_t;

/* ======================================================================= */
/* Provided vtable — ILifecycle (§4.2)                                     */
/* ======================================================================= */

/** @brief ILifecycle vtable. */
typedef struct
{
    /**
     * @brief Current lifecycle state. Read-only, any task.
     */
    /* cppcheck-suppress unusedStructMember -- called via vtable by HealthMonitor, Console, ... */
    lifecycle_state_t (*get_state)(void);

    /**
     * @brief Reset cause detected once at boot.
     */
    /* cppcheck-suppress unusedStructMember -- called via vtable by the consumers */
    lifecycle_reset_cause_t (*get_reset_cause)(void);

    /**
     * @brief Queue an event for LifecycleTask. Non-blocking, task context only.
     *
     * @return true if queued; false if the queue is full, the controller is not initialised, or
     *         the event is an OTA request while an update is already running (REQ-DM-054).
     */
    /* cppcheck-suppress unusedStructMember -- called via vtable by the producers */
    bool (*post_event)(lifecycle_event_t event);

    /**
     * @brief Dispatch a remote command that changes system state (LLD-D12, LLD-D15).
     *
     * @return LIFECYCLE_ERR_OK; LIFECYCLE_ERR_NOT_INIT; LIFECYCLE_ERR_QUEUE_FULL;
     *         LIFECYCLE_ERR_UNKNOWN_CMD.
     */
    /* cppcheck-suppress unusedStructMember -- called via vtable by ModbusPoller / CloudPublisher */
    lifecycle_err_t (*handle_remote_command)(lifecycle_remote_cmd_t cmd);
} ilifecycle_t;

/** @brief Singleton pointer to the LifecycleController vtable. */
extern const ilifecycle_t *const lifecycle_controller;

#endif /* ILIFECYCLE_H */
