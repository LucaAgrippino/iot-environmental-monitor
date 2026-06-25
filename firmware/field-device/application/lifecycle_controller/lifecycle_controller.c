/**
 * @file lifecycle_controller.c
 * @brief LifecycleController implementation.
 *
 * Boot orchestrator and event router for both Field Device and Gateway.
 * All state is file-scope static; no heap allocation after init (P5).
 *
 * Board-conditional code is guarded with BOARD_FIELD_DEVICE / BOARD_GATEWAY.
 *
 * @see docs/lld/application/lifecycle-controller.md
 */

#include "lifecycle_controller/lifecycle_controller.h"

#include "FreeRTOS.h"
#include "timers.h"

#ifndef TEST
#include "stm32f469xx.h"
#include "queue.h"
#include "logger/logger.h"
/* logger.h defines LOG_*(mod, fmt, ...) — shadow with a pinned "LC" module. */
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARN
#undef LOG_ERROR
#define LOG_DEBUG(fmt, ...)                                                                        \
    do                                                                                             \
    {                                                                                              \
        char _b[96];                                                                               \
        (void) snprintf(_b, sizeof(_b), fmt, ##__VA_ARGS__);                                       \
        logger_log(LOG_LVL_DEBUG, "LC", _b);                                                       \
    } while (0)
#define LOG_INFO(fmt, ...)                                                                         \
    do                                                                                             \
    {                                                                                              \
        char _b[96];                                                                               \
        (void) snprintf(_b, sizeof(_b), fmt, ##__VA_ARGS__);                                       \
        logger_log(LOG_LVL_INFO, "LC", _b);                                                        \
    } while (0)
#define LOG_WARN(fmt, ...)                                                                         \
    do                                                                                             \
    {                                                                                              \
        char _b[96];                                                                               \
        (void) snprintf(_b, sizeof(_b), fmt, ##__VA_ARGS__);                                       \
        logger_log(LOG_LVL_WARN, "LC", _b);                                                        \
    } while (0)
#define LOG_ERROR(fmt, ...)                                                                        \
    do                                                                                             \
    {                                                                                              \
        char _b[96];                                                                               \
        (void) snprintf(_b, sizeof(_b), fmt, ##__VA_ARGS__);                                       \
        logger_log(LOG_LVL_ERROR, "LC", _b);                                                       \
    } while (0)
#else
#include "stm32_cmsis_mock.h"
#define LOG_DEBUG(m, ...) ((void) 0)
#define LOG_INFO(m, ...) ((void) 0)
#define LOG_WARN(m, ...) ((void) 0)
#define LOG_ERROR(m, ...) ((void) 0)
#endif /* TEST */

#include <string.h>

/* ======================================================================= */
/* Internal configuration                                                  */
/* ======================================================================= */

/* Edit-session timeout per REQ-NF-214 */
#define LC_EDIT_TIMEOUT_MS (5U * 60U * 1000U) /* 5 minutes */

/* Init watchdog timeout per LC-O2 (provisional) */
#define LC_INIT_TIMEOUT_MS (10U * 1000U) /* 10 seconds */

/* Restart confirmation window per LC-O4 (provisional, GW only) */
#define LC_RESTART_TIMEOUT_MS (30U * 1000U) /* 30 seconds */

/* Event-queue depth */
#define LC_QUEUE_DEPTH (4U)

/* Queue receive timeout (watchdog kick period) */
#define LC_QUEUE_WAIT_MS (1000U)

/* ======================================================================= */
/* File-scope state                                                        */
/* ======================================================================= */

/* Injected interface pointers */
static const iconfig_store_t *s_config_store;
static const iconfig_provider_t *s_cfg_read;
static const iconfig_manager_t *s_cfg_write;
static const isensor_service_t *s_sensors;
static const ialarm_service_t *s_alarms;
static const iconsole_service_t *s_console;
static const ihealth_report_t *s_health_report;

#ifdef USE_GUI
static const igraphics_library_t *s_graphics;
static const ilcd_ui_t *s_lcd_ui;
#endif
#ifdef BOARD_FIELD_DEVICE
static const imodbus_slave_t *s_modbus_slave;
#else  /* BOARD_GATEWAY */
static const icloud_publisher_t *s_cloud;
static const imodbus_poller_t *s_modbus_poller;
static const iupdate_service_t *s_update_service;
static const itime_service_t *s_time_service;
static const ifirmware_store_t *s_firmware_store;
static const ireset_driver_t *s_reset_driver;
static const ihealth_admin_t *s_health_admin;
#endif /* BOARD_FIELD_DEVICE */

/* State machine */
static volatile lifecycle_state_t s_state;
static lifecycle_reset_cause_t s_reset_cause;
static bool s_initialised;

/* GW-only runtime flags */
#ifdef BOARD_GATEWAY
static bool s_restart_pending;
static bool s_pending_self_check;
static bool s_pending_rollback;
#endif /* BOARD_GATEWAY */

/* Event queue */
static StaticQueue_t s_queue_struct;
static uint8_t s_queue_storage[LC_QUEUE_DEPTH * sizeof(lifecycle_event_t)];
static QueueHandle_t s_event_queue;

/* EditingConfig timeout timer */
static StaticTimer_t s_edit_timer_struct;
static TimerHandle_t s_edit_timer;

/* Init watchdog timer (LC-O2) */
static StaticTimer_t s_init_timer_struct;
static TimerHandle_t s_init_timer;

/* Restart confirmation timer (GW only, LC-O4) */
#ifdef BOARD_GATEWAY
static StaticTimer_t s_restart_timer_struct;
static TimerHandle_t s_restart_timer;
#endif /* BOARD_GATEWAY */

/* Start-gate event group */
static StaticEventGroup_t s_start_gate_struct;
static EventGroupHandle_t s_start_gate;

/* Config snapshot buffer (LoadingConfig and EditingConfig) */
static uint8_t s_cfg_snapshot[CONFIG_STORE_MAX_DATA_BYTES];
static uint32_t s_cfg_snapshot_len;

/* ======================================================================= */
/* Internal helpers — forward declarations                                 */
/* ======================================================================= */

static void post_fault(uint32_t fault_code);
static void enter_faulted(uint32_t fault_code);
static bool poll_for_abort(void);

/* ======================================================================= */
/* Timer callbacks                                                         */
/* ======================================================================= */

static void edit_timer_cb(TimerHandle_t timer)
{
    (void) timer;
    lifecycle_event_t ev = {.type = LC_EVENT_CONFIG_EDIT_TIMEOUT, .param = 0U};
    (void) xQueueSend(s_event_queue, &ev, 0U);
}

static void init_timer_cb(TimerHandle_t timer)
{
    (void) timer;
    lifecycle_event_t ev = {.type = LC_EVENT_UNRECOVERABLE_FAULT, .param = LC_FAULT_INIT_TIMEOUT};
    (void) xQueueSend(s_event_queue, &ev, 0U);
}

#ifdef BOARD_GATEWAY
static void restart_timer_cb(TimerHandle_t timer)
{
    (void) timer;
    lifecycle_event_t ev = {.type = LC_EVENT_RESTART_TIMEOUT, .param = 0U};
    (void) xQueueSend(s_event_queue, &ev, 0U);
}
#endif /* BOARD_GATEWAY */

/* ======================================================================= */
/* Reset-cause detection (§13)                                             */
/* ======================================================================= */

lifecycle_reset_cause_t lifecycle_detect_reset_cause(void)
{
    uint32_t csr = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF; /* clear flags for next boot */

    if (csr & RCC_CSR_IWDGRSTF)
    {
        return LIFECYCLE_RESET_WATCHDOG;
    }
    if (csr & RCC_CSR_SFTRSTF)
    {
        return LIFECYCLE_RESET_SOFT;
    }
    if (csr & RCC_CSR_PINRSTF)
    {
        return LIFECYCLE_RESET_POWER_ON;
    }
    return LIFECYCLE_RESET_UNKNOWN;
}

/* ======================================================================= */
/* Internal helpers                                                        */
/* ======================================================================= */

static void post_fault(uint32_t fault_code)
{
    lifecycle_event_t ev = {.type = LC_EVENT_UNRECOVERABLE_FAULT, .param = fault_code};
    (void) xQueueSend(s_event_queue, &ev, 0U);
}

static void enter_faulted(uint32_t fault_code)
{
    s_state = LIFECYCLE_STATE_FAULTED;
    LOG_ERROR("LC: FAULTED (code=0x%08lx)", (unsigned long) fault_code);
    (void) s_health_report->push_event(HEALTH_EVENT_FAULT, fault_code);
}

/* Non-blocking check for an abort (fault) event queued during init.
 * Returns true and transitions to FAULTED if one is found. */
static bool poll_for_abort(void)
{
    lifecycle_event_t ev;
    if (xQueueReceive(s_event_queue, &ev, 0U) == pdTRUE)
    {
        if (ev.type == LC_EVENT_UNRECOVERABLE_FAULT)
        {
            enter_faulted(ev.param);
            return true;
        }
        /* Other events queued during init are discarded silently */
    }
    return false;
}

/* ======================================================================= */
/* FD Init sub-state sequence (Machine 5)                                  */
/* ======================================================================= */

#ifdef BOARD_FIELD_DEVICE

static void fd_run_init_sequence(void)
{
    /* Start init watchdog (LC-O2) */
    (void) xTimerStart(s_init_timer, 0U);

    /* Sub-state 1: CheckingIntegrity */
    if (s_config_store->check_integrity() != CONFIG_STORE_OK)
    {
        LOG_ERROR("LC: CheckingIntegrity failed");
        enter_faulted(1U);
        return;
    }
    if (poll_for_abort())
    {
        return;
    }

    /* Sub-state 2: LoadingConfig */
    if (s_config_store->load(s_cfg_snapshot, &s_cfg_snapshot_len, sizeof(s_cfg_snapshot)) !=
        CONFIG_STORE_OK)
    {
        LOG_ERROR("LC: LoadingConfig load failed");
        enter_faulted(2U);
        return;
    }
    if (s_cfg_write->apply_loaded(s_cfg_snapshot, s_cfg_snapshot_len) != CONFIG_SERVICE_OK)
    {
        LOG_ERROR("LC: LoadingConfig apply_loaded failed");
        enter_faulted(3U);
        return;
    }
    if (poll_for_abort())
    {
        return;
    }

    /* Sub-state 3: BringingUpSensors */
    if (s_sensors->init() != SENSOR_SERVICE_ERR_OK)
    {
        LOG_ERROR("LC: BringingUpSensors sensors init failed");
        enter_faulted(4U);
        return;
    }
    if (s_alarms->init() != ALARM_SERVICE_ERR_OK)
    {
        LOG_ERROR("LC: BringingUpSensors alarms init failed");
        enter_faulted(5U);
        return;
    }
    if (poll_for_abort())
    {
        return;
    }

#ifdef USE_GUI
    /* Sub-state 4: BringingUpLCD (FD only) */
    if (s_graphics->init() != GRAPHICS_ERR_OK)
    {
        LOG_ERROR("LC: BringingUpLCD graphics init failed");
        enter_faulted(6U);
        return;
    }
    if (s_lcd_ui->init() != LCD_UI_ERR_OK)
    {
        LOG_ERROR("LC: BringingUpLCD lcd_ui init failed");
        enter_faulted(7U);
        return;
    }
    (void) s_lcd_ui->show_splash();
    if (poll_for_abort())
    {
        return;
    }
#endif

    /* Sub-state 5: StartingMiddleware */
    {
        const config_params_t *cfg = s_cfg_read->get_params();
        if (cfg != NULL)
        {
            (void) s_modbus_slave->set_address(cfg->modbus_slave_addr);
        }
    }
    (void) s_console->init_finalise();
    (void) s_health_report->init();

    /* Enter Operational */
    (void) xTimerStop(s_init_timer, 0U);
    s_state = LIFECYCLE_STATE_OPERATIONAL;
    LOG_INFO("LC: Operational");

#ifdef USE_GUI
    (void) s_lcd_ui->dismiss_splash();
#endif

    (void) xEventGroupSetBits(s_start_gate, LIFECYCLE_START_GATE_BIT);
}

#else /* BOARD_GATEWAY */

/* ======================================================================= */
/* GW Init sub-state sequence (Machine 1)                                  */
/* ======================================================================= */

static bool gw_await_self_check_result(void)
{
    lifecycle_event_t ev;
    /* Block until a self-check result event arrives */
    while (xQueueReceive(s_event_queue, &ev, portMAX_DELAY) == pdTRUE)
    {
        if (ev.type == LC_EVENT_SELF_CHECK_PASS)
        {
            (void) s_firmware_store->confirm_self_check();
            return true; /* → Operational */
        }
        if (ev.type == LC_EVENT_SELF_CHECK_FAIL)
        {
            (void) s_update_service->resume_rollback();
            (void) s_firmware_store->rollback();
            s_reset_driver->soft_reset(); /* will not return */
            return false;
        }
        if (ev.type == LC_EVENT_UNRECOVERABLE_FAULT)
        {
            enter_faulted(ev.param);
            return false;
        }
        /* ignore other events during self-check wait */
    }
    return false;
}

static void gw_run_init_sequence(void)
{
    /* Start init watchdog (LC-O2) */
    (void) xTimerStart(s_init_timer, 0U);

    /* Sub-state 1: CheckingIntegrity */
    if (s_config_store->check_integrity() != CONFIG_STORE_OK)
    {
        LOG_ERROR("LC: GW CheckingIntegrity failed");
        enter_faulted(1U);
        return;
    }
    {
        bool self_check = false;
        bool rollback = false;
        (void) s_firmware_store->get_pending_flags(&self_check, &rollback);
        s_pending_self_check = self_check;
        s_pending_rollback = rollback;
    }
    if (poll_for_abort())
    {
        return;
    }

    /* Sub-state 2: LoadingConfig */
    if (s_config_store->load(s_cfg_snapshot, &s_cfg_snapshot_len, sizeof(s_cfg_snapshot)) !=
        CONFIG_STORE_OK)
    {
        LOG_ERROR("LC: GW LoadingConfig load failed");
        enter_faulted(2U);
        return;
    }
    if (s_cfg_write->apply_loaded(s_cfg_snapshot, s_cfg_snapshot_len) != CONFIG_SERVICE_OK)
    {
        LOG_ERROR("LC: GW LoadingConfig apply_loaded failed");
        enter_faulted(3U);
        return;
    }
    if (poll_for_abort())
    {
        return;
    }

    /* Sub-state 3: BringingUpSensors */
    if (s_sensors->init() != SENSOR_SERVICE_ERR_OK)
    {
        LOG_ERROR("LC: GW BringingUpSensors sensors init failed");
        enter_faulted(4U);
        return;
    }
    if (s_alarms->init() != ALARM_SERVICE_ERR_OK)
    {
        LOG_ERROR("LC: GW BringingUpSensors alarms init failed");
        enter_faulted(5U);
        return;
    }
    if (poll_for_abort())
    {
        return;
    }

    /* Sub-state 4: StartingMiddleware */
    if (s_modbus_poller->init() != GW_SVC_ERR_OK)
    {
        LOG_ERROR("LC: GW StartingMiddleware modbus_poller init failed");
        enter_faulted(8U);
        return;
    }
    if (s_cloud->init() != GW_SVC_ERR_OK)
    {
        LOG_ERROR("LC: GW StartingMiddleware cloud init failed");
        enter_faulted(9U);
        return;
    }
    if (s_time_service->init() != GW_SVC_ERR_OK)
    {
        LOG_ERROR("LC: GW StartingMiddleware time_service init failed");
        enter_faulted(10U);
        return;
    }
    if (s_update_service->init() != GW_SVC_ERR_OK)
    {
        LOG_ERROR("LC: GW StartingMiddleware update_service init failed");
        enter_faulted(11U);
        return;
    }
    (void) s_console->init_finalise();
    (void) s_health_report->init();
    if (poll_for_abort())
    {
        return;
    }

    /* Sub-state 5: SelfChecking */
    if (s_pending_self_check)
    {
        (void) s_update_service->resume_self_checking();
        if (!gw_await_self_check_result())
        {
            return; /* faulted or rollback triggered inside */
        }
        /* → Operational on pass */
    }
    else if (s_pending_rollback)
    {
        (void) s_update_service->resume_after_rollback();
        (void) s_cloud->report_rollback_result();
        /* → Operational */
    }
    else
    {
        /* Normal probe */
        bool sensors_ok = s_sensors->is_ready();
        bool modbus_ok = s_modbus_poller->is_ready();
        bool cloud_ok = s_cloud->is_ready();
        if (!sensors_ok || !modbus_ok || !cloud_ok)
        {
            LOG_ERROR("LC: GW SelfChecking probes failed");
            enter_faulted(12U);
            return;
        }
    }

    /* Enter Operational */
    (void) xTimerStop(s_init_timer, 0U);
    s_state = LIFECYCLE_STATE_OPERATIONAL;
    LOG_INFO("LC: GW Operational");
    (void) xEventGroupSetBits(s_start_gate, LIFECYCLE_START_GATE_BIT);
}

#endif /* BOARD_FIELD_DEVICE / BOARD_GATEWAY */

/* ======================================================================= */
/* Event loop — shared FD/GW                                               */
/* ======================================================================= */

static void handle_editing_config(const lifecycle_event_t *ev)
{
    switch (ev->type)
    {
    case LC_EVENT_CONFIG_EDIT_APPLY:
        (void) xTimerStop(s_edit_timer, 0U);
        (void) s_cfg_write->flush(); /* commit = flush to flash */
#ifdef BOARD_FIELD_DEVICE
        {
            const config_params_t *cfg = s_cfg_read->get_params();
            if (cfg != NULL)
            {
                (void) s_modbus_slave->set_address(cfg->modbus_slave_addr);
            }
        }
        (void) s_sensors->reconfigure();
#endif
        s_state = LIFECYCLE_STATE_OPERATIONAL;
        LOG_INFO("LC: EditingConfig APPLY → Operational");
        break;

    case LC_EVENT_CONFIG_EDIT_CANCEL:
    case LC_EVENT_CONFIG_EDIT_TIMEOUT:
        (void) xTimerStop(s_edit_timer, 0U);
        (void) s_cfg_write->restore_snapshot();
        s_state = LIFECYCLE_STATE_OPERATIONAL;
        LOG_INFO("LC: EditingConfig CANCEL/TIMEOUT → Operational");
        break;

    case LC_EVENT_UNRECOVERABLE_FAULT:
        (void) xTimerStop(s_edit_timer, 0U);
        enter_faulted(ev->param);
        break;

    default:
        /* All other events queued during edit are ignored */
        break;
    }
}

#ifdef BOARD_GATEWAY
static void handle_restarting(const lifecycle_event_t *ev)
{
    switch (ev->type)
    {
    case LC_EVENT_RESTART_CONFIRMED:
    case LC_EVENT_RESTART_REQUESTED: /* second request treated as confirmation */
        (void) xTimerStop(s_restart_timer, 0U);
        LOG_INFO("LC: Restarting CONFIRMED → soft_reset");
        s_reset_driver->soft_reset(); /* will not return */
        break;

    case LC_EVENT_RESTART_TIMEOUT:
        s_restart_pending = false;
        s_state = LIFECYCLE_STATE_OPERATIONAL;
        LOG_INFO("LC: Restarting TIMEOUT → Operational");
        break;

    case LC_EVENT_UNRECOVERABLE_FAULT:
        (void) xTimerStop(s_restart_timer, 0U);
        enter_faulted(ev->param);
        break;

    default:
        break;
    }
}

static void handle_updating_fw(const lifecycle_event_t *ev)
{
    switch (ev->type)
    {
    case LC_EVENT_SELF_CHECK_PASS:
        (void) s_firmware_store->confirm_self_check();
        s_state = LIFECYCLE_STATE_OPERATIONAL;
        LOG_INFO("LC: UpdatingFW self-check PASS → Operational");
        break;

    case LC_EVENT_SELF_CHECK_FAIL:
        (void) s_update_service->resume_rollback();
        (void) s_firmware_store->rollback();
        s_reset_driver->soft_reset(); /* will not return */
        break;

    case LC_EVENT_UNRECOVERABLE_FAULT:
        enter_faulted(ev->param);
        break;

    default:
        break;
    }
}
#endif /* BOARD_GATEWAY */

static void handle_operational(const lifecycle_event_t *ev)
{
    switch (ev->type)
    {
    case LC_EVENT_CONFIG_EDIT_ENTER:
        (void) s_cfg_write->snapshot();
        (void) xTimerStart(s_edit_timer, 0U);
        s_state = LIFECYCLE_STATE_EDITING_CONFIG;
        LOG_INFO("LC: → EditingConfig");
        break;

#ifdef BOARD_GATEWAY
    case LC_EVENT_RESTART_REQUESTED:
        s_restart_pending = true;
        (void) s_cloud->flush();
        (void) xTimerStart(s_restart_timer, 0U);
        s_state = LIFECYCLE_STATE_RESTARTING;
        LOG_INFO("LC: → Restarting");
        break;

    case LC_EVENT_OTA_REQUESTED:
        (void) s_update_service->start(ev->param);
        s_state = LIFECYCLE_STATE_UPDATING_FW;
        LOG_INFO("LC: → UpdatingFirmware");
        break;
#endif /* BOARD_GATEWAY */

    case LC_EVENT_UNRECOVERABLE_FAULT:
        enter_faulted(ev->param);
        break;

    default:
        LOG_WARN("LC: Operational: ignoring event %u", (unsigned) ev->type);
        break;
    }
}

static void run_event_loop_once(void)
{
    lifecycle_event_t ev;
    BaseType_t got = xQueueReceive(s_event_queue, &ev, pdMS_TO_TICKS(LC_QUEUE_WAIT_MS));

    if (got != pdTRUE)
    {
        /* Timeout — kick watchdog (not yet implemented) and return */
        return;
    }

    switch (s_state)
    {
    case LIFECYCLE_STATE_OPERATIONAL:
        handle_operational(&ev);
        break;

    case LIFECYCLE_STATE_EDITING_CONFIG:
        handle_editing_config(&ev);
        break;

#ifdef BOARD_GATEWAY
    case LIFECYCLE_STATE_RESTARTING:
        handle_restarting(&ev);
        break;

    case LIFECYCLE_STATE_UPDATING_FW:
        handle_updating_fw(&ev);
        break;
#endif /* BOARD_GATEWAY */

    case LIFECYCLE_STATE_FAULTED:
        /* In Faulted: only kick watchdog; no event causes exit */
        break;

    case LIFECYCLE_STATE_INIT:
    default:
        /* Should not happen after init completes */
        break;
    }
}

/* ======================================================================= */
/* Vtable implementations                                                  */
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
    return (xQueueSend(s_event_queue, &event, 0U) == pdTRUE);
}

static lifecycle_err_t vtbl_handle_remote_command(lifecycle_remote_cmd_t cmd)
{
    if (!s_initialised)
    {
        LOG_ERROR("LC: handle_remote_command called before init");
        return LIFECYCLE_ERR_NOT_INIT;
    }

    switch (cmd)
    {
    case LC_REMOTE_CMD_RESET_METRICS:
#ifdef BOARD_GATEWAY
        (void) s_health_admin->reset_metrics();
#endif
        /* On FD: uniform dispatch route; no health_admin on FD — no-op */
        return LIFECYCLE_ERR_OK;

    case LC_REMOTE_CMD_SOFT_RESTART:
    {
        lifecycle_event_t ev = {.type = LC_EVENT_RESTART_REQUESTED, .param = 0U};
        if (xQueueSend(s_event_queue, &ev, 0U) != pdTRUE)
        {
            LOG_WARN("LC: SOFT_RESTART: queue full");
            return LIFECYCLE_ERR_QUEUE_FULL;
        }
        return LIFECYCLE_ERR_OK;
    }

    default:
        LOG_WARN("LC: unknown remote command %u", (unsigned) cmd);
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
/* Initialisation                                                          */
/* ======================================================================= */

lifecycle_err_t
lifecycle_controller_init(lifecycle_reset_cause_t reset_cause, const iconfig_store_t *config_store,
                          const iconfig_provider_t *cfg_read, const iconfig_manager_t *cfg_write,
                          const isensor_service_t *sensors, const ialarm_service_t *alarms,
                          const iconsole_service_t *console, const ihealth_report_t *health_report,
#ifdef USE_GUI
                          const igraphics_library_t *graphics, const ilcd_ui_t *lcd_ui,
#endif /* USE_GUI */

#ifdef BOARD_FIELD_DEVICE
                          const imodbus_slave_t *modbus_slave
#else  /* BOARD_GATEWAY */
                          const icloud_publisher_t *cloud, const imodbus_poller_t *modbus_poller,
                          const iupdate_service_t *update_service,
                          const itime_service_t *time_service,
                          const ifirmware_store_t *firmware_store,
                          const ireset_driver_t *reset_driver, const ihealth_admin_t *health_admin
#endif /* BOARD_FIELD_DEVICE */
)
{
    /* Null checks — all pointers must be non-NULL */
    if ((config_store == NULL) || (cfg_read == NULL) || (cfg_write == NULL) || (sensors == NULL) ||
        (alarms == NULL) || (console == NULL) || (health_report == NULL))
    {
        return LIFECYCLE_ERR_NULL_ARG;
    }

#ifdef USE_GUI
    if ((graphics == NULL) || (lcd_ui == NULL))
    {
        return LIFECYCLE_ERR_NULL_ARG;
    }
#endif /* USE_GUI */

#ifdef BOARD_FIELD_DEVICE
    if (modbus_slave == NULL)
    {
        return LIFECYCLE_ERR_NULL_ARG;
    }
#else  /* BOARD_GATEWAY */
    if ((cloud == NULL) || (modbus_poller == NULL) || (update_service == NULL) ||
        (time_service == NULL) || (firmware_store == NULL) || (reset_driver == NULL) ||
        (health_admin == NULL))
    {
        return LIFECYCLE_ERR_NULL_ARG;
    }
#endif /* BOARD_FIELD_DEVICE */

    /* Store injected pointers */
    s_config_store = config_store;
    s_cfg_read = cfg_read;
    s_cfg_write = cfg_write;
    s_sensors = sensors;
    s_alarms = alarms;
    s_console = console;
    s_health_report = health_report;

#ifdef USE_GUI
    s_graphics = graphics;
    s_lcd_ui = lcd_ui;
#endif

#ifdef BOARD_FIELD_DEVICE
    s_modbus_slave = modbus_slave;
#else  /* BOARD_GATEWAY */
    s_cloud = cloud;
    s_modbus_poller = modbus_poller;
    s_update_service = update_service;
    s_time_service = time_service;
    s_firmware_store = firmware_store;
    s_reset_driver = reset_driver;
    s_health_admin = health_admin;
#endif /* BOARD_FIELD_DEVICE */

    s_reset_cause = reset_cause;
    s_state = LIFECYCLE_STATE_INIT;

    /* Create event queue */
    s_event_queue = xQueueCreateStatic(LC_QUEUE_DEPTH, sizeof(lifecycle_event_t), s_queue_storage,
                                       &s_queue_struct);

    /* Create edit-timeout timer */
    s_edit_timer = xTimerCreateStatic("lc_edit", pdMS_TO_TICKS(LC_EDIT_TIMEOUT_MS), pdFALSE, NULL,
                                      edit_timer_cb, &s_edit_timer_struct);

    /* Create init-watchdog timer */
    s_init_timer = xTimerCreateStatic("lc_init", pdMS_TO_TICKS(LC_INIT_TIMEOUT_MS), pdFALSE, NULL,
                                      init_timer_cb, &s_init_timer_struct);

#ifdef BOARD_GATEWAY
    /* Create restart-confirmation timer */
    s_restart_timer = xTimerCreateStatic("lc_restart", pdMS_TO_TICKS(LC_RESTART_TIMEOUT_MS),
                                         pdFALSE, NULL, restart_timer_cb, &s_restart_timer_struct);
#endif /* BOARD_GATEWAY */

    /* Create start-gate event group */
    s_start_gate = xEventGroupCreateStatic(&s_start_gate_struct);

    s_initialised = true;
    return LIFECYCLE_ERR_OK;
}

/* ======================================================================= */
/* Task entry (§4.4)                                                       */
/* ======================================================================= */

void lifecycle_task_body(void *arg)
{
    (void) arg;

    /* Run boot sequence once (only if still in INIT state) */
    if (s_state == LIFECYCLE_STATE_INIT)
    {
#ifdef BOARD_FIELD_DEVICE
        fd_run_init_sequence();
#else
        gw_run_init_sequence();
#endif
    }

#ifdef TEST
    /* Single-step mode: process one event from the queue and return */
    run_event_loop_once();
#else
    /* Production: never-return event loop */
    for (;;)
    {
        run_event_loop_once();
    }
#endif
}

/* ======================================================================= */
/* Start-gate accessor                                                     */
/* ======================================================================= */

EventGroupHandle_t lifecycle_get_start_gate(void)
{
    return s_start_gate;
}

/* ======================================================================= */
/* Test-only hooks                                                         */
/* ======================================================================= */

#ifdef TEST
void lifecycle_controller_reset_for_test(void)
{
    s_config_store = NULL;
    s_cfg_read = NULL;
    s_cfg_write = NULL;
    s_sensors = NULL;
    s_alarms = NULL;
    s_console = NULL;
    s_health_report = NULL;

#ifdef USE_GUI
    s_graphics = NULL;
    s_lcd_ui = NULL;
#endif

#ifdef BOARD_FIELD_DEVICE
    s_modbus_slave = NULL;
#else  /* BOARD_GATEWAY */
    s_cloud = NULL;
    s_modbus_poller = NULL;
    s_update_service = NULL;
    s_time_service = NULL;
    s_firmware_store = NULL;
    s_reset_driver = NULL;
    s_health_admin = NULL;
#endif /* BOARD_FIELD_DEVICE */

    s_state = LIFECYCLE_STATE_INIT;
    s_reset_cause = LIFECYCLE_RESET_UNKNOWN;
    s_initialised = false;

#ifdef BOARD_GATEWAY
    s_restart_pending = false;
    s_pending_self_check = false;
    s_pending_rollback = false;
#endif

    (void) memset(s_cfg_snapshot, 0, sizeof(s_cfg_snapshot));
    s_cfg_snapshot_len = 0U;

    s_event_queue = NULL;
    s_edit_timer = NULL;
    s_init_timer = NULL;
    s_start_gate = NULL;

#ifdef BOARD_GATEWAY
    s_restart_timer = NULL;
#endif
}
#endif /* TEST */
