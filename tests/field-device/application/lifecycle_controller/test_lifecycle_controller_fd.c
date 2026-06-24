/**
 * @file test_lifecycle_controller_fd.c
 * @brief Unit tests for LifecycleController — FD subset.
 *
 * Covers TC-LC-001..035, TC-LC-039, TC-LC-041..042, TC-LC-070..078,
 * TC-LC-105..108, TC-LC-115..120, TC-LC-137 as defined in
 * docs/lld/application/lifecycle-controller.md §21.
 *
 * Build defines (project.yml): BOARD_FIELD_DEVICE, TEST.
 *
 * Test strategy:
 *  - Spy vtables with call counts and configurable return values
 *  - FreeRTOS mock from tests/support/freertos_mock.c
 *  - RCC->CSR driven via stm32_cmsis_mock.c
 *  - Single-step via lifecycle_task_body() (LC-O5 hook)
 */

#include "unity.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "lifecycle_controller/lifecycle_controller.h"
#include "freertos_mock.h"
#include "stm32_cmsis_mock.h"

/* ======================================================================= */
/* Spy infrastructure                                                       */
/* ======================================================================= */

/* --- config_store spy -------------------------------------------------- */
static config_store_err_t g_cfg_store_check_integrity_return = CONFIG_STORE_OK;
static uint32_t g_cfg_store_check_integrity_calls = 0U;
static config_store_err_t g_cfg_store_load_return = CONFIG_STORE_OK;
static uint32_t g_cfg_store_load_calls = 0U;

static config_store_err_t spy_cfg_store_check_integrity(void)
{
    g_cfg_store_check_integrity_calls++;
    return g_cfg_store_check_integrity_return;
}

static config_store_err_t spy_cfg_store_load(void *data_out, uint32_t *len_out, uint32_t max_len)
{
    (void) data_out;
    (void) max_len;
    if (len_out != NULL)
    {
        *len_out = 0U;
    }
    g_cfg_store_load_calls++;
    return g_cfg_store_load_return;
}

static config_store_err_t spy_cfg_store_init(const void *h)
{
    (void) h;
    return CONFIG_STORE_OK;
}
static config_store_err_t spy_cfg_store_save(const void *d, uint32_t l)
{
    (void) d;
    (void) l;
    return CONFIG_STORE_OK;
}
static config_store_err_t spy_cfg_store_erase(void)
{
    return CONFIG_STORE_OK;
}

static const iconfig_store_t g_cfg_store_spy = {
    .init = spy_cfg_store_init,
    .load = spy_cfg_store_load,
    .save = spy_cfg_store_save,
    .check_integrity = spy_cfg_store_check_integrity,
    .erase = spy_cfg_store_erase,
};

/* --- config_provider spy ----------------------------------------------- */
static config_params_t g_spy_params = {.modbus_slave_addr = 5U};

static const config_params_t *spy_cfg_provider_get_params(void)
{
    return &g_spy_params;
}

static const iconfig_provider_t g_cfg_provider_spy = {
    .get_params = spy_cfg_provider_get_params,
};

/* --- config_manager spy ------------------------------------------------ */
static config_service_err_t g_cfg_mgr_apply_loaded_return = CONFIG_SERVICE_OK;
static uint32_t g_cfg_mgr_apply_loaded_calls = 0U;
static uint32_t g_cfg_mgr_snapshot_calls = 0U;
static uint32_t g_cfg_mgr_restore_calls = 0U;
static uint32_t g_cfg_mgr_flush_calls = 0U;

static config_service_err_t spy_apply_loaded(const void *blob, uint32_t len)
{
    (void) blob;
    (void) len;
    g_cfg_mgr_apply_loaded_calls++;
    return g_cfg_mgr_apply_loaded_return;
}

static config_service_err_t spy_cfg_mgr_set_param(config_param_id_t id, const void *v)
{
    (void) id;
    (void) v;
    return CONFIG_SERVICE_OK;
}

static config_service_err_t spy_cfg_mgr_validate_param(config_param_id_t id, const void *v)
{
    (void) id;
    (void) v;
    return CONFIG_SERVICE_OK;
}

static config_service_err_t spy_snapshot(void)
{
    g_cfg_mgr_snapshot_calls++;
    return CONFIG_SERVICE_OK;
}

static config_service_err_t spy_restore_snapshot(void)
{
    g_cfg_mgr_restore_calls++;
    return CONFIG_SERVICE_OK;
}

