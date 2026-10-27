/**
 * @file lifecycle_wiring.c
 * @brief Adapters from the real Gateway modules to LifecycleController's interfaces.
 *
 * @see docs/lld/application/lifecycle-controller-lld.md section 26
 */

#include "lifecycle_wiring.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#ifndef TEST
#include "event_groups.h"
#endif /* TEST */

#include "lifecycle_wiring_internal.h"

#ifdef TEST
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

#define MODULE_TAG "LCW"

/* ======================================================================= */
/* State                                                                    */
/* ======================================================================= */

static lifecycle_wiring_config_t s_cfg;

/** True when ConfigStore found no valid slot and the defaults are standing in (LC-D16). */
static bool s_using_defaults;

static lc_dep_status_t status_of(bool ok)
{
    return ok ? LC_DEP_OK : LC_DEP_FAIL;
}

/* ======================================================================= */
/* IConfigStore                                                             */
/* ======================================================================= */

static lc_dep_status_t cs_check_integrity(void *ctx)
{
    (void) ctx;
    const config_store_err_t rc = config_store_check_integrity(s_cfg.config_store);

    if (rc == CONFIG_STORE_ERR_OK)
    {
        s_using_defaults = false;
        return LC_DEP_OK;
    }
    if ((rc == CONFIG_STORE_ERR_NO_VALID_SLOT) && !s_cfg.fault_on_no_config)
    {
        /* A never-programmed device and a corrupt one look the same here; the first is far
         * more common, and ConfigService already holds the defaults (LC-D16). */
        LOG_WARN(MODULE_TAG, "no valid stored configuration: running on the defaults");
        s_using_defaults = true;
        return LC_DEP_OK;
    }
    return LC_DEP_FAIL;
}

static lc_dep_status_t cs_load(void *ctx, void *buf, uint32_t *len_out, uint32_t max_len)
{
    (void) ctx;
    if (s_using_defaults)
    {
        *len_out = 0U;
        return LC_DEP_OK;
    }
    return status_of(config_store_load(s_cfg.config_store, buf, len_out, max_len) ==
                     CONFIG_STORE_ERR_OK);
}

static const lc_config_store_if_t k_config_store = {
    .ctx = NULL, .check_integrity = cs_check_integrity, .load = cs_load};

/* ======================================================================= */
/* IConfigManager                                                           */
/* ======================================================================= */

static lc_dep_status_t cm_apply_loaded(void *ctx, const void *buf, uint32_t len)
{
    (void) ctx;
    if (len == 0U)
    {
        return LC_DEP_OK; /* defaults are already in place */
    }

    const config_service_err_t rc = config_service_apply_loaded(buf, len);

    if (rc == CONFIG_SERVICE_ERR_INVALID)
    {
        /* Wrong length or schema version: ConfigService discarded the blob and applied the
         * defaults, which is a sound state (for example after a firmware update). */
        LOG_WARN(MODULE_TAG, "stored configuration rejected: running on the defaults");
        return LC_DEP_OK;
    }
    return status_of(rc == CONFIG_SERVICE_OK);
}

/* ConfigService keeps the snapshot itself, so no bytes travel through the controller's buffer. */
static lc_dep_status_t cm_snapshot(void *ctx, void *buf, uint32_t *len_out, uint32_t max_len)
{
    (void) ctx;
    (void) buf;
    (void) max_len;
    *len_out = 0U;
    return status_of(config_service_snapshot() == CONFIG_SERVICE_OK);
}

static lc_dep_status_t cm_restore_snapshot(void *ctx, const void *buf, uint32_t len)
{
    (void) ctx;
    (void) buf;
    (void) len;
    return status_of(config_service_restore_snapshot() == CONFIG_SERVICE_OK);
}

static lc_dep_status_t cm_commit(void *ctx)
{
    (void) ctx;
    return status_of(config_service_flush() == CONFIG_SERVICE_OK);
}

