/**
 * @file lifecycle_controller.c
 * @brief LifecycleController implementation (Gateway) — Init sequence, event routing, Faulted.
 *
 * All state is file-scope static; nothing is allocated after init (P5). Every write to s_state
 * happens inside LifecycleTask, so get_state() needs no lock.
 *
 * @see docs/lld/application/lifecycle-controller-lld.md
 */

#include "lifecycle_controller.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "stm32l475xx.h"
#include "timers.h"

#ifdef TEST
#define LOG_ERROR(m, f, ...) ((void) 0)
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

#define MODULE_TAG "LC"

/* ======================================================================= */
/* Internal configuration                                                   */
/* ======================================================================= */

/** Events held back during EditingConfig, replayed on return to Operational (§9). */
#define LC_DEFERRED_DEPTH (LIFECYCLE_QUEUE_DEPTH)

/* ======================================================================= */
/* File-scope state                                                         */
/* ======================================================================= */

static lifecycle_deps_t s_deps;

static volatile lifecycle_state_t s_state;
static lifecycle_reset_cause_t s_reset_cause;
static bool s_initialised;
static bool s_init_started;
static bool s_awaiting_self_check; /**< Init is parked in SelfChecking, waiting for the verdict. */
static bool s_restart_pending;
static bool s_pending_self_check;
static bool s_pending_rollback;

static StaticQueue_t s_queue_struct;
static uint8_t s_queue_storage[LIFECYCLE_QUEUE_DEPTH * sizeof(lifecycle_event_t)];
static QueueHandle_t s_event_queue;

static StaticTimer_t s_edit_timer_struct;
static TimerHandle_t s_edit_timer;
static StaticTimer_t s_init_timer_struct;
static TimerHandle_t s_init_timer;
static StaticTimer_t s_restart_timer_struct;
static TimerHandle_t s_restart_timer;

static StaticEventGroup_t s_start_gate_struct;
static EventGroupHandle_t s_start_gate;

static uint8_t s_cfg_snapshot[LIFECYCLE_CFG_SNAPSHOT_BYTES];
static uint32_t s_cfg_snapshot_len;

static lifecycle_event_t s_deferred[LC_DEFERRED_DEPTH];
static uint32_t s_deferred_count;

#ifdef TEST
static lifecycle_err_t s_last_result;
#endif /* TEST */

/* ======================================================================= */
/* Timer callbacks — run in the timer service task                          */
/* ======================================================================= */

static void post_from_timer(lifecycle_event_type_t type, uint32_t param)
{
    lifecycle_event_t ev = {.type = type, .param = param};
    (void) xQueueSend(s_event_queue, &ev, 0U);
}

static void edit_timer_cb(TimerHandle_t timer)
{
    (void) timer;
    post_from_timer(LC_EVENT_CONFIG_EDIT_TIMEOUT, 0U);
}

static void init_timer_cb(TimerHandle_t timer)
{
    (void) timer;
    post_from_timer(LC_EVENT_UNRECOVERABLE_FAULT, (uint32_t) LC_FAULT_INIT_TIMEOUT);
}

static void restart_timer_cb(TimerHandle_t timer)
{
    (void) timer;
    post_from_timer(LC_EVENT_RESTART_TIMEOUT, 0U);
}

/* ======================================================================= */
/* Reset-cause detection (§13)                                              */
/* ======================================================================= */

lifecycle_reset_cause_t lifecycle_detect_reset_cause(void)
{
    uint32_t csr = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF; /* clear the flags for the next boot */

    if ((csr & RCC_CSR_IWDGRSTF) != 0U)
    {
        return LIFECYCLE_RESET_WATCHDOG;
    }
    if ((csr & RCC_CSR_SFTRSTF) != 0U)
    {
        return LIFECYCLE_RESET_SOFT;
    }
    /* A power-up sets the brown-out flag together with the pin flag (LC-D5). */
    if ((csr & (RCC_CSR_PINRSTF | RCC_CSR_BORRSTF)) != 0U)
    {
        return LIFECYCLE_RESET_POWER_ON;
    }
    return LIFECYCLE_RESET_UNKNOWN;
}

/* ======================================================================= */
/* State helpers                                                            */
/* ======================================================================= */

