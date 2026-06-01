/**
 * @file test_health_monitor_main.c
 * @brief Integration test for HealthMonitor — manual verification on target hardware.
 *
 * Run on the STM32F469I-DISCO board after flashing. Observe LEDs and check
 * SWO/UART log output. No automated pass/fail — visual and logical inspection.
 *
 * TODO: Logger must be initialised before this test runs. Replace the
 * placeholders below with the real init calls once Logger and DebugUart
 * are implemented.
 *
 * ---
 * Visual checklist (tick off on the board):
 *
 *   Init phase (before OS starts):
 *   [ ] Both LEDs off immediately after reset (led_init sets both low).
 *   [ ] health_monitor_init() succeeds — UART shows "[HM] Initialised".
 *
 *   Phase 1 — push OPERATIONAL lifecycle state:
 *   [ ] LIFECYCLE_STATE_OPERATIONAL pushed → GREEN LED on, RED off.
 *   [ ] UART shows "[TEST] Phase 1: Operational".
 *
 *   Phase 2 — push ALARM_RAISED for SENSOR_ID_TEMPERATURE:
 *   [ ] Both GREEN and RED LEDs on (adaptation: no orange on this board).
 *   [ ] UART shows "[TEST] Phase 2: Alarm raised".
 *
 *   Phase 3 — push ALARM_CLEARED:
 *   [ ] GREEN on, RED off (back to operational).
 *   [ ] UART shows "[TEST] Phase 3: Alarm cleared".
 *
 *   Phase 4 — push FAULT:
 *   [ ] RED on, GREEN off. Fault overrides all.
 *   [ ] UART shows "[TEST] Phase 4: Fault".
 *
 *   Phase 5 — push ALARM_RAISED after FAULT:
 *   [ ] LED stays RED on, GREEN off (fault takes priority).
 *   [ ] UART shows "[TEST] Phase 5: Alarm after fault — LED should stay RED".
 *
 *   Phase 6 — health_monitor_get_snapshot() query:
 *   [ ] UART dumps snapshot fields: alarm_raise_count == 2,
 *       lifecycle_state == LIFECYCLE_STATE_FAULTED.
 *
 *   Phase 7 — health_monitor_reset_metrics():
 *   [ ] UART shows alarm_raise_count == 0 after reset.
 *
 *   Phase 8 — poll (stack watermarks):
 *   [ ] UART shows stack_watermark_words[] for each task.
 *   [ ] Note: watermark[6] will always be 0 (see bug hunting exercise).
 *
 *   After scheduler starts:
 *   [ ] Test task enters idle blink — GREEN blinks at 1 Hz.
 *
 * ---
 * Init ordering (health-monitor.md §1):
 *   1. gpio_init()
 *   2. led_init()
 *   3. debug_uart_init()    (TODO: uncomment when implemented)
 *   4. logger_init()        (TODO: uncomment when implemented)
 *   5. health_monitor_init()
 * ---
 */

#include <stdint.h>
#include <stdbool.h>

#include "system_clock.h"

#include "FreeRTOS.h"
#include "task.h"

#include "gpio/gpio_driver.h"
#include "led/led_driver.h"
#include "rtc/rtc_driver.h"
#include "debug-uart/debug_uart_driver.h"

#include "logger/logger.h"

#include "health_monitor/health_monitor.h"

/* ======================================================================= */
/* Constants                                                               */
/* ======================================================================= */

#define TEST_TASK_STACK_WORDS  (256U)
#define TEST_TASK_PRIORITY     (tskIDLE_PRIORITY + 1U)
#define PHASE_DELAY_MS         (2000U)
#define HEARTBEAT_PERIOD_MS    (500U)

/* ===================================================================== */
/* Board pin table (Field Device — STM32F469I-DISCO)                    */
/* ===================================================================== */

static const led_pin_t k_fd_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_G, .pin =  6U, .active_high = false, .fitted = true},
    [LED_RED]   = {.port = GPIO_PORT_D, .pin =  5U, .active_high = false, .fitted = true},
};

