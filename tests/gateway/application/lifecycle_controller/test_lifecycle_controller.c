/**
 * @file test_lifecycle_controller.c
 * @brief Unity tests for LifecycleController (Gateway).
 *
 * Every collaborator is a spy defined in this file; the test includes none of their headers, so
 * Ceedling links none of their .c files. FreeRTOS comes from freertos_mock, RCC from the L475
 * CMSIS mock. Test IDs follow companion §21 (TC-LC-001..137); the FD-only cases (TC-LC-002,
 * 030..042, 075, 091, 137) have no Gateway counterpart. TC-LC-14x.. are additions.
 *
 * Driving the machine: lifecycle_task_body() runs the Init sequence once and then handles at most
 * one event per call (LC-O5). boot() runs Init; step() arms one event on the mocked queue and
 * calls the task body. Events are armed AFTER boot() because Init polls the queue between
 * sub-states.
 */

#include <stddef.h>
#include <string.h>

#include "freertos_mock.h"
#include "lifecycle_controller.h"
#include "stm32l475_cmsis_mock.h"
#include "unity.h"

/* ======================================================================= */
/* Spy infrastructure                                                       */
/* ======================================================================= */

typedef enum
{
    C_CS_CHECK,
    C_CS_LOAD,
    C_CM_APPLY,
    C_CM_SNAPSHOT,
    C_CM_RESTORE,
    C_CM_COMMIT,
    C_SENS_INIT,
    C_SENS_READY,
    C_SENS_RECONF,
    C_ALARM_INIT,
    C_CON_INIT,
    C_CLOUD_INIT,
    C_CLOUD_FLUSH,
    C_CLOUD_READY,
    C_POLL_INIT,
    C_POLL_READY,
    C_TIME_INIT,
    C_UPD_INIT,
    C_UPD_START,
    C_UPD_SELF,
    C_UPD_RB,
    C_UPD_AFTER_RB,
    C_FW_FLAGS,
    C_FW_CONFIRM,
    C_FW_ROLLBACK,
    C_RESET,
    C_HR_INIT,
    C_HR_FAULT,
    C_HR_LIFE,
    C_HA_RESET,
    C_HA_LED,
    C_WDG,
    C_COUNT
} call_id_t;

#define TRACE_MAX (64U)
#define SNAPSHOT_PATTERN (0xA5U)
#define SNAPSHOT_LEN (48U)
#define CTX_MARKER_VALUE (0x5AU)

static uint32_t g_calls[C_COUNT];
static lc_dep_status_t g_status[C_COUNT];
static bool g_ready[C_COUNT];
static call_id_t g_trace[TRACE_MAX];
static uint32_t g_trace_len;

static bool g_flag_self_check;
static bool g_flag_rollback;
static uint32_t g_fault_code_seen;
static lifecycle_state_t g_life_seen[16];
static uint32_t g_life_count;
static uint32_t g_load_max_seen;
static uint32_t g_snapshot_max_seen;
static uint32_t g_restore_len_seen;
static uint8_t g_restore_first_byte;
static uint32_t g_update_size_seen;
static uint8_t g_ctx_marker = CTX_MARKER_VALUE;
static const void *g_cs_ctx_seen;

static void note(call_id_t id)
{
    g_calls[id]++;
    if (g_trace_len < TRACE_MAX)
    {
        g_trace[g_trace_len] = id;
        g_trace_len++;
    }
}

#define SPY_STATUS(fn, id)                                                                         \
    static lc_dep_status_t fn(void *ctx)                                                           \
    {                                                                                              \
        (void) ctx;                                                                                \
        note(id);                                                                                  \
        return g_status[id];                                                                       \
    }

#define SPY_READY(fn, id)                                                                          \
    static bool fn(void *ctx)                                                                      \
    {                                                                                              \
        (void) ctx;                                                                                \
        note(id);                                                                                  \
        return g_ready[id];                                                                        \
    }

static lc_dep_status_t spy_cs_check(void *ctx)
{
    g_cs_ctx_seen = ctx;
    note(C_CS_CHECK);
    return g_status[C_CS_CHECK];
}

static lc_dep_status_t spy_cs_load(void *ctx, void *buf, uint32_t *len_out, uint32_t max_len)
{
    (void) ctx;
    (void) buf;
    note(C_CS_LOAD);
    g_load_max_seen = max_len;
    *len_out = SNAPSHOT_LEN;
    return g_status[C_CS_LOAD];
}

static lc_dep_status_t spy_cm_apply(void *ctx, const void *buf, uint32_t len)
{
    (void) ctx;
    (void) buf;
    (void) len;
    note(C_CM_APPLY);
    return g_status[C_CM_APPLY];
}

static lc_dep_status_t spy_cm_snapshot(void *ctx, void *buf, uint32_t *len_out, uint32_t max_len)
{
    (void) ctx;
    note(C_CM_SNAPSHOT);
    g_snapshot_max_seen = max_len;
    (void) memset(buf, (int) SNAPSHOT_PATTERN, SNAPSHOT_LEN);
    *len_out = SNAPSHOT_LEN;
    return g_status[C_CM_SNAPSHOT];
}

static lc_dep_status_t spy_cm_restore(void *ctx, const void *buf, uint32_t len)
{
    (void) ctx;
    note(C_CM_RESTORE);
    g_restore_len_seen = len;
    g_restore_first_byte = ((const uint8_t *) buf)[0];
    return g_status[C_CM_RESTORE];
}

SPY_STATUS(spy_cm_commit, C_CM_COMMIT)
SPY_STATUS(spy_sens_init, C_SENS_INIT)
SPY_READY(spy_sens_ready, C_SENS_READY)
SPY_STATUS(spy_sens_reconf, C_SENS_RECONF)
SPY_STATUS(spy_alarm_init, C_ALARM_INIT)
SPY_STATUS(spy_con_init, C_CON_INIT)
SPY_STATUS(spy_cloud_init, C_CLOUD_INIT)
SPY_STATUS(spy_cloud_flush, C_CLOUD_FLUSH)
SPY_READY(spy_cloud_ready, C_CLOUD_READY)
SPY_STATUS(spy_poll_init, C_POLL_INIT)
SPY_READY(spy_poll_ready, C_POLL_READY)
SPY_STATUS(spy_time_init, C_TIME_INIT)
SPY_STATUS(spy_upd_init, C_UPD_INIT)
SPY_STATUS(spy_upd_self, C_UPD_SELF)
SPY_STATUS(spy_upd_rb, C_UPD_RB)
SPY_STATUS(spy_upd_after_rb, C_UPD_AFTER_RB)
SPY_STATUS(spy_fw_confirm, C_FW_CONFIRM)
SPY_STATUS(spy_fw_rollback, C_FW_ROLLBACK)
SPY_STATUS(spy_hr_init, C_HR_INIT)
SPY_STATUS(spy_ha_reset, C_HA_RESET)
SPY_STATUS(spy_ha_led, C_HA_LED)