static void kick_watchdog(void)
{
    if ((s_deps.watchdog != NULL) && (s_deps.watchdog->kick != NULL))
    {
        s_deps.watchdog->kick(s_deps.watchdog->ctx);
    }
}

/** Record a state change and tell HealthMonitor, which drives the status LED from it. */
static void set_state(lifecycle_state_t state)
{
    s_state = state;
    if (s_deps.health_report->update_lifecycle(s_deps.health_report->ctx, state) != LC_DEP_OK)
    {
        LOG_WARN(MODULE_TAG, "health update_lifecycle(%u) failed", (unsigned) state);
    }
}

static void stop_all_timers(void)
{
    (void) xTimerStop(s_edit_timer, 0U);
    (void) xTimerStop(s_init_timer, 0U);
    (void) xTimerStop(s_restart_timer, 0U);
}

/**
 * Write a deferred configuration change to flash if one is waiting (LC-D23). Remote and console
 * changes are applied in RAM with set_param_deferred() and are not written by the caller, so
 * this is where they reach flash. A failure is never a fault: the change stays pending and the
 * next call retries.
 */
static void persist_config(void)
{
    if (s_deps.config_manager->persist_pending(s_deps.config_manager->ctx) != LC_DEP_OK)
    {
        LOG_WARN(MODULE_TAG, "configuration persist failed, change stays pending");
    }
}

/** Every reset goes through here, so a pending configuration change is written first (LC-D23). */
static void reset_system(void)
{
    persist_config();
    s_deps.reset_driver->soft_reset(s_deps.reset_driver->ctx); /* does not return on target */
}

/** Enter Faulted, once. Only a hardware reset leaves it (§12). */
static void enter_faulted(uint32_t fault_code)
{
    if (s_state == LIFECYCLE_STATE_FAULTED)
    {
        return;
    }

    stop_all_timers();
    s_awaiting_self_check = false;
    LOG_ERROR(MODULE_TAG, "FAULTED (code=0x%04lx)", (unsigned long) fault_code);

    if (s_deps.health_report->push_fault(s_deps.health_report->ctx, fault_code) != LC_DEP_OK)
    {
        LOG_WARN(MODULE_TAG, "health push_fault failed");
    }
    persist_config(); /* before the LED latch: a pending change must not die with the fault */
    if (s_deps.health_admin->set_led_fault(s_deps.health_admin->ctx) != LC_DEP_OK)
    {
        LOG_WARN(MODULE_TAG, "health set_led_fault failed");
    }
    set_state(LIFECYCLE_STATE_FAULTED);
}

static void enter_operational(void)
{
    (void) xTimerStop(s_init_timer, 0U);
    s_awaiting_self_check = false;
    set_state(LIFECYCLE_STATE_OPERATIONAL);
    LOG_INFO(MODULE_TAG, "Operational");
    (void) xEventGroupSetBits(s_start_gate, LIFECYCLE_START_GATE_BIT);
}

/* ======================================================================= */
/* Init sequence — Machine 1, sub-states 1..5 (§7.2)                        */
/* ======================================================================= */

/** Fault the system if an Init step failed. Returns true when the step succeeded. */
static bool init_step_ok(lc_dep_status_t status, lifecycle_fault_t fault, const char *what)
{
    if (status == LC_DEP_OK)
    {
        return true;
    }
    LOG_ERROR(MODULE_TAG, "Init: %s failed", what);
    (void) what;
    enter_faulted((uint32_t) fault);
    return false;
}

/**
 * Between sub-states, look for a fault queued meanwhile (the Init timer, or another task).
 *
 * Any other event posted during Init is dropped: no task runs before the start gate opens, so
 * none is expected. The check runs at sub-state boundaries only, so it cannot interrupt a
 * collaborator call that never returns (LC-O2).
 */
static bool init_aborted(void)
{
    lifecycle_event_t ev = {.type = LC_EVENT_CONFIG_EDIT_ENTER, .param = 0U};

    if (xQueueReceive(s_event_queue, &ev, 0U) != pdTRUE)
    {
        return false;
    }
    if (ev.type == LC_EVENT_UNRECOVERABLE_FAULT)
    {
        enter_faulted(ev.param);
        return true;
    }
    LOG_WARN(MODULE_TAG, "event %u dropped during Init", (unsigned) ev.type);
    return false;
}

