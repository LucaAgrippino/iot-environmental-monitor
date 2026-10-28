/**
 * @file main_test_lifecycle_controller.c
 * @brief Hardware bring-up test for LifecycleController (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. Add cloud_publisher (the current harness) to the `excluding=` list of the
 *      `integration-tests` source entry and remove lifecycle_controller.
 *   2. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/lifecycle_controller.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * Real: LifecycleController on its own FreeRTOS task, HealthMonitor, LedDriver (LD2, PB14),
 * Logger, the 80 MHz clock and the reset-cause flags. Scripted: every other collaborator
 * (ConfigStore, ConfigService, SensorService, CloudPublisher, ModbusPoller, TimeService,
 * UpdateService, FirmwareStore, ResetDriver). The Gateway has no UpdateService, FirmwareStore or
 * ResetDriver yet, and the others expose different APIs than the LLD assumed (LC-D1), so each is
 * a small fake here that prints its name when LifecycleTask calls it - the printed order IS the
 * Init sequence. The real wiring comes with the Gateway main().
 *
 * Real timers are used throughout: the 30 s restart window and the 5 min edit window are waited
 * out, so the whole run takes about 6 minutes.
 *
 * Automated test sequence:
 *   TC-HW-LC-001  Reset cause is detected, the RCC flags are cleared (L4 RMVF bit), and
 *                 get_reset_cause() agrees.
 *   TC-HW-LC-002  LifecycleTask runs the Init sequence in order and reaches OPERATIONAL; the
 *                 HealthMonitor snapshot and LD2 (solid on) follow.
 *   TC-HW-LC-003  A task blocked on the start gate is released by Operational entry.
 *   TC-HW-LC-004  The idle loop kicks the watchdog about once a second.
 *   TC-HW-LC-005  RESET_METRICS zeroes the HealthMonitor counters and does not change state.
 *   TC-HW-LC-006  EditingConfig APPLY: snapshot, then commit and reconfigure.
 *   TC-HW-LC-007  EditingConfig CANCEL: snapshot, then restore, no commit.
 *   TC-HW-LC-008  A restart requested while editing is held back, then replayed after APPLY.
 *   TC-HW-LC-009  Unconfirmed restart: the real 30 s timer returns to OPERATIONAL, no reset.
 *   TC-HW-LC-010  Confirmed restart: soft_reset is called (really, with BENCH_REAL_RESET).
 *   TC-HW-LC-011  OTA accepted; a second OTA request is refused; SELF_CHECK_PASS confirms.
 *   TC-HW-LC-012  The real 5 min edit timer rolls the edit back.
 *   TC-HW-LC-013  OTA then SELF_CHECK_FAIL: rollback and soft_reset.
 *   TC-HW-LC-014  Fault: FAULTED, LD2 fast blink, every later event ignored, watchdog still kicked.
 *
 * Integration checklist:
 *   [ ] All fourteen cases pass
 *   [ ] LD2 ends in a rapid blink (the fault pattern) and stays there
 *   [ ] With -DBENCH_REAL_RESET=1 (after a power cycle, so the boot cause is not SOFT) the board
 *       resets in TC-HW-LC-010 and the next banner reports "reset cause: SOFT"
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "event_groups.h"
#include "task.h"

#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "health_monitor/health_monitor.h"
#include "led/led_driver.h"
#include "lifecycle_controller/lifecycle_controller.h"
#include "logger/logger.h"
#include "rtc/rtc.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Board and timing constants                                              */
/* ---------------------------------------------------------------------- */

#define LD2_PIN (14U)

#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)
#define BRINGUP_TX_TIMEOUT_MS (100U)

#define LIFECYCLE_STACK_WORDS (2048U) /* companion §1: 8 KB */
#define LIFECYCLE_PRIORITY (1U)
#define BRINGUP_STACK_WORDS (768U)
#define WAITER_STACK_WORDS (256U)
#define TASK_PRIORITY (2U)

#define POLL_MS (10U)
#define SETTLE_MS (150U)
#define OPERATIONAL_TIMEOUT_MS (3000U)
#define STATE_TIMEOUT_MS (1000U)
#define WATCHDOG_WINDOW_MS (3500U)
#define WATCHDOG_KICKS_MIN (2U)
#define WATCHDOG_KICKS_MAX (5U)

#define RESTART_WINDOW_MIN_MS (28000U)
#define RESTART_WINDOW_MAX_MS (33000U)
#define EDIT_WINDOW_MIN_MS (295000U)
#define EDIT_WINDOW_MAX_MS (306000U)

#define FAKE_SNAPSHOT_BYTES (16U)
#define FAKE_OTA_IMAGE_BYTES (65536UL)
#define FAULT_CODE (0xABU)
#define CALL_LOG_MAX (32U)

#ifndef BENCH_REAL_RESET
#define BENCH_REAL_RESET (0)
#endif

/* ---------------------------------------------------------------------- */
/* Pin table — Gateway                                                     */
/* ---------------------------------------------------------------------- */