static lc_dep_status_t spy_upd_start(void *ctx, uint32_t image_size)
{
    (void) ctx;
    note(C_UPD_START);
    g_update_size_seen = image_size;
    return g_status[C_UPD_START];
}

static lc_dep_status_t spy_fw_flags(void *ctx, bool *self_check_out, bool *rollback_out)
{
    (void) ctx;
    note(C_FW_FLAGS);
    *self_check_out = g_flag_self_check;
    *rollback_out = g_flag_rollback;
    return g_status[C_FW_FLAGS];
}

static void spy_reset(void *ctx)
{
    (void) ctx;
    note(C_RESET);
}

static lc_dep_status_t spy_hr_fault(void *ctx, uint32_t fault_code)
{
    (void) ctx;
    note(C_HR_FAULT);
    g_fault_code_seen = fault_code;
    return g_status[C_HR_FAULT];
}

static lc_dep_status_t spy_hr_life(void *ctx, lifecycle_state_t state)
{
    (void) ctx;
    note(C_HR_LIFE);
    if (g_life_count < (sizeof(g_life_seen) / sizeof(g_life_seen[0])))
    {
        g_life_seen[g_life_count] = state;
    }
    g_life_count++;
    return g_status[C_HR_LIFE];
}

static void spy_wdg(void *ctx)
{
    (void) ctx;
    note(C_WDG);
}

static const lc_config_store_if_t k_config_store = {
    .ctx = &g_ctx_marker, .check_integrity = spy_cs_check, .load = spy_cs_load};
static const lc_config_manager_if_t k_config_manager = {.apply_loaded = spy_cm_apply,
                                                        .snapshot = spy_cm_snapshot,
                                                        .restore_snapshot = spy_cm_restore,
                                                        .commit = spy_cm_commit};
static const lc_sensors_if_t k_sensors = {
    .init = spy_sens_init, .is_ready = spy_sens_ready, .reconfigure = spy_sens_reconf};
static const lc_alarms_if_t k_alarms = {.init = spy_alarm_init};
static const lc_console_if_t k_console = {.init_finalise = spy_con_init};
static const lc_health_report_if_t k_health_report = {
    .init = spy_hr_init, .push_fault = spy_hr_fault, .update_lifecycle = spy_hr_life};
static const lc_health_admin_if_t k_health_admin = {.reset_metrics = spy_ha_reset,
                                                    .set_led_fault = spy_ha_led};
static const lc_cloud_if_t k_cloud = {
    .init = spy_cloud_init, .flush = spy_cloud_flush, .is_ready = spy_cloud_ready};
static const lc_modbus_poller_if_t k_poller = {.init = spy_poll_init, .is_ready = spy_poll_ready};
static const lc_update_service_if_t k_update = {.init = spy_upd_init,
                                                .start = spy_upd_start,
                                                .resume_self_checking = spy_upd_self,
                                                .resume_rollback = spy_upd_rb,
                                                .resume_after_rollback = spy_upd_after_rb};
static const lc_time_service_if_t k_time = {.init = spy_time_init};
static const lc_firmware_store_if_t k_firmware = {.get_pending_flags = spy_fw_flags,
                                                  .confirm_self_check = spy_fw_confirm,
                                                  .rollback = spy_fw_rollback};
static const lc_reset_driver_if_t k_reset = {.soft_reset = spy_reset};
static const lc_watchdog_if_t k_watchdog = {.kick = spy_wdg};

static lifecycle_deps_t g_deps;

static void spy_reset_all(void)
{
    (void) memset(g_calls, 0, sizeof(g_calls));
    (void) memset(g_trace, 0, sizeof(g_trace));
    g_trace_len = 0U;
    for (uint32_t i = 0U; i < (uint32_t) C_COUNT; i++)
    {
        g_status[i] = LC_DEP_OK;
        g_ready[i] = true;
    }
    g_flag_self_check = false;
    g_flag_rollback = false;
    g_fault_code_seen = 0U;
    (void) memset(g_life_seen, 0, sizeof(g_life_seen));
    g_life_count = 0U;
    g_load_max_seen = 0U;
    g_snapshot_max_seen = 0U;
    g_restore_len_seen = 0U;
    g_restore_first_byte = 0U;
    g_update_size_seen = 0U;
    g_cs_ctx_seen = NULL;

    g_deps = (lifecycle_deps_t){.config_store = &k_config_store,
                                .config_manager = &k_config_manager,
                                .sensors = &k_sensors,
                                .alarms = &k_alarms,
                                .console = &k_console,
                                .health_report = &k_health_report,
                                .health_admin = &k_health_admin,
                                .cloud = &k_cloud,
                                .modbus_poller = &k_poller,
                                .update_service = &k_update,
                                .time_service = &k_time,
                                .firmware_store = &k_firmware,
                                .reset_driver = &k_reset,
                                .watchdog = &k_watchdog};
}

/* ======================================================================= */
/* Drivers                                                                  */
/* ======================================================================= */

void setUp(void)
{
    stm32l475_cmsis_mock_reset();
    mock_freertos_reset();
    lifecycle_controller_reset_for_test();
    spy_reset_all();
    /* Nothing is delivered unless a test arms an event. */
    g_mock_xQueueReceive_available = 0U;
    /* Capture whole events from xQueueSend. */
    g_mock_xQueueSend_last_item_size = sizeof(lifecycle_event_t);
}

void tearDown(void)
{
}

static lifecycle_err_t init_ok(void)
{
    return lifecycle_controller_init(LIFECYCLE_RESET_POWER_ON, &g_deps);
}

/** Initialise and run the Init sequence (plus one empty event pass). */
static void boot(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, init_ok());
    lifecycle_task_body(NULL);
}

/** Arm one event for the next xQueueReceive. */
static void arm(lifecycle_event_type_t type, uint32_t param)
{
    const lifecycle_event_t ev = {.type = type, .param = param};

    g_mock_xQueueReceive_return = pdTRUE;
    (void) memcpy(g_mock_xQueueReceive_next_item, &ev, sizeof(ev));
    g_mock_xQueueReceive_next_item_size = sizeof(ev);
    g_mock_xQueueReceive_available = 1U;
}

/** Deliver one event to a booted controller. */
static void step(lifecycle_event_type_t type, uint32_t param)
{
    arm(type, param);
    lifecycle_task_body(NULL);
}

static void boot_operational(void)
{
    boot();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

static void boot_editing(void)
{
    boot_operational();
    step(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_EDITING_CONFIG, lifecycle_controller->get_state());
}

static void boot_restarting(void)
{
    boot_operational();
    step(LC_EVENT_RESTART_REQUESTED, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_RESTARTING, lifecycle_controller->get_state());
}

static void boot_updating(uint32_t size)
{
    boot_operational();
    step(LC_EVENT_OTA_REQUESTED, size);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_UPDATING_FW, lifecycle_controller->get_state());
}

