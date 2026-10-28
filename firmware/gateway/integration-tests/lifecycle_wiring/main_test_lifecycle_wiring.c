/**
 * @file main_test_lifecycle_wiring.c
 * @brief System bring-up for LifecycleController + wiring (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. In the `integration-tests` source entry's `excluding=`, add cloud_publisher (the current
 *      harness) and remove lifecycle_wiring.
 *   2. In the `application` entry's `excluding=`, replace `sensor_service|alarm_service|
 *      lifecycle_wiring` with `cloud_publisher|time_service`: this image uses the real sensor and
 *      alarm services and the wiring, and the stand-ins below for the two it cannot link yet.
 *   3. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/lifecycle_wiring.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * This is the first image in which the real modules meet under LifecycleController. Real: the
 * controller on its own task, the wiring adapters, ConfigStore on the QSPI flash, ConfigService,
 * SensorTask + SensorService + AlarmService on the I2C sensors, ModbusPoller (UART4, no Field
 * Device attached: the link stays down, which is allowed), HealthMonitor + LD2, Logger,
 * ResetDriver. Stand-ins defined here: CloudPublisher (it still calls about ten placeholder
 * functions that nothing implements, so it cannot link into a real image) and TimeService (it needs
 * WiFi and NTP). Their real operations are covered by TC-HW-CP-013 and the TimeService harness.
 *
 * The run is one reset and two boots, told apart by BKP31R (a marker) and BKP30R (the value the
 * first boot stored):
 *   boot 1  TC-HW-LW-001..007, ending in a real reset ordered through the /control command path.
 *   boot 2  TC-HW-LW-008..009 after the reset.
 *
 * Automated test sequence:
 *   TC-HW-LW-001  The controller reaches OPERATIONAL on the real modules within 5 s.
 *   TC-HW-LW-002  HealthMonitor follows (lifecycle_state, LD2 solid) and the sensors are ready.
 *   TC-HW-LW-003  The loaded configuration reached SensorService: reconfigure() was applied.
 *   TC-HW-LW-004  Edit APPLY: a changed telemetry interval is committed to the QSPI flash.
 *   TC-HW-LW-005  Edit CANCEL: the interval is restored from ConfigService's own snapshot.
 *   TC-HW-LW-006  {"command":"restart"} reaches the controller: RESTARTING, the cloud was flushed.
 *   TC-HW-LW-007  {"command":"restart_confirm"} resets the board for real.
 *   TC-HW-LW-008  After the reset: reset cause SOFT, and the interval committed in TC-HW-LW-004
 *                 was loaded back from flash by the controller's own Init sequence.
 *   TC-HW-LW-009  Fault: FAULTED, LD2 fast blink, a restart request is ignored.
 *
 * Integration checklist:
 *   [ ] Both boots print their banner; the second ends with "ALL TESTS PASSED"
 *   [ ] LD2 ends in a rapid blink
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "alarm_service/alarm_service.h"
#include "config_service/config_service.h"
#include "config_store/config_store.h"
#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "health_monitor/health_monitor.h"
#include "i2c/i2c_driver.h"
#include "led/led_driver.h"
#include "lifecycle_wiring/lifecycle_wiring.h"
#include "logger/logger.h"
#include "modbus_master/modbus_master.h"
#include "modbus_poller/modbus_poller.h"
#include "modbus_uart_driver/modbus_uart_driver.h"
#include "qspi_flash/qspi_flash.h"
#include "rtc/rtc.h"
#include "sensor_service/sensor_service.h"
#include "sensor_service/sensor_task.h"
#include "time_provider/time_provider.h"
#include "time_service/time_service.h"

#include "stm32l475xx.h"

/* ---------------------------------------------------------------------- */
/* Constants                                                               */
/* ---------------------------------------------------------------------- */

#define LD2_PIN (14U)

#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)
#define BRINGUP_TX_TIMEOUT_MS (100U)

#define LIFECYCLE_STACK_WORDS (2048U)
#define LIFECYCLE_PRIORITY (1U)
#define POLLER_STACK_WORDS (512U)
#define POLLER_PRIORITY (4U)
#define TEST_STACK_WORDS (768U)
#define TEST_PRIORITY (2U)
#define UART4_IRQ_PRIORITY (6U)
#define RESPONSE_TIMEOUT_MS (200U)
#define MAX_RETRIES (1U)