/** Sub-state 1: CheckingIntegrity. */
static bool init_checking_integrity(void)
{
    bool self_check = false;
    bool rollback = false;

    if (!init_step_ok(s_deps.config_store->check_integrity(s_deps.config_store->ctx),
                      LC_FAULT_INTEGRITY, "config integrity"))
    {
        return false;
    }

    if (s_deps.firmware_store->get_pending_flags(s_deps.firmware_store->ctx, &self_check,
                                                 &rollback) != LC_DEP_OK)
    {
        /* Treated as "nothing pending": an unreadable flag must not fault a healthy boot. */
        LOG_WARN(MODULE_TAG, "firmware flags unreadable; assuming none pending");
        self_check = false;
        rollback = false;
    }
    s_pending_self_check = self_check;
    s_pending_rollback = rollback;
    return true;
}

/** Sub-state 2: LoadingConfig. */
static bool init_loading_config(void)
{
    s_cfg_snapshot_len = 0U;
    if (!init_step_ok(s_deps.config_store->load(s_deps.config_store->ctx, s_cfg_snapshot,
                                                &s_cfg_snapshot_len, sizeof(s_cfg_snapshot)),
                      LC_FAULT_CONFIG_LOAD, "config load"))
    {
        return false;
    }
    return init_step_ok(s_deps.config_manager->apply_loaded(s_deps.config_manager->ctx,
                                                            s_cfg_snapshot, s_cfg_snapshot_len),
                        LC_FAULT_CONFIG_APPLY, "config apply");
}

/** Sub-state 3: BringingUpSensors. */
static bool init_bringing_up_sensors(void)
{
    return init_step_ok(s_deps.sensors->init(s_deps.sensors->ctx), LC_FAULT_SENSORS_INIT,
                        "sensors init") &&
           init_step_ok(s_deps.alarms->init(s_deps.alarms->ctx), LC_FAULT_ALARMS_INIT,
                        "alarms init");
}

/** Sub-state 4: StartingMiddleware. */
static bool init_starting_middleware(void)
{
    return init_step_ok(s_deps.modbus_poller->init(s_deps.modbus_poller->ctx), LC_FAULT_POLLER_INIT,
                        "modbus poller init") &&
           init_step_ok(s_deps.cloud->init(s_deps.cloud->ctx), LC_FAULT_CLOUD_INIT, "cloud init") &&
           init_step_ok(s_deps.time_service->init(s_deps.time_service->ctx), LC_FAULT_TIME_INIT,
                        "time service init") &&
           init_step_ok(s_deps.update_service->init(s_deps.update_service->ctx),
                        LC_FAULT_UPDATE_INIT, "update service init") &&
           init_step_ok(s_deps.console->init_finalise(s_deps.console->ctx), LC_FAULT_CONSOLE_INIT,
                        "console init_finalise") &&
           init_step_ok(s_deps.health_report->init(s_deps.health_report->ctx), LC_FAULT_HEALTH_INIT,
                        "health init");
}

/** Sub-state 5: SelfChecking (REQ-DM-040), including the two post-update boot paths (SD-00c). */
static void init_self_checking(void)
{
    if (s_pending_self_check)
    {
        if (!init_step_ok(s_deps.update_service->resume_self_checking(s_deps.update_service->ctx),
                          LC_FAULT_SELF_CHECK, "resume self-check"))
        {
            return;
        }
        /* The verdict arrives as an event. UpdateService owns that wait, so the Init budget no
         * longer applies. */
        (void) xTimerStop(s_init_timer, 0U);
        s_awaiting_self_check = true;
        LOG_INFO(MODULE_TAG, "SelfChecking: waiting for the post-update verdict");
        return;
    }

    if (s_pending_rollback)
    {
        /* UpdateService reports the rollback itself (US-D9); nothing is sent from here. */
        if (s_deps.update_service->resume_after_rollback(s_deps.update_service->ctx) != LC_DEP_OK)
        {
            LOG_WARN(MODULE_TAG, "resume_after_rollback failed");
        }
        enter_operational();
        return;
    }

    const bool sensors_ok = s_deps.sensors->is_ready(s_deps.sensors->ctx);
    const bool poller_ok = s_deps.modbus_poller->is_ready(s_deps.modbus_poller->ctx);
    const bool cloud_ok = s_deps.cloud->is_ready(s_deps.cloud->ctx);

    if (sensors_ok && poller_ok && cloud_ok)
    {
        enter_operational();
        return;
    }
    LOG_ERROR(MODULE_TAG, "SelfChecking: sensors=%d poller=%d cloud=%d", (int) sensors_ok,
              (int) poller_ok, (int) cloud_ok);
    enter_faulted((uint32_t) LC_FAULT_SELF_CHECK);
}

