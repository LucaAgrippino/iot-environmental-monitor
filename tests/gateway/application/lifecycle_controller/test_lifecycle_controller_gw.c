/**
 * @file test_lifecycle_controller_gw.c
 * @brief Unit tests for LifecycleController — GW subset.
 *
 * Covers TC-LC-050..062, TC-LC-085..091, TC-LC-095..099, TC-LC-105..108,
 * TC-LC-130..136 as defined in docs/lld/application/lifecycle-controller.md §21.
 *
 * Build defines (project.yml): STM32F469xx, BOARD_GATEWAY, TEST.
 *
 * Test strategy: spy vtables, FreeRTOS mock, single-step via lifecycle_task_body().
 */

#include "unity.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "lifecycle_controller/lifecycle_controller.h"
#include "freertos_mock.h"
#include "stm32_cmsis_mock.h"

/* ======================================================================= */
/* Spy infrastructure                                                       */
/* ======================================================================= */

/* --- config_store spy -------------------------------------------------- */
static config_store_err_t g_cfg_store_check_integrity_return = CONFIG_STORE_OK;
static config_store_err_t g_cfg_store_load_return            = CONFIG_STORE_OK;
static uint32_t           g_cfg_store_load_calls             = 0U;

static config_store_err_t spy_cfg_store_check_integrity(void)
{
    return g_cfg_store_check_integrity_return;
}

static config_store_err_t spy_cfg_store_load(void *d, uint32_t *len_out, uint32_t max)
{
    (void)d; (void)max;
    if (len_out) { *len_out = 0U; }
    g_cfg_store_load_calls++;
    return g_cfg_store_load_return;
}

static config_store_err_t spy_cfg_store_init(const void *h) { (void)h; return CONFIG_STORE_OK; }
static config_store_err_t spy_cfg_store_save(const void *d, uint32_t l) { (void)d; (void)l; return CONFIG_STORE_OK; }
static config_store_err_t spy_cfg_store_erase(void) { return CONFIG_STORE_OK; }

static const iconfig_store_t g_cfg_store_spy = {
    .init            = spy_cfg_store_init,
    .load            = spy_cfg_store_load,
    .save            = spy_cfg_store_save,
    .check_integrity = spy_cfg_store_check_integrity,
    .erase           = spy_cfg_store_erase,
};

/* --- config_provider spy ----------------------------------------------- */
static config_params_t g_spy_params = { .modbus_slave_addr = 1U };
static const config_params_t *spy_cfg_provider_get_params(void) { return &g_spy_params; }
static const iconfig_provider_t g_cfg_provider_spy = { .get_params = spy_cfg_provider_get_params };

/* --- config_manager spy ------------------------------------------------ */
static uint32_t             g_cfg_mgr_apply_loaded_calls  = 0U;
static config_service_err_t g_cfg_mgr_apply_loaded_return = CONFIG_SERVICE_OK;
static uint32_t             g_cfg_mgr_snapshot_calls      = 0U;
static uint32_t             g_cfg_mgr_restore_calls       = 0U;
static uint32_t             g_cfg_mgr_flush_calls         = 0U;

static config_service_err_t spy_apply_loaded(const void *b, uint32_t l)
{
    (void)b; (void)l;
    g_cfg_mgr_apply_loaded_calls++;
    return g_cfg_mgr_apply_loaded_return;
}
static config_service_err_t spy_set_param(config_param_id_t id, const void *v) { (void)id; (void)v; return CONFIG_SERVICE_OK; }
static config_service_err_t spy_validate_param(config_param_id_t id, const void *v) { (void)id; (void)v; return CONFIG_SERVICE_OK; }
static config_service_err_t spy_snapshot(void)          { g_cfg_mgr_snapshot_calls++; return CONFIG_SERVICE_OK; }
static config_service_err_t spy_restore_snapshot(void)  { g_cfg_mgr_restore_calls++;  return CONFIG_SERVICE_OK; }
static config_service_err_t spy_flush(void)             { g_cfg_mgr_flush_calls++;    return CONFIG_SERVICE_OK; }