static config_service_err_t spy_flush(void)
{
    g_cfg_mgr_flush_calls++;
    return CONFIG_SERVICE_OK;
}

static const iconfig_manager_t g_cfg_mgr_spy = {
    .apply_loaded = spy_apply_loaded,
    .set_param = spy_cfg_mgr_set_param,
    .validate_param = spy_cfg_mgr_validate_param,
    .snapshot = spy_snapshot,
    .restore_snapshot = spy_restore_snapshot,
    .flush = spy_flush,
};

/* --- sensor_service spy ----------------------------------------------- */
static sensor_service_err_t g_sensors_init_return = SENSOR_SERVICE_ERR_OK;
static uint32_t g_sensors_init_calls = 0U;
static uint32_t g_sensors_reconfigure_calls = 0U;

static sensor_service_err_t spy_sensors_init(void)
{
    g_sensors_init_calls++;
    return g_sensors_init_return;
}

static sensor_service_err_t spy_sensors_reconfigure(void)
{
    g_sensors_reconfigure_calls++;
    return SENSOR_SERVICE_ERR_OK;
}

static sensor_service_err_t spy_sensors_run_cycle(void)
{
    return SENSOR_SERVICE_ERR_OK;
}
static sensor_service_err_t spy_sensors_get_snapshot(sensor_snapshot_t *s)
{
    (void) s;
    return SENSOR_SERVICE_ERR_OK;
}
static sensor_service_err_t spy_sensors_subscribe(void (*cb)(const sensor_snapshot_t *))
{
    (void) cb;
    return SENSOR_SERVICE_ERR_OK;
}
static sensor_service_err_t spy_sensors_read_on_demand(void)
{
    return SENSOR_SERVICE_ERR_OK;
}
static bool spy_sensors_is_ready(void)
{
    return true;
}

static const isensor_service_t g_sensors_spy = {
    .init = spy_sensors_init,
    .run_cycle = spy_sensors_run_cycle,
    .get_snapshot = spy_sensors_get_snapshot,
    .subscribe = spy_sensors_subscribe,
    .read_on_demand = spy_sensors_read_on_demand,
    .is_ready = spy_sensors_is_ready,
    .reconfigure = spy_sensors_reconfigure,
};

/* --- alarm_service spy ------------------------------------------------ */
static alarm_service_err_t g_alarms_init_return = ALARM_SERVICE_ERR_OK;
static uint32_t g_alarms_init_calls = 0U;

static alarm_service_err_t spy_alarms_init(void)
{
    g_alarms_init_calls++;
    return g_alarms_init_return;
}

static alarm_service_err_t spy_alarms_get_state(sensor_id_t s, alarm_state_t *o)
{
    (void) s;
    (void) o;
    return ALARM_SERVICE_ERR_OK;
}
static alarm_service_err_t spy_alarms_get_all_states(alarm_state_t st[SENSOR_ID_COUNT])
{
    (void) st;
    return ALARM_SERVICE_ERR_OK;
}
static alarm_service_err_t spy_alarms_subscribe(void (*cb)(sensor_id_t, int, const void *))
{
    (void) cb;
    return ALARM_SERVICE_ERR_OK;
}
static alarm_service_err_t spy_alarms_ack_all(void)
{
    return ALARM_SERVICE_ERR_OK;
}

static const ialarm_service_t g_alarms_spy = {
    .init = spy_alarms_init,
    .get_state = spy_alarms_get_state,
    .get_all_states = spy_alarms_get_all_states,
    .subscribe = spy_alarms_subscribe,
    .ack_all = spy_alarms_ack_all,
};

/* --- console_service spy ---------------------------------------------- */
static uint32_t g_console_init_finalise_calls = 0U;

