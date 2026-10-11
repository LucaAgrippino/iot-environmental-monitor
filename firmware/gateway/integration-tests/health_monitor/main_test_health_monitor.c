/**
 * @file main_test_health_monitor.c
 * @brief Hardware bring-up test for HealthMonitor (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. Add cloud_publisher (the current harness) to the `excluding=` list of
 *      the `integration-tests` source entry and remove health_monitor.
 *   2. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/health_monitor.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * Hardware under test:
 *   LD2 green  PB14  active-high  (the Gateway's one usable user LED)
 *   USART1 TX  PB6   115 200 8N1  Test results
 *
 * Real LedDriver, real FreeRTOS scheduler, real tasks. The LED cannot be
 * watched from a script, so the LED cases read the pin's output data register
 * (GPIOB->ODR) and time its transitions with the DWT cycle counter; the final
 * step leaves LD2 in the fault pattern for a check by eye.
 *
 * Boot order follows the companion: cpu_init, gpio_init, debug_uart_init,
 * led_init, health_monitor_create, all before the scheduler.
 *
 * Automated test sequence:
 *   TC-HW-HM-001  create(): INIT lifecycle, boot reset cause recorded, every
 *                  sensor valid, LedDriver left in the init pattern.
 *   TC-HW-HM-002  The init pattern toggles PB14 every 250 ms (medium blink).
 *   TC-HW-HM-003  update_lifecycle(OPERATIONAL): LD2 solid on and steady.
 *   TC-HW-HM-004  ALARM_RAISED: LD2 blinks slowly (500 ms half-period);
 *                  ALARM_CLEARED returns it to solid.
 *   TC-HW-HM-005  get_snapshot(): uptime_s advances with the tick count.
 *   TC-HW-HM-006  poll(): real stack watermarks for three registered tasks are
 *                  non-zero and below each task's stack size.
 *   TC-HW-HM-007  Event, stats and occupancy updates round-trip through the
 *                  snapshot (including the real mqtt_stats_t fields).
 *   TC-HW-HM-008  reset_metrics() zeroes the counters and keeps the rest.
 *   TC-HW-HM-009  Two writer tasks and a reader: the multi-field MQTT stats are
 *                  never torn, and no event is lost (the mutex holds).
 *   TC-HW-HM-010  Error paths on silicon.
 *   TC-HW-HM-011  set_led_fault(): LD2 blinks fast (100 ms half-period) and
 *                  stays there through a later alarm and lifecycle change.
 *
 * Integration checklist:
 *   [ ] All eleven cases pass
 *   [ ] LD2 (green, beside the ST-LINK connector) ends in a rapid blink: confirm
 *       by eye that this is the LED the driver controls (LED-O7)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "health_monitor/health_monitor.h"
#include "led/led_driver.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Board constants                                                         */
/* ---------------------------------------------------------------------- */

#define LD2_PIN (14U)
#define LD2_ODR_MASK (1UL << LD2_PIN)

#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)
#define BRINGUP_TX_TIMEOUT_MS (100U)

#define MAIN_STACK_WORDS (512U)
#define WORKER_STACK_WORDS (384U)
#define TASK_PRIORITY (2U)

/* ---------------------------------------------------------------------- */
/* Timing constants                                                        */
/* ---------------------------------------------------------------------- */

#define HALF_PERIOD_FAST_MS (100UL)
#define HALF_PERIOD_MEDIUM_MS (250UL)
#define HALF_PERIOD_SLOW_MS (500UL)

#define TOLERANCE_PERCENT (15UL)
#define TOLERANCE_SLACK_US (3000UL)
#define TRANSITIONS_PER_CHECK (4U)
#define STABLE_WINDOW_MS (700UL)
#define MEASURE_TIMEOUT_MS (3000UL)
#define MAX_INTERVALS (8U)

#define MS_TO_US (1000UL)
#define US_PER_S (1000000UL)

#define STRESS_ITERATIONS (1000U)
#define STRESS_TIMEOUT_MS (20000UL)
#define STRESS_POLL_MS (50U)

#define EXPECTED_RESET_CAUSE (LIFECYCLE_RESET_SOFT)

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
                 .fitted = false}, /* LD1 / SB1 — shared with SPI1_SCK */
};