static void run_init_sequence(void)
{
    s_init_started = true;
    (void) xTimerStart(s_init_timer, 0U);

    if (!init_checking_integrity() || init_aborted())
    {
        return;
    }
    if (!init_loading_config() || init_aborted())
    {
        return;
    }
    if (!init_bringing_up_sensors() || init_aborted())
    {
        return;
    }
    if (!init_starting_middleware() || init_aborted())
    {
        return;
    }
    init_self_checking();
}

/* ======================================================================= */
/* Event handling                                                           */
/* ======================================================================= */

static void roll_back_and_reset(void)
{
    if (s_deps.update_service->resume_rollback(s_deps.update_service->ctx) != LC_DEP_OK)
    {
        LOG_WARN(MODULE_TAG, "resume_rollback failed");
    }
    if (s_deps.firmware_store->rollback(s_deps.firmware_store->ctx) != LC_DEP_OK)
    {
        LOG_ERROR(MODULE_TAG, "firmware rollback failed");
    }
    reset_system(); /* does not return on target */
}

/** The post-update verdict, in Init (SelfChecking) or in UpdatingFirmware (§7.2, §11). */
static void handle_self_check_event(const lifecycle_event_t *ev)
{
    switch (ev->type)
    {
    case LC_EVENT_SELF_CHECK_PASS:
        if (s_deps.firmware_store->confirm_self_check(s_deps.firmware_store->ctx) != LC_DEP_OK)
        {
            LOG_ERROR(MODULE_TAG, "confirm_self_check failed");
            enter_faulted((uint32_t) LC_FAULT_SELF_CHECK);
            break;
        }
        enter_operational();
        break;

    case LC_EVENT_SELF_CHECK_FAIL:
        roll_back_and_reset();
        break;

    case LC_EVENT_UNRECOVERABLE_FAULT:
        enter_faulted(ev->param);
        break;

    default:
        LOG_WARN(MODULE_TAG, "event %u ignored while waiting for the verdict", (unsigned) ev->type);
        break;
    }
}

static void handle_init_event(const lifecycle_event_t *ev)
{
    if (s_awaiting_self_check)
    {
        handle_self_check_event(ev);
        return;
    }
    if (ev->type == LC_EVENT_UNRECOVERABLE_FAULT)
    {
        enter_faulted(ev->param);
    }
}

static void defer_event(const lifecycle_event_t *ev)
{
    if (s_deferred_count >= LC_DEFERRED_DEPTH)
    {
        LOG_WARN(MODULE_TAG, "deferred queue full; event %u dropped", (unsigned) ev->type);
        return;
    }
    s_deferred[s_deferred_count] = *ev;
    s_deferred_count++;
}

/** Leave EditingConfig for Operational. The deferred events replay on the next loop pass. */
static void leave_editing(void)
{
    (void) xTimerStop(s_edit_timer, 0U);
    set_state(LIFECYCLE_STATE_OPERATIONAL);
}

static void restore_edit_snapshot(void)
{
    if (s_deps.config_manager->restore_snapshot(s_deps.config_manager->ctx, s_cfg_snapshot,
                                                s_cfg_snapshot_len) != LC_DEP_OK)
    {
        LOG_ERROR(MODULE_TAG, "restore_snapshot failed");
    }
}