static const lc_config_manager_if_t k_config_manager = {.ctx = NULL,
                                                        .apply_loaded = cm_apply_loaded,
                                                        .snapshot = cm_snapshot,
                                                        .restore_snapshot = cm_restore_snapshot,
                                                        .commit = cm_commit};

/* ======================================================================= */
/* ISensorService / IAlarmService                                           */
/* ======================================================================= */

static lc_dep_status_t sensors_reconfigure(void *ctx)
{
    (void) ctx;
    return status_of(sensor_service_reconfigure(config_service_get_params()) ==
                     SENSOR_SERVICE_ERR_OK);
}

/* SensorService is initialised by sensor_task_create() before the scheduler (SS-D9). The task
 * handle exists only if that succeeded, so it is the proof; then the loaded configuration is
 * applied, which is what this sub-state is for. */
static lc_dep_status_t sensors_init(void *ctx)
{
    if (sensor_task_get_handle() == NULL)
    {
        LOG_WARN(MODULE_TAG, "SensorTask was not created: sensors are not initialised");
        return LC_DEP_FAIL;
    }
    return sensors_reconfigure(ctx);
}

static bool sensors_is_ready(void *ctx)
{
    (void) ctx;
    return sensor_service_is_ready();
}

static const lc_sensors_if_t k_sensors = {.ctx = NULL,
                                          .init = sensors_init,
                                          .is_ready = sensors_is_ready,
                                          .reconfigure = sensors_reconfigure};

static lc_dep_status_t alarms_init(void *ctx)
{
    (void) ctx;
    alarm_state_t state = ALARM_STATE_CLEAR;

    /* Initialised together with SensorService (SS-D9); a query that works proves it. */
    return status_of(alarm_service->get_state(SENSOR_ID_TEMPERATURE, &state) ==
                     ALARM_SERVICE_ERR_OK);
}

static const lc_alarms_if_t k_alarms = {.ctx = NULL, .init = alarms_init};

/* ======================================================================= */
/* IConsoleService                                                          */
/* ======================================================================= */

/* console_service_init() runs in main(), after the services it uses and before the scheduler; the
 * first frame goes out when ConsoleTask first runs. Nothing is left to finish here. */
static lc_dep_status_t console_init_finalise(void *ctx)
{
    (void) ctx;
    return LC_DEP_OK;
}

static const lc_console_if_t k_console = {.ctx = NULL, .init_finalise = console_init_finalise};

/* ======================================================================= */
/* ICloudPublisher                                                          */
/* ======================================================================= */

static lc_dep_status_t cloud_init(void *ctx)
{
    (void) ctx;
    return status_of(cloud_publisher_is_ready(s_cfg.cloud));
}

static lc_dep_status_t cloud_flush(void *ctx)
{
    (void) ctx;
    return status_of(cloud_publisher_flush(s_cfg.cloud) == CP_ERR_OK);
}

static bool cloud_is_ready(void *ctx)
{
    (void) ctx;
    return cloud_publisher_is_ready(s_cfg.cloud);
}

static const lc_cloud_if_t k_cloud = {
    .ctx = NULL, .init = cloud_init, .flush = cloud_flush, .is_ready = cloud_is_ready};

/* ======================================================================= */
/* IModbusPoller / ITimeService                                             */
/* ======================================================================= */

static lc_dep_status_t poller_init(void *ctx)
{
    (void) ctx;
    const config_params_t *params = config_service_get_params();

    if (params == NULL)
    {
        return LC_DEP_FAIL;
    }
    return status_of(modbus_poller_init(params->modbus_poll_period_ms, s_cfg.health) ==
                     MODBUS_POLLER_ERR_OK);
}

static bool poller_is_ready(void *ctx)
{
    (void) ctx;
    return modbus_poller_is_ready();
}

static const lc_modbus_poller_if_t k_poller = {
    .ctx = NULL, .init = poller_init, .is_ready = poller_is_ready};