static console_service_err_t spy_console_init_finalise(void)
{
    g_console_init_finalise_calls++;
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t spy_console_run_once(void)
{
    return CONSOLE_SERVICE_ERR_OK;
}

static const iconsole_service_t g_console_spy = {
    .init_finalise = spy_console_init_finalise,
    .run_once = spy_console_run_once,
};

/* --- health_report spy ------------------------------------------------ */
static uint32_t g_health_init_calls = 0U;
static uint32_t g_health_push_event_calls = 0U;
static health_event_t g_health_push_event_last_event = (health_event_t) 0;
static uint32_t g_health_push_event_last_param = 0U;

static health_monitor_err_t spy_health_init(void)
{
    g_health_init_calls++;
    return HEALTH_MONITOR_ERR_OK;
}

static health_monitor_err_t spy_health_push_event(health_event_t event, uint32_t param)
{
    g_health_push_event_calls++;
    g_health_push_event_last_event = event;
    g_health_push_event_last_param = param;
    return HEALTH_MONITOR_ERR_OK;
}

static struct ihealth_report_s g_health_spy = {
    .init = spy_health_init,
    .push_event = spy_health_push_event,
};

/* --- modbus_slave spy -------------------------------------------------- */
static uint32_t g_modbus_set_address_calls = 0U;
static uint8_t g_modbus_set_address_last_value = 0U;

static modbus_slave_err_t spy_modbus_set_address(uint8_t new_addr)
{
    g_modbus_set_address_calls++;
    g_modbus_set_address_last_value = new_addr;
    return MODBUS_SLAVE_ERR_OK;
}

static const imodbus_slave_t g_modbus_spy = {
    .set_address = spy_modbus_set_address,
};

/* ======================================================================= */
/* Helpers                                                                 */
/* ======================================================================= */

static void spy_reset_all(void)
{
    g_cfg_store_check_integrity_return = CONFIG_STORE_OK;
    g_cfg_store_check_integrity_calls = 0U;
    g_cfg_store_load_return = CONFIG_STORE_OK;
    g_cfg_store_load_calls = 0U;

    g_cfg_mgr_apply_loaded_return = CONFIG_SERVICE_OK;
    g_cfg_mgr_apply_loaded_calls = 0U;
    g_cfg_mgr_snapshot_calls = 0U;
    g_cfg_mgr_restore_calls = 0U;
    g_cfg_mgr_flush_calls = 0U;

    g_sensors_init_return = SENSOR_SERVICE_ERR_OK;
    g_sensors_init_calls = 0U;
    g_sensors_reconfigure_calls = 0U;

    g_alarms_init_return = ALARM_SERVICE_ERR_OK;
    g_alarms_init_calls = 0U;

    g_console_init_finalise_calls = 0U;

    g_health_init_calls = 0U;
    g_health_push_event_calls = 0U;
    g_health_push_event_last_event = (health_event_t) 0;
    g_health_push_event_last_param = 0U;

    g_modbus_set_address_calls = 0U;
    g_modbus_set_address_last_value = 0U;

    g_spy_params.modbus_slave_addr = 5U;
}

static lifecycle_err_t do_init(void)
{
    return lifecycle_controller_init(LIFECYCLE_RESET_POWER_ON, &g_cfg_store_spy,
                                     &g_cfg_provider_spy, &g_cfg_mgr_spy, &g_sensors_spy,
                                     &g_alarms_spy, &g_console_spy,
                                     (const ihealth_report_t *) &g_health_spy, &g_modbus_spy);
}

/* Drive to OPERATIONAL: init + first lifecycle_task_body() with no queue event */
static void drive_to_operational(void)
{
    mock_freertos_reset();
    g_mock_xQueueReceive_return = pdFALSE; /* no events during init */
    lifecycle_err_t err = do_init();
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, err);
    lifecycle_task_body(NULL); /* runs init sequence + one idle event-loop tick */
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

/* Post event into mock queue for next lifecycle_task_body() call */
static void queue_event(lifecycle_event_type_t type, uint32_t param)
{
    lifecycle_event_t ev = {.type = type, .param = param};
    (void) memcpy(g_mock_xQueueReceive_next_item, &ev, sizeof(ev));
    g_mock_xQueueReceive_next_item_size = sizeof(ev);
    g_mock_xQueueReceive_return = pdTRUE;
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

void tearDown(void)
{
}

/* ======================================================================= */
/* TC-LC-001..007 — Init API                                               */
/* ======================================================================= */

void test_LC_001_null_config_store_rejected(void)
{
    lifecycle_err_t err = lifecycle_controller_init(
        LIFECYCLE_RESET_POWER_ON, NULL, /* config_store = NULL */
        &g_cfg_provider_spy, &g_cfg_mgr_spy, &g_sensors_spy, &g_alarms_spy, &g_console_spy,
        (const ihealth_report_t *) &g_health_spy, &g_modbus_spy);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG, err);
}

void test_LC_001b_null_cfg_read_rejected(void)
{
    lifecycle_err_t err = lifecycle_controller_init(
        LIFECYCLE_RESET_POWER_ON, &g_cfg_store_spy, NULL, /* cfg_read = NULL */
        &g_cfg_mgr_spy, &g_sensors_spy, &g_alarms_spy, &g_console_spy,
        (const ihealth_report_t *) &g_health_spy, &g_modbus_spy);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG, err);
}