static void boot_faulted(uint32_t code)
{
    boot_operational();
    step(LC_EVENT_UNRECOVERABLE_FAULT, code);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

static lifecycle_event_t last_sent_event(void)
{
    lifecycle_event_t ev;

    TEST_ASSERT_EQUAL_UINT32(sizeof(ev), g_mock_xQueueSend_last_item_size);
    (void) memcpy(&ev, g_mock_xQueueSend_last_item, sizeof(ev));
    return ev;
}

static bool trace_has(call_id_t id)
{
    for (uint32_t i = 0U; i < g_trace_len; i++)
    {
        if (g_trace[i] == id)
        {
            return true;
        }
    }
    return false;
}

static uint32_t trace_index(call_id_t id)
{
    for (uint32_t i = 0U; i < g_trace_len; i++)
    {
        if (g_trace[i] == id)
        {
            return i;
        }
    }
    return 0xFFFFFFFFU;
}

/* ======================================================================= */
/* TC-LC-001..007 — init                                                    */
/* ======================================================================= */

void test_LC_001_init_rejects_null_deps(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG,
                      lifecycle_controller_init(LIFECYCLE_RESET_POWER_ON, NULL));
}

void test_LC_001b_init_rejects_each_null_member(void)
{
    static const size_t k_offsets[] = {
        offsetof(lifecycle_deps_t, config_store),  offsetof(lifecycle_deps_t, config_manager),
        offsetof(lifecycle_deps_t, sensors),       offsetof(lifecycle_deps_t, alarms),
        offsetof(lifecycle_deps_t, console),       offsetof(lifecycle_deps_t, health_report),
        offsetof(lifecycle_deps_t, health_admin),  offsetof(lifecycle_deps_t, cloud),
        offsetof(lifecycle_deps_t, modbus_poller), offsetof(lifecycle_deps_t, update_service),
        offsetof(lifecycle_deps_t, time_service),  offsetof(lifecycle_deps_t, firmware_store),
        offsetof(lifecycle_deps_t, reset_driver),
    };

    for (uint32_t i = 0U; i < (sizeof(k_offsets) / sizeof(k_offsets[0])); i++)
    {
        lifecycle_deps_t d = g_deps;
        const void **member = (const void **) ((uint8_t *) &d + k_offsets[i]);

        *member = NULL;
        TEST_ASSERT_EQUAL_MESSAGE(LIFECYCLE_ERR_NULL_ARG,
                                  lifecycle_controller_init(LIFECYCLE_RESET_POWER_ON, &d),
                                  "member");
    }
}

void test_LC_001c_init_rejects_a_null_function_pointer(void)
{
    lc_cloud_if_t cloud = k_cloud;
    lifecycle_deps_t d = g_deps;

    cloud.flush = NULL;
    d.cloud = &cloud;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG,
                      lifecycle_controller_init(LIFECYCLE_RESET_POWER_ON, &d));
}

void test_LC_003_init_accepts_a_missing_watchdog(void)
{
    g_deps.watchdog = NULL;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, init_ok());
}

void test_LC_004_init_leaves_the_state_INIT_and_runs_nothing(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, init_ok());
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_INIT, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_trace_len);
}

void test_LC_005_init_stores_the_reset_cause(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK,
                      lifecycle_controller_init(LIFECYCLE_RESET_WATCHDOG, &g_deps));
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_WATCHDOG, lifecycle_controller->get_reset_cause());
}

void test_LC_006_init_creates_the_queue_three_timers_and_the_start_gate(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, init_ok());
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xQueueCreateStatic_call_count);
    TEST_ASSERT_EQUAL_UINT32(3U, g_mock_xTimerCreateStatic_call_count);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xEventGroupCreateStatic_call_count);
    TEST_ASSERT_NOT_NULL(lifecycle_get_start_gate());
}

void test_LC_006b_init_reports_a_failed_rtos_object(void)
{
    g_mock_xTimerCreateStatic_return = NULL;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NOT_INIT, init_ok());
    TEST_ASSERT_FALSE(lifecycle_controller->post_event(
        (lifecycle_event_t){.type = LC_EVENT_CONFIG_EDIT_ENTER, .param = 0U}));
}

void test_LC_007_the_vtable_before_init_is_safe(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_INIT, lifecycle_controller->get_state());
    TEST_ASSERT_FALSE(lifecycle_controller->post_event(
        (lifecycle_event_t){.type = LC_EVENT_CONFIG_EDIT_ENTER, .param = 0U}));
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NOT_INIT,
                      lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS));
    TEST_ASSERT_NULL(lifecycle_get_start_gate());
}

void test_LC_007b_the_task_body_before_init_does_nothing(void)
{
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL_UINT32(0U, g_trace_len);
}

/* ======================================================================= */
/* TC-LC-010..015 — reset-cause detection                                   */
/* ======================================================================= */

void test_LC_010_watchdog_flag_gives_WATCHDOG(void)
{
    RCC->CSR = RCC_CSR_IWDGRSTF;
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_WATCHDOG, lifecycle_detect_reset_cause());
}

void test_LC_011_software_flag_gives_SOFT(void)
{
    RCC->CSR = RCC_CSR_SFTRSTF;
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_SOFT, lifecycle_detect_reset_cause());
}

void test_LC_012_pin_flag_gives_POWER_ON(void)
{
    RCC->CSR = RCC_CSR_PINRSTF;
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_POWER_ON, lifecycle_detect_reset_cause());
}

void test_LC_012b_a_power_up_sets_pin_and_brown_out_together(void)
{
    RCC->CSR = RCC_CSR_PINRSTF | RCC_CSR_BORRSTF;
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_POWER_ON, lifecycle_detect_reset_cause());
}

void test_LC_012c_brown_out_alone_gives_POWER_ON(void)
{
    RCC->CSR = RCC_CSR_BORRSTF;
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_POWER_ON, lifecycle_detect_reset_cause());
}

void test_LC_013_no_flag_gives_UNKNOWN(void)
{
    RCC->CSR = 0U;
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_UNKNOWN, lifecycle_detect_reset_cause());
}

void test_LC_014_detection_clears_the_flags_for_the_next_boot(void)
{
    RCC->CSR = RCC_CSR_PINRSTF;
    (void) lifecycle_detect_reset_cause();
    TEST_ASSERT_BITS_HIGH(RCC_CSR_RMVF, RCC->CSR);
}

void test_LC_015_watchdog_wins_over_soft_and_pin(void)
{
    RCC->CSR = RCC_CSR_IWDGRSTF | RCC_CSR_SFTRSTF | RCC_CSR_PINRSTF;
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_WATCHDOG, lifecycle_detect_reset_cause());
}