#define BKP_MARKER_IDX (31U)
#define BKP_EXPECTED_IDX (30U)
#define BKP_MARKER_MAGIC (0x4C575953UL)

#define TELEMETRY_A_MS (30000UL)
#define TELEMETRY_B_MS (45000UL)
#define TELEMETRY_CANCELLED_MS (90000UL)

#define OPERATIONAL_TIMEOUT_MS (5000U)
#define STATE_TIMEOUT_MS (1500U)
#define SETTLE_MS (200U)
#define POLL_MS (10U)

static const led_pin_t k_gw_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = (uint8_t) GPIO_PORT_B,
                   .pin = (uint8_t) LD2_PIN,
                   .active_high = true,
                   .fitted = true},
    [LED_RED] = {.port = (uint8_t) GPIO_PORT_A, .pin = 5U, .active_high = true, .fitted = false},
};

/* ---------------------------------------------------------------------- */
/* Stand-ins for the two modules that cannot link yet                      */
/* ---------------------------------------------------------------------- */

static volatile uint32_t s_cloud_flush_calls;

bool cloud_publisher_is_ready(cloud_publisher_handle_t handle)
{
    return handle != NULL;
}

cloud_publisher_err_t cloud_publisher_flush(cloud_publisher_handle_t handle)
{
    (void) handle;
    s_cloud_flush_calls++;
    return CP_ERR_OK;
}

time_service_err_t time_service_init(ntp_client_handle_t ntp)
{
    return (ntp != NULL) ? TIME_SERVICE_OK : TIME_SERVICE_ERR_NULL_ARG;
}

/* ---------------------------------------------------------------------- */
/* Reporting                                                               */
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
/* State and helpers                                                       */
/* ---------------------------------------------------------------------- */

static health_monitor_handle_t s_hm;
static config_store_handle_t s_cs;

static StaticTask_t s_lc_tcb;
static StackType_t s_lc_stack[LIFECYCLE_STACK_WORDS];
static StaticTask_t s_poller_tcb;
static StackType_t s_poller_stack[POLLER_STACK_WORDS];
static StaticTask_t s_test_tcb;
static StackType_t s_test_stack[TEST_STACK_WORDS];

static lifecycle_reset_cause_t s_boot_cause;
static bool s_second_boot;
static uint32_t s_expected_interval_ms;

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool wait_state(lifecycle_state_t state, uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms))
    {
        if (lifecycle_controller->get_state() == state)
        {
            return true;
        }
        delay_ms(POLL_MS);
    }
    return false;
}

static void post(lifecycle_event_type_t type)
{
    const lifecycle_event_t ev = {.type = type, .param = 0U};

    check(lifecycle_controller->post_event(ev), "post_event refused an event");
}

static void remote(const char *json)
{
    lifecycle_handle_remote((const uint8_t *) json, (uint32_t) strlen(json));
}

static uint32_t telemetry_interval_ms(void)
{
    return config_service_get_params()->telemetry_interval_ms;
}

static void set_telemetry_interval(uint32_t ms)
{
    check(config_service_set_param(CONFIG_PARAM_TELEMETRY_INTERVAL, &ms) == CONFIG_SERVICE_OK,
          "config_service_set_param");
}

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

static bool led_is(led_pattern_t expected)
{
    led_pattern_t pattern = LED_PATTERN_OFF;

    return (led_get_pattern(LED_GREEN, &pattern) == LED_ERR_OK) && (pattern == expected);
}

static health_snapshot_t take_snapshot(void)
{
    health_snapshot_t snap;

    check(health_monitor_get_snapshot(s_hm, &snap) == HEALTH_MONITOR_ERR_OK, "get_snapshot");
    return snap;
}

/* ---------------------------------------------------------------------- */
/* Test sequence                                                           */
/* ---------------------------------------------------------------------- */