static const iconfig_manager_t g_cfg_mgr_spy = {
    .apply_loaded     = spy_apply_loaded,
    .set_param        = spy_set_param,
    .validate_param   = spy_validate_param,
    .snapshot         = spy_snapshot,
    .restore_snapshot = spy_restore_snapshot,
    .flush            = spy_flush,
};

/* --- sensor_service spy ----------------------------------------------- */
static sensor_service_err_t g_sensors_init_return = SENSOR_SERVICE_ERR_OK;
static bool                 g_sensors_is_ready_return = true;
static uint32_t             g_sensors_init_calls     = 0U;

static sensor_service_err_t spy_sensors_init(void) { g_sensors_init_calls++; return g_sensors_init_return; }
static sensor_service_err_t spy_sensors_run_cycle(void) { return SENSOR_SERVICE_ERR_OK; }
static sensor_service_err_t spy_sensors_get_snapshot(sensor_snapshot_t *s) { (void)s; return SENSOR_SERVICE_ERR_OK; }
static sensor_service_err_t spy_sensors_subscribe(void (*cb)(const sensor_snapshot_t *)) { (void)cb; return SENSOR_SERVICE_ERR_OK; }
static sensor_service_err_t spy_sensors_read_on_demand(void) { return SENSOR_SERVICE_ERR_OK; }
static bool                 spy_sensors_is_ready(void) { return g_sensors_is_ready_return; }
static sensor_service_err_t spy_sensors_reconfigure(void) { return SENSOR_SERVICE_ERR_OK; }

static const isensor_service_t g_sensors_spy = {
    .init           = spy_sensors_init,
    .run_cycle      = spy_sensors_run_cycle,
    .get_snapshot   = spy_sensors_get_snapshot,
    .subscribe      = spy_sensors_subscribe,
    .read_on_demand = spy_sensors_read_on_demand,
    .is_ready       = spy_sensors_is_ready,
    .reconfigure    = spy_sensors_reconfigure,
};

/* --- alarm_service spy ------------------------------------------------ */
static alarm_service_err_t g_alarms_init_return = ALARM_SERVICE_ERR_OK;

static alarm_service_err_t spy_alarms_init(void) { return g_alarms_init_return; }
static alarm_service_err_t spy_alarms_get_state(sensor_id_t s, alarm_state_t *o) { (void)s; (void)o; return ALARM_SERVICE_ERR_OK; }
static alarm_service_err_t spy_alarms_get_all_states(alarm_state_t st[SENSOR_ID_COUNT]) { (void)st; return ALARM_SERVICE_ERR_OK; }
static alarm_service_err_t spy_alarms_subscribe(void (*cb)(sensor_id_t, int, const void *)) { (void)cb; return ALARM_SERVICE_ERR_OK; }
static alarm_service_err_t spy_alarms_ack_all(void) { return ALARM_SERVICE_ERR_OK; }

static const ialarm_service_t g_alarms_spy = {
    .init           = spy_alarms_init,
    .get_state      = spy_alarms_get_state,
    .get_all_states = spy_alarms_get_all_states,
    .subscribe      = spy_alarms_subscribe,
    .ack_all        = spy_alarms_ack_all,
};

/* --- console_service spy ---------------------------------------------- */
static uint32_t g_console_init_finalise_calls = 0U;

static console_service_err_t spy_console_init_finalise(void) { g_console_init_finalise_calls++; return CONSOLE_SERVICE_ERR_OK; }
static console_service_err_t spy_console_run_once(void) { return CONSOLE_SERVICE_ERR_OK; }

static const iconsole_service_t g_console_spy = {
    .init_finalise = spy_console_init_finalise,
    .run_once      = spy_console_run_once,
};

/* --- health_report spy ------------------------------------------------ */
static uint32_t       g_health_push_event_calls      = 0U;
static health_event_t g_health_push_event_last_event = (health_event_t)0;
static uint32_t       g_health_push_event_last_param = 0U;