static const led_pin_t k_gw_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = (uint8_t) GPIO_PORT_B,
                   .pin = (uint8_t) LD2_PIN,
                   .active_high = true,
                   .fitted = true},
    [LED_RED] = {.port = (uint8_t) GPIO_PORT_A,
                 .pin = 5U,
                 .active_high = true,
                 .fitted = false}, /* LD1 / SB1 - shared with SPI1_SCK */
};

/* ---------------------------------------------------------------------- */
/* Reporting helpers                                                       */
/* ---------------------------------------------------------------------- */

static void bringup_puts(const char *s)
{
    (void) debug_uart_send((const uint8_t *) s, strlen(s), BRINGUP_TX_TIMEOUT_MS);
}

static void bringup_pass(const char *label)
{
    bringup_puts("[PASS] ");
    bringup_puts(label);
    bringup_puts("\r\n");
}

static void bringup_info(const char *label)
{
    bringup_puts("[INFO] ");
    bringup_puts(label);
    bringup_puts("\r\n");
}

static void bringup_fail(const char *label)
{
    bringup_puts("[FAIL] ");
    bringup_puts(label);
    bringup_puts(" - HALTED\r\n");

    /* LD2 belongs to HealthMonitor, so failure is reported on the UART only. */
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

static void check(bool condition, const char *label)
{
    if (!condition)
    {
        bringup_fail(label);
    }
}

/* ---------------------------------------------------------------------- */
/* Fake collaborators                                                      */
/* ---------------------------------------------------------------------- */

typedef enum
{
    F_CS_CHECK,
    F_FW_FLAGS,
    F_CS_LOAD,
    F_CM_APPLY,
    F_SENS_INIT,
    F_ALARM_INIT,
    F_POLL_INIT,
    F_CLOUD_INIT,
    F_TIME_INIT,
    F_UPD_INIT,
    F_CON_INIT,
    F_HR_INIT,
    F_SENS_READY,
    F_POLL_READY,
    F_CLOUD_READY,
    F_CM_SNAPSHOT,
    F_CM_RESTORE,
    F_CM_COMMIT,
    F_SENS_RECONF,
    F_CLOUD_FLUSH,
    F_UPD_START,
    F_UPD_ROLLBACK,
    F_FW_CONFIRM,
    F_FW_ROLLBACK,
    F_RESET,
    F_WDG,
    F_COUNT
} fake_id_t;

static const char *const k_fake_names[F_COUNT] = {
    [F_CS_CHECK] = "config_store.check_integrity",
    [F_FW_FLAGS] = "firmware_store.get_pending_flags",
    [F_CS_LOAD] = "config_store.load",
    [F_CM_APPLY] = "config_manager.apply_loaded",
    [F_SENS_INIT] = "sensors.init",
    [F_ALARM_INIT] = "alarms.init",
    [F_POLL_INIT] = "modbus_poller.init",
    [F_CLOUD_INIT] = "cloud.init",
    [F_TIME_INIT] = "time_service.init",
    [F_UPD_INIT] = "update_service.init",
    [F_CON_INIT] = "console.init_finalise",
    [F_HR_INIT] = "health_report.init",
    [F_SENS_READY] = "sensors.is_ready",
    [F_POLL_READY] = "modbus_poller.is_ready",
    [F_CLOUD_READY] = "cloud.is_ready",
    [F_CM_SNAPSHOT] = "config_manager.snapshot",
    [F_CM_RESTORE] = "config_manager.restore_snapshot",
    [F_CM_COMMIT] = "config_manager.commit",
    [F_SENS_RECONF] = "sensors.reconfigure",
    [F_CLOUD_FLUSH] = "cloud.flush",
    [F_UPD_START] = "update_service.start",
    [F_UPD_ROLLBACK] = "update_service.resume_rollback",
    [F_FW_CONFIRM] = "firmware_store.confirm_self_check",
    [F_FW_ROLLBACK] = "firmware_store.rollback",
    [F_RESET] = "reset_driver.soft_reset",
    [F_WDG] = "watchdog.kick",
};

static volatile uint32_t s_calls[F_COUNT];
static volatile fake_id_t s_call_log[CALL_LOG_MAX];
static volatile uint32_t s_call_log_len;
static volatile uint32_t s_update_size_seen;
static volatile bool s_real_reset_armed;

/** Count a call, log its order, and print its name (the watchdog is too chatty to print). */
static void fake_note(fake_id_t id)
{
    s_calls[id]++;
    if (s_call_log_len < CALL_LOG_MAX)
    {
        s_call_log[s_call_log_len] = id;
        s_call_log_len++;
    }
    if (id != F_WDG)
    {
        bringup_puts("       . ");
        bringup_puts(k_fake_names[id]);
        bringup_puts("\r\n");
    }
}

static lc_dep_status_t fake_cs_check(void *ctx)
{
    (void) ctx;
    fake_note(F_CS_CHECK);
    return LC_DEP_OK;
}

static lc_dep_status_t fake_cs_load(void *ctx, void *buf, uint32_t *len_out, uint32_t max_len)
{
    (void) ctx;
    (void) buf;
    (void) max_len;
    fake_note(F_CS_LOAD);
    *len_out = FAKE_SNAPSHOT_BYTES;
    return LC_DEP_OK;
}

static lc_dep_status_t fake_cm_apply(void *ctx, const void *buf, uint32_t len)
{
    (void) ctx;
    (void) buf;
    (void) len;
    fake_note(F_CM_APPLY);
    return LC_DEP_OK;
}

static lc_dep_status_t fake_cm_snapshot(void *ctx, void *buf, uint32_t *len_out, uint32_t max_len)
{
    (void) ctx;
    (void) max_len;
    fake_note(F_CM_SNAPSHOT);
    (void) memset(buf, 0x5A, FAKE_SNAPSHOT_BYTES);
    *len_out = FAKE_SNAPSHOT_BYTES;
    return LC_DEP_OK;
}

static lc_dep_status_t fake_cm_restore(void *ctx, const void *buf, uint32_t len)
{
    (void) ctx;
    (void) buf;
    (void) len;
    fake_note(F_CM_RESTORE);
    return LC_DEP_OK;
}

static lc_dep_status_t fake_cm_commit(void *ctx)
{
    (void) ctx;
    fake_note(F_CM_COMMIT);
    return LC_DEP_OK;
}

/* Nothing is deferred in this harness, so the periodic persist is a silent no-op. */
static lc_dep_status_t fake_cm_persist(void *ctx)
{
    (void) ctx;
    return LC_DEP_OK;
}

static lc_dep_status_t fake_sens_init(void *ctx)
{
    (void) ctx;
    fake_note(F_SENS_INIT);
    return LC_DEP_OK;
}

static bool fake_sens_ready(void *ctx)
{
    (void) ctx;
    fake_note(F_SENS_READY);
    return true;
}

static lc_dep_status_t fake_sens_reconf(void *ctx)
{
    (void) ctx;
    fake_note(F_SENS_RECONF);
    return LC_DEP_OK;
}

static lc_dep_status_t fake_alarm_init(void *ctx)
{
    (void) ctx;
    fake_note(F_ALARM_INIT);
    return LC_DEP_OK;
}

static lc_dep_status_t fake_con_init(void *ctx)
{
    (void) ctx;
    fake_note(F_CON_INIT);
    return LC_DEP_OK;
}

static lc_dep_status_t fake_cloud_init(void *ctx)
{
    (void) ctx;
    fake_note(F_CLOUD_INIT);
    return LC_DEP_OK;
}

static lc_dep_status_t fake_cloud_flush(void *ctx)
{
    (void) ctx;
    fake_note(F_CLOUD_FLUSH);
    return LC_DEP_OK;
}

static bool fake_cloud_ready(void *ctx)
{
    (void) ctx;
    fake_note(F_CLOUD_READY);
    return true;
}

static lc_dep_status_t fake_poll_init(void *ctx)
{
    (void) ctx;
    fake_note(F_POLL_INIT);
    return LC_DEP_OK;
}

static bool fake_poll_ready(void *ctx)
{
    (void) ctx;
    fake_note(F_POLL_READY);
    return true;
}

static lc_dep_status_t fake_time_init(void *ctx)
{
    (void) ctx;
    fake_note(F_TIME_INIT);
    return LC_DEP_OK;
}

static lc_dep_status_t fake_upd_init(void *ctx)
{
    (void) ctx;
    fake_note(F_UPD_INIT);
    return LC_DEP_OK;
}

static lc_dep_status_t fake_upd_start(void *ctx, uint32_t job)
{
    (void) ctx;
    s_update_size_seen = job;
    fake_note(F_UPD_START);
    return LC_DEP_OK;
}

static lc_dep_status_t fake_upd_self(void *ctx)
{
    (void) ctx;
    return LC_DEP_OK;
}

static lc_dep_status_t fake_upd_rollback(void *ctx)
{
    (void) ctx;
    fake_note(F_UPD_ROLLBACK);
    return LC_DEP_OK;
}

static lc_dep_status_t fake_upd_after_rb(void *ctx)
{
    (void) ctx;
    return LC_DEP_OK;
}

static lc_dep_status_t fake_fw_flags(void *ctx, bool *self_check_out, bool *rollback_out)
{
    (void) ctx;
    fake_note(F_FW_FLAGS);
    *self_check_out = false;
    *rollback_out = false;
    return LC_DEP_OK;
}

static lc_dep_status_t fake_fw_confirm(void *ctx)
{
    (void) ctx;
    fake_note(F_FW_CONFIRM);
    return LC_DEP_OK;
}

static lc_dep_status_t fake_fw_rollback(void *ctx)
{
    (void) ctx;
    fake_note(F_FW_ROLLBACK);
    return LC_DEP_OK;
}

static void fake_reset(void *ctx)
{
    (void) ctx;
    fake_note(F_RESET);
#ifdef STM32L475xx
    if (s_real_reset_armed)
    {
        bringup_puts("[INFO] reset_driver.soft_reset: resetting the board now\r\n");
        NVIC_SystemReset();
    }
#endif
}

static void fake_wdg(void *ctx)
{
    (void) ctx;
    fake_note(F_WDG);
}

/* ---------------------------------------------------------------------- */
/* Real HealthMonitor behind the controller's health interfaces            */
/* ---------------------------------------------------------------------- */

static health_monitor_handle_t s_hm;

static lc_dep_status_t hm_status(health_monitor_err_t err)
{
    return (err == HEALTH_MONITOR_ERR_OK) ? LC_DEP_OK : LC_DEP_FAIL;
}

static lc_dep_status_t hr_init(void *ctx)
{
    (void) ctx;
    fake_note(F_HR_INIT);
    return LC_DEP_OK; /* HealthMonitor was created in main(); nothing left to start */
}

static lc_dep_status_t hr_push_fault(void *ctx, uint32_t fault_code)
{
    (void) ctx;
    return hm_status(health_monitor_push_event(s_hm, HEALTH_EVENT_FAULT, fault_code));
}

static lc_dep_status_t hr_update_lifecycle(void *ctx, lifecycle_state_t state)
{
    (void) ctx;
    return hm_status(health_monitor_update_lifecycle(s_hm, state));
}

static lc_dep_status_t ha_reset_metrics(void *ctx)
{
    (void) ctx;
    return hm_status(health_monitor_reset_metrics(s_hm));
}

static lc_dep_status_t ha_set_led_fault(void *ctx)
{
    (void) ctx;
    return hm_status(health_monitor_set_led_fault(s_hm));
}

/* ---------------------------------------------------------------------- */
/* Interface tables                                                        */
/* ---------------------------------------------------------------------- */

static const lc_config_store_if_t k_config_store = {.check_integrity = fake_cs_check,
                                                    .load = fake_cs_load};
static const lc_config_manager_if_t k_config_manager = {.apply_loaded = fake_cm_apply,
                                                        .snapshot = fake_cm_snapshot,
                                                        .restore_snapshot = fake_cm_restore,
                                                        .commit = fake_cm_commit,
                                                        .persist_pending = fake_cm_persist};
static const lc_sensors_if_t k_sensors = {
    .init = fake_sens_init, .is_ready = fake_sens_ready, .reconfigure = fake_sens_reconf};
static const lc_alarms_if_t k_alarms = {.init = fake_alarm_init};
static const lc_console_if_t k_console = {.init_finalise = fake_con_init};
static const lc_health_report_if_t k_health_report = {
    .init = hr_init, .push_fault = hr_push_fault, .update_lifecycle = hr_update_lifecycle};
static const lc_health_admin_if_t k_health_admin = {.reset_metrics = ha_reset_metrics,
                                                    .set_led_fault = ha_set_led_fault};
static const lc_cloud_if_t k_cloud = {
    .init = fake_cloud_init, .flush = fake_cloud_flush, .is_ready = fake_cloud_ready};
static const lc_modbus_poller_if_t k_poller = {.init = fake_poll_init, .is_ready = fake_poll_ready};
static const lc_update_service_if_t k_update = {.init = fake_upd_init,
                                                .start = fake_upd_start,
                                                .resume_self_checking = fake_upd_self,
                                                .resume_rollback = fake_upd_rollback,
                                                .resume_after_rollback = fake_upd_after_rb};
static const lc_time_service_if_t k_time = {.init = fake_time_init};
static const lc_firmware_store_if_t k_firmware = {.get_pending_flags = fake_fw_flags,
                                                  .confirm_self_check = fake_fw_confirm,
                                                  .rollback = fake_fw_rollback};
static const lc_reset_driver_if_t k_reset = {.soft_reset = fake_reset};
static const lc_watchdog_if_t k_watchdog = {.kick = fake_wdg};

static const lifecycle_deps_t k_deps = {.config_store = &k_config_store,
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

/* ---------------------------------------------------------------------- */
/* Tasks and static storage                                                */
/* ---------------------------------------------------------------------- */

static StaticTask_t s_lc_tcb;
static StackType_t s_lc_stack[LIFECYCLE_STACK_WORDS];
static StaticTask_t s_main_tcb;
static StackType_t s_main_stack[BRINGUP_STACK_WORDS];
static StaticTask_t s_waiter_tcb;
static StackType_t s_waiter_stack[WAITER_STACK_WORDS];

static lifecycle_reset_cause_t s_boot_cause;
static uint32_t s_csr_after_detect;
static volatile bool s_gate_released;
static volatile TickType_t s_gate_released_tick;

static const char *cause_name(lifecycle_reset_cause_t cause)
{
    switch (cause)
    {
    case LIFECYCLE_RESET_POWER_ON:
        return "POWER_ON";
    case LIFECYCLE_RESET_SOFT:
        return "SOFT";
    case LIFECYCLE_RESET_WATCHDOG:
        return "WATCHDOG";
    default:
        return "UNKNOWN";
    }
}

/** Blocks on the start gate, exactly as the other application tasks will. */
static void gate_waiter_task(void *arg)
{
    (void) arg;
    (void) xEventGroupWaitBits(lifecycle_get_start_gate(), LIFECYCLE_START_GATE_BIT, pdFALSE,
                               pdTRUE, portMAX_DELAY);
    s_gate_released_tick = xTaskGetTickCount();
    s_gate_released = true;
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------- */
/* Test helpers                                                            */
/* ---------------------------------------------------------------------- */

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/** Wait until the controller reports @p state; returns the elapsed ms, or 0xFFFFFFFF on timeout. */
static uint32_t wait_state(lifecycle_state_t state, uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms))
    {
        if (lifecycle_controller->get_state() == state)
        {
            return (uint32_t) (xTaskGetTickCount() - start);
        }
        delay_ms(POLL_MS);
    }
    return 0xFFFFFFFFU;
}

static void post(lifecycle_event_type_t type, uint32_t param)
{
    const lifecycle_event_t ev = {.type = type, .param = param};

    check(lifecycle_controller->post_event(ev), "post_event refused an event");
}

static health_snapshot_t take_snapshot(void)
{
    health_snapshot_t snap;

    check(health_monitor_get_snapshot(s_hm, &snap) == HEALTH_MONITOR_ERR_OK, "get_snapshot");
    return snap;
}

static bool led_is(led_pattern_t expected)
{
    led_pattern_t pattern = LED_PATTERN_OFF;

    return (led_get_pattern(LED_GREEN, &pattern) == LED_ERR_OK) && (pattern == expected);
}

static bool init_order_is_correct(void)
{
    static const fake_id_t k_expected[] = {
        F_CS_CHECK,   F_FW_FLAGS,  F_CS_LOAD,    F_CM_APPLY,   F_SENS_INIT,
        F_ALARM_INIT, F_POLL_INIT, F_CLOUD_INIT, F_TIME_INIT,  F_UPD_INIT,
        F_CON_INIT,   F_HR_INIT,   F_SENS_READY, F_POLL_READY, F_CLOUD_READY,
    };

    for (uint32_t i = 0U; i < (sizeof(k_expected) / sizeof(k_expected[0])); i++)
    {
        if ((i >= s_call_log_len) || (s_call_log[i] != k_expected[i]))
        {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* The test sequence                                                       */
/* ---------------------------------------------------------------------- */

static void bringup_task(void *arg)
{
    (void) arg;

    char msg[112];

    bringup_puts("\r\n===== LifecycleController Hardware Bring-up =====\r\n");
    bringup_puts("Board : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    (void) snprintf(msg, sizeof(msg), "Boot  : reset cause: %s\r\n", cause_name(s_boot_cause));
    bringup_puts(msg);
    bringup_puts("LED   : LD2 green, PB14 (HealthMonitor)\r\n");
    bringup_puts("=================================================\r\n\r\n");

    /* TC-HW-LC-001 */
    check(s_boot_cause != LIFECYCLE_RESET_UNKNOWN, "TC-HW-LC-001 no reset flag was set");
    check(lifecycle_controller->get_reset_cause() == s_boot_cause, "TC-HW-LC-001 get_reset_cause");
    check((s_csr_after_detect &
           (RCC_CSR_PINRSTF | RCC_CSR_BORRSTF | RCC_CSR_SFTRSTF | RCC_CSR_IWDGRSTF)) == 0U,
          "TC-HW-LC-001 RMVF did not clear the flags");
    bringup_pass("TC-HW-LC-001 reset cause detected, flags cleared, get_reset_cause() agrees");

    /* TC-HW-LC-002 - LifecycleTask has been running since the scheduler started. */
    check(wait_state(LIFECYCLE_STATE_OPERATIONAL, OPERATIONAL_TIMEOUT_MS) != 0xFFFFFFFFU,
          "TC-HW-LC-002 never reached OPERATIONAL");
    delay_ms(SETTLE_MS);
    check(init_order_is_correct(), "TC-HW-LC-002 Init sequence order");
    check(take_snapshot().lifecycle_state == LIFECYCLE_STATE_OPERATIONAL,
          "TC-HW-LC-002 HealthMonitor lifecycle_state");
    check(led_is(LED_PATTERN_ON), "TC-HW-LC-002 LD2 is not solid on");
    bringup_pass("TC-HW-LC-002 Init sequence in order, OPERATIONAL, HealthMonitor + LD2 follow");

    /* TC-HW-LC-003 */
    check(s_gate_released, "TC-HW-LC-003 the waiting task was not released");
    bringup_pass("TC-HW-LC-003 start gate released the blocked task");

    /* TC-HW-LC-004 */
    {
        const uint32_t before = s_calls[F_WDG];

        delay_ms(WATCHDOG_WINDOW_MS);
        const uint32_t kicks = s_calls[F_WDG] - before;

        (void) snprintf(msg, sizeof(msg), "%lu kicks in %u ms", (unsigned long) kicks,
                        (unsigned) WATCHDOG_WINDOW_MS);
        bringup_info(msg);
        check((kicks >= WATCHDOG_KICKS_MIN) && (kicks <= WATCHDOG_KICKS_MAX),
              "TC-HW-LC-004 watchdog cadence");
    }
    bringup_pass("TC-HW-LC-004 idle loop kicks the watchdog about once a second");

    /* TC-HW-LC-005 */
    check(health_monitor_push_event(s_hm, HEALTH_EVENT_SENSOR_FAIL, 0U) == HEALTH_MONITOR_ERR_OK,
          "TC-HW-LC-005 push_event");
    check(take_snapshot().sensor_fail_count == 1U, "TC-HW-LC-005 counter did not rise");
    check(lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS) ==
              LIFECYCLE_ERR_OK,
          "TC-HW-LC-005 RESET_METRICS");
    check(take_snapshot().sensor_fail_count == 0U, "TC-HW-LC-005 counter not zeroed");
    check(lifecycle_controller->get_state() == LIFECYCLE_STATE_OPERATIONAL,
          "TC-HW-LC-005 state changed");
    bringup_pass("TC-HW-LC-005 RESET_METRICS zeroes the counters, state unchanged");

    /* TC-HW-LC-006 */
    post(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    check(wait_state(LIFECYCLE_STATE_EDITING_CONFIG, STATE_TIMEOUT_MS) != 0xFFFFFFFFU,
          "TC-HW-LC-006 EDITING_CONFIG");
    check(s_calls[F_CM_SNAPSHOT] == 1U, "TC-HW-LC-006 snapshot");
    check(take_snapshot().lifecycle_state == LIFECYCLE_STATE_EDITING_CONFIG,
          "TC-HW-LC-006 HealthMonitor follows");
    post(LC_EVENT_CONFIG_EDIT_APPLY, 0U);
    check(wait_state(LIFECYCLE_STATE_OPERATIONAL, STATE_TIMEOUT_MS) != 0xFFFFFFFFU,
          "TC-HW-LC-006 back to OPERATIONAL");
    check((s_calls[F_CM_COMMIT] == 1U) && (s_calls[F_SENS_RECONF] == 1U) &&
              (s_calls[F_CM_RESTORE] == 0U),
          "TC-HW-LC-006 commit / reconfigure");
    bringup_pass("TC-HW-LC-006 EditingConfig APPLY: snapshot, commit, reconfigure");

    /* TC-HW-LC-007 */
    post(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    check(wait_state(LIFECYCLE_STATE_EDITING_CONFIG, STATE_TIMEOUT_MS) != 0xFFFFFFFFU,
          "TC-HW-LC-007 EDITING_CONFIG");
    post(LC_EVENT_CONFIG_EDIT_CANCEL, 0U);
    check(wait_state(LIFECYCLE_STATE_OPERATIONAL, STATE_TIMEOUT_MS) != 0xFFFFFFFFU,
          "TC-HW-LC-007 back to OPERATIONAL");
    check((s_calls[F_CM_SNAPSHOT] == 2U) && (s_calls[F_CM_RESTORE] == 1U) &&
              (s_calls[F_CM_COMMIT] == 1U),
          "TC-HW-LC-007 restore without commit");
    bringup_pass("TC-HW-LC-007 EditingConfig CANCEL: snapshot, restore, no commit");

    /* TC-HW-LC-008 */
    post(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    check(wait_state(LIFECYCLE_STATE_EDITING_CONFIG, STATE_TIMEOUT_MS) != 0xFFFFFFFFU,
          "TC-HW-LC-008 EDITING_CONFIG");
    check(lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_SOFT_RESTART) ==
              LIFECYCLE_ERR_OK,
          "TC-HW-LC-008 SOFT_RESTART");
    delay_ms(SETTLE_MS);
    check(lifecycle_controller->get_state() == LIFECYCLE_STATE_EDITING_CONFIG,
          "TC-HW-LC-008 restart interrupted the edit");
    check(s_calls[F_CLOUD_FLUSH] == 0U, "TC-HW-LC-008 restart acted during the edit");
    post(LC_EVENT_CONFIG_EDIT_APPLY, 0U);
    check(wait_state(LIFECYCLE_STATE_RESTARTING, STATE_TIMEOUT_MS) != 0xFFFFFFFFU,
          "TC-HW-LC-008 held restart was not replayed");
    check(s_calls[F_CLOUD_FLUSH] == 1U, "TC-HW-LC-008 flush");
    bringup_pass("TC-HW-LC-008 restart requested while editing waits, then replays after APPLY");

    /* TC-HW-LC-009 - still RESTARTING from the replay; the real 30 s window runs from there. */
    {
        const uint32_t elapsed = wait_state(LIFECYCLE_STATE_OPERATIONAL, RESTART_WINDOW_MAX_MS);

        (void) snprintf(msg, sizeof(msg), "restart window expired after %lu ms",
                        (unsigned long) elapsed);
        bringup_info(msg);
        check(elapsed != 0xFFFFFFFFU, "TC-HW-LC-009 never returned to OPERATIONAL");
        check(elapsed >= RESTART_WINDOW_MIN_MS, "TC-HW-LC-009 window shorter than 30 s");
        check(s_calls[F_RESET] == 0U, "TC-HW-LC-009 the board was reset");
    }
    bringup_pass("TC-HW-LC-009 unconfirmed restart: the 30 s timer returns to OPERATIONAL");

    /* TC-HW-LC-010 */
    check(lifecycle_controller->handle_remote_command(LC_REMOTE_CMD_SOFT_RESTART) ==
              LIFECYCLE_ERR_OK,
          "TC-HW-LC-010 SOFT_RESTART");
    check(wait_state(LIFECYCLE_STATE_RESTARTING, STATE_TIMEOUT_MS) != 0xFFFFFFFFU,
          "TC-HW-LC-010 RESTARTING");
    check(s_calls[F_CLOUD_FLUSH] == 2U, "TC-HW-LC-010 flush");
    check(take_snapshot().lifecycle_state == LIFECYCLE_STATE_RESTARTING,
          "TC-HW-LC-010 HealthMonitor follows");
#if BENCH_REAL_RESET
    s_real_reset_armed = (s_boot_cause != LIFECYCLE_RESET_SOFT);
#endif
    post(LC_EVENT_RESTART_CONFIRMED, 0U);
    delay_ms(SETTLE_MS); /* a real reset ends the run here and the next banner says SOFT */
    check(s_calls[F_RESET] == 1U, "TC-HW-LC-010 soft_reset was not called");
    s_real_reset_armed = false;
    post(LC_EVENT_RESTART_TIMEOUT, 0U); /* the fake returned, so leave RESTARTING by hand */
    check(wait_state(LIFECYCLE_STATE_OPERATIONAL, STATE_TIMEOUT_MS) != 0xFFFFFFFFU,
          "TC-HW-LC-010 back to OPERATIONAL");
    bringup_pass("TC-HW-LC-010 confirmed restart calls soft_reset after the cloud flush");

    /* TC-HW-LC-011 */
    post(LC_EVENT_OTA_REQUESTED, (uint32_t) FAKE_OTA_IMAGE_BYTES);
    check(wait_state(LIFECYCLE_STATE_UPDATING_FW, STATE_TIMEOUT_MS) != 0xFFFFFFFFU,
          "TC-HW-LC-011 UPDATING_FW");
    check((s_calls[F_UPD_START] == 1U) && (s_update_size_seen == FAKE_OTA_IMAGE_BYTES),
          "TC-HW-LC-011 start(size)");
    check(!lifecycle_controller->post_event(
              (lifecycle_event_t){.type = LC_EVENT_OTA_REQUESTED, .param = 1U}),
          "TC-HW-LC-011 a second OTA request was accepted");
    post(LC_EVENT_SELF_CHECK_PASS, 0U);
    check(wait_state(LIFECYCLE_STATE_OPERATIONAL, STATE_TIMEOUT_MS) != 0xFFFFFFFFU,
          "TC-HW-LC-011 back to OPERATIONAL");
    check((s_calls[F_FW_CONFIRM] == 1U) && (s_calls[F_UPD_START] == 1U),
          "TC-HW-LC-011 confirm / no second start");
    bringup_pass("TC-HW-LC-011 OTA accepted, second request refused, SELF_CHECK_PASS confirms");

    /* TC-HW-LC-012 */
    post(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    check(wait_state(LIFECYCLE_STATE_EDITING_CONFIG, STATE_TIMEOUT_MS) != 0xFFFFFFFFU,
          "TC-HW-LC-012 EDITING_CONFIG");
    bringup_info("waiting out the real 5 minute edit timer ...");
    {
        const uint32_t restores = s_calls[F_CM_RESTORE];
        const uint32_t elapsed = wait_state(LIFECYCLE_STATE_OPERATIONAL, EDIT_WINDOW_MAX_MS);

        (void) snprintf(msg, sizeof(msg), "edit window expired after %lu ms",
                        (unsigned long) elapsed);
        bringup_info(msg);
        check(elapsed != 0xFFFFFFFFU, "TC-HW-LC-012 never returned to OPERATIONAL");
        check(elapsed >= EDIT_WINDOW_MIN_MS, "TC-HW-LC-012 window shorter than 5 min");
        check(s_calls[F_CM_RESTORE] == (restores + 1U), "TC-HW-LC-012 edit not rolled back");
    }
    bringup_pass("TC-HW-LC-012 the 5 min edit timer rolls the edit back");

    /* TC-HW-LC-013 */
    post(LC_EVENT_OTA_REQUESTED, 4096U);
    check(wait_state(LIFECYCLE_STATE_UPDATING_FW, STATE_TIMEOUT_MS) != 0xFFFFFFFFU,
          "TC-HW-LC-013 UPDATING_FW");
    post(LC_EVENT_SELF_CHECK_FAIL, 0U);
    delay_ms(SETTLE_MS);
    check((s_calls[F_UPD_ROLLBACK] == 1U) && (s_calls[F_FW_ROLLBACK] == 1U) &&
              (s_calls[F_RESET] == 2U),
          "TC-HW-LC-013 rollback / reset");
    bringup_pass("TC-HW-LC-013 SELF_CHECK_FAIL: resume_rollback, rollback, soft_reset");

    /* TC-HW-LC-014 - from UPDATING_FW, where the faked reset left the machine. */
    post(LC_EVENT_UNRECOVERABLE_FAULT, FAULT_CODE);
    check(wait_state(LIFECYCLE_STATE_FAULTED, STATE_TIMEOUT_MS) != 0xFFFFFFFFU,
          "TC-HW-LC-014 FAULTED");
    delay_ms(SETTLE_MS);
    check(take_snapshot().lifecycle_state == LIFECYCLE_STATE_FAULTED,
          "TC-HW-LC-014 HealthMonitor follows");
    check(led_is(LED_PATTERN_BLINK_FAST), "TC-HW-LC-014 LD2 is not blinking fast");
    post(LC_EVENT_RESTART_REQUESTED, 0U);
    post(LC_EVENT_CONFIG_EDIT_ENTER, 0U);
    delay_ms(SETTLE_MS);
    check((lifecycle_controller->get_state() == LIFECYCLE_STATE_FAULTED) &&
              (s_calls[F_CLOUD_FLUSH] == 2U) && (s_calls[F_CM_SNAPSHOT] == 4U),
          "TC-HW-LC-014 an event left FAULTED");
    {
        const uint32_t before = s_calls[F_WDG];

        delay_ms(WATCHDOG_WINDOW_MS);
        check((s_calls[F_WDG] - before) >= WATCHDOG_KICKS_MIN,
              "TC-HW-LC-014 watchdog not kicked while FAULTED");
    }
    bringup_pass("TC-HW-LC-014 FAULTED: LD2 fast blink, events ignored, watchdog still kicked");

    bringup_puts("\r\n===== ALL TESTS PASSED =====\r\n");
    bringup_puts("LD2 should now be blinking rapidly (the fault pattern).\r\n");

    for (;;)
    {
        delay_ms(1000U);
    }
}

/* ---------------------------------------------------------------------- */
/* main                                                                    */
/* ---------------------------------------------------------------------- */

int main(void)
{
    /* The reset flags are read before anything else can disturb them (companion §13). */
    s_boot_cause = lifecycle_detect_reset_cause();
    s_csr_after_detect = RCC->CSR;

    /* cpu_init() first - clock to 80 MHz, DWT, fault handlers. */
    if (cpu_init() != STATUS_OK)
    {
        for (;;)
        {
            /* Nothing can be reported before the UART exists. */
        }
    }

    gpio_pin_config_t uart_tx_config = {
        .port = BRINGUP_UART_TX_PORT,
        .pin = BRINGUP_UART_TX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_NONE,
        .alternate = BRINGUP_UART_AF,
    };
    gpio_pin_config_t uart_rx_config = {
        .port = BRINGUP_UART_RX_PORT,
        .pin = BRINGUP_UART_RX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_UP,
        .alternate = BRINGUP_UART_AF,
    };

    if ((gpio_init() != GPIO_OK) || (gpio_configure_pin(&uart_tx_config) != GPIO_OK) ||
        (gpio_configure_pin(&uart_rx_config) != GPIO_OK) || (debug_uart_init() != DEBUG_UART_OK) ||
        (rtc_init() != RTC_OK)) /* Logger timestamps from the RTC */
    {
        for (;;)
        {
            /* No sink to report to. */
        }
    }

    (void) logger_init(LOG_LEVEL_INFO);

    if (led_init(k_gw_led_pins, (uint8_t) LED_COUNT) != LED_ERR_OK)
    {
        bringup_puts("[FAIL] led_init - HALTED\r\n");
        for (;;)
        {
        }
    }

    health_monitor_config_t hm_config = {.boot_reset_cause = s_boot_cause};

    if (health_monitor_create(&hm_config, &s_hm) != HEALTH_MONITOR_ERR_OK)
    {
        bringup_puts("[FAIL] health_monitor_create - HALTED\r\n");
        for (;;)
        {
        }
    }

    if (lifecycle_controller_init(s_boot_cause, &k_deps) != LIFECYCLE_ERR_OK)
    {
        bringup_puts("[FAIL] lifecycle_controller_init - HALTED\r\n");
        for (;;)
        {
        }
    }

    /* LifecycleTask is created last, as the companion's boot order says (§17). */
    (void) xTaskCreateStatic(gate_waiter_task, "gate_wait", WAITER_STACK_WORDS, NULL, TASK_PRIORITY,
                             s_waiter_stack, &s_waiter_tcb);
    (void) xTaskCreateStatic(bringup_task, "lc_bringup", BRINGUP_STACK_WORDS, NULL, TASK_PRIORITY,
                             s_main_stack, &s_main_tcb);
    (void) xTaskCreateStatic(lifecycle_task_body, "lifecycle", LIFECYCLE_STACK_WORDS, NULL,
                             LIFECYCLE_PRIORITY, s_lc_stack, &s_lc_tcb);

    vTaskStartScheduler();

    /* Only reached if the scheduler fails to start. */
    for (;;)
    {
    }
}