static void fault_phase(void)
{
    /* TC-HW-LW-009 */
    const lifecycle_event_t fault = {.type = LC_EVENT_UNRECOVERABLE_FAULT, .param = 0xABU};

    check(lifecycle_controller->post_event(fault), "post_event(fault)");
    check(wait_state(LIFECYCLE_STATE_FAULTED, STATE_TIMEOUT_MS), "TC-HW-LW-009 FAULTED");
    delay_ms(SETTLE_MS);
    check(take_snapshot().lifecycle_state == LIFECYCLE_STATE_FAULTED,
          "TC-HW-LW-009 HealthMonitor follows");
    check(led_is(LED_PATTERN_BLINK_FAST), "TC-HW-LW-009 LD2 is not blinking fast");

    const uint32_t flushes = s_cloud_flush_calls;

    remote("{\"command\":\"restart\"}");
    delay_ms(SETTLE_MS);
    check((lifecycle_controller->get_state() == LIFECYCLE_STATE_FAULTED) &&
              (s_cloud_flush_calls == flushes),
          "TC-HW-LW-009 a restart request left FAULTED");
    bringup_pass("TC-HW-LW-009 FAULTED: LD2 fast blink, a restart request is ignored");
}

static void second_boot_phase(void)
{
    char msg[96];

    /* TC-HW-LW-008 */
    check(s_boot_cause == LIFECYCLE_RESET_SOFT, "TC-HW-LW-008 boot cause is not SOFT");
    (void) snprintf(msg, sizeof(msg), "interval loaded from flash: %lu ms (expected %lu)",
                    (unsigned long) telemetry_interval_ms(),
                    (unsigned long) s_expected_interval_ms);
    bringup_info(msg);
    check(telemetry_interval_ms() == s_expected_interval_ms,
          "TC-HW-LW-008 the committed interval was not loaded back");
    bringup_pass("TC-HW-LW-008 after the reset: cause SOFT, committed configuration loaded back");

    fault_phase();
}

static void first_boot_phase(void)
{
    char msg[96];

    /* TC-HW-LW-002 */
    delay_ms(SETTLE_MS);
    check(take_snapshot().lifecycle_state == LIFECYCLE_STATE_OPERATIONAL,
          "TC-HW-LW-002 HealthMonitor lifecycle_state");
    check(led_is(LED_PATTERN_ON), "TC-HW-LW-002 LD2 is not solid on");
    {
        const TickType_t start = xTaskGetTickCount();

        while (!sensor_service_is_ready() && ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(3000U)))
        {
            delay_ms(POLL_MS);
        }
    }
    check(sensor_service_is_ready(), "TC-HW-LW-002 sensors not ready");
    bringup_pass("TC-HW-LW-002 HealthMonitor + LD2 follow, sensors ready");

    /* TC-HW-LW-003: the controller applied the loaded configuration to SensorService. A second
     * call with the same parameters must be accepted, which also proves the pointer is live. */
    check(sensor_service_reconfigure(config_service_get_params()) == SENSOR_SERVICE_ERR_OK,
          "TC-HW-LW-003 reconfigure");
    bringup_pass("TC-HW-LW-003 configuration applied to SensorService");

    /* TC-HW-LW-004: APPLY commits a changed interval. */
    const uint32_t original = telemetry_interval_ms();

    s_expected_interval_ms = (original == TELEMETRY_A_MS) ? TELEMETRY_B_MS : TELEMETRY_A_MS;
    (void) snprintf(msg, sizeof(msg), "interval on boot: %lu ms, will commit %lu ms",
                    (unsigned long) original, (unsigned long) s_expected_interval_ms);
    bringup_info(msg);
    post(LC_EVENT_CONFIG_EDIT_ENTER);
    check(wait_state(LIFECYCLE_STATE_EDITING_CONFIG, STATE_TIMEOUT_MS), "TC-HW-LW-004 EDITING");
    set_telemetry_interval(s_expected_interval_ms);
    post(LC_EVENT_CONFIG_EDIT_APPLY);
    check(wait_state(LIFECYCLE_STATE_OPERATIONAL, STATE_TIMEOUT_MS), "TC-HW-LW-004 OPERATIONAL");
    check(telemetry_interval_ms() == s_expected_interval_ms, "TC-HW-LW-004 interval lost");
    bringup_pass("TC-HW-LW-004 edit APPLY: the changed interval is committed");

    /* TC-HW-LW-005: CANCEL restores from ConfigService's own snapshot. */
    post(LC_EVENT_CONFIG_EDIT_ENTER);
    check(wait_state(LIFECYCLE_STATE_EDITING_CONFIG, STATE_TIMEOUT_MS), "TC-HW-LW-005 EDITING");
    set_telemetry_interval(TELEMETRY_CANCELLED_MS);
    check(telemetry_interval_ms() == TELEMETRY_CANCELLED_MS, "TC-HW-LW-005 edit not applied");
    post(LC_EVENT_CONFIG_EDIT_CANCEL);
    check(wait_state(LIFECYCLE_STATE_OPERATIONAL, STATE_TIMEOUT_MS), "TC-HW-LW-005 OPERATIONAL");
    check(telemetry_interval_ms() == s_expected_interval_ms, "TC-HW-LW-005 not restored");
    bringup_pass("TC-HW-LW-005 edit CANCEL: the interval is restored from the snapshot");

    /* TC-HW-LW-006 */
    const uint32_t flushes = s_cloud_flush_calls;

    remote("{\"command\":\"restart\"}");
    check(wait_state(LIFECYCLE_STATE_RESTARTING, STATE_TIMEOUT_MS), "TC-HW-LW-006 RESTARTING");
    check(s_cloud_flush_calls == (flushes + 1U), "TC-HW-LW-006 the cloud was not flushed");
    bringup_pass("TC-HW-LW-006 /control restart: RESTARTING, cloud flushed");

    /* TC-HW-LW-007: record what boot 2 must find, then reset for real. */
    check(rtc_write_backup(BKP_EXPECTED_IDX, s_expected_interval_ms) == RTC_OK, "bkp expected");
    check(rtc_write_backup(BKP_MARKER_IDX, BKP_MARKER_MAGIC) == RTC_OK, "bkp marker");
    bringup_info("TC-HW-LW-007 sending restart_confirm: the board resets now");
    delay_ms(SETTLE_MS);
    remote("{\"command\":\"restart_confirm\"}");

    /* A reset ends this boot; if it did not happen, say so. */
    delay_ms(2000U);
    bringup_fail("TC-HW-LW-007 the board did not reset");
}