/* ---------------------------------------------------------------------- */
/* Reporting helpers — built on DebugUartDriver.                           */
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

    /* LD2 belongs to HealthMonitor under test, so failure is only reported on
     * the UART. */
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

static void expect_hm(health_monitor_err_t got, health_monitor_err_t expected, const char *label)
{
    if (got != expected)
    {
        char msg[96];

        (void) snprintf(msg, sizeof(msg), "%s (err=%d, expected=%d)", label, (int) got,
                        (int) expected);
        bringup_fail(msg);
    }
}

/* ---------------------------------------------------------------------- */
/* DWT timing and pin sampling                                             */
/* ---------------------------------------------------------------------- */

static uint32_t dwt_now(void)
{
    return DWT->CYCCNT;
}

static uint32_t dwt_elapsed_us(uint32_t start)
{
    const uint32_t cycles_per_us = cpu_get_sysclk_hz() / US_PER_S;

    return (DWT->CYCCNT - start) / cycles_per_us;
}

static bool ld2_is_high(void)
{
    return (GPIOB->ODR & LD2_ODR_MASK) != 0U;
}

/** @brief Poll PB14 and record the time between transitions. */
static uint8_t measure_transitions(uint8_t wanted, uint32_t intervals_us[MAX_INTERVALS])
{
    uint8_t seen = 0U;
    bool level = ld2_is_high();
    uint32_t mark = dwt_now();
    const uint32_t begin = mark;

    while ((seen < wanted) && (seen < MAX_INTERVALS) &&
           (dwt_elapsed_us(begin) < (MEASURE_TIMEOUT_MS * MS_TO_US)))
    {
        bool now = ld2_is_high();

        if (now != level)
        {
            intervals_us[seen] = dwt_elapsed_us(mark);
            mark = dwt_now();
            level = now;
            seen++;
        }
    }

    return seen;
}

static bool intervals_match(const uint32_t *intervals_us, uint8_t count, uint32_t half_period_ms)
{
    const uint32_t expected_us = half_period_ms * MS_TO_US;
    const uint32_t low = ((expected_us * (100UL - TOLERANCE_PERCENT)) / 100UL) - TOLERANCE_SLACK_US;
    const uint32_t high =
        ((expected_us * (100UL + TOLERANCE_PERCENT)) / 100UL) + TOLERANCE_SLACK_US;

    for (uint8_t i = 0U; i < count; i++)
    {
        if ((intervals_us[i] < low) || (intervals_us[i] > high))
        {
            return false;
        }
    }

    return true;
}

static void report_intervals(const char *label, const uint32_t *intervals_us, uint8_t count)
{
    char msg[112];
    uint32_t sum = 0U;

    for (uint8_t i = 0U; i < count; i++)
    {
        sum += intervals_us[i];
    }

    (void) snprintf(msg, sizeof(msg), "%s: %u intervals, mean %lu us", label, (unsigned) count,
                    (unsigned long) ((count > 0U) ? (sum / count) : 0U));
    bringup_info(msg);
}

/** @brief Confirm PB14 toggles at the half-period of the current pattern. */
static void check_blink(uint32_t half_period_ms, const char *label)
{
    uint32_t intervals[MAX_INTERVALS] = {0U};
    uint8_t seen = measure_transitions(TRANSITIONS_PER_CHECK, intervals);

    report_intervals(label, intervals, seen);
    check(seen == TRANSITIONS_PER_CHECK, label);
    check(intervals_match(intervals, seen, half_period_ms), label);
}

/** @brief Confirm PB14 holds @p level for the whole stable window. */
static void check_steady(bool level, const char *label)
{
    const uint32_t start = dwt_now();

    while (dwt_elapsed_us(start) < (STABLE_WINDOW_MS * MS_TO_US))
    {
        if (ld2_is_high() != level)
        {
            bringup_fail(label);
        }
    }
}

static void check_led_pattern(led_pattern_t expected, const char *label)
{
    led_pattern_t got = LED_PATTERN_OFF;

    check(led_get_pattern(LED_GREEN, &got) == LED_ERR_OK, label);
    check(got == expected, label);
}