static void handle_editing_event(const lifecycle_event_t *ev)
{
    switch (ev->type)
    {
    case LC_EVENT_CONFIG_EDIT_APPLY:
        if (s_deps.config_manager->commit(s_deps.config_manager->ctx) != LC_DEP_OK)
        {
            /* An edit that could not be saved must not stay live. */
            LOG_ERROR(MODULE_TAG, "commit failed; restoring the snapshot");
            restore_edit_snapshot();
        }
        else if (s_deps.sensors->reconfigure(s_deps.sensors->ctx) != LC_DEP_OK)
        {
            LOG_WARN(MODULE_TAG, "sensors reconfigure failed");
        }
        leave_editing();
        LOG_INFO(MODULE_TAG, "EditingConfig APPLY -> Operational");
        break;

    case LC_EVENT_CONFIG_EDIT_CANCEL:
    case LC_EVENT_CONFIG_EDIT_TIMEOUT:
        restore_edit_snapshot();
        leave_editing();
        LOG_INFO(MODULE_TAG, "EditingConfig CANCEL/TIMEOUT -> Operational");
        break;

    case LC_EVENT_RESTART_REQUESTED:
    case LC_EVENT_OTA_REQUESTED:
        defer_event(ev);
        break;

    case LC_EVENT_UNRECOVERABLE_FAULT:
        enter_faulted(ev->param);
        break;

    default:
        break;
    }
}

static void handle_operational_event(const lifecycle_event_t *ev)
{
    switch (ev->type)
    {
    case LC_EVENT_CONFIG_EDIT_ENTER:
        s_cfg_snapshot_len = 0U;
        if (s_deps.config_manager->snapshot(s_deps.config_manager->ctx, s_cfg_snapshot,
                                            &s_cfg_snapshot_len,
                                            sizeof(s_cfg_snapshot)) != LC_DEP_OK)
        {
            /* Without a snapshot a cancel could not roll back, so do not open the session. */
            LOG_ERROR(MODULE_TAG, "snapshot failed; EditingConfig refused");
            break;
        }
        (void) xTimerStart(s_edit_timer, 0U);
        set_state(LIFECYCLE_STATE_EDITING_CONFIG);
        LOG_INFO(MODULE_TAG, "-> EditingConfig");
        break;

    case LC_EVENT_RESTART_REQUESTED:
        s_restart_pending = true;
        if (s_deps.cloud->flush(s_deps.cloud->ctx) != LC_DEP_OK)
        {
            LOG_WARN(MODULE_TAG, "cloud flush before restart failed");
        }
        (void) xTimerStart(s_restart_timer, 0U);
        set_state(LIFECYCLE_STATE_RESTARTING);
        LOG_INFO(MODULE_TAG, "-> Restarting");
        break;

    case LC_EVENT_OTA_REQUESTED:
        if (s_deps.update_service->start(s_deps.update_service->ctx, ev->param) != LC_DEP_OK)
        {
            LOG_ERROR(MODULE_TAG, "update start refused");
            enter_faulted((uint32_t) LC_FAULT_OTA_START);
            break;
        }
        set_state(LIFECYCLE_STATE_UPDATING_FW);
        LOG_INFO(MODULE_TAG, "-> UpdatingFirmware");
        break;

    case LC_EVENT_UNRECOVERABLE_FAULT:
        enter_faulted(ev->param);
        break;

    default:
        LOG_WARN(MODULE_TAG, "Operational: event %u ignored", (unsigned) ev->type);
        break;
    }
}

static void handle_restarting_event(const lifecycle_event_t *ev)
{
    switch (ev->type)
    {
    case LC_EVENT_RESTART_CONFIRMED:
    case LC_EVENT_RESTART_REQUESTED: /* a second request is the confirmation (§10) */
        (void) xTimerStop(s_restart_timer, 0U);
        LOG_INFO(MODULE_TAG, "Restarting confirmed -> soft reset");
        reset_system(); /* does not return on target */
        break;

    case LC_EVENT_RESTART_TIMEOUT:
        s_restart_pending = false;
        set_state(LIFECYCLE_STATE_OPERATIONAL);
        LOG_INFO(MODULE_TAG, "Restarting timed out -> Operational");
        break;

    case LC_EVENT_UNRECOVERABLE_FAULT:
        enter_faulted(ev->param);
        break;

    default:
        break;
    }
}

/**
 * The update was activated: flush CloudPublisher, give the broker time to take the last
 * publishes, then reset (Machine 1 I18). This is the single canonical reset site of the OTA
 * flow. The state stays UpdatingFirmware: the reset ends it.
 */
static void ota_reset(void)
{
    LOG_INFO(MODULE_TAG, "OTA activated: flush, %u ms grace, reset",
             (unsigned) LC_OTA_RESET_GRACE_MS);

    if (s_deps.cloud->flush(s_deps.cloud->ctx) != LC_DEP_OK)
    {
        LOG_WARN(MODULE_TAG, "cloud flush before the OTA reset failed");
    }
    vTaskDelay(pdMS_TO_TICKS(LC_OTA_RESET_GRACE_MS));
    reset_system(); /* does not return on target */
}