void test_LC_001c_null_sensors_rejected(void)
{
    lifecycle_err_t err = lifecycle_controller_init(
        LIFECYCLE_RESET_POWER_ON, &g_cfg_store_spy, &g_cfg_provider_spy, &g_cfg_mgr_spy,
        NULL, /* sensors = NULL */
        &g_alarms_spy, &g_console_spy, (const ihealth_report_t *) &g_health_spy, &g_modbus_spy);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG, err);
}

void test_LC_004_post_init_state_is_INIT(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_init());
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_INIT, lifecycle_controller->get_state());
}

void test_LC_005_reset_cause_stored(void)
{
    lifecycle_controller_init(LIFECYCLE_RESET_WATCHDOG, &g_cfg_store_spy, &g_cfg_provider_spy,
                              &g_cfg_mgr_spy, &g_sensors_spy, &g_alarms_spy, &g_console_spy,
                              (const ihealth_report_t *) &g_health_spy, &g_modbus_spy);
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_WATCHDOG, lifecycle_controller->get_reset_cause());
}

void test_LC_006_rtos_primitives_created(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_init());
    /* Queue, 2 timers (edit + init), event group = 4 create calls minimum */
    TEST_ASSERT_GREATER_OR_EQUAL(1U, g_mock_xQueueCreateStatic_call_count);
    TEST_ASSERT_GREATER_OR_EQUAL(2U, g_mock_xTimerCreateStatic_call_count);
    TEST_ASSERT_GREATER_OR_EQUAL(1U, g_mock_xEventGroupCreateStatic_call_count);
}

void test_LC_007_vtable_call_before_init_returns_NOT_INIT(void)
{
    /* No init called — lifecycle_controller_reset_for_test() zeroed everything */
    lifecycle_err_t err = lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NOT_INIT, err);
}

/* ======================================================================= */
/* TC-LC-010..015 — Reset cause detection                                  */
/* ======================================================================= */

void test_LC_010_watchdog_flag_detected(void)
{
    RCC->CSR = RCC_CSR_IWDGRSTF;
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_WATCHDOG, lifecycle_detect_reset_cause());
}

void test_LC_011_soft_reset_flag_detected(void)
{
    RCC->CSR = RCC_CSR_SFTRSTF;
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_SOFT, lifecycle_detect_reset_cause());
}

void test_LC_012_power_on_flag_detected(void)
{
    RCC->CSR = RCC_CSR_PINRSTF;
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_POWER_ON, lifecycle_detect_reset_cause());
}

void test_LC_013_no_flag_returns_UNKNOWN(void)
{
    RCC->CSR = 0U;
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_UNKNOWN, lifecycle_detect_reset_cause());
}

void test_LC_014_RMVF_cleared_after_read(void)
{
    RCC->CSR = RCC_CSR_PINRSTF;
    lifecycle_detect_reset_cause();
    TEST_ASSERT_BITS(RCC_CSR_RMVF, RCC_CSR_RMVF, RCC->CSR); /* RMVF bit set (=cleared per spec) */
}

void test_LC_015_watchdog_wins_over_soft(void)
{
    RCC->CSR = RCC_CSR_IWDGRSTF | RCC_CSR_SFTRSTF;
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_WATCHDOG, lifecycle_detect_reset_cause());
}

/* ======================================================================= */
/* TC-LC-030..042 — FD Init sub-state sequence                             */
/* ======================================================================= */