/* ---------------------------------------------------------------------- */
/* State shared with the tasks                                             */
/* ---------------------------------------------------------------------- */

static health_monitor_handle_t s_hm;

static volatile bool s_init_state_ok;
static volatile bool s_init_pattern_ok;

static StaticTask_t s_main_tcb;
static StackType_t s_main_stack[MAIN_STACK_WORDS];
static TaskHandle_t s_main_handle;

static StaticTask_t s_writer_a_tcb;
static StackType_t s_writer_a_stack[WORKER_STACK_WORDS];
static TaskHandle_t s_writer_a_handle;

static StaticTask_t s_writer_b_tcb;
static StackType_t s_writer_b_stack[WORKER_STACK_WORDS];
static TaskHandle_t s_writer_b_handle;

static volatile bool s_stress_go;
static volatile uint32_t s_stress_done;
static volatile uint32_t s_stress_errors;
static volatile uint32_t s_stress_torn;

/* ---------------------------------------------------------------------- */
/* Stress workers (TC-HW-HM-009)                                           */
/* ---------------------------------------------------------------------- */

/** Writer A: updates a three-field MQTT stats block whose fields are related. */
static void wait_for_go(void)
{
    while (!s_stress_go)
    {
        vTaskDelay(pdMS_TO_TICKS(STRESS_POLL_MS));
    }
}