void test_LC_015b_soft_wins_over_pin(void)
{
    RCC->CSR = RCC_CSR_SFTRSTF | RCC_CSR_PINRSTF;
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_SOFT, lifecycle_detect_reset_cause());
}

/* ======================================================================= */
/* TC-LC-050..064 — Init sequence                                           */
/* ======================================================================= */

void test_LC_050_a_normal_boot_reaches_OPERATIONAL(void)
{
    boot();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_050b_a_normal_boot_calls_the_collaborators_in_order(void)
{
    static const call_id_t k_expected[] = {
        C_CS_CHECK,   C_FW_FLAGS,   C_CS_LOAD,     C_CM_APPLY, C_SENS_INIT, C_ALARM_INIT,
        C_POLL_INIT,  C_CLOUD_INIT, C_TIME_INIT,   C_UPD_INIT, C_CON_INIT,  C_HR_INIT,
        C_SENS_READY, C_POLL_READY, C_CLOUD_READY, C_HR_LIFE,  C_WDG,
    };

    boot();
    TEST_ASSERT_EQUAL_UINT32(sizeof(k_expected) / sizeof(k_expected[0]), g_trace_len);
    for (uint32_t i = 0U; i < g_trace_len; i++)
    {
        TEST_ASSERT_EQUAL_MESSAGE(k_expected[i], g_trace[i], "call order");
    }
}

void test_LC_050c_operational_entry_sets_the_start_gate_bit(void)
{
    boot();
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xEventGroupSetBits_call_count);
    TEST_ASSERT_EQUAL_UINT32(LIFECYCLE_START_GATE_BIT, g_mock_xEventGroupSetBits_last_bits);
}

void test_LC_050d_the_start_gate_stays_closed_until_operational(void)
{
    g_status[C_SENS_INIT] = LC_DEP_FAIL;
    boot();
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xEventGroupSetBits_call_count);
}

void test_LC_050e_the_init_timer_runs_for_init_and_stops_at_operational(void)
{
    boot();
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTimerStart_call_count);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTimerStop_call_count);
}

void test_LC_050f_operational_entry_tells_HealthMonitor(void)
{
    boot();
    TEST_ASSERT_EQUAL_UINT32(1U, g_life_count);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, g_life_seen[0]);
}

void test_LC_050g_a_failing_health_update_does_not_block_the_transition(void)
{
    g_status[C_HR_LIFE] = LC_DEP_FAIL;
    boot();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_050h_the_collaborators_get_their_own_ctx(void)
{
    boot();
    TEST_ASSERT_EQUAL_PTR(&g_ctx_marker, g_cs_ctx_seen);
}

void test_LC_050i_the_config_load_buffer_is_the_snapshot_size(void)
{
    boot();
    TEST_ASSERT_EQUAL_UINT32(LIFECYCLE_CFG_SNAPSHOT_BYTES, g_load_max_seen);
}

void test_LC_050j_the_init_sequence_runs_once(void)
{
    boot();
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_CS_CHECK]);
}

void test_LC_051_CheckingIntegrity_reads_the_pending_self_check_flag(void)
{
    g_flag_self_check = true;
    boot();
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_UPD_SELF]);
}

void test_LC_052_CheckingIntegrity_reads_the_pending_rollback_flag(void)
{
    g_flag_rollback = true;
    boot();
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_UPD_AFTER_RB]);
}

void test_LC_053_SelfChecking_with_every_probe_passing_reaches_OPERATIONAL(void)
{
    boot();
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_SENS_READY]);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_POLL_READY]);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_CLOUD_READY]);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_054_a_sensor_probe_failure_gives_FAULTED(void)
{
    g_ready[C_SENS_READY] = false;
    boot();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(LC_FAULT_SELF_CHECK, g_fault_code_seen);
}

void test_LC_055_a_poller_probe_failure_gives_FAULTED(void)
{
    g_ready[C_POLL_READY] = false;
    boot();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

void test_LC_056_a_cloud_probe_failure_gives_FAULTED(void)
{
    g_ready[C_CLOUD_READY] = false;
    boot();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

void test_LC_056b_every_probe_runs_so_the_log_shows_all_of_them(void)
{
    g_ready[C_SENS_READY] = false;
    boot();
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_POLL_READY]);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_CLOUD_READY]);
}

void test_LC_057_a_pending_self_check_resumes_it_and_waits_in_INIT(void)
{
    g_flag_self_check = true;
    boot();
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_UPD_SELF]);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_INIT, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xEventGroupSetBits_call_count);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_SENS_READY]);
}

void test_LC_057b_the_init_budget_does_not_cover_the_self_check_wait(void)
{
    g_flag_self_check = true;
    boot();
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTimerStop_call_count);
}

void test_LC_057c_a_failing_resume_self_checking_gives_FAULTED(void)
{
    g_flag_self_check = true;
    g_status[C_UPD_SELF] = LC_DEP_FAIL;
    boot();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

void test_LC_058_SELF_CHECK_PASS_confirms_and_reaches_OPERATIONAL(void)
{
    g_flag_self_check = true;
    boot();
    step(LC_EVENT_SELF_CHECK_PASS, 0U);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_FW_CONFIRM]);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xEventGroupSetBits_call_count);
}

void test_LC_058b_a_failing_confirm_gives_FAULTED(void)
{
    g_flag_self_check = true;
    g_status[C_FW_CONFIRM] = LC_DEP_FAIL;
    boot();
    step(LC_EVENT_SELF_CHECK_PASS, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

void test_LC_059_SELF_CHECK_FAIL_rolls_back_and_resets(void)
{
    g_flag_self_check = true;
    boot();
    step(LC_EVENT_SELF_CHECK_FAIL, 0U);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_UPD_RB]);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_FW_ROLLBACK]);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_RESET]);
    TEST_ASSERT_TRUE(trace_index(C_UPD_RB) < trace_index(C_FW_ROLLBACK));
    TEST_ASSERT_TRUE(trace_index(C_FW_ROLLBACK) < trace_index(C_RESET));
}

void test_LC_059b_a_fault_during_the_self_check_wait_gives_FAULTED(void)
{
    g_flag_self_check = true;
    boot();
    step(LC_EVENT_UNRECOVERABLE_FAULT, 0x123U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0x123U, g_fault_code_seen);
}

void test_LC_059c_other_events_during_the_self_check_wait_are_ignored(void)
{
    g_flag_self_check = true;
    boot();
    step(LC_EVENT_RESTART_REQUESTED, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_INIT, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_CLOUD_FLUSH]);
}

void test_LC_060_a_pending_rollback_resumes_after_it(void)
{
    g_flag_rollback = true;
    boot();
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_UPD_AFTER_RB]);
}

