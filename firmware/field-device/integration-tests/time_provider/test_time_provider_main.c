/**
 * @file test_time_provider_main.c
 * @brief Integration test for TimeProvider on STM32F469I-DISCO hardware.
 *
 * Flashes to the board and exercises the full TimeProvider surface against
 * real RtcDriver, HealthMonitor, and Logger. All observations are via UART
 * (115200 8N1) and the two on-board LEDs. No automated pass/fail — visual
 * and logical inspection against the checklist below.
 *
 * Activation in CubeIDE:
 *   1. Exclude Src/main.c from build (Resource Config → Exclude from Build).
 *   2. Add integration-tests/time_provider/ to project source paths.
 *   3. Build, flash, open PuTTY on the ST-Link VCP at 115 200 / 8N1.
 *
 * Init ordering (time-provider.md §8):
 *   system_clock_init() → gpio_init() → led_init() → debug_uart_init()
 *   → rtc_init() → logger_init() → health_monitor_init()
 *   → time_provider_init()
 *
 * ---
 * Visual checklist — tick each item before declaring the build good:
 *
 *   Pre-scheduler:
 *   [ ] UART: "[TP] ===== TimeProvider integration test ====="
 *   [ ] UART: "[TP] Init: sync_state=0 (UNSYNCHRONISED)"
 *   [ ] UART: "[TP] Phase 1: get() returned UNSYNCHRONISED + uptime epoch"
 *   [ ] GREEN LED off, RED LED off (HealthMonitor init: INIT state)
 *
 *   Phase 2 — first set_time():
 *   [ ] UART: "[TP] Phase 2: set_time() OK — sync acquired"
 *   [ ] UART: "[TP] Phase 2: get() epoch=<unix_value> sync=SYNCHRONISED"
 *   [ ] GREEN LED on (HealthMonitor sees TIME_SYNC_ACQUIRED and is in INIT state)
 *
 *   Phase 3 — mark_unsynchronised():
 *   [ ] UART: "[TP] Phase 3: mark_unsynchronised() OK"
 *   [ ] UART: "[TP] Phase 3: sync_state=0 (UNSYNCHRONISED)"
 *   [ ] GREEN LED state unchanged (HealthMonitor LED driven by lifecycle, not sync state alone)
 *
 *   Phase 4 — sanity-check rejection:
 *   [ ] UART: "[TP] Phase 4: sanity check rejection — result=TIME_PROVIDER_ERR_RTC_FAIL"
 *   [ ] UART: "[TP] Phase 4: state still UNSYNCHRONISED (correct)"
 *
 *   Phase 5 — second set_time() (good value):
 *   [ ] UART: "[TP] Phase 5: second set_time() OK — sync acquired again"
 *
 *   Phase 6 — vtable access via singleton pointer:
 *   [ ] UART: "[TP] Phase 6: vtable get() epoch=<value> sync=1"
 *
 *   After scheduler starts:
 *   [ ] UART: periodic "[TP-task] tick N uptime=N epoch=<value>" at 2 Hz
 *   [ ] No UART freeze or watchdog reset
 *
 * ---
 */

#include <stdint.h>
#include <stdbool.h>

#include "stm32f469xx.h"

#include "FreeRTOS.h"
#include "task.h"

#include "system_clock.h"
#include "rtc/rtc_driver.h"
#include "debug-uart/debug_uart_driver.h"
#include "logger/logger.h"
#include "gpio/gpio_driver.h"
#include "led/led_driver.h"
#include "health_monitor/health_monitor.h"
#include "time_provider/time_provider.h"
#include "time_provider/time_provider_config.h"

/* ========================================================================= */
/* Configuration                                                             */
/* ========================================================================= */

#define TEST_TASK_STACK_WORDS  (512U)
#define TEST_TASK_PRIORITY     (tskIDLE_PRIORITY + 1U)
#define TASK_PERIOD_MS         (500U)

/** A known Unix epoch used for initial sync (2024-01-01 00:00:00 UTC). */
#define TEST_EPOCH_2024        (1703865600UL)

/**
 * Delta that exceeds TIME_PROVIDER_SANITY_DELTA_S when applied after
 * a synchronised state has been established.
 */
#define TEST_SANITY_VIOLATION  (TIME_PROVIDER_SANITY_DELTA_S + 1UL)

/* ===================================================================== */
/* Board pin table (Field Device — STM32F469I-DISCO)                    */
/* ===================================================================== */

static const led_pin_t k_fd_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_G, .pin =  6U, .active_high = false, .fitted = true},
    [LED_RED]   = {.port = GPIO_PORT_D, .pin =  5U, .active_high = false, .fitted = true},
};