/** @brief Park after the work so the task handle stays valid for poll(). */
static void park_forever(void)
{
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

static void writer_a_task(void *arg)
{
    (void) arg;

    wait_for_go();

    for (uint32_t i = 1U; i <= STRESS_ITERATIONS; i++)
    {
        mqtt_stats_t stats;

        (void) memset(&stats, 0, sizeof(stats));
        stats.publishes_sent = i;
        stats.publish_failures = 2U * i;
        stats.reconnect_count = 3U * i;

        if (health_monitor_update_mqtt_stats(s_hm, &stats) != HEALTH_MONITOR_ERR_OK)
        {
            s_stress_errors++;
        }
    }

    s_stress_done++;
    park_forever();
}

/** Writer B: pushes counting events; every one must be reflected exactly once. */
static void writer_b_task(void *arg)
{
    (void) arg;

    wait_for_go();

    for (uint32_t i = 0U; i < STRESS_ITERATIONS; i++)
    {
        if ((health_monitor_push_event(s_hm, HEALTH_EVENT_NTP_SYNC_FAILED, 0U) !=
             HEALTH_MONITOR_ERR_OK) ||
            (health_monitor_push_event(s_hm, HEALTH_EVENT_BUFFER_OVERFLOW, 0U) !=
             HEALTH_MONITOR_ERR_OK))
        {
            s_stress_errors++;
        }
    }

    s_stress_done++;
    park_forever();
}

/* ---------------------------------------------------------------------- */
/* Bring-up task                                                           */
/* ---------------------------------------------------------------------- */

static void bringup_task(void *arg)
{
    (void) arg;

    health_snapshot_t snap;

    bringup_puts("\r\n===== HealthMonitor Hardware Bring-up =====\r\n");
    bringup_puts("Board : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("LED   : LD2 green, PB14, active-high\r\n");
    bringup_puts("===========================================\r\n\r\n");

    /* TC-HW-HM-001 - observed in main() before the scheduler started. */
    check(s_init_state_ok, "TC-HW-HM-001 create() initial snapshot");
    check(s_init_pattern_ok, "TC-HW-HM-001 create() left the init pattern");
    bringup_pass("TC-HW-HM-001 create(): INIT state, reset cause, sensors valid, init pattern");

    /* TC-HW-HM-002 */
    check_led_pattern(LED_PATTERN_BLINK_MEDIUM, "TC-HW-HM-002 pattern");
    check_blink(HALF_PERIOD_MEDIUM_MS, "TC-HW-HM-002 init blink");
    bringup_pass("TC-HW-HM-002 init pattern toggles PB14 every 250 ms");

    /* TC-HW-HM-003 */
    expect_hm(health_monitor_update_lifecycle(s_hm, LIFECYCLE_STATE_OPERATIONAL),
              HEALTH_MONITOR_ERR_OK, "TC-HW-HM-003 update_lifecycle");
    vTaskDelay(pdMS_TO_TICKS(50U));
    check_led_pattern(LED_PATTERN_ON, "TC-HW-HM-003 pattern");
    check(ld2_is_high(), "TC-HW-HM-003 PB14 high");
    check_steady(true, "TC-HW-HM-003 steady");
    bringup_pass("TC-HW-HM-003 OPERATIONAL: LD2 solid on and steady");

    /* TC-HW-HM-004 */
    expect_hm(health_monitor_push_event(s_hm, HEALTH_EVENT_ALARM_RAISED, 2U), HEALTH_MONITOR_ERR_OK,
              "TC-HW-HM-004 alarm raised");
    check_led_pattern(LED_PATTERN_BLINK_SLOW, "TC-HW-HM-004 pattern");
    check_blink(HALF_PERIOD_SLOW_MS, "TC-HW-HM-004 alarm blink");

    expect_hm(health_monitor_push_event(s_hm, HEALTH_EVENT_ALARM_CLEARED, 2U),
              HEALTH_MONITOR_ERR_OK, "TC-HW-HM-004 alarm cleared");
    vTaskDelay(pdMS_TO_TICKS(50U));
    check_led_pattern(LED_PATTERN_ON, "TC-HW-HM-004 back to solid");
    check_steady(true, "TC-HW-HM-004 steady again");
    bringup_pass("TC-HW-HM-004 alarm blinks slowly (500 ms); clearing it restores solid");

    /* TC-HW-HM-005 */
    expect_hm(health_monitor_get_snapshot(s_hm, &snap), HEALTH_MONITOR_ERR_OK, "TC-HW-HM-005 get");
    uint32_t first_uptime = snap.uptime_s;

    vTaskDelay(pdMS_TO_TICKS(2100U));
    expect_hm(health_monitor_get_snapshot(s_hm, &snap), HEALTH_MONITOR_ERR_OK,
              "TC-HW-HM-005 get 2");
    check(snap.uptime_s >= (first_uptime + 2U), "TC-HW-HM-005 uptime advances");
    check(snap.uptime_s <= (first_uptime + 3U), "TC-HW-HM-005 uptime not fast");
    check(snap.uptime_s == (xTaskGetTickCount() / configTICK_RATE_HZ),
          "TC-HW-HM-005 matches ticks");
    bringup_pass("TC-HW-HM-005 uptime_s follows the tick count");

    /* TC-HW-HM-006 */
    expect_hm(health_monitor_poll(s_hm), HEALTH_MONITOR_ERR_OK, "TC-HW-HM-006 poll");
    expect_hm(health_monitor_get_snapshot(s_hm, &snap), HEALTH_MONITOR_ERR_OK, "TC-HW-HM-006 get");
    check((snap.stack_watermark_words[0] > 0U) &&
              (snap.stack_watermark_words[0] < MAIN_STACK_WORDS),
          "TC-HW-HM-006 main task watermark");
    check((snap.stack_watermark_words[1] > 0U) &&
              (snap.stack_watermark_words[1] < WORKER_STACK_WORDS),
          "TC-HW-HM-006 writer A watermark");
    check((snap.stack_watermark_words[2] > 0U) &&
              (snap.stack_watermark_words[2] < WORKER_STACK_WORDS),
          "TC-HW-HM-006 writer B watermark");
    {
        char msg[96];

        (void) snprintf(msg, sizeof(msg), "TC-HW-HM-006 watermarks: main %u/%u words free",
                        (unsigned) snap.stack_watermark_words[0], (unsigned) MAIN_STACK_WORDS);
        bringup_info(msg);
    }
    bringup_pass("TC-HW-HM-006 poll(): real stack watermarks for three tasks");

    /* TC-HW-HM-007 */
    mqtt_stats_t mq;
    modbus_master_stats_t mm = {.transactions_ok = 120U, .timeouts = 3U, .link_online = true};

    (void) memset(&mq, 0, sizeof(mq));
    mq.publishes_sent = 50U;
    mq.publish_failures = 2U;
    mq.reconnect_count = 4U;

    expect_hm(health_monitor_update_mqtt_stats(s_hm, &mq), HEALTH_MONITOR_ERR_OK,
              "TC-HW-HM-007 mqtt");
    expect_hm(health_monitor_update_modbus_master_stats(s_hm, &mm), HEALTH_MONITOR_ERR_OK,
              "TC-HW-HM-007 modbus");
    expect_hm(health_monitor_update_buffer_occupancy(s_hm, 17U), HEALTH_MONITOR_ERR_OK,
              "TC-HW-HM-007 occupancy");
    expect_hm(health_monitor_update_wifi_rssi(s_hm, -62), HEALTH_MONITOR_ERR_OK,
              "TC-HW-HM-007 rssi");
    expect_hm(health_monitor_push_event(s_hm, HEALTH_EVENT_NTP_SYNC_OK, 1791750615UL),
              HEALTH_MONITOR_ERR_OK, "TC-HW-HM-007 ntp ok");
    expect_hm(health_monitor_push_event(s_hm, HEALTH_EVENT_CLOUD_CONNECTED, 0U),
              HEALTH_MONITOR_ERR_OK, "TC-HW-HM-007 cloud");
    expect_hm(health_monitor_push_event(s_hm, HEALTH_EVENT_TIME_SYNC_ACQUIRED, 0U),
              HEALTH_MONITOR_ERR_OK, "TC-HW-HM-007 time sync");
    expect_hm(health_monitor_push_event(s_hm, HEALTH_EVENT_SENSOR_FAIL, 4U), HEALTH_MONITOR_ERR_OK,
              "TC-HW-HM-007 sensor fail");
    expect_hm(health_monitor_get_snapshot(s_hm, &snap), HEALTH_MONITOR_ERR_OK, "TC-HW-HM-007 get");
    check(snap.mqtt_publishes_sent == 50U, "TC-HW-HM-007 mqtt sent");
    check(snap.mqtt_publishes_failed == 2U, "TC-HW-HM-007 mqtt failed");
    check(snap.mqtt_reconnect_count == 4U, "TC-HW-HM-007 mqtt reconnect");
    check((snap.modbus_transactions_ok == 120U) && (snap.modbus_timeouts == 3U) &&
              snap.modbus_link_online,
          "TC-HW-HM-007 modbus");
    check(snap.buffer_entry_count == 17U, "TC-HW-HM-007 occupancy");
    check(snap.wifi_rssi_dbm == -62, "TC-HW-HM-007 rssi");
    check(snap.last_ntp_sync_epoch == 1791750615UL, "TC-HW-HM-007 ntp epoch");
    check(snap.cloud_connected, "TC-HW-HM-007 cloud flag");
    check(snap.time_sync_state == TIME_SYNC_SYNCHRONISED, "TC-HW-HM-007 time sync flag");
    check(!snap.sensor_valid[4] && (snap.sensor_fail_count == 1U), "TC-HW-HM-007 sensor fail");
    bringup_pass("TC-HW-HM-007 events, stats and occupancy round-trip through the snapshot");

    /* TC-HW-HM-008 */
    expect_hm(health_monitor_reset_metrics(s_hm), HEALTH_MONITOR_ERR_OK, "TC-HW-HM-008 reset");
    expect_hm(health_monitor_get_snapshot(s_hm, &snap), HEALTH_MONITOR_ERR_OK, "TC-HW-HM-008 get");
    check((snap.sensor_fail_count == 0U) && (snap.mqtt_publishes_sent == 0U) &&
              (snap.mqtt_publishes_failed == 0U) && (snap.mqtt_reconnect_count == 0U) &&
              (snap.modbus_transactions_ok == 0U) && (snap.modbus_timeouts == 0U),
          "TC-HW-HM-008 counters zeroed");
    check(!snap.sensor_valid[4] && snap.cloud_connected && (snap.buffer_entry_count == 17U) &&
              (snap.wifi_rssi_dbm == -62) && (snap.lifecycle_state == LIFECYCLE_STATE_OPERATIONAL),
          "TC-HW-HM-008 state preserved");
    bringup_pass("TC-HW-HM-008 reset_metrics zeroes the counters and keeps the rest");

    /* TC-HW-HM-009 */
    s_stress_done = 0U;
    s_stress_errors = 0U;
    s_stress_torn = 0U;
    s_stress_go = true; /* release the two writers created in main() */

    uint32_t reads = 0U;
    uint32_t waited_ms = 0U;
    const uint32_t stress_start = dwt_now();

    while ((s_stress_done < 2U) && (waited_ms < STRESS_TIMEOUT_MS))
    {
        health_snapshot_t s;

        if (health_monitor_get_snapshot(s_hm, &s) == HEALTH_MONITOR_ERR_OK)
        {
            reads++;

            /* The three MQTT fields are written together under one lock, so a
             * reader must never see a half-updated block. */
            if ((s.mqtt_publishes_failed != (2U * s.mqtt_publishes_sent)) ||
                (s.mqtt_reconnect_count != (3U * s.mqtt_publishes_sent)))
            {
                s_stress_torn++;
            }
        }
        else
        {
            s_stress_errors++;
        }

        waited_ms = dwt_elapsed_us(stress_start) / MS_TO_US;
    }

    check(s_stress_done == 2U, "TC-HW-HM-009 writers finished (no deadlock)");
    expect_hm(health_monitor_get_snapshot(s_hm, &snap), HEALTH_MONITOR_ERR_OK,
              "TC-HW-HM-009 final");
    {
        char msg[112];

        (void) snprintf(msg, sizeof(msg),
                        "TC-HW-HM-009 %lu reads while %u+%u writes ran: %lu errors, %lu torn",
                        (unsigned long) reads, (unsigned) STRESS_ITERATIONS,
                        (unsigned) (2U * STRESS_ITERATIONS), (unsigned long) s_stress_errors,
                        (unsigned long) s_stress_torn);
        bringup_info(msg);
    }
    check(s_stress_errors == 0U, "TC-HW-HM-009 no API errors");
    check(s_stress_torn == 0U, "TC-HW-HM-009 no torn multi-field reads");
    check(snap.mqtt_publishes_sent == STRESS_ITERATIONS, "TC-HW-HM-009 final stats");
    check(snap.ntp_sync_fail_count == STRESS_ITERATIONS, "TC-HW-HM-009 no lost NTP events");
    check(snap.buffer_overflow_count == STRESS_ITERATIONS, "TC-HW-HM-009 no lost overflow events");
    bringup_pass("TC-HW-HM-009 concurrent writers: no torn reads, no lost events");

    /* TC-HW-HM-010 */
    check(health_monitor_get_snapshot(s_hm, NULL) == HEALTH_MONITOR_ERR_NULL_PTR,
          "TC-HW-HM-010 NULL snapshot");
    check(health_monitor_update_mqtt_stats(s_hm, NULL) == HEALTH_MONITOR_ERR_NULL_PTR,
          "TC-HW-HM-010 NULL stats");
    check(health_monitor_push_event(NULL, HEALTH_EVENT_FAULT, 0U) == HEALTH_MONITOR_ERR_NULL_PTR,
          "TC-HW-HM-010 NULL handle");
    check(health_monitor_push_event(s_hm, HEALTH_EVENT_COUNT, 0U) == HEALTH_MONITOR_ERR_OK,
          "TC-HW-HM-010 bad event is best-effort");
    check(health_monitor_push_event(s_hm, HEALTH_EVENT_SENSOR_FAIL, SENSOR_ID_COUNT) ==
              HEALTH_MONITOR_ERR_OK,
          "TC-HW-HM-010 bad sensor id is best-effort");
    {
        health_monitor_handle_t second = NULL;
        health_monitor_config_t cfg = {.boot_reset_cause = LIFECYCLE_RESET_POWER_ON};

        check(health_monitor_create(&cfg, &second) == HEALTH_MONITOR_ERR_NO_RESOURCE,
              "TC-HW-HM-010 second create");
    }
    bringup_pass("TC-HW-HM-010 error paths");

    /* TC-HW-HM-011 - last, because the fault latch lasts until reboot. */
    expect_hm(health_monitor_set_led_fault(s_hm), HEALTH_MONITOR_ERR_OK, "TC-HW-HM-011 fault");
    check_led_pattern(LED_PATTERN_BLINK_FAST, "TC-HW-HM-011 pattern");
    check_blink(HALF_PERIOD_FAST_MS, "TC-HW-HM-011 fault blink");

    expect_hm(health_monitor_push_event(s_hm, HEALTH_EVENT_ALARM_RAISED, 1U), HEALTH_MONITOR_ERR_OK,
              "TC-HW-HM-011 alarm");
    expect_hm(health_monitor_update_lifecycle(s_hm, LIFECYCLE_STATE_OPERATIONAL),
              HEALTH_MONITOR_ERR_OK, "TC-HW-HM-011 lifecycle");
    check_led_pattern(LED_PATTERN_BLINK_FAST, "TC-HW-HM-011 stays latched");
    check_blink(HALF_PERIOD_FAST_MS, "TC-HW-HM-011 still fast");
    bringup_pass("TC-HW-HM-011 fault blinks fast (100 ms) and stays latched");

    bringup_puts("\r\n=====================================================\r\n");
    bringup_puts("All 11 hardware cases passed.\r\n");
    bringup_puts("LD2 is now in the fault pattern (rapid blink): confirm it by eye.\r\n");
    bringup_puts("=====================================================\r\n");

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/* ---------------------------------------------------------------------- */
/* main                                                                    */
/* ---------------------------------------------------------------------- */

int main(void)
{
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
        (gpio_configure_pin(&uart_rx_config) != GPIO_OK) || (debug_uart_init() != DEBUG_UART_OK))
    {
        for (;;)
        {
            /* No sink to report to. */
        }
    }

    if (led_init(k_gw_led_pins, (uint8_t) LED_COUNT) != LED_ERR_OK)
    {
        bringup_puts("[FAIL] led_init - HALTED\r\n");
        for (;;)
        {
        }
    }

    health_monitor_config_t config = {.boot_reset_cause = EXPECTED_RESET_CAUSE};

    if (health_monitor_create(&config, &s_hm) != HEALTH_MONITOR_ERR_OK)
    {
        bringup_puts("[FAIL] health_monitor_create - HALTED\r\n");
        for (;;)
        {
        }
    }

    /* TC-HW-HM-001 observations, taken before the scheduler exists. */
    {
        health_snapshot_t snap;
        led_pattern_t pattern = LED_PATTERN_OFF;
        bool sensors_valid = true;

        if (health_monitor_get_snapshot(s_hm, &snap) == HEALTH_MONITOR_ERR_OK)
        {
            for (uint8_t i = 0U; i < (uint8_t) SENSOR_ID_COUNT; i++)
            {
                sensors_valid = sensors_valid && snap.sensor_valid[i];
            }

            s_init_state_ok = (snap.lifecycle_state == LIFECYCLE_STATE_INIT) &&
                              (snap.last_reset_cause == EXPECTED_RESET_CAUSE) && sensors_valid &&
                              (snap.uptime_s == 0U);
        }

        s_init_pattern_ok = (led_get_pattern(LED_GREEN, &pattern) == LED_ERR_OK) &&
                            (pattern == LED_PATTERN_BLINK_MEDIUM) && ld2_is_high();
    }

    s_main_handle = xTaskCreateStatic(bringup_task, "hm_bringup", MAIN_STACK_WORDS, NULL,
                                      TASK_PRIORITY, s_main_stack, &s_main_tcb);

    /* The stress writers exist from the start (idle until released), so
     * TC-HW-HM-006 can poll real watermarks for three tasks. */
    s_writer_a_handle = xTaskCreateStatic(writer_a_task, "hm_wr_a", WORKER_STACK_WORDS, NULL,
                                          TASK_PRIORITY, s_writer_a_stack, &s_writer_a_tcb);
    s_writer_b_handle = xTaskCreateStatic(writer_b_task, "hm_wr_b", WORKER_STACK_WORDS, NULL,
                                          TASK_PRIORITY, s_writer_b_stack, &s_writer_b_tcb);

    (void) health_monitor_register_task(s_hm, "hm_bringup", s_main_handle);
    (void) health_monitor_register_task(s_hm, "hm_wr_a", s_writer_a_handle);
    (void) health_monitor_register_task(s_hm, "hm_wr_b", s_writer_b_handle);

    vTaskStartScheduler();

    /* Only reached if the scheduler fails to start. */
    for (;;)
    {
    }
}