static health_monitor_err_t spy_health_init(void) { return HEALTH_MONITOR_ERR_OK; }
static health_monitor_err_t spy_health_push_event(health_event_t event, uint32_t param)
{
    g_health_push_event_calls++;
    g_health_push_event_last_event = event;
    g_health_push_event_last_param = param;
    return HEALTH_MONITOR_ERR_OK;
}
static struct ihealth_report_s g_health_spy = {
    .init       = spy_health_init,
    .push_event = spy_health_push_event,
};

/* --- cloud_publisher spy ---------------------------------------------- */
static gw_svc_err_t g_cloud_init_return     = GW_SVC_ERR_OK;
static bool         g_cloud_is_ready_return = true;
static uint32_t     g_cloud_flush_calls     = 0U;
static uint32_t     g_cloud_init_calls      = 0U;
static uint32_t     g_cloud_rollback_result_calls = 0U;

static gw_svc_err_t spy_cloud_init(void)                  { g_cloud_init_calls++;            return g_cloud_init_return; }
static gw_svc_err_t spy_cloud_flush(void)                  { g_cloud_flush_calls++;           return GW_SVC_ERR_OK; }
static bool         spy_cloud_is_ready(void)               { return g_cloud_is_ready_return;  }
static gw_svc_err_t spy_cloud_report_rollback_result(void) { g_cloud_rollback_result_calls++; return GW_SVC_ERR_OK; }

static const icloud_publisher_t g_cloud_spy = {
    .init                  = spy_cloud_init,
    .flush                 = spy_cloud_flush,
    .is_ready              = spy_cloud_is_ready,
    .report_rollback_result = spy_cloud_report_rollback_result,
};

/* --- modbus_poller spy ------------------------------------------------- */
static gw_svc_err_t g_modbus_poller_init_return     = GW_SVC_ERR_OK;
static bool         g_modbus_poller_is_ready_return = true;
static uint32_t     g_modbus_poller_init_calls      = 0U;

static gw_svc_err_t spy_modbus_poller_init(void)    { g_modbus_poller_init_calls++; return g_modbus_poller_init_return; }
static bool         spy_modbus_poller_is_ready(void) { return g_modbus_poller_is_ready_return; }

static const imodbus_poller_t g_modbus_poller_spy = {
    .init     = spy_modbus_poller_init,
    .is_ready = spy_modbus_poller_is_ready,
};

/* --- update_service spy ------------------------------------------------ */
static uint32_t g_update_start_calls                = 0U;
static uint32_t g_update_resume_self_checking_calls = 0U;
static uint32_t g_update_resume_rollback_calls      = 0U;
static uint32_t g_update_resume_after_rollback_calls = 0U;

static gw_svc_err_t spy_update_init(void)                        { return GW_SVC_ERR_OK; }
static gw_svc_err_t spy_update_start(uint32_t sz)                { (void)sz; g_update_start_calls++; return GW_SVC_ERR_OK; }
static gw_svc_err_t spy_update_resume_self_checking(void)        { g_update_resume_self_checking_calls++; return GW_SVC_ERR_OK; }
static gw_svc_err_t spy_update_resume_rollback(void)             { g_update_resume_rollback_calls++; return GW_SVC_ERR_OK; }
static gw_svc_err_t spy_update_resume_after_rollback(void)       { g_update_resume_after_rollback_calls++; return GW_SVC_ERR_OK; }

static const iupdate_service_t g_update_spy = {
    .init                  = spy_update_init,
    .start                 = spy_update_start,
    .resume_self_checking  = spy_update_resume_self_checking,
    .resume_rollback       = spy_update_resume_rollback,
    .resume_after_rollback = spy_update_resume_after_rollback,
};

/* --- time_service spy -------------------------------------------------- */
static gw_svc_err_t spy_time_service_init(void) { return GW_SVC_ERR_OK; }
static const itime_service_t g_time_service_spy = { .init = spy_time_service_init };