/* ========================================================================= */
/* Static task storage (no heap)                                             */
/* ========================================================================= */

static StackType_t  s_test_stack[TEST_TASK_STACK_WORDS];
static StaticTask_t s_test_tcb;

/* ========================================================================= */
/* Test task                                                                 */
/* ========================================================================= */

static void time_provider_test_task(void *arg)
{
    time_provider_ts_t ts;
    uint32_t           i = 0U;

    (void)arg;

    for (;;)
    {
        (void)time_provider_get(&ts);
        LOG_INFO("TP-task", "tick %lu uptime=%lu epoch=%lu sync=%d",
                 (unsigned long)i,
                 (unsigned long)(xTaskGetTickCount() / configTICK_RATE_HZ),
                 (unsigned long)ts.epoch,
                 (int)ts.sync_state);
        i++;
        vTaskDelay(pdMS_TO_TICKS(TASK_PERIOD_MS));
    }
}

/* ========================================================================= */
/* Entry point                                                               */
/* ========================================================================= */

int main(void)
{
    time_provider_err_t tp_err;
    time_provider_ts_t  ts;
    time_sync_state_t   state;

    /* 1. Clock tree. */
    system_clock_init();

    /* 2. Peripherals. */
    (void)gpio_init();
    (void)led_init(k_fd_led_pins, LED_COUNT);
    (void)debug_uart_init();
    (void)rtc_init();

    /* 3. Middleware — Logger first (RtcDriver already up). */
    (void)logger_init(LOG_LEVEL_DEBUG);

    /* 4. Application — HealthMonitor. */
    (void)health_monitor_init();

    /* 5. Middleware — TimeProvider. */
    tp_err = time_provider_init(health_report);

    LOG_INFO("TP", "===== TimeProvider integration test =====");
    LOG_INFO("TP", "Init result=%d sync_state=%d",
             (int)tp_err, (int)time_provider_get_sync_state());

    /* --- Phase 1: get() before first set_time() — expect UNSYNCHRONISED --- */
    (void)time_provider_get(&ts);
    LOG_INFO("TP", "Phase 1: get() epoch=%lu sync=%d (expect UNSYNCED=0)",
             (unsigned long)ts.epoch, (int)ts.sync_state);

    /* --- Phase 2: set_time() — first sync, no sanity check applied --- */
    tp_err = time_provider_set_time(TEST_EPOCH_2024);
    LOG_INFO("TP", "Phase 2: set_time() result=%d (expect OK=0)", (int)tp_err);

    (void)time_provider_get(&ts);
    LOG_INFO("TP", "Phase 2: get() epoch=%lu sync=%d (expect SYNCED=1)",
             (unsigned long)ts.epoch, (int)ts.sync_state);

    /* --- Phase 3: mark_unsynchronised() --- */
    tp_err = time_provider_mark_unsynchronised();
    state  = time_provider_get_sync_state();
    LOG_INFO("TP", "Phase 3: mark_unsync result=%d sync=%d (expect 0,0)",
             (int)tp_err, (int)state);

    /* --- Phase 4: sanity check — re-sync, then attempt large-delta jump --- */
    /* First restore synchronised state. */
    (void)time_provider_set_time(TEST_EPOCH_2024);

    /* Now attempt a jump beyond the sanity delta. */
    tp_err = time_provider_set_time(TEST_EPOCH_2024 + TEST_SANITY_VIOLATION);
    LOG_INFO("TP", "Phase 4: sanity rejection result=%d (expect RTC_FAIL=2)",
             (int)tp_err);
    LOG_INFO("TP", "Phase 4: state=%d (expect SYNCED=1)",
             (int)time_provider_get_sync_state());

    /* --- Phase 5: good second set_time() within delta --- */
    tp_err = time_provider_set_time(TEST_EPOCH_2024 + 60UL);
    LOG_INFO("TP", "Phase 5: within-delta set_time result=%d (expect OK=0)",
             (int)tp_err);

    /* --- Phase 6: vtable access via singleton --- */
    (void)time_provider->get(&ts);
    LOG_INFO("TP", "Phase 6: vtable get() epoch=%lu sync=%d",
             (unsigned long)ts.epoch, (int)ts.sync_state);

    LOG_INFO("TP", "Pre-scheduler diagnostics done — starting scheduler");

    /* 6. Spawn periodic task. */
    (void)xTaskCreateStatic(time_provider_test_task,
                            "tp_test",
                            TEST_TASK_STACK_WORDS,
                            NULL,
                            TEST_TASK_PRIORITY,
                            s_test_stack,
                            &s_test_tcb);

    /* 7. Start the scheduler. Does not return under normal operation. */
    vTaskStartScheduler();

    for (;;)
    {
    }
}