/**
 * UpdatingFirmware: a second OTA request is refused (REQ-DM-054). OTA_FINISHED ends an update
 * that ended without a reset (Machine 1 T19); OTA_RESET_REQUESTED resets after activate (I18).
 */
static lifecycle_err_t handle_updating_event(const lifecycle_event_t *ev)
{
    switch (ev->type)
    {
    case LC_EVENT_OTA_REQUESTED:
        LOG_WARN(MODULE_TAG, "OTA request refused: an update is already running");
        return LIFECYCLE_ERR_BAD_STATE;

    case LC_EVENT_OTA_FINISHED:
        set_state(LIFECYCLE_STATE_OPERATIONAL);
        LOG_INFO(MODULE_TAG, "OTA finished -> Operational");
        break;

    case LC_EVENT_OTA_RESET_REQUESTED:
        ota_reset();
        break;

    default:
        handle_self_check_event(ev);
        break;
    }
    return LIFECYCLE_ERR_OK;
}

static lifecycle_err_t process_event(const lifecycle_event_t *ev)
{
    lifecycle_err_t result = LIFECYCLE_ERR_OK;

    /* The two UpdateService events mean something only in UpdatingFirmware (LCC-2). */
    if (((ev->type == LC_EVENT_OTA_RESET_REQUESTED) || (ev->type == LC_EVENT_OTA_FINISHED)) &&
        (s_state != LIFECYCLE_STATE_UPDATING_FW))
    {
        LOG_WARN(MODULE_TAG, "event %u ignored outside UpdatingFirmware", (unsigned) ev->type);
        return LIFECYCLE_ERR_OK;
    }

    switch (s_state)
    {
    case LIFECYCLE_STATE_INIT:
        handle_init_event(ev);
        break;

    case LIFECYCLE_STATE_OPERATIONAL:
        handle_operational_event(ev);
        break;

    case LIFECYCLE_STATE_EDITING_CONFIG:
        handle_editing_event(ev);
        break;

    case LIFECYCLE_STATE_RESTARTING:
        handle_restarting_event(ev);
        break;

    case LIFECYCLE_STATE_UPDATING_FW:
        result = handle_updating_event(ev);
        break;

    case LIFECYCLE_STATE_FAULTED:
    default:
        /* No event leaves Faulted (§12). */
        break;
    }

#ifdef TEST
    s_last_result = result;
#endif /* TEST */
    return result;
}

/** Take the oldest deferred event, if any. */
static bool pop_deferred(lifecycle_event_t *ev)
{
    if (s_deferred_count == 0U)
    {
        return false;
    }
    *ev = s_deferred[0];
    s_deferred_count--;
    (void) memmove(&s_deferred[0], &s_deferred[1], s_deferred_count * sizeof(s_deferred[0]));
    return true;
}

static void run_event_loop_once(void)
{
    lifecycle_event_t ev = {.type = LC_EVENT_CONFIG_EDIT_ENTER, .param = 0U};

    const bool replay = (s_state == LIFECYCLE_STATE_OPERATIONAL) && pop_deferred(&ev);

    if (replay ||
        (xQueueReceive(s_event_queue, &ev, pdMS_TO_TICKS(LIFECYCLE_QUEUE_WAIT_MS)) == pdTRUE))
    {
        (void) process_event(&ev);
    }
    if (s_state == LIFECYCLE_STATE_OPERATIONAL)
    {
        persist_config(); /* the idle tick: cheap when nothing is pending (LC-D23) */
    }
    kick_watchdog();
}

/* ======================================================================= */
/* Vtable implementations                                                   */
/* ======================================================================= */

static lifecycle_state_t vtbl_get_state(void)
{
    return s_state;
}

static lifecycle_reset_cause_t vtbl_get_reset_cause(void)
{
    return s_reset_cause;
}

static bool vtbl_post_event(lifecycle_event_t event)
{
    if (!s_initialised)
    {
        return false;
    }
    if ((event.type == LC_EVENT_OTA_REQUESTED) && (s_state == LIFECYCLE_STATE_UPDATING_FW))
    {
        LOG_WARN(MODULE_TAG, "OTA request refused: an update is already running");
        return false;
    }
    if (xQueueSend(s_event_queue, &event, 0U) != pdTRUE)
    {
        LOG_WARN(MODULE_TAG, "event queue full; event %u dropped", (unsigned) event.type);
        return false;
    }
    return true;
}