void test_LC_061_a_pending_rollback_sends_no_report_itself_and_reaches_OPERATIONAL(void)
{
    /* UpdateService reports the rollback (US-D9, LCC-3): LifecycleController only resumes it. */
    g_flag_rollback = true;
    boot();
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_UPD_AFTER_RB]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_CLOUD_FLUSH]);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_SENS_READY]);
}

void test_LC_061b_failures_in_the_rollback_path_are_logged_not_fatal(void)
{
    g_flag_rollback = true;
    g_status[C_UPD_AFTER_RB] = LC_DEP_FAIL;
    boot();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_062_with_no_pending_flag_the_normal_probes_run(void)
{
    boot();
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_UPD_SELF]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_UPD_AFTER_RB]);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_SENS_READY]);
}

void test_LC_063_unreadable_firmware_flags_are_treated_as_none_pending(void)
{
    g_status[C_FW_FLAGS] = LC_DEP_FAIL;
    g_flag_self_check = true; /* a failed read must not leak a stale value */
    boot();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_UPD_SELF]);
}

void test_LC_064_a_pending_self_check_takes_precedence_over_a_pending_rollback(void)
{
    g_flag_self_check = true;
    g_flag_rollback = true;
    boot();
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_UPD_SELF]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_UPD_AFTER_RB]);
}

typedef struct
{
    call_id_t failing;
    lifecycle_fault_t fault;
    call_id_t must_not_run;
} init_failure_row_t;

void test_LC_065_each_init_failure_gives_FAULTED_with_its_fault_code(void)
{
    static const init_failure_row_t k_rows[] = {
        {C_CS_CHECK, LC_FAULT_INTEGRITY, C_FW_FLAGS},
        {C_CS_LOAD, LC_FAULT_CONFIG_LOAD, C_CM_APPLY},
        {C_CM_APPLY, LC_FAULT_CONFIG_APPLY, C_SENS_INIT},
        {C_SENS_INIT, LC_FAULT_SENSORS_INIT, C_ALARM_INIT},
        {C_ALARM_INIT, LC_FAULT_ALARMS_INIT, C_POLL_INIT},
        {C_POLL_INIT, LC_FAULT_POLLER_INIT, C_CLOUD_INIT},
        {C_CLOUD_INIT, LC_FAULT_CLOUD_INIT, C_TIME_INIT},
        {C_TIME_INIT, LC_FAULT_TIME_INIT, C_UPD_INIT},
        {C_UPD_INIT, LC_FAULT_UPDATE_INIT, C_CON_INIT},
        {C_CON_INIT, LC_FAULT_CONSOLE_INIT, C_HR_INIT},
        {C_HR_INIT, LC_FAULT_HEALTH_INIT, C_SENS_READY},
    };

    for (uint32_t i = 0U; i < (sizeof(k_rows) / sizeof(k_rows[0])); i++)
    {
        setUp();
        g_status[k_rows[i].failing] = LC_DEP_FAIL;
        boot();
        TEST_ASSERT_EQUAL_MESSAGE(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state(),
                                  "state");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(k_rows[i].fault, g_fault_code_seen, "fault code");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, g_calls[k_rows[i].must_not_run], "next step ran");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, g_mock_xEventGroupSetBits_call_count, "gate");
    }
}

void test_LC_066_a_fault_queued_during_init_aborts_it(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, init_ok());
    arm(LC_EVENT_UNRECOVERABLE_FAULT, 0x77U); /* seen by the first sub-state boundary */
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0x77U, g_fault_code_seen);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_CS_LOAD]);
}

void test_LC_067_the_init_timer_expiry_gives_FAULTED_with_the_timeout_code(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, init_ok());

    lifecycle_timer_callback_for_test(LC_TEST_TIMER_INIT)(NULL);
    const lifecycle_event_t sent = last_sent_event();
    TEST_ASSERT_EQUAL(LC_EVENT_UNRECOVERABLE_FAULT, sent.type);
    TEST_ASSERT_EQUAL_UINT32(LC_FAULT_INIT_TIMEOUT, sent.param);

    arm(sent.type, sent.param);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(LC_FAULT_INIT_TIMEOUT, g_fault_code_seen);
}

void test_LC_068_a_non_fault_event_during_init_is_dropped(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, init_ok());
    arm(LC_EVENT_RESTART_REQUESTED, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_CLOUD_FLUSH]);
}

/* ======================================================================= */
/* TC-LC-070..079 — EditingConfig                                           */
/* ======================================================================= */

void test_LC_070_CONFIG_EDIT_ENTER_snapshots_and_starts_the_timer(void)
{
    boot_operational();
    const uint32_t starts = g_mock_xTimerStart_call_count;

    step(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_EDITING_CONFIG, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_CM_SNAPSHOT]);
    TEST_ASSERT_EQUAL_UINT32(LIFECYCLE_CFG_SNAPSHOT_BYTES, g_snapshot_max_seen);
    TEST_ASSERT_EQUAL_UINT32(starts + 1U, g_mock_xTimerStart_call_count);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_EDITING_CONFIG, g_life_seen[g_life_count - 1U]);
}

void test_LC_070b_a_failed_snapshot_refuses_the_session(void)
{
    boot_operational();
    const uint32_t starts = g_mock_xTimerStart_call_count;

    g_status[C_CM_SNAPSHOT] = LC_DEP_FAIL;
    step(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(starts, g_mock_xTimerStart_call_count);
}

void test_LC_071_CONFIG_EDIT_APPLY_commits_and_reconfigures(void)
{
    boot_editing();
    const uint32_t stops = g_mock_xTimerStop_call_count;

    step(LC_EVENT_CONFIG_EDIT_APPLY, 0U);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_CM_COMMIT]);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_SENS_RECONF]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_CM_RESTORE]);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(stops + 1U, g_mock_xTimerStop_call_count);
}

void test_LC_071b_a_failed_commit_restores_the_snapshot(void)
{
    boot_editing();
    g_status[C_CM_COMMIT] = LC_DEP_FAIL;
    step(LC_EVENT_CONFIG_EDIT_APPLY, 0U);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_CM_RESTORE]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_SENS_RECONF]);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_071c_a_failed_reconfigure_still_reaches_OPERATIONAL(void)
{
    boot_editing();
    g_status[C_SENS_RECONF] = LC_DEP_FAIL;
    step(LC_EVENT_CONFIG_EDIT_APPLY, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_072_CONFIG_EDIT_CANCEL_restores_the_snapshot(void)
{
    boot_editing();
    step(LC_EVENT_CONFIG_EDIT_CANCEL, 0U);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_CM_RESTORE]);
    TEST_ASSERT_EQUAL_UINT32(SNAPSHOT_LEN, g_restore_len_seen);
    TEST_ASSERT_EQUAL_UINT8(SNAPSHOT_PATTERN, g_restore_first_byte);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_CM_COMMIT]);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_073_CONFIG_EDIT_TIMEOUT_acts_as_a_cancel(void)
{
    boot_editing();
    step(LC_EVENT_CONFIG_EDIT_TIMEOUT, 0U);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_CM_RESTORE]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_CM_COMMIT]);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_074_the_edit_timer_expiry_posts_CONFIG_EDIT_TIMEOUT(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, init_ok());
    lifecycle_timer_callback_for_test(LC_TEST_TIMER_EDIT)(NULL);
    TEST_ASSERT_EQUAL(LC_EVENT_CONFIG_EDIT_TIMEOUT, last_sent_event().type);
}