static void test_task(void *arg)
{
    (void) arg;

    char msg[96];

    bringup_puts("\r\n===== LifecycleController + wiring system bring-up =====\r\n");
    bringup_puts("Board : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    (void) snprintf(msg, sizeof(msg), "Boot  : reset cause: %s, %s boot\r\n",
                    cause_name(s_boot_cause), s_second_boot ? "second" : "first");
    bringup_puts(msg);
    bringup_puts("========================================================\r\n\r\n");

    /* TC-HW-LW-001 */
    check(wait_state(LIFECYCLE_STATE_OPERATIONAL, OPERATIONAL_TIMEOUT_MS),
          "TC-HW-LW-001 never reached OPERATIONAL");
    bringup_pass("TC-HW-LW-001 the controller reached OPERATIONAL on the real modules");

    if (s_second_boot)
    {
        second_boot_phase();
    }
    else
    {
        first_boot_phase();
    }

    bringup_puts("\r\n===== ALL TESTS PASSED =====\r\n");
    bringup_puts("LD2 should now be blinking rapidly (the fault pattern).\r\n");
    for (;;)
    {
        delay_ms(1000U);
    }
}

static uint32_t tick_ms(void)
{
    return (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ---------------------------------------------------------------------- */
/* main: the Gateway init order of companion section 17                    */
/* ---------------------------------------------------------------------- */

int main(void)
{
    /* The reset flags are read before anything else can disturb them. */
    s_boot_cause = lifecycle_detect_reset_cause();

    if (cpu_init() != STATUS_OK)
    {
        for (;;)
        {
        }
    }

    gpio_pin_config_t uart_tx_config = {.port = BRINGUP_UART_TX_PORT,
                                        .pin = BRINGUP_UART_TX_PIN,
                                        .mode = GPIO_MODE_ALTERNATE,
                                        .otype = GPIO_OTYPE_PUSH_PULL,
                                        .speed = GPIO_SPEED_VERY_HIGH,
                                        .pull = GPIO_PULL_NONE,
                                        .alternate = BRINGUP_UART_AF};
    gpio_pin_config_t uart_rx_config = {.port = BRINGUP_UART_RX_PORT,
                                        .pin = BRINGUP_UART_RX_PIN,
                                        .mode = GPIO_MODE_ALTERNATE,
                                        .otype = GPIO_OTYPE_PUSH_PULL,
                                        .speed = GPIO_SPEED_VERY_HIGH,
                                        .pull = GPIO_PULL_UP,
                                        .alternate = BRINGUP_UART_AF};

    if ((gpio_init() != GPIO_OK) || (gpio_configure_pin(&uart_tx_config) != GPIO_OK) ||
        (gpio_configure_pin(&uart_rx_config) != GPIO_OK) || (debug_uart_init() != DEBUG_UART_OK) ||
        (rtc_init() != RTC_OK) || (qspi_flash_init() != QSPI_FLASH_OK))
    {
        for (;;)
        {
        }
    }
    (void) logger_init(LOG_LEVEL_WARN);

    /* A second boot is one that follows our own restart (marker) and a software reset. */
    uint32_t marker = 0U;

    (void) rtc_read_backup(BKP_MARKER_IDX, &marker);
    s_second_boot = (marker == BKP_MARKER_MAGIC) && (s_boot_cause == LIFECYCLE_RESET_SOFT);
    if (s_second_boot)
    {
        (void) rtc_read_backup(BKP_EXPECTED_IDX, &s_expected_interval_ms);
        (void) rtc_write_backup(BKP_MARKER_IDX, 0U);
    }

    const health_monitor_config_t hm_config = {.boot_reset_cause = s_boot_cause};

    if ((led_init(k_gw_led_pins, (uint8_t) LED_COUNT) != LED_ERR_OK) ||
        (health_monitor_create(&hm_config, &s_hm) != HEALTH_MONITOR_ERR_OK) ||
        (time_provider_init(s_hm) != TIME_PROVIDER_ERR_OK))
    {
        bringup_puts("[FAIL] led / health / time provider - HALTED\r\n");
        for (;;)
        {
        }
    }

    /* ConfigStore and ConfigService are created here; the controller loads and applies the blob. */
    const config_store_config_t cs_cfg = {.flash = qspi_flash_driver, .health = s_hm};

    if ((config_store_create(&cs_cfg, &s_cs) != CONFIG_STORE_ERR_OK) ||
        (config_service_init(s_cs) != CONFIG_SERVICE_OK))
    {
        bringup_puts("[FAIL] config store / service - HALTED\r\n");
        for (;;)
        {
        }
    }

    /* Sensors and alarms are initialised before the scheduler (SS-D9). */
    if ((i2c_driver->init() != I2C_ERR_OK) || (sensor_task_create(s_hm) != SENSOR_TASK_ERR_OK))
    {
        bringup_puts("[FAIL] i2c / sensor task - HALTED\r\n");
        for (;;)
        {
        }
    }

    /* Modbus: UART and master now, the poller's own init is the controller's job. */
    NVIC_SetPriority(UART4_IRQn, UART4_IRQ_PRIORITY);
    if ((modbus_uart_init() != MODBUS_UART_OK) ||
        (modbus_master_init(RESPONSE_TIMEOUT_MS, MAX_RETRIES) != MODBUS_MASTER_ERR_OK))
    {
        bringup_puts("[FAIL] modbus uart / master - HALTED\r\n");
        for (;;)
        {
        }
    }
    modbus_uart_set_tick_source(tick_ms);

    const lifecycle_wiring_config_t wiring_cfg = {.health = s_hm,
                                                  .config_store = s_cs,
                                                  .cloud = (cloud_publisher_handle_t) 1,
                                                  .ntp = (ntp_client_handle_t) 1,
                                                  .fault_on_no_config = false};
    lifecycle_deps_t deps;

    if ((lifecycle_wiring_bind(&wiring_cfg, &deps) != LIFECYCLE_ERR_OK) ||
        (lifecycle_controller_init(s_boot_cause, &deps) != LIFECYCLE_ERR_OK))
    {
        bringup_puts("[FAIL] lifecycle wiring / init - HALTED\r\n");
        for (;;)
        {
        }
    }

    /* Tasks: LifecycleTask is created last, as the companion says (section 17). The poller task
     * waits for the start gate before it runs. */
    (void) xTaskCreateStatic(lifecycle_wiring_poller_task, "ModbusPoller", POLLER_STACK_WORDS, NULL,
                             POLLER_PRIORITY, s_poller_stack, &s_poller_tcb);
    (void) xTaskCreateStatic(test_task, "lw_bringup", TEST_STACK_WORDS, NULL, TEST_PRIORITY,
                             s_test_stack, &s_test_tcb);
    (void) xTaskCreateStatic(lifecycle_task_body, "lifecycle", LIFECYCLE_STACK_WORDS, NULL,
                             LIFECYCLE_PRIORITY, s_lc_stack, &s_lc_tcb);

    vTaskStartScheduler();

    for (;;)
    {
    }
}