static lc_dep_status_t time_init(void *ctx)
{
    (void) ctx;
    return status_of(time_service_init(s_cfg.ntp) == TIME_SERVICE_OK);
}

static const lc_time_service_if_t k_time = {.ctx = NULL, .init = time_init};

/* ======================================================================= */
/* IUpdateService / IFirmwareStore: stand-ins (no OTA) and the real adapters */
/* ======================================================================= */

static lc_dep_status_t upd_ok(void *ctx)
{
    (void) ctx;
    return LC_DEP_OK;
}

static lc_dep_status_t upd_start(void *ctx, uint32_t image_size)
{
    (void) ctx;
    (void) image_size;
    LOG_WARN(MODULE_TAG, "OTA requested but UpdateService does not exist");
    return LC_DEP_FAIL;
}

static const lc_update_service_if_t k_update_standin = {.ctx = NULL,
                                                        .init = upd_ok,
                                                        .start = upd_start,
                                                        .resume_self_checking = upd_ok,
                                                        .resume_rollback = upd_ok,
                                                        .resume_after_rollback = upd_ok};

static lc_dep_status_t fw_get_pending_flags(void *ctx, bool *self_check_out, bool *rollback_out)
{
    (void) ctx;
    *self_check_out = false;
    *rollback_out = false;
    return LC_DEP_OK;
}

static lc_dep_status_t fw_unavailable(void *ctx)
{
    (void) ctx;
    LOG_WARN(MODULE_TAG, "FirmwareStore does not exist");
    return LC_DEP_FAIL;
}

static const lc_firmware_store_if_t k_firmware_standin = {.ctx = NULL,
                                                          .get_pending_flags = fw_get_pending_flags,
                                                          .confirm_self_check = fw_unavailable,
                                                          .rollback = fw_unavailable};

/* --- Real adapters (LCC-4, update-service-lld.md 7.5) ------------------------------------ */

static lc_dep_status_t ota_init(void *ctx)
{
    (void) ctx;
    return status_of(s_cfg.update_ops->is_created(s_cfg.update));
}

static lc_dep_status_t ota_start(void *ctx, uint32_t job)
{
    (void) ctx;
    return status_of(s_cfg.update_ops->start(s_cfg.update, job) == UPDATE_SERVICE_ERR_OK);
}

static lc_dep_status_t ota_resume_self_checking(void *ctx)
{
    (void) ctx;
    return status_of(s_cfg.update_ops->resume_self_checking(s_cfg.update) == UPDATE_SERVICE_ERR_OK);
}

static lc_dep_status_t ota_resume_rollback(void *ctx)
{
    (void) ctx;
    return status_of(s_cfg.update_ops->resume_rollback(s_cfg.update) == UPDATE_SERVICE_ERR_OK);
}

static lc_dep_status_t ota_resume_after_rollback(void *ctx)
{
    (void) ctx;
    return status_of(s_cfg.update_ops->resume_after_rollback(s_cfg.update) ==
                     UPDATE_SERVICE_ERR_OK);
}

static const lc_update_service_if_t k_update_real = {
    .ctx = NULL,
    .init = ota_init,
    .start = ota_start,
    .resume_self_checking = ota_resume_self_checking,
    .resume_rollback = ota_resume_rollback,
    .resume_after_rollback = ota_resume_after_rollback};

/** The pending flags are derived from the boot state (firmware-store.md 7.3). */
static lc_dep_status_t ota_fw_pending_flags(void *ctx, bool *self_check_out, bool *rollback_out)
{
    (void) ctx;

    firmware_boot_info_t info;

    if (s_cfg.firmware_ops->get_boot_info(s_cfg.firmware, &info) != FIRMWARE_STORE_ERR_OK)
    {
        return LC_DEP_FAIL;
    }

    *self_check_out = info.ota_capable && (info.state == FIRMWARE_BOOT_TRIAL_RUNNING);
    *rollback_out = info.ota_capable && (info.state == FIRMWARE_BOOT_ROLLED_BACK);

    return LC_DEP_OK;
}