/* ======================================================================= */
/* Static task storage (no heap)                                           */
/* ======================================================================= */

static StackType_t  s_test_stack[TEST_TASK_STACK_WORDS];
static StaticTask_t s_test_tcb;

/* ======================================================================= */
/* Test task                                                               */
/* ======================================================================= */

static void health_monitor_test_task(void *arg)
{
    device_health_snapshot_t snap;
    modbus_slave_stats_t     modbus_stats;

    (void)arg;

    /* Phase 1: push Operational state. */
    (void)health_monitor_push_event(HEALTH_EVENT_TIME_SYNC_ACQUIRED, 0U);
    vTaskDelay(pdMS_TO_TICKS(PHASE_DELAY_MS));

    /* Phase 2: raise alarm on temperature sensor. */
    (void)health_monitor_push_event(HEALTH_EVENT_ALARM_RAISED,
                                    (uint32_t)SENSOR_ID_TEMPERATURE);
    vTaskDelay(pdMS_TO_TICKS(PHASE_DELAY_MS));

    /* Phase 3: clear alarm. */
    (void)health_monitor_push_event(HEALTH_EVENT_ALARM_CLEARED,
                                    (uint32_t)SENSOR_ID_TEMPERATURE);
    vTaskDelay(pdMS_TO_TICKS(PHASE_DELAY_MS));

    /* Phase 4: fault. */
    (void)health_monitor_push_event(HEALTH_EVENT_FAULT, 0U);
    vTaskDelay(pdMS_TO_TICKS(PHASE_DELAY_MS));

    /* Phase 5: alarm after fault — LED stays RED. */
    (void)health_monitor_push_event(HEALTH_EVENT_ALARM_RAISED,
                                    (uint32_t)SENSOR_ID_HUMIDITY);
    vTaskDelay(pdMS_TO_TICKS(PHASE_DELAY_MS));

    /* Phase 6: read snapshot. */
    (void)health_monitor_get_snapshot(&snap);
    (void)snap;  /* Inspect via debugger: snap.alarm_raise_count == 2. */

    /* Phase 7: reset counters. */
    (void)health_monitor_reset_metrics();
    (void)health_monitor_get_snapshot(&snap);
    /* Inspect via debugger: snap.alarm_raise_count == 0. */

    /* Phase 8: update Modbus stats. */
    modbus_stats.valid_frames       = 500U;
    modbus_stats.crc_errors         = 3U;
    modbus_stats.address_mismatches = 1U;
    modbus_stats.exception_responses = 0U;
    modbus_stats.unsupported_fc     = 0U;
    modbus_stats.successful_responses = 496U;
    (void)health_monitor_update_modbus_slave_stats(&modbus_stats);

    /* Phase 9: poll — note that stack_watermark_words[6] stays 0 (bug hunt). */
    health_monitor_poll();
    (void)health_monitor_get_snapshot(&snap);
    /* Inspect via debugger: snap.stack_watermark_words[6] == 0. */

    /* Heartbeat: blink GREEN to signal scheduler is alive. */
    for (;;)
    {
        (void)led_toggle(LED_GREEN);
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}

/* ======================================================================= */
/* Entry point                                                             */
/* ======================================================================= */

int main(void)
{
	system_clock_init();

    /* Init ordering per health-monitor.md §1. */
    (void)gpio_init();
    (void)led_init(k_fd_led_pins, LED_COUNT);
    (void) rtc_init();
    (void)debug_uart_init();

    (void)logger_init(LOG_LEVEL_DEBUG);

    (void)health_monitor_init();

    (void)health_monitor_register_task("hm_test",
                                       (void *)xTaskGetCurrentTaskHandle());

    (void)xTaskCreateStatic(health_monitor_test_task,
                            "hm_test",
                            TEST_TASK_STACK_WORDS,
                            NULL,
                            TEST_TASK_PRIORITY,
                            s_test_stack,
                            &s_test_tcb);

    vTaskStartScheduler();

    for (;;)
    {
    }
}