static lifecycle_err_t vtbl_handle_remote_command(lifecycle_remote_cmd_t cmd)
{
    if (!s_initialised)
    {
        return LIFECYCLE_ERR_NOT_INIT;
    }

    switch (cmd)
    {
    case LC_REMOTE_CMD_RESET_METRICS:
        /* Direct dispatch: no lifecycle side effect, and it must not wait on the queue. */
        if (s_deps.health_admin->reset_metrics(s_deps.health_admin->ctx) != LC_DEP_OK)
        {
            LOG_WARN(MODULE_TAG, "reset_metrics failed");
        }
        return LIFECYCLE_ERR_OK;

    case LC_REMOTE_CMD_SOFT_RESTART:
    {
        const lifecycle_event_t ev = {.type = LC_EVENT_RESTART_REQUESTED, .param = 0U};
        if (xQueueSend(s_event_queue, &ev, 0U) != pdTRUE)
        {
            LOG_WARN(MODULE_TAG, "SOFT_RESTART: event queue full");
            return LIFECYCLE_ERR_QUEUE_FULL;
        }
        return LIFECYCLE_ERR_OK;
    }

    default:
        LOG_WARN(MODULE_TAG, "unknown remote command %u", (unsigned) cmd);
        return LIFECYCLE_ERR_UNKNOWN_CMD;
    }
}

static const ilifecycle_t s_vtable = {
    .get_state = vtbl_get_state,
    .get_reset_cause = vtbl_get_reset_cause,
    .post_event = vtbl_post_event,
    .handle_remote_command = vtbl_handle_remote_command,
};

const ilifecycle_t *const lifecycle_controller = &s_vtable;

/* ======================================================================= */
/* Initialisation                                                           */
/* ======================================================================= */

static bool deps_storage_valid(const lifecycle_deps_t *d)
{
    return (d->config_store->check_integrity != NULL) && (d->config_store->load != NULL) &&
           (d->config_manager->apply_loaded != NULL) && (d->config_manager->snapshot != NULL) &&
           (d->config_manager->restore_snapshot != NULL) && (d->config_manager->commit != NULL) &&
           (d->config_manager->persist_pending != NULL) &&
           (d->firmware_store->get_pending_flags != NULL) &&
           (d->firmware_store->confirm_self_check != NULL) &&
           (d->firmware_store->rollback != NULL) && (d->reset_driver->soft_reset != NULL);
}

static bool deps_services_valid(const lifecycle_deps_t *d)
{
    return (d->sensors->init != NULL) && (d->sensors->is_ready != NULL) &&
           (d->sensors->reconfigure != NULL) && (d->alarms->init != NULL) &&
           (d->console->init_finalise != NULL) && (d->cloud->init != NULL) &&
           (d->cloud->flush != NULL) && (d->cloud->is_ready != NULL) &&
           (d->modbus_poller->init != NULL) && (d->modbus_poller->is_ready != NULL) &&
           (d->time_service->init != NULL) && (d->update_service->init != NULL) &&
           (d->update_service->start != NULL) &&
           (d->update_service->resume_self_checking != NULL) &&
           (d->update_service->resume_rollback != NULL) &&
           (d->update_service->resume_after_rollback != NULL);
}

static bool deps_valid(const lifecycle_deps_t *d)
{
    if ((d == NULL) || (d->config_store == NULL) || (d->config_manager == NULL) ||
        (d->sensors == NULL) || (d->alarms == NULL) || (d->console == NULL) ||
        (d->health_report == NULL) || (d->health_admin == NULL) || (d->cloud == NULL) ||
        (d->modbus_poller == NULL) || (d->update_service == NULL) || (d->time_service == NULL) ||
        (d->firmware_store == NULL) || (d->reset_driver == NULL))
    {
        return false;
    }
    return deps_storage_valid(d) && deps_services_valid(d) && (d->health_report->init != NULL) &&
           (d->health_report->push_fault != NULL) && (d->health_report->update_lifecycle != NULL) &&
           (d->health_admin->reset_metrics != NULL) && (d->health_admin->set_led_fault != NULL);
}