static lc_dep_status_t ota_fw_confirm(void *ctx)
{
    (void) ctx;
    return status_of(s_cfg.firmware_ops->confirm(s_cfg.firmware) == FIRMWARE_STORE_ERR_OK);
}

static lc_dep_status_t ota_fw_rollback(void *ctx)
{
    (void) ctx;
    return status_of(s_cfg.firmware_ops->rollback(s_cfg.firmware) == FIRMWARE_STORE_ERR_OK);
}

static const lc_firmware_store_if_t k_firmware_real = {.ctx = NULL,
                                                       .get_pending_flags = ota_fw_pending_flags,
                                                       .confirm_self_check = ota_fw_confirm,
                                                       .rollback = ota_fw_rollback};

/* ======================================================================= */
/* IResetDriver                                                             */
/* ======================================================================= */

static void reset_soft_reset(void *ctx)
{
    (void) ctx;
    reset_trigger();
}

static const lc_reset_driver_if_t k_reset = {.ctx = NULL, .soft_reset = reset_soft_reset};

/* ======================================================================= */
/* IHealthReport / IHealthAdmin                                             */
/* ======================================================================= */

static lc_dep_status_t health_status(health_monitor_err_t err)
{
    return status_of(err == HEALTH_MONITOR_ERR_OK);
}

static lc_dep_status_t hr_init(void *ctx)
{
    (void) ctx;
    return LC_DEP_OK; /* created in main(); nothing left to start */
}

static lc_dep_status_t hr_push_fault(void *ctx, uint32_t fault_code)
{
    (void) ctx;
    return health_status(health_monitor_push_event(s_cfg.health, HEALTH_EVENT_FAULT, fault_code));
}

static lc_dep_status_t hr_update_lifecycle(void *ctx, lifecycle_state_t state)
{
    (void) ctx;
    return health_status(health_monitor_update_lifecycle(s_cfg.health, state));
}

static const lc_health_report_if_t k_health_report = {.ctx = NULL,
                                                      .init = hr_init,
                                                      .push_fault = hr_push_fault,
                                                      .update_lifecycle = hr_update_lifecycle};

static lc_dep_status_t ha_reset_metrics(void *ctx)
{
    (void) ctx;
    return health_status(health_monitor_reset_metrics(s_cfg.health));
}

static lc_dep_status_t ha_set_led_fault(void *ctx)
{
    (void) ctx;
    return health_status(health_monitor_set_led_fault(s_cfg.health));
}

static const lc_health_admin_if_t k_health_admin = {
    .ctx = NULL, .reset_metrics = ha_reset_metrics, .set_led_fault = ha_set_led_fault};

/* ======================================================================= */
/* Public API                                                               */
/* ======================================================================= */