/* --- firmware_store spy ----------------------------------------------- */
static bool         g_firmware_store_self_check_pending  = false;
static bool         g_firmware_store_rollback_pending    = false;
static uint32_t     g_firmware_store_confirm_calls       = 0U;
static uint32_t     g_firmware_store_rollback_calls      = 0U;

static gw_svc_err_t spy_firmware_store_get_pending_flags(bool *sc_out, bool *rb_out)
{
    if (sc_out) { *sc_out = g_firmware_store_self_check_pending; }
    if (rb_out) { *rb_out = g_firmware_store_rollback_pending; }
    return GW_SVC_ERR_OK;
}
static gw_svc_err_t spy_firmware_store_confirm_self_check(void) { g_firmware_store_confirm_calls++;  return GW_SVC_ERR_OK; }
static gw_svc_err_t spy_firmware_store_rollback(void)           { g_firmware_store_rollback_calls++; return GW_SVC_ERR_OK; }

static const ifirmware_store_t g_firmware_store_spy = {
    .get_pending_flags  = spy_firmware_store_get_pending_flags,
    .confirm_self_check = spy_firmware_store_confirm_self_check,
    .rollback           = spy_firmware_store_rollback,
};

/* --- reset_driver spy -------------------------------------------------- */
static uint32_t g_reset_driver_soft_reset_calls = 0U;

static void spy_soft_reset(void) { g_reset_driver_soft_reset_calls++; /* no return in production */ }

static const ireset_driver_t g_reset_driver_spy = { .soft_reset = spy_soft_reset };

/* --- health_admin spy -------------------------------------------------- */
static uint32_t g_health_admin_reset_metrics_calls = 0U;

static health_monitor_err_t spy_health_admin_reset_metrics(void)
{
    g_health_admin_reset_metrics_calls++;
    return HEALTH_MONITOR_ERR_OK;
}
static const ihealth_admin_t g_health_admin_spy = { .reset_metrics = spy_health_admin_reset_metrics };

/* ======================================================================= */
/* Helpers                                                                 */
/* ======================================================================= */

static void spy_reset_all(void)
{
    g_cfg_store_check_integrity_return = CONFIG_STORE_OK;
    g_cfg_store_load_return            = CONFIG_STORE_OK;
    g_cfg_store_load_calls             = 0U;
    g_cfg_mgr_apply_loaded_calls       = 0U;
    g_cfg_mgr_apply_loaded_return      = CONFIG_SERVICE_OK;
    g_cfg_mgr_snapshot_calls           = 0U;
    g_cfg_mgr_restore_calls            = 0U;
    g_cfg_mgr_flush_calls              = 0U;
    g_sensors_init_return              = SENSOR_SERVICE_ERR_OK;
    g_sensors_is_ready_return          = true;
    g_sensors_init_calls               = 0U;
    g_alarms_init_return               = ALARM_SERVICE_ERR_OK;
    g_console_init_finalise_calls      = 0U;
    g_health_push_event_calls          = 0U;
    g_health_push_event_last_event     = (health_event_t)0;
    g_health_push_event_last_param     = 0U;
    g_cloud_init_return                = GW_SVC_ERR_OK;
    g_cloud_is_ready_return            = true;
    g_cloud_flush_calls                = 0U;
    g_cloud_init_calls                 = 0U;
    g_cloud_rollback_result_calls      = 0U;
    g_modbus_poller_init_return        = GW_SVC_ERR_OK;
    g_modbus_poller_is_ready_return    = true;
    g_modbus_poller_init_calls         = 0U;
    g_update_start_calls               = 0U;
    g_update_resume_self_checking_calls= 0U;
    g_update_resume_rollback_calls     = 0U;
    g_update_resume_after_rollback_calls = 0U;
    g_firmware_store_self_check_pending= false;
    g_firmware_store_rollback_pending  = false;
    g_firmware_store_confirm_calls     = 0U;
    g_firmware_store_rollback_calls    = 0U;
    g_reset_driver_soft_reset_calls    = 0U;
    g_health_admin_reset_metrics_calls = 0U;
}