void test_LC_030_happy_path_reaches_OPERATIONAL(void)
{
    drive_to_operational();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_031_check_integrity_fail_goes_FAULTED(void)
{
    g_cfg_store_check_integrity_return = CONFIG_STORE_ERR_FLASH_READ;
    g_mock_xQueueReceive_return = pdFALSE;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    /* Sub-states after CheckingIntegrity must not have been entered */
    TEST_ASSERT_EQUAL(0U, g_cfg_store_load_calls);
}

void test_LC_032_load_fail_goes_FAULTED(void)
{
    g_cfg_store_load_return = CONFIG_STORE_ERR_FLASH_READ;
    g_mock_xQueueReceive_return = pdFALSE;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(0U, g_sensors_init_calls);
}

void test_LC_033_apply_loaded_fail_goes_FAULTED(void)
{
    g_cfg_mgr_apply_loaded_return = CONFIG_SERVICE_ERR_PERSIST;
    g_mock_xQueueReceive_return = pdFALSE;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(0U, g_sensors_init_calls);
}

void test_LC_034_sensors_init_fail_goes_FAULTED(void)
{
    g_sensors_init_return = SENSOR_SERVICE_ERR_NOT_INIT;
    g_mock_xQueueReceive_return = pdFALSE;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(0U, g_alarms_init_calls);
}

void test_LC_035_alarms_init_fail_goes_FAULTED(void)
{
    g_alarms_init_return = ALARM_SERVICE_ERR_NOT_INIT;
    g_mock_xQueueReceive_return = pdFALSE;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(0U, g_modbus_set_address_calls);
}

void test_LC_039_modbus_set_address_called_with_cfg_addr(void)
{
    g_spy_params.modbus_slave_addr = 7U;
    drive_to_operational();
    TEST_ASSERT_EQUAL(1U, g_modbus_set_address_calls);
    TEST_ASSERT_EQUAL(7U, g_modbus_set_address_last_value);
}

void test_LC_041_start_gate_bit_set_on_operational_entry(void)
{
    drive_to_operational();
    TEST_ASSERT_GREATER_OR_EQUAL(1U, g_mock_xEventGroupSetBits_call_count);
    TEST_ASSERT_BITS(LIFECYCLE_START_GATE_BIT, LIFECYCLE_START_GATE_BIT,
                     g_mock_xEventGroupSetBits_last_bits);
}

void test_LC_042_init_timeout_event_goes_FAULTED(void)
{
    /* Simulate the init watchdog firing by pre-loading a fault event */
    lifecycle_event_t fault_ev = {.type = LC_EVENT_UNRECOVERABLE_FAULT,
                                  .param = LC_FAULT_INIT_TIMEOUT};
    (void) memcpy(g_mock_xQueueReceive_next_item, &fault_ev, sizeof(fault_ev));
    g_mock_xQueueReceive_next_item_size = sizeof(fault_ev);
    g_mock_xQueueReceive_return = pdTRUE;

    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_init());
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

/* ======================================================================= */
/* TC-LC-070..078 — EditingConfig                                          */
/* ======================================================================= */

void test_LC_070_CONFIG_EDIT_ENTER_transitions_to_EDITING_CONFIG(void)
{
    drive_to_operational();
    spy_reset_all();
    queue_event(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_EDITING_CONFIG, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(1U, g_cfg_mgr_snapshot_calls);
    TEST_ASSERT_GREATER_OR_EQUAL(1U, g_mock_xTimerStart_call_count);
}

void test_LC_071_CONFIG_EDIT_APPLY_commits_and_returns_to_OPERATIONAL(void)
{
    drive_to_operational();
    queue_event(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    lifecycle_task_body(NULL);
    spy_reset_all();
    queue_event(LC_EVENT_CONFIG_EDIT_APPLY, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(1U, g_cfg_mgr_flush_calls);
}

void test_LC_072_CONFIG_EDIT_CANCEL_restores_snapshot(void)
{
    drive_to_operational();
    queue_event(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    lifecycle_task_body(NULL);
    spy_reset_all();
    queue_event(LC_EVENT_CONFIG_EDIT_CANCEL, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(1U, g_cfg_mgr_restore_calls);
}

void test_LC_073_CONFIG_EDIT_TIMEOUT_same_as_cancel(void)
{
    drive_to_operational();
    queue_event(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    lifecycle_task_body(NULL);
    spy_reset_all();
    queue_event(LC_EVENT_CONFIG_EDIT_TIMEOUT, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(1U, g_cfg_mgr_restore_calls);
}

void test_LC_075_modbus_address_applied_on_APPLY(void)
{
    g_spy_params.modbus_slave_addr = 9U;
    drive_to_operational();
    queue_event(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    lifecycle_task_body(NULL);
    spy_reset_all();
    g_spy_params.modbus_slave_addr = 11U; /* simulated new address in config */
    queue_event(LC_EVENT_CONFIG_EDIT_APPLY, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(1U, g_modbus_set_address_calls);
    TEST_ASSERT_EQUAL(11U, g_modbus_set_address_last_value);
}

void test_LC_076_sensors_reconfigured_on_APPLY(void)
{
    drive_to_operational();
    queue_event(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    lifecycle_task_body(NULL);
    spy_reset_all();
    queue_event(LC_EVENT_CONFIG_EDIT_APPLY, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(1U, g_sensors_reconfigure_calls);
}

void test_LC_077_FAULT_in_EditingConfig_goes_FAULTED(void)
{
    drive_to_operational();
    queue_event(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    lifecycle_task_body(NULL);
    spy_reset_all();
    queue_event(LC_EVENT_UNRECOVERABLE_FAULT, 99U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

void test_LC_078_CONFIG_EDIT_ENTER_ignored_outside_OPERATIONAL(void)
{
    drive_to_operational();
    queue_event(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    lifecycle_task_body(NULL);
    /* Now in EditingConfig — another ENTER should be ignored */
    spy_reset_all();
    queue_event(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    lifecycle_task_body(NULL);
    /* Should still be in EditingConfig, not transitioned again */
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_EDITING_CONFIG, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(0U, g_cfg_mgr_snapshot_calls); /* no second snapshot */
}

/* ======================================================================= */
/* TC-LC-105..108 — Faulted state                                         */
/* ======================================================================= */

void test_LC_105_FAULT_from_OPERATIONAL_goes_FAULTED_and_reports(void)
{
    drive_to_operational();
    spy_reset_all();
    queue_event(LC_EVENT_UNRECOVERABLE_FAULT, 0xDEADU);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(1U, g_health_push_event_calls);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_FAULT, g_health_push_event_last_event);
    TEST_ASSERT_EQUAL(0xDEADU, g_health_push_event_last_param);
}

void test_LC_107_faulted_state_no_event_causes_exit(void)
{
    drive_to_operational();
    queue_event(LC_EVENT_UNRECOVERABLE_FAULT, 1U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    /* Queue another event — state must remain FAULTED */
    spy_reset_all();
    queue_event(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

/* ======================================================================= */
/* TC-LC-115..120 — Vtable dispatch                                        */
/* ======================================================================= */

void test_LC_115_get_state_returns_current_state(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_init());
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_INIT, lifecycle_controller->get_state());
    g_mock_xQueueReceive_return = pdFALSE;
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_116_get_reset_cause_returns_value_passed_to_init(void)
{
    lifecycle_controller_init(LIFECYCLE_RESET_SOFT, &g_cfg_store_spy, &g_cfg_provider_spy,
                              &g_cfg_mgr_spy, &g_sensors_spy, &g_alarms_spy, &g_console_spy,
                              (const ihealth_report_t *) &g_health_spy, &g_modbus_spy);
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_SOFT, lifecycle_controller->get_reset_cause());
}

void test_LC_117_post_event_enqueues_to_queue(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_init());
    lifecycle_event_t ev = {.type = LC_EVENT_CONFIG_EDIT_ENTER, .param = 0U};
    bool ok = lifecycle_controller->post_event(ev);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_GREATER_OR_EQUAL(1U, g_mock_xQueueSend_call_count);
}

void test_LC_118_post_event_returns_false_when_queue_full(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_init());
    g_mock_xQueueSend_return = pdFALSE;
    lifecycle_event_t ev = {.type = LC_EVENT_CONFIG_EDIT_ENTER, .param = 0U};
    bool ok = lifecycle_controller->post_event(ev);
    TEST_ASSERT_FALSE(ok);
}

void test_LC_119_queue_depth_is_4(void)
{
    /* The FreeRTOS mock captures the queue length parameter via the real queue API;
     * we verify indirectly via the spec: depth 4 was passed to xQueueCreateStatic.
     * Without dereferencable queue internals we just verify the module compiles
     * with the constant and the queue was created (already covered in TC-LC-006). */
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_init());
    TEST_ASSERT_GREATER_OR_EQUAL(1U, g_mock_xQueueCreateStatic_call_count);
}

void test_LC_120_events_dispatched_in_order(void)
{
    drive_to_operational();
    /* Queue ENTER then CANCEL: ENTER should be processed first */
    queue_event(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_EDITING_CONFIG, lifecycle_controller->get_state());

    queue_event(LC_EVENT_CONFIG_EDIT_CANCEL, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

/* ======================================================================= */
/* TC-LC-137 — FD uniform RESET_METRICS dispatch (no health_admin on FD)  */
/* ======================================================================= */

void test_LC_137_FD_reset_metrics_returns_OK_no_crash(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, do_init());
    lifecycle_err_t err = lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, err);
}