lifecycle_err_t lifecycle_wiring_bind(const lifecycle_wiring_config_t *config,
                                      lifecycle_deps_t *deps_out)
{
    if ((config == NULL) || (deps_out == NULL) || (config->health == NULL) ||
        (config->config_store == NULL) || (config->cloud == NULL) || (config->ntp == NULL))
    {
        return LIFECYCLE_ERR_NULL_ARG;
    }

    /* The four OTA members are all or nothing. */
    const bool ota_any = (config->update_ops != NULL) || (config->update != NULL) ||
                         (config->firmware_ops != NULL) || (config->firmware != NULL);
    const bool ota_all = (config->update_ops != NULL) && (config->update != NULL) &&
                         (config->firmware_ops != NULL) && (config->firmware != NULL);

    if (ota_any && !ota_all)
    {
        return LIFECYCLE_ERR_NULL_ARG;
    }

    if (ota_all &&
        ((config->update_ops->is_created == NULL) || (config->update_ops->start == NULL) ||
         (config->update_ops->resume_self_checking == NULL) ||
         (config->update_ops->resume_rollback == NULL) ||
         (config->update_ops->resume_after_rollback == NULL) ||
         (config->firmware_ops->get_boot_info == NULL) || (config->firmware_ops->confirm == NULL) ||
         (config->firmware_ops->rollback == NULL)))
    {
        return LIFECYCLE_ERR_NULL_ARG;
    }

    s_cfg = *config;
    s_using_defaults = false;

    *deps_out =
        (lifecycle_deps_t){.config_store = &k_config_store,
                           .config_manager = &k_config_manager,
                           .sensors = &k_sensors,
                           .alarms = &k_alarms,
                           .console = &k_console,
                           .health_report = &k_health_report,
                           .health_admin = &k_health_admin,
                           .cloud = &k_cloud,
                           .modbus_poller = &k_poller,
                           .update_service = ota_all ? &k_update_real : &k_update_standin,
                           .time_service = &k_time,
                           .firmware_store = ota_all ? &k_firmware_real : &k_firmware_standin,
                           .reset_driver = &k_reset,
                           .watchdog = NULL};
    return LIFECYCLE_ERR_OK;
}

/* ======================================================================= */
/* Remote command path: CloudPublisher -> LifecycleController              */
/* ======================================================================= */

/** Longest payload examined; a control command is a short JSON object. */
#define REMOTE_PAYLOAD_MAX (63U)

static bool payload_has_command(const char *text, const char *name)
{
    char needle[40];

    (void) snprintf(needle, sizeof(needle), "\"command\":\"%s\"", name);
    return strstr(text, needle) != NULL;
}

void lifecycle_handle_remote(lifecycle_handle_t handle, const uint8_t *payload, uint32_t len)
{
    (void) handle; /* the controller is a singleton; the handle only satisfies the interface */

    if ((payload == NULL) || (len == 0U))
    {
        return;
    }

    char text[REMOTE_PAYLOAD_MAX + 1U];
    const uint32_t n = (len < REMOTE_PAYLOAD_MAX) ? len : REMOTE_PAYLOAD_MAX;

    (void) memcpy(text, payload, n);
    text[n] = '\0';

    /* Each needle ends with the closing quote, so "restart" does not match inside
     * "restart_confirm"; the order below is for readability only. */
    if (payload_has_command(text, LIFECYCLE_REMOTE_CMD_RESTART_CONFIRM))
    {
        const lifecycle_event_t ev = {.type = LC_EVENT_RESTART_CONFIRMED, .param = 0U};

        if (!lifecycle_controller->post_event(ev))
        {
            LOG_WARN(MODULE_TAG, "restart confirmation not queued");
        }
    }
    else if (payload_has_command(text, LIFECYCLE_REMOTE_CMD_RESTART))
    {
        if (lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_SOFT_RESTART) !=
            LIFECYCLE_ERR_OK)
        {
            LOG_WARN(MODULE_TAG, "restart request not queued");
        }
    }
    else if (payload_has_command(text, LIFECYCLE_REMOTE_CMD_RESET_METRICS))
    {
        if (lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS) !=
            LIFECYCLE_ERR_OK)
        {
            LOG_WARN(MODULE_TAG, "reset_metrics not executed");
        }
    }
    else
    {
        LOG_WARN(MODULE_TAG, "unknown control command ignored");
    }
}

void lifecycle_wiring_poller_task(void *arg)
{
    (void) arg;
    (void) xEventGroupWaitBits(lifecycle_get_start_gate(), LIFECYCLE_START_GATE_BIT, pdFALSE,
                               pdTRUE, portMAX_DELAY);
    modbus_poller_run();
}

#ifdef TEST
void lifecycle_wiring_reset_for_test(void)
{
    (void) memset(&s_cfg, 0, sizeof(s_cfg));
    s_using_defaults = false;
}
#endif /* TEST */