static lifecycle_err_t do_gw_init(void)
{
    return lifecycle_controller_init(
        LIFECYCLE_RESET_POWER_ON,
        &g_cfg_store_spy,
        &g_cfg_provider_spy,
        &g_cfg_mgr_spy,
        &g_sensors_spy,
        &g_alarms_spy,
        &g_console_spy,
        (const ihealth_report_t *)&g_health_spy,
        &g_cloud_spy,
        &g_modbus_poller_spy,
        &g_update_spy,
        &g_time_service_spy,
        &g_firmware_store_spy,
        &g_reset_driver_spy,
        &g_health_admin_spy
    );
}

static void drive_to_operational(void)
{
    mock_freertos_reset();
    g_mock_xQueueReceive_return = pdFALSE; /* no events during init */
    lifecycle_err_t err = do_gw_init();
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, err);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

static void queue_event(lifecycle_event_type_t type, uint32_t param)
{
    lifecycle_event_t ev = { .type = type, .param = param };
    (void)memcpy(g_mock_xQueueReceive_next_item, &ev, sizeof(ev));
    g_mock_xQueueReceive_next_item_size = sizeof(ev);
    g_mock_xQueueReceive_return         = pdTRUE;
}

/* ======================================================================= */
/* Unity fixtures                                                          */
/* ======================================================================= */

void setUp(void)
{
    stm32_cmsis_mock_reset();
    mock_freertos_reset();
    lifecycle_controller_reset_for_test();
    spy_reset_all();
}

void tearDown(void) {}

/* ======================================================================= */
/* TC-LC-050..053 — GW Init sub-state sequence (normal boot)               */
/* ======================================================================= */