void test_LC_074b_the_edit_timeout_is_five_minutes(void)
{
    TEST_ASSERT_EQUAL_UINT32(300000U, LIFECYCLE_EDIT_TIMEOUT_MS);
}

void test_LC_077_a_fault_while_editing_gives_FAULTED(void)
{
    boot_editing();
    const uint32_t stops = g_mock_xTimerStop_call_count;

    step(LC_EVENT_UNRECOVERABLE_FAULT, 0x42U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_TRUE(g_mock_xTimerStop_call_count > stops);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_CM_RESTORE]);
}

void test_LC_078_CONFIG_EDIT_ENTER_outside_OPERATIONAL_is_ignored(void)
{
    boot_restarting();
    step(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_RESTARTING, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_CM_SNAPSHOT]);
}

void test_LC_078b_CONFIG_EDIT_ENTER_while_FAULTED_is_ignored(void)
{
    boot_faulted(1U);
    step(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_CM_SNAPSHOT]);
}

void test_LC_079_a_second_ENTER_while_editing_is_ignored(void)
{
    boot_editing();
    step(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_CM_SNAPSHOT]);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_EDITING_CONFIG, lifecycle_controller->get_state());
}

void test_LC_080_a_restart_request_while_editing_waits_for_the_edit_to_end(void)
{
    boot_editing();
    step(LC_EVENT_RESTART_REQUESTED, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_EDITING_CONFIG, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(1U, lifecycle_deferred_count_for_test());
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_CLOUD_FLUSH]);

    step(LC_EVENT_CONFIG_EDIT_APPLY, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());

    lifecycle_task_body(NULL); /* replays the held request */
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_RESTARTING, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_CLOUD_FLUSH]);
    TEST_ASSERT_EQUAL_UINT32(0U, lifecycle_deferred_count_for_test());
}

void test_LC_081_deferred_events_replay_in_arrival_order(void)
{
    boot_editing();
    step(LC_EVENT_OTA_REQUESTED, 4096U);
    step(LC_EVENT_RESTART_REQUESTED, 0U);
    TEST_ASSERT_EQUAL_UINT32(2U, lifecycle_deferred_count_for_test());

    step(LC_EVENT_CONFIG_EDIT_CANCEL, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_UPDATING_FW, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(4096U, g_update_size_seen);
}

void test_LC_082_the_deferred_list_is_bounded(void)
{
    boot_editing();
    for (uint32_t i = 0U; i < (LIFECYCLE_QUEUE_DEPTH + 2U); i++)
    {
        step(LC_EVENT_RESTART_REQUESTED, 0U);
    }
    TEST_ASSERT_EQUAL_UINT32(LIFECYCLE_QUEUE_DEPTH, lifecycle_deferred_count_for_test());
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_EDITING_CONFIG, lifecycle_controller->get_state());
}

void test_LC_083_events_meaningless_while_editing_are_not_deferred(void)
{
    boot_editing();
    step(LC_EVENT_RESTART_CONFIRMED, 0U);
    step(LC_EVENT_SELF_CHECK_PASS, 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, lifecycle_deferred_count_for_test());
}

/* ======================================================================= */
/* TC-LC-085..090 — Restarting                                              */
/* ======================================================================= */

void test_LC_085_RESTART_REQUESTED_flushes_and_starts_the_timer(void)
{
    boot_operational();
    const uint32_t starts = g_mock_xTimerStart_call_count;

    step(LC_EVENT_RESTART_REQUESTED, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_RESTARTING, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_CLOUD_FLUSH]);
    TEST_ASSERT_EQUAL_UINT32(starts + 1U, g_mock_xTimerStart_call_count);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_RESTARTING, g_life_seen[g_life_count - 1U]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_RESET]);
}

void test_LC_085b_a_failed_flush_still_enters_RESTARTING(void)
{
    boot_operational();
    g_status[C_CLOUD_FLUSH] = LC_DEP_FAIL;
    step(LC_EVENT_RESTART_REQUESTED, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_RESTARTING, lifecycle_controller->get_state());
}

void test_LC_086_RESTART_CONFIRMED_resets(void)
{
    boot_restarting();
    const uint32_t stops = g_mock_xTimerStop_call_count;

    step(LC_EVENT_RESTART_CONFIRMED, 0U);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_RESET]);
    TEST_ASSERT_EQUAL_UINT32(stops + 1U, g_mock_xTimerStop_call_count);
}

void test_LC_087_RESTART_TIMEOUT_returns_to_OPERATIONAL_without_a_reset(void)
{
    boot_restarting();
    step(LC_EVENT_RESTART_TIMEOUT, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_RESET]);
}

void test_LC_087b_a_new_restart_works_after_a_timeout(void)
{
    boot_restarting();
    step(LC_EVENT_RESTART_TIMEOUT, 0U);
    step(LC_EVENT_RESTART_REQUESTED, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_RESTARTING, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(2U, g_calls[C_CLOUD_FLUSH]);
}

void test_LC_088_the_restart_timer_expiry_posts_RESTART_TIMEOUT(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, init_ok());
    lifecycle_timer_callback_for_test(LC_TEST_TIMER_RESTART)(NULL);
    TEST_ASSERT_EQUAL(LC_EVENT_RESTART_TIMEOUT, last_sent_event().type);
    TEST_ASSERT_EQUAL_UINT32(30000U, LIFECYCLE_RESTART_TIMEOUT_MS);
}

void test_LC_089_a_second_RESTART_REQUESTED_counts_as_the_confirmation(void)
{
    boot_restarting();
    step(LC_EVENT_RESTART_REQUESTED, 0U);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_RESET]);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_CLOUD_FLUSH]);
}

void test_LC_090_a_fault_while_restarting_gives_FAULTED(void)
{
    boot_restarting();
    step(LC_EVENT_UNRECOVERABLE_FAULT, 9U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_RESET]);
}

void test_LC_090b_RESTART_CONFIRMED_without_a_pending_restart_is_ignored(void)
{
    boot_operational();
    step(LC_EVENT_RESTART_CONFIRMED, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_RESET]);
}

/* ======================================================================= */
/* TC-LC-095..099 — UpdatingFirmware                                        */
/* ======================================================================= */