lifecycle_err_t lifecycle_controller_init(lifecycle_reset_cause_t reset_cause,
                                          const lifecycle_deps_t *deps)
{
    if (!deps_valid(deps))
    {
        return LIFECYCLE_ERR_NULL_ARG;
    }

    s_deps = *deps;
    s_reset_cause = reset_cause;
    s_state = LIFECYCLE_STATE_INIT;
    s_init_started = false;
    s_awaiting_self_check = false;
    s_restart_pending = false;
    s_pending_self_check = false;
    s_pending_rollback = false;
    s_cfg_snapshot_len = 0U;
    s_deferred_count = 0U;

    s_event_queue = xQueueCreateStatic(LIFECYCLE_QUEUE_DEPTH, sizeof(lifecycle_event_t),
                                       s_queue_storage, &s_queue_struct);
    s_edit_timer = xTimerCreateStatic("lc_edit", pdMS_TO_TICKS(LIFECYCLE_EDIT_TIMEOUT_MS), pdFALSE,
                                      NULL, edit_timer_cb, &s_edit_timer_struct);
    s_init_timer = xTimerCreateStatic("lc_init", pdMS_TO_TICKS(LIFECYCLE_INIT_TIMEOUT_MS), pdFALSE,
                                      NULL, init_timer_cb, &s_init_timer_struct);
    s_restart_timer = xTimerCreateStatic("lc_restart", pdMS_TO_TICKS(LIFECYCLE_RESTART_TIMEOUT_MS),
                                         pdFALSE, NULL, restart_timer_cb, &s_restart_timer_struct);
    s_start_gate = xEventGroupCreateStatic(&s_start_gate_struct);

    if ((s_event_queue == NULL) || (s_edit_timer == NULL) || (s_init_timer == NULL) ||
        (s_restart_timer == NULL) || (s_start_gate == NULL))
    {
        LOG_ERROR(MODULE_TAG, "RTOS object creation failed");
        return LIFECYCLE_ERR_NOT_INIT;
    }

    s_initialised = true;
    return LIFECYCLE_ERR_OK;
}

/* ======================================================================= */
/* Task entry and start gate                                                */
/* ======================================================================= */

void lifecycle_task_body(void *arg)
{
    (void) arg;

    if (!s_initialised)
    {
        return;
    }

    if (!s_init_started)
    {
        run_init_sequence();
    }

#ifdef TEST
    run_event_loop_once();
#else
    for (;;)
    {
        run_event_loop_once();
    }
#endif /* TEST */
}

EventGroupHandle_t lifecycle_get_start_gate(void)
{
    return s_start_gate;
}

/* ======================================================================= */
/* Test-only hooks                                                          */
/* ======================================================================= */

#ifdef TEST
void lifecycle_controller_reset_for_test(void)
{
    (void) memset(&s_deps, 0, sizeof(s_deps));
    s_state = LIFECYCLE_STATE_INIT;
    s_reset_cause = LIFECYCLE_RESET_UNKNOWN;
    s_initialised = false;
    s_init_started = false;
    s_awaiting_self_check = false;
    s_restart_pending = false;
    s_pending_self_check = false;
    s_pending_rollback = false;
    s_cfg_snapshot_len = 0U;
    (void) memset(s_cfg_snapshot, 0, sizeof(s_cfg_snapshot));
    s_deferred_count = 0U;
    s_last_result = LIFECYCLE_ERR_OK;
    s_event_queue = NULL;
    s_edit_timer = NULL;
    s_init_timer = NULL;
    s_restart_timer = NULL;
    s_start_gate = NULL;
}

lifecycle_err_t lifecycle_last_event_result_for_test(void)
{
    return s_last_result;
}

TimerCallbackFunction_t lifecycle_timer_callback_for_test(lc_test_timer_t which)
{
    switch (which)
    {
    case LC_TEST_TIMER_EDIT:
        return edit_timer_cb;
    case LC_TEST_TIMER_INIT:
        return init_timer_cb;
    case LC_TEST_TIMER_RESTART:
        return restart_timer_cb;
    default:
        return NULL;
    }
}

uint32_t lifecycle_deferred_count_for_test(void)
{
    return s_deferred_count;
}
#endif /* TEST */