void test_LC_050_normal_boot_reaches_OPERATIONAL(void)
{
    drive_to_operational();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_051_check_integrity_reads_pending_self_check_flag(void)
{
    g_firmware_store_self_check_pending = true;
    /* Pre-load SELF_CHECK_PASS so gw_await_self_check_result() can proceed */
    queue_event(LC_EVENT_SELF_CHECK_PASS, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(1U, g_update_resume_self_checking_calls);
}

void test_LC_052_check_integrity_reads_pending_rollback_flag(void)
{
    g_firmware_store_rollback_pending = true;
    g_mock_xQueueReceive_return = pdFALSE;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(1U, g_update_resume_after_rollback_calls);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_053_selfcheck_normal_probes_pass_goes_OPERATIONAL(void)
{
    g_sensors_is_ready_return       = true;
    g_modbus_poller_is_ready_return = true;
    g_cloud_is_ready_return         = true;
    g_mock_xQueueReceive_return     = pdFALSE;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_054_sensors_not_ready_goes_FAULTED(void)
{
    g_sensors_is_ready_return   = false;
    g_mock_xQueueReceive_return = pdFALSE;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

void test_LC_055_modbus_poller_not_ready_goes_FAULTED(void)
{
    g_modbus_poller_is_ready_return = false;
    g_mock_xQueueReceive_return     = pdFALSE;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

void test_LC_056_cloud_not_ready_goes_FAULTED(void)
{
    g_cloud_is_ready_return     = false;
    g_mock_xQueueReceive_return = pdFALSE;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

void test_LC_057_pending_self_check_calls_resume_self_checking(void)
{
    g_firmware_store_self_check_pending = true;
    queue_event(LC_EVENT_SELF_CHECK_PASS, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(1U, g_update_resume_self_checking_calls);
}

void test_LC_058_self_check_pass_confirms_and_goes_OPERATIONAL(void)
{
    g_firmware_store_self_check_pending = true;
    queue_event(LC_EVENT_SELF_CHECK_PASS, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(1U, g_firmware_store_confirm_calls);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_059_self_check_fail_triggers_rollback_and_reset(void)
{
    g_firmware_store_self_check_pending = true;
    queue_event(LC_EVENT_SELF_CHECK_FAIL, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(1U, g_update_resume_rollback_calls);
    TEST_ASSERT_EQUAL(1U, g_firmware_store_rollback_calls);
    TEST_ASSERT_EQUAL(1U, g_reset_driver_soft_reset_calls);
}

void test_LC_060_pending_rollback_calls_resume_after_rollback(void)
{
    g_firmware_store_rollback_pending = true;
    g_mock_xQueueReceive_return = pdFALSE;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(1U, g_update_resume_after_rollback_calls);
}

void test_LC_061_pending_rollback_reports_result_and_goes_OPERATIONAL(void)
{
    g_firmware_store_rollback_pending = true;
    g_mock_xQueueReceive_return = pdFALSE;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(1U, g_cloud_rollback_result_calls);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_062_no_pending_flags_uses_normal_probe_path(void)
{
    g_firmware_store_self_check_pending = false;
    g_firmware_store_rollback_pending   = false;
    g_mock_xQueueReceive_return         = pdFALSE;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_task_body(NULL);
    /* Neither resume_self_checking nor resume_after_rollback called */
    TEST_ASSERT_EQUAL(0U, g_update_resume_self_checking_calls);
    TEST_ASSERT_EQUAL(0U, g_update_resume_after_rollback_calls);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

/* ======================================================================= */
/* TC-LC-085..090 — Restarting (GW only)                                   */
/* ======================================================================= */

void test_LC_085_restart_requested_goes_RESTARTING(void)
{
    drive_to_operational();
    spy_reset_all();
    queue_event(LC_EVENT_RESTART_REQUESTED, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_RESTARTING, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(1U, g_cloud_flush_calls);
    TEST_ASSERT_GREATER_OR_EQUAL(1U, g_mock_xTimerStart_call_count);
}

void test_LC_086_restart_confirmed_calls_soft_reset(void)
{
    drive_to_operational();
    queue_event(LC_EVENT_RESTART_REQUESTED, 0U);
    lifecycle_task_body(NULL);
    spy_reset_all();
    queue_event(LC_EVENT_RESTART_CONFIRMED, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(1U, g_reset_driver_soft_reset_calls);
}

void test_LC_087_restart_timeout_returns_to_OPERATIONAL(void)
{
    drive_to_operational();
    queue_event(LC_EVENT_RESTART_REQUESTED, 0U);
    lifecycle_task_body(NULL);
    spy_reset_all();
    queue_event(LC_EVENT_RESTART_TIMEOUT, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_089_second_restart_requested_treated_as_confirmation(void)
{
    drive_to_operational();
    queue_event(LC_EVENT_RESTART_REQUESTED, 0U);
    lifecycle_task_body(NULL);
    spy_reset_all();
    /* Second RESTART_REQUESTED (same command re-sent) = confirmation */
    queue_event(LC_EVENT_RESTART_REQUESTED, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(1U, g_reset_driver_soft_reset_calls);
}

void test_LC_090_fault_during_restarting_goes_FAULTED(void)
{
    drive_to_operational();
    queue_event(LC_EVENT_RESTART_REQUESTED, 0U);
    lifecycle_task_body(NULL);
    spy_reset_all();
    queue_event(LC_EVENT_UNRECOVERABLE_FAULT, 0xEEU);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

/* ======================================================================= */
/* TC-LC-095..099 — UpdatingFirmware (GW only)                             */
/* ======================================================================= */

void test_LC_095_OTA_requested_goes_UPDATING_FW(void)
{
    drive_to_operational();
    spy_reset_all();
    queue_event(LC_EVENT_OTA_REQUESTED, 1024U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_UPDATING_FW, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(1U, g_update_start_calls);
}

void test_LC_097_self_check_pass_confirms_and_goes_OPERATIONAL(void)
{
    drive_to_operational();
    queue_event(LC_EVENT_OTA_REQUESTED, 512U);
    lifecycle_task_body(NULL);
    spy_reset_all();
    queue_event(LC_EVENT_SELF_CHECK_PASS, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(1U, g_firmware_store_confirm_calls);
}

void test_LC_098_self_check_fail_triggers_rollback_and_reset(void)
{
    drive_to_operational();
    queue_event(LC_EVENT_OTA_REQUESTED, 512U);
    lifecycle_task_body(NULL);
    spy_reset_all();
    queue_event(LC_EVENT_SELF_CHECK_FAIL, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(1U, g_update_resume_rollback_calls);
    TEST_ASSERT_EQUAL(1U, g_firmware_store_rollback_calls);
    TEST_ASSERT_EQUAL(1U, g_reset_driver_soft_reset_calls);
}

void test_LC_099_fault_during_updating_goes_FAULTED(void)
{
    drive_to_operational();
    queue_event(LC_EVENT_OTA_REQUESTED, 512U);
    lifecycle_task_body(NULL);
    spy_reset_all();
    queue_event(LC_EVENT_UNRECOVERABLE_FAULT, 0xFFU);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

/* ======================================================================= */
/* TC-LC-105..107 — Faulted state (GW)                                    */
/* ======================================================================= */

void test_LC_105_GW_fault_goes_FAULTED_and_reports(void)
{
    drive_to_operational();
    spy_reset_all();
    queue_event(LC_EVENT_UNRECOVERABLE_FAULT, 0x1234U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(1U, g_health_push_event_calls);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_FAULT, g_health_push_event_last_event);
    TEST_ASSERT_EQUAL(0x1234U, g_health_push_event_last_param);
}

void test_LC_107_GW_faulted_no_event_exits_faulted(void)
{
    drive_to_operational();
    queue_event(LC_EVENT_UNRECOVERABLE_FAULT, 1U);
    lifecycle_task_body(NULL);
    spy_reset_all();
    queue_event(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

/* ======================================================================= */
/* TC-LC-130..136 — Remote command dispatch (GW)                           */
/* ======================================================================= */

void test_LC_130_reset_metrics_calls_health_admin(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_err_t err = lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, err);
    TEST_ASSERT_EQUAL(1U, g_health_admin_reset_metrics_calls);
}

void test_LC_131_reset_metrics_does_not_change_state(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_state_t before = lifecycle_controller->get_state();
    (void)lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS);
    TEST_ASSERT_EQUAL(before, lifecycle_controller->get_state());
}

void test_LC_132_reset_metrics_does_not_post_event(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    uint32_t sends_before = g_mock_xQueueSend_call_count;
    (void)lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS);
    TEST_ASSERT_EQUAL(sends_before, g_mock_xQueueSend_call_count);
}

void test_LC_133_soft_restart_posts_RESTART_REQUESTED(void)
{
    g_mock_xQueueSend_last_item_size = sizeof(lifecycle_event_t);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_err_t err = lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_SOFT_RESTART);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, err);
    TEST_ASSERT_GREATER_OR_EQUAL(1U, g_mock_xQueueSend_call_count);
    lifecycle_event_t posted;
    (void)memcpy(&posted, g_mock_xQueueSend_last_item, sizeof(posted));
    TEST_ASSERT_EQUAL(LC_EVENT_RESTART_REQUESTED, posted.type);
}

void test_LC_134_soft_restart_full_queue_returns_QUEUE_FULL(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    g_mock_xQueueSend_return = pdFALSE;
    lifecycle_err_t err = lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_SOFT_RESTART);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_QUEUE_FULL, err);
}

void test_LC_135_unknown_command_returns_UNKNOWN_CMD(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_gw_init());
    lifecycle_err_t err = lifecycle_controller->handle_remote_command((lifecycle_remote_cmd_t)99U);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_UNKNOWN_CMD, err);
}

void test_LC_136_handle_remote_command_before_init_returns_NOT_INIT(void)
{
    /* No init called */
    lifecycle_err_t err = lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NOT_INIT, err);
}