void test_LC_095_OTA_REQUESTED_starts_the_update(void)
{
    boot_operational();
    step(LC_EVENT_OTA_REQUESTED, 123456U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_UPDATING_FW, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_UPD_START]);
    TEST_ASSERT_EQUAL_UINT32(123456U, g_update_size_seen);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_UPDATING_FW, g_life_seen[g_life_count - 1U]);
}

void test_LC_095b_a_refused_start_gives_FAULTED(void)
{
    boot_operational();
    g_status[C_UPD_START] = LC_DEP_FAIL;
    step(LC_EVENT_OTA_REQUESTED, 1U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(LC_FAULT_OTA_START, g_fault_code_seen);
}

void test_LC_096_a_second_OTA_request_is_refused_with_BAD_STATE(void)
{
    boot_updating(1000U);
    step(LC_EVENT_OTA_REQUESTED, 2000U);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_BAD_STATE, lifecycle_last_event_result_for_test());
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_UPD_START]);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_UPDATING_FW, lifecycle_controller->get_state());
}

void test_LC_096b_post_event_refuses_an_OTA_request_while_updating(void)
{
    boot_updating(1000U);
    const uint32_t sent = g_mock_xQueueSend_call_count;

    TEST_ASSERT_FALSE(lifecycle_controller->post_event(
        (lifecycle_event_t){.type = LC_EVENT_OTA_REQUESTED, .param = 5U}));
    TEST_ASSERT_EQUAL_UINT32(sent, g_mock_xQueueSend_call_count);
}

void test_LC_097_SELF_CHECK_PASS_while_updating_confirms_and_reaches_OPERATIONAL(void)
{
    boot_updating(1000U);
    step(LC_EVENT_SELF_CHECK_PASS, 0U);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_FW_CONFIRM]);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_097b_a_failing_confirm_while_updating_gives_FAULTED(void)
{
    boot_updating(1000U);
    g_status[C_FW_CONFIRM] = LC_DEP_FAIL;
    step(LC_EVENT_SELF_CHECK_PASS, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

void test_LC_098_SELF_CHECK_FAIL_while_updating_rolls_back_and_resets(void)
{
    boot_updating(1000U);
    step(LC_EVENT_SELF_CHECK_FAIL, 0U);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_UPD_RB]);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_FW_ROLLBACK]);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_RESET]);
}

void test_LC_099_a_fault_while_updating_gives_FAULTED(void)
{
    boot_updating(1000U);
    step(LC_EVENT_UNRECOVERABLE_FAULT, 0x101U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0x101U, g_fault_code_seen);
}

void test_LC_099b_a_restart_request_while_updating_is_ignored(void)
{
    boot_updating(1000U);
    step(LC_EVENT_RESTART_REQUESTED, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_UPDATING_FW, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_CLOUD_FLUSH]);
}

/* ======================================================================= */
/* TC-LC-100..103 — the UpdateService events (LCC-2)                         */
/* ======================================================================= */

void test_LC_100_OTA_FINISHED_while_updating_returns_to_OPERATIONAL(void)
{
    boot_updating(42U);
    step(LC_EVENT_OTA_FINISHED, 42U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, g_life_seen[g_life_count - 1U]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_RESET]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_FW_ROLLBACK]);
}

void test_LC_100b_after_OTA_FINISHED_a_new_update_can_start(void)
{
    boot_updating(42U);
    step(LC_EVENT_OTA_FINISHED, 42U);
    step(LC_EVENT_OTA_REQUESTED, 43U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_UPDATING_FW, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(2U, g_calls[C_UPD_START]);
    TEST_ASSERT_EQUAL_UINT32(43U, g_update_size_seen);
}

void test_LC_101_OTA_RESET_REQUESTED_flushes_waits_the_grace_period_then_resets(void)
{
    boot_updating(42U);
    step(LC_EVENT_OTA_RESET_REQUESTED, 42U);

    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_CLOUD_FLUSH]);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_vTaskDelay_call_count);
    TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(LC_OTA_RESET_GRACE_MS), g_mock_vTaskDelay_last_ticks);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_RESET]);
    TEST_ASSERT_TRUE(trace_index(C_CLOUD_FLUSH) < trace_index(C_RESET));
    /* The reset ends the state: it is left as it is. */
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_UPDATING_FW, lifecycle_controller->get_state());
    /* The rollback path is not involved. */
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_FW_ROLLBACK]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_UPD_RB]);
}

void test_LC_101b_a_failing_flush_still_resets(void)
{
    boot_updating(42U);
    g_status[C_CLOUD_FLUSH] = LC_DEP_FAIL;
    step(LC_EVENT_OTA_RESET_REQUESTED, 42U);

    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_CLOUD_FLUSH]);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_RESET]);
}

void test_LC_102_the_rollback_path_has_no_grace_delay(void)
{
    boot_updating(42U);
    step(LC_EVENT_SELF_CHECK_FAIL, 0U);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_RESET]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_vTaskDelay_call_count);
}

void test_LC_103_the_two_events_are_ignored_outside_UpdatingFirmware(void)
{
    /* Operational. */
    boot_operational();
    step(LC_EVENT_OTA_RESET_REQUESTED, 1U);
    step(LC_EVENT_OTA_FINISHED, 1U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_RESET]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_CLOUD_FLUSH]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_vTaskDelay_call_count);

    /* EditingConfig. */
    setUp();
    boot_editing();
    step(LC_EVENT_OTA_FINISHED, 1U);
    step(LC_EVENT_OTA_RESET_REQUESTED, 1U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_EDITING_CONFIG, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_RESET]);

    /* Restarting. */
    setUp();
    boot_restarting();
    step(LC_EVENT_OTA_FINISHED, 1U);
    step(LC_EVENT_OTA_RESET_REQUESTED, 1U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_RESTARTING, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_RESET]);
}

void test_LC_103b_the_two_events_are_ignored_while_Init_waits_for_the_verdict(void)
{
    g_flag_self_check = true;
    boot();
    step(LC_EVENT_OTA_FINISHED, 1U);
    step(LC_EVENT_OTA_RESET_REQUESTED, 1U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_INIT, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_RESET]);
}

/* ======================================================================= */
/* TC-LC-105..108 — Faulted                                                 */
/* ======================================================================= */

void test_LC_105_a_fault_from_OPERATIONAL_reports_the_code(void)
{
    boot_operational();
    step(LC_EVENT_UNRECOVERABLE_FAULT, 0xBEEFU);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_HR_FAULT]);
    TEST_ASSERT_EQUAL_UINT32(0xBEEFU, g_fault_code_seen);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_HA_LED]);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, g_life_seen[g_life_count - 1U]);
}

void test_LC_105b_entering_FAULTED_stops_every_timer(void)
{
    boot_operational();
    const uint32_t stops = g_mock_xTimerStop_call_count;

    step(LC_EVENT_UNRECOVERABLE_FAULT, 1U);
    TEST_ASSERT_EQUAL_UINT32(stops + 3U, g_mock_xTimerStop_call_count);
}

void test_LC_105c_a_failing_health_report_does_not_stop_the_transition(void)
{
    boot_operational();
    g_status[C_HR_FAULT] = LC_DEP_FAIL;
    g_status[C_HA_LED] = LC_DEP_FAIL;
    step(LC_EVENT_UNRECOVERABLE_FAULT, 1U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
}

void test_LC_106_entering_FAULTED_happens_once(void)
{
    boot_faulted(1U);
    step(LC_EVENT_UNRECOVERABLE_FAULT, 2U);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_HR_FAULT]);
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_HA_LED]);
    TEST_ASSERT_EQUAL_UINT32(1U, g_fault_code_seen);
}

void test_LC_107_no_event_leaves_FAULTED(void)
{
    boot_faulted(1U);
    for (uint32_t t = 0U; t <= (uint32_t) LC_EVENT_UNRECOVERABLE_FAULT; t++)
    {
        step((lifecycle_event_type_t) t, 100U);
        TEST_ASSERT_EQUAL_MESSAGE(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state(),
                                  "left FAULTED");
    }
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_RESET]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_UPD_START]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_CLOUD_FLUSH]);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_CM_SNAPSHOT]);
}

void test_LC_108_the_watchdog_is_kicked_on_an_idle_pass_even_when_FAULTED(void)
{
    boot_faulted(1U);
    const uint32_t kicks = g_calls[C_WDG];

    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL_UINT32(kicks + 1U, g_calls[C_WDG]);
}

void test_LC_108b_the_watchdog_is_kicked_on_every_pass(void)
{
    boot_operational();
    const uint32_t kicks = g_calls[C_WDG];

    step(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL_UINT32(kicks + 2U, g_calls[C_WDG]);
}

void test_LC_108c_the_loop_runs_without_a_watchdog(void)
{
    g_deps.watchdog = NULL;
    boot_operational();
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_WDG]);
}

/* ======================================================================= */
/* TC-LC-115..120 — vtable                                                  */
/* ======================================================================= */

void test_LC_115_get_state_returns_the_current_state(void)
{
    boot_operational();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    step(LC_EVENT_RESTART_REQUESTED, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_RESTARTING, lifecycle_controller->get_state());
}

void test_LC_116_get_reset_cause_returns_the_stored_cause(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, lifecycle_controller_init(LIFECYCLE_RESET_SOFT, &g_deps));
    TEST_ASSERT_EQUAL(LIFECYCLE_RESET_SOFT, lifecycle_controller->get_reset_cause());
}

void test_LC_117_post_event_enqueues_the_event(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, init_ok());
    TEST_ASSERT_TRUE(lifecycle_controller->post_event(
        (lifecycle_event_t){.type = LC_EVENT_OTA_REQUESTED, .param = 77U}));
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xQueueSend_call_count);
    TEST_ASSERT_EQUAL(LC_EVENT_OTA_REQUESTED, last_sent_event().type);
    TEST_ASSERT_EQUAL_UINT32(77U, last_sent_event().param);
}

void test_LC_118_post_event_returns_false_on_a_full_queue(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, init_ok());
    g_mock_xQueueSend_return = pdFALSE;
    TEST_ASSERT_FALSE(lifecycle_controller->post_event(
        (lifecycle_event_t){.type = LC_EVENT_CONFIG_EDIT_ENTER, .param = 0U}));
}

void test_LC_119_the_queue_depth_is_four(void)
{
    TEST_ASSERT_EQUAL_UINT32(4U, LIFECYCLE_QUEUE_DEPTH);
}

void test_LC_120_events_are_handled_in_the_order_they_are_delivered(void)
{
    boot_operational();
    step(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_EDITING_CONFIG, lifecycle_controller->get_state());
    step(LC_EVENT_CONFIG_EDIT_APPLY, 0U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_TRUE(trace_index(C_CM_SNAPSHOT) < trace_index(C_CM_COMMIT));
}

void test_LC_121_the_start_gate_bit_is_bit_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(1U, LIFECYCLE_START_GATE_BIT);
}

/* ======================================================================= */
/* TC-LC-130..136 — remote commands                                         */
/* ======================================================================= */

void test_LC_130_RESET_METRICS_calls_the_admin_interface(void)
{
    boot_operational();
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK,
                      lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS));
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_HA_RESET]);
}

void test_LC_131_RESET_METRICS_does_not_change_the_state(void)
{
    boot_operational();
    (void) lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LC_132_RESET_METRICS_posts_no_event(void)
{
    boot_operational();
    (void) lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend_call_count);
}

void test_LC_132b_RESET_METRICS_works_in_every_state_including_FAULTED(void)
{
    boot_faulted(1U);
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK,
                      lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS));
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_HA_RESET]);
}

void test_LC_132c_a_failing_reset_metrics_is_logged_and_still_returns_OK(void)
{
    boot_operational();
    g_status[C_HA_RESET] = LC_DEP_FAIL;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK,
                      lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS));
}

void test_LC_133_SOFT_RESTART_posts_RESTART_REQUESTED(void)
{
    boot_operational();
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK,
                      lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_SOFT_RESTART));
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xQueueSend_call_count);
    TEST_ASSERT_EQUAL(LC_EVENT_RESTART_REQUESTED, last_sent_event().type);
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_RESET]); /* queued, never direct */
}

void test_LC_134_SOFT_RESTART_with_a_full_queue_returns_QUEUE_FULL(void)
{
    boot_operational();
    g_mock_xQueueSend_return = pdFALSE;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_QUEUE_FULL,
                      lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_SOFT_RESTART));
}

void test_LC_135_an_unknown_command_returns_UNKNOWN_CMD(void)
{
    boot_operational();
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_UNKNOWN_CMD,
                      lifecycle_controller->handle_remote_command((lifecycle_remote_cmd_t) 99));
    TEST_ASSERT_EQUAL_UINT32(0U, g_calls[C_HA_RESET]);
}

void test_LC_136_a_remote_command_before_init_returns_NOT_INIT(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NOT_INIT,
                      lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_SOFT_RESTART));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend_call_count);
}

void test_LC_138_a_soft_restart_command_drives_the_whole_two_step_flow(void)
{
    boot_operational();
    (void) lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_SOFT_RESTART);

    step(LC_EVENT_RESTART_REQUESTED, 0U); /* the queued request is delivered */
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_RESTARTING, lifecycle_controller->get_state());
    step(LC_EVENT_RESTART_CONFIRMED, 0U); /* the confirmation token arrives */
    TEST_ASSERT_EQUAL_UINT32(1U, g_calls[C_RESET]);
    TEST_ASSERT_TRUE(trace_has(C_CLOUD_FLUSH));
    TEST_ASSERT_TRUE(trace_index(C_CLOUD_FLUSH) < trace_index(C_RESET));
}
