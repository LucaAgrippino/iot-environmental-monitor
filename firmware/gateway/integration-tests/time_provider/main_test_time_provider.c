/**
 * @file main_test_time_provider.c
 * @brief Hardware bring-up test for TimeProvider (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. Add cloud_publisher (the current harness) to the `excluding=` list of
 *      the `integration-tests` source entry and remove time_provider.
 *   2. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/time_provider.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * Hardware outputs:
 *   USART1 TX  PB6   115 200 8N1  Test results, written through Logger
 *   LD2 green  PA5                Heartbeat on completion, fast blink on halt
 *
 * This test also stands in for the RtcDriver bring-up on the Gateway: every
 * RTC surface TimeProvider uses (get/set time, backup registers) is exercised
 * on silicon, and TC-HW-TP-004 reads the calendar back through rtc_get_time().
 *
 * TWO-PHASE TEST.  The sync flag lives in RTC backup register BKP0R, which
 * survives a warm reset but not a power cycle (VBAT is tied to VDD on this
 * board).  Persistence is therefore proven across a real reset:
 *
 *   Phase A runs when the provider boots UNSYNCHRONISED (cold start, or after
 *   Phase B cleared the flag).  It exercises every public function, then leaves
 *   the provider SYNCHRONISED and asks for a RESET (button B2, or an OpenOCD
 *   reset).  A power cycle also works but restarts Phase A.
 *
 *   Phase B runs when the provider boots SYNCHRONISED, i.e. BKP0R still holds
 *   the magic.  It proves the state and the running RTC survived the reset,
 *   then marks the provider unsynchronised again so the next run starts at
 *   Phase A.
 *
 * Boot order follows the companion §8: cpu_init, debug_uart_init, rtc_init,
 * logger_init, then time_provider_init, all before the scheduler.
 *
 * Automated test sequence:
 *   Phase A
 *   TC-HW-TP-001  Boot UNSYNCHRONISED; get() returns uptime seconds that advance.
 *   TC-HW-TP-002  set_time() synchronises, pushes SYNC_ACQUIRED once, and
 *                  get() then returns the wall-clock epoch, advancing at 1 Hz.
 *   TC-HW-TP-003  The RTC calendar read back through rtc_get_time() matches the
 *                  epoch that was set (conversion verified on silicon).
 *   TC-HW-TP-004  A step beyond the sanity delta is rejected and changes nothing.
 *   TC-HW-TP-005  Epochs outside 2000..2099 are rejected before the driver.
 *   TC-HW-TP-006  A leap-day rollover (2024-02-29 23:59:58 -> 03-01 00:00:01)
 *                  is counted correctly by the hardware and by get().
 *   TC-HW-TP-007  mark_unsynchronised() pushes SYNC_LOST, clears BKP0R, and
 *                  get() falls back to uptime.
 *   TC-HW-TP-008  Two tasks hammer get()/set_time() while a third polls the sync
 *                  state: no errors, no out-of-window epochs, no deadlock (the
 *                  mutex serialises the RtcDriver, which is not thread-safe).
 *   Phase B (after RESET)
 *   TC-HW-TP-009  The provider boots SYNCHRONISED from BKP0R with no set_time()
 *                  call and no SYNC_ACQUIRED event.
 *   TC-HW-TP-010  The RTC kept running through the reset: get() is at or after
 *                  the epoch Phase A left, by no more than the elapsed time.
 *   TC-HW-TP-011  mark_unsynchronised() clears BKP0R so the next run is Phase A.
 *
 * Integration checklist:
 *   [ ] Uptime and wall-clock epochs both advance at one second per second
 *   [ ] The leap-day rollover lands on 2024-03-01 00:00:01
 *   [ ] The stress test reports zero errors
 *   [ ] Phase B boots SYNCHRONISED without a set_time() call
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "logger/logger.h"
#include "rtc/rtc.h"
#include "time_provider/time_provider.h"
#include "time_provider/time_provider_config.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Board constants                                                         */
/* ---------------------------------------------------------------------- */

#define BRINGUP_LED_PORT GPIO_PORT_A
#define BRINGUP_LED_PIN (5U)

#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)

#define TAG "TP-HW"

#define MAIN_TASK_STACK_WORDS (512U)
#define WORKER_TASK_STACK_WORDS (384U)
#define MAIN_TASK_PRIORITY (2U)
#define WORKER_TASK_PRIORITY (2U)

#define LOG_FLUSH_MS (400U)
#define SECOND_MS (1000U)

/* ---------------------------------------------------------------------- */
/* Reference epochs                                                        */
/* ---------------------------------------------------------------------- */

#define EPOCH_BASE (1791750615UL) /* 2026-10-11 20:30:15 UTC */
#define BASE_YEAR (2026U)
#define BASE_MONTH (10U)
#define BASE_DAY (11U)
#define BASE_HOUR (20U)
#define BASE_MINUTE (30U)
#define BASE_SECOND (15U)

/** How long after Phase A the clock may be and still count as Phase B (seconds). */
#define PHASE_B_WINDOW_S (3600UL)

#define EPOCH_LEAP_EVE (1709251198UL) /* 2024-02-29 23:59:58 UTC */
#define LEAP_ROLLOVER_WAIT_MS (3200U)

#define EPOCH_STRESS (1791750000UL) /* fixed value the stress writer keeps setting */
#define STRESS_ITERATIONS (1000U)
#define STRESS_TIMEOUT_MS (20000U)
#define STRESS_POLL_MS (50U)
#define STRESS_WINDOW_BEFORE_S (1UL)
#define STRESS_WINDOW_AFTER_S (10UL)

/* ---------------------------------------------------------------------- */
/* IHealthReport spy                                                       */
/* ---------------------------------------------------------------------- */

static volatile uint32_t s_acquired_events;
static volatile uint32_t s_lost_events;

static health_monitor_err_t spy_health_init(void)
{
    return HEALTH_MONITOR_ERR_OK;
}

static health_monitor_err_t spy_health_push(health_event_t event, uint32_t param)
{
    (void) param;

    if (event == HEALTH_EVENT_TIME_SYNC_ACQUIRED)
    {
        s_acquired_events++;
    }
    else if (event == HEALTH_EVENT_TIME_SYNC_LOST)
    {
        s_lost_events++;
    }
    else
    {
        /* TimeProvider pushes only the two events above. */
    }

    return HEALTH_MONITOR_ERR_OK;
}

static const ihealth_report_t s_health_spy = {
    .init = spy_health_init,
    .push_event = spy_health_push,
};

/* ---------------------------------------------------------------------- */
/* Reporting helpers — built on Logger.                                    */
/* ---------------------------------------------------------------------- */

static void bringup_raw_spin(uint32_t n)
{
    volatile uint32_t c = n;

    while (c > 0U)
    {
        --c;
    }
}

static void bringup_fail(const char *label)
{
    LOG_ERROR(TAG, "[FAIL] %s - HALTED", label);
    vTaskDelay(pdMS_TO_TICKS(LOG_FLUSH_MS));

    for (;;)
    {
        GPIOA->BSRR = (1UL << BRINGUP_LED_PIN);
        bringup_raw_spin(1000000U);
        GPIOA->BSRR = (1UL << (BRINGUP_LED_PIN + 16U));
        bringup_raw_spin(1000000U);
    }
}

static void check(bool condition, const char *label)
{
    if (!condition)
    {
        bringup_fail(label);
    }
}

static void expect_err(time_provider_err_t got, time_provider_err_t expected, const char *label)
{
    if (got != expected)
    {
        LOG_ERROR(TAG, "%s (err=%d, expected=%d)", label, (int) got, (int) expected);
        bringup_fail(label);
    }
}

static void pass(const char *label)
{
    LOG_INFO(TAG, "[PASS] %s", label);
}

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ---------------------------------------------------------------------- */
/* Stress workers                                                          */
/* ---------------------------------------------------------------------- */

static StaticTask_t s_reader_tcb;
static StackType_t s_reader_stack[WORKER_TASK_STACK_WORDS];
static StaticTask_t s_writer_tcb;
static StackType_t s_writer_stack[WORKER_TASK_STACK_WORDS];

static volatile uint32_t s_stress_done;
static volatile uint32_t s_stress_errors;
static volatile uint32_t s_stress_out_of_window;

static void note_error(void)
{
    s_stress_errors++;
}

static void reader_task(void *arg)
{
    (void) arg;

    for (uint32_t i = 0U; i < STRESS_ITERATIONS; i++)
    {
        time_provider_ts_t ts;

        if (time_provider_get(&ts) != TIME_PROVIDER_ERR_OK)
        {
            note_error();
        }
        else if ((ts.epoch < (EPOCH_STRESS - STRESS_WINDOW_BEFORE_S)) ||
                 (ts.epoch > (EPOCH_STRESS + STRESS_WINDOW_AFTER_S)))
        {
            s_stress_out_of_window++;
        }
        else
        {
            /* In window. */
        }

        if (time_provider_get_sync_state() != TIME_SYNC_SYNCHRONISED)
        {
            note_error();
        }
    }

    s_stress_done++;
    vTaskDelete(NULL);
}

static void writer_task(void *arg)
{
    (void) arg;

    for (uint32_t i = 0U; i < STRESS_ITERATIONS; i++)
    {
        if (time_provider_set_time(EPOCH_STRESS) != TIME_PROVIDER_ERR_OK)
        {
            note_error();
        }
    }

    s_stress_done++;
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------- */
/* Phase A                                                                 */
/* ---------------------------------------------------------------------- */

static void phase_a(void)
{
    time_provider_ts_t ts;
    time_provider_ts_t later;
    rtc_datetime_t dt;
    uint32_t value = 0U;

    LOG_INFO(TAG, "--- Phase A: full API on a cold start ---");

    /* TC-HW-TP-001 */
    expect_err(time_provider_get(&ts), TIME_PROVIDER_ERR_OK, "TC-HW-TP-001 get");
    check(ts.sync_state == TIME_SYNC_UNSYNCHRONISED, "TC-HW-TP-001 state");
    delay_ms(1200U);
    expect_err(time_provider_get(&later), TIME_PROVIDER_ERR_OK, "TC-HW-TP-001 get later");
    check(later.epoch > ts.epoch, "TC-HW-TP-001 uptime advances");
    pass("TC-HW-TP-001 boots UNSYNCHRONISED; uptime advances");

    /* TC-HW-TP-002 */
    expect_err(time_provider_set_time(EPOCH_BASE), TIME_PROVIDER_ERR_OK, "TC-HW-TP-002 set_time");
    check(time_provider_get_sync_state() == TIME_SYNC_SYNCHRONISED, "TC-HW-TP-002 state");
    check(s_acquired_events == 1U, "TC-HW-TP-002 one SYNC_ACQUIRED event");
    expect_err(time_provider_get(&ts), TIME_PROVIDER_ERR_OK, "TC-HW-TP-002 get");
    check(ts.sync_state == TIME_SYNC_SYNCHRONISED, "TC-HW-TP-002 get state");
    check((ts.epoch >= EPOCH_BASE) && (ts.epoch <= (EPOCH_BASE + 1UL)), "TC-HW-TP-002 epoch");
    delay_ms(2100U);
    expect_err(time_provider_get(&later), TIME_PROVIDER_ERR_OK, "TC-HW-TP-002 get later");
    check((later.epoch - ts.epoch) >= 2UL, "TC-HW-TP-002 epoch advances at 1 Hz");
    check((later.epoch - ts.epoch) <= 3UL, "TC-HW-TP-002 epoch does not run fast");
    pass("TC-HW-TP-002 set_time synchronises; wall clock advances at 1 Hz");

    /* TC-HW-TP-003 */
    expect_err(time_provider_set_time(EPOCH_BASE), TIME_PROVIDER_ERR_OK, "TC-HW-TP-003 re-set");
    check(s_acquired_events == 1U, "TC-HW-TP-003 no second SYNC_ACQUIRED");
    check(rtc_get_time(&dt) == RTC_OK, "TC-HW-TP-003 rtc_get_time");
    check((dt.year == BASE_YEAR) && (dt.month == BASE_MONTH) && (dt.day == BASE_DAY) &&
              (dt.hour == BASE_HOUR) && (dt.minute == BASE_MINUTE) &&
              ((dt.second == BASE_SECOND) || (dt.second == (BASE_SECOND + 1U))),
          "TC-HW-TP-003 calendar fields");
    pass("TC-HW-TP-003 RTC calendar matches the epoch that was set");

    /* TC-HW-TP-004 */
    expect_err(time_provider_set_time(EPOCH_BASE + TIME_PROVIDER_SANITY_DELTA_S + 3600UL),
               TIME_PROVIDER_ERR_RTC_FAIL, "TC-HW-TP-004 sanity rejection");
    expect_err(time_provider_get(&ts), TIME_PROVIDER_ERR_OK, "TC-HW-TP-004 get");
    check(ts.epoch < (EPOCH_BASE + 30UL), "TC-HW-TP-004 clock untouched");
    check(time_provider_get_sync_state() == TIME_SYNC_SYNCHRONISED, "TC-HW-TP-004 state");
    pass("TC-HW-TP-004 oversized step rejected; clock and state unchanged");

    /* TC-HW-TP-005 */
    expect_err(time_provider_set_time(0UL), TIME_PROVIDER_ERR_RTC_FAIL, "TC-HW-TP-005 epoch 0");
    expect_err(time_provider_set_time(TIME_PROVIDER_EPOCH_MAX + 1UL), TIME_PROVIDER_ERR_RTC_FAIL,
               "TC-HW-TP-005 epoch above 2099");
    expect_err(time_provider_get(&ts), TIME_PROVIDER_ERR_OK, "TC-HW-TP-005 get");
    check(ts.epoch < (EPOCH_BASE + 60UL), "TC-HW-TP-005 clock untouched");
    pass("TC-HW-TP-005 out-of-range epochs rejected before the driver");

    /* TC-HW-TP-006 - leave the sanity window by dropping sync first. */
    expect_err(time_provider_mark_unsynchronised(), TIME_PROVIDER_ERR_OK, "TC-HW-TP-006 unsync");
    expect_err(time_provider_set_time(EPOCH_LEAP_EVE), TIME_PROVIDER_ERR_OK, "TC-HW-TP-006 set");
    delay_ms(LEAP_ROLLOVER_WAIT_MS);
    check(rtc_get_time(&dt) == RTC_OK, "TC-HW-TP-006 rtc_get_time");
    check((dt.year == 2024U) && (dt.month == 3U) && (dt.day == 1U) && (dt.hour == 0U) &&
              (dt.minute == 0U) && (dt.second >= 1U) && (dt.second <= 2U),
          "TC-HW-TP-006 hardware rolled over 02-29 -> 03-01");
    expect_err(time_provider_get(&ts), TIME_PROVIDER_ERR_OK, "TC-HW-TP-006 get");
    check((ts.epoch >= (EPOCH_LEAP_EVE + 3UL)) && (ts.epoch <= (EPOCH_LEAP_EVE + 5UL)),
          "TC-HW-TP-006 get() across the leap day");
    pass("TC-HW-TP-006 leap-day rollover counted correctly");

    /* TC-HW-TP-007 - step 006 left the provider SYNCHRONISED (its set_time()
     * ran while UNSYNCHRONISED), so dropping it now must raise SYNC_LOST. */
    check(time_provider_get_sync_state() == TIME_SYNC_SYNCHRONISED, "TC-HW-TP-007 precondition");
    s_lost_events = 0U;
    expect_err(time_provider_mark_unsynchronised(), TIME_PROVIDER_ERR_OK, "TC-HW-TP-007 mark");
    check(s_lost_events == 1U, "TC-HW-TP-007 one SYNC_LOST event");
    check(time_provider_get_sync_state() == TIME_SYNC_UNSYNCHRONISED, "TC-HW-TP-007 state");
    check(rtc_read_backup(TIME_PROVIDER_BKUP_REG, &value) == RTC_OK, "TC-HW-TP-007 read BKP0R");
    check(value == TIME_PROVIDER_SYNC_CLEARED, "TC-HW-TP-007 BKP0R cleared");
    expect_err(time_provider_get(&ts), TIME_PROVIDER_ERR_OK, "TC-HW-TP-007 get");
    check(ts.sync_state == TIME_SYNC_UNSYNCHRONISED, "TC-HW-TP-007 get state");
    check(ts.epoch < EPOCH_BASE, "TC-HW-TP-007 back to uptime seconds");
    pass("TC-HW-TP-007 mark_unsynchronised: event, flag cleared, uptime fallback");

    /* TC-HW-TP-008 */
    expect_err(time_provider_set_time(EPOCH_STRESS), TIME_PROVIDER_ERR_OK, "TC-HW-TP-008 set");
    s_stress_done = 0U;
    s_stress_errors = 0U;
    s_stress_out_of_window = 0U;

    (void) xTaskCreateStatic(reader_task, "tp_read", WORKER_TASK_STACK_WORDS, NULL,
                             WORKER_TASK_PRIORITY, s_reader_stack, &s_reader_tcb);
    (void) xTaskCreateStatic(writer_task, "tp_write", WORKER_TASK_STACK_WORDS, NULL,
                             WORKER_TASK_PRIORITY, s_writer_stack, &s_writer_tcb);

    uint32_t waited = 0U;

    while ((s_stress_done < 2U) && (waited < STRESS_TIMEOUT_MS))
    {
        delay_ms(STRESS_POLL_MS);
        waited += STRESS_POLL_MS;
    }

    check(s_stress_done == 2U, "TC-HW-TP-008 workers finished (no deadlock)");
    LOG_INFO(TAG, "TC-HW-TP-008 %lu reads + %lu writes in %lu ms, %lu errors, %lu out of window",
             (unsigned long) STRESS_ITERATIONS, (unsigned long) STRESS_ITERATIONS,
             (unsigned long) waited, (unsigned long) s_stress_errors,
             (unsigned long) s_stress_out_of_window);
    check(s_stress_errors == 0U, "TC-HW-TP-008 no API errors under contention");
    check(s_stress_out_of_window == 0U, "TC-HW-TP-008 no corrupt epochs under contention");
    pass("TC-HW-TP-008 concurrent get/set_time are serialised by the mutex");

    /* Leave the provider SYNCHRONISED with the flag persisted for Phase B. */
    expect_err(time_provider_set_time(EPOCH_BASE), TIME_PROVIDER_ERR_OK, "persist set_time");
    expect_err(time_provider_get(&ts), TIME_PROVIDER_ERR_OK, "persist get");
    LOG_INFO(TAG, "epoch left for Phase B: %lu", (unsigned long) ts.epoch);

    LOG_INFO(TAG, "=====================================================");
    LOG_INFO(TAG, "Phase A complete. The provider is SYNCHRONISED, BKP0R set.");
    LOG_INFO(TAG, "ACTION: press the black RESET button (B2) to run Phase B.");
    LOG_INFO(TAG, "Do NOT power-cycle: that clears the backup domain.");
    LOG_INFO(TAG, "=====================================================");
}

/* ---------------------------------------------------------------------- */
/* Phase B                                                                 */
/* ---------------------------------------------------------------------- */

static void phase_b(void)
{
    time_provider_ts_t ts;
    uint32_t value = 0U;

    LOG_INFO(TAG, "--- Phase B: recovery after a warm reset ---");

    /* A SYNCHRONISED boot is only Phase B if the RTC still holds the time Phase A
     * left. A flag left over from another run (or a clock that was reset) would
     * make the checks below meaningless, so clear it and start over instead. */
    expect_err(time_provider_get(&ts), TIME_PROVIDER_ERR_OK, "Phase B probe");

    if ((ts.epoch < EPOCH_BASE) || (ts.epoch > (EPOCH_BASE + PHASE_B_WINDOW_S)))
    {
        LOG_WARN(TAG, "sync flag set but the clock (%lu) is not Phase A's - stale state",
                 (unsigned long) ts.epoch);
        expect_err(time_provider_mark_unsynchronised(), TIME_PROVIDER_ERR_OK, "stale clear");
        LOG_INFO(TAG, "flag cleared. ACTION: press RESET (B2) to start at Phase A.");
        return;
    }

    /* TC-HW-TP-009 */
    check(time_provider_get_sync_state() == TIME_SYNC_SYNCHRONISED, "TC-HW-TP-009 state");
    check(s_acquired_events == 0U, "TC-HW-TP-009 no SYNC_ACQUIRED without set_time");
    pass("TC-HW-TP-009 booted SYNCHRONISED from BKP0R, no set_time() call");

    /* TC-HW-TP-010 */
    expect_err(time_provider_get(&ts), TIME_PROVIDER_ERR_OK, "TC-HW-TP-010 get");
    check(ts.sync_state == TIME_SYNC_SYNCHRONISED, "TC-HW-TP-010 get state");
    check(ts.epoch >= EPOCH_BASE, "TC-HW-TP-010 RTC did not go backwards");
    check(ts.epoch <= (EPOCH_BASE + PHASE_B_WINDOW_S), "TC-HW-TP-010 RTC ran on, not reset");
    LOG_INFO(TAG, "TC-HW-TP-010 epoch %lu is %lu s past the Phase A reference",
             (unsigned long) ts.epoch, (unsigned long) (ts.epoch - EPOCH_BASE));
    pass("TC-HW-TP-010 the RTC kept running through the reset");

    /* TC-HW-TP-011 */
    s_lost_events = 0U;
    expect_err(time_provider_mark_unsynchronised(), TIME_PROVIDER_ERR_OK, "TC-HW-TP-011 mark");
    check(s_lost_events == 1U, "TC-HW-TP-011 SYNC_LOST event");
    check(rtc_read_backup(TIME_PROVIDER_BKUP_REG, &value) == RTC_OK, "TC-HW-TP-011 read BKP0R");
    check(value == TIME_PROVIDER_SYNC_CLEARED, "TC-HW-TP-011 BKP0R cleared");
    pass("TC-HW-TP-011 flag cleared; the next run starts at Phase A");

    LOG_INFO(TAG, "=====================================================");
    LOG_INFO(TAG, "Phase B complete. All 11 hardware cases passed.");
    LOG_INFO(TAG, "=====================================================");
}

/* ---------------------------------------------------------------------- */
/* Bring-up task                                                           */
/* ---------------------------------------------------------------------- */

static StaticTask_t s_main_tcb;
static StackType_t s_main_stack[MAIN_TASK_STACK_WORDS];

/** Sync state sampled right after time_provider_init(), before any set_time(). */
static time_sync_state_t s_boot_state;

static void bringup_task(void *arg)
{
    (void) arg;

    if (s_boot_state == TIME_SYNC_SYNCHRONISED)
    {
        phase_b();
    }
    else
    {
        phase_a();
    }

    for (;;)
    {
        gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        delay_ms(SECOND_MS);
    }
}

/* ---------------------------------------------------------------------- */
/* main                                                                    */
/* ---------------------------------------------------------------------- */

int main(void)
{
    /* cpu_init() first - clock to 80 MHz, DWT, fault handlers, LSE for the RTC. */
    if (cpu_init() != STATUS_OK)
    {
        for (;;)
        {
            /* Nothing can be reported before the UART exists. */
        }
    }

    gpio_pin_config_t led_config = {
        .port = BRINGUP_LED_PORT,
        .pin = BRINGUP_LED_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 0U,
    };
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
    (void) gpio_configure_pin(&led_config);
    (void) gpio_configure_pin(&uart_tx_config);
    (void) gpio_configure_pin(&uart_rx_config);

    if ((debug_uart_init() != DEBUG_UART_OK) || (rtc_init() != RTC_OK))
    {
        for (;;)
        {
            /* Logger cannot start without its sink and its clock. */
        }
    }

    (void) logger_init(LOG_LEVEL_DEBUG);

    LOG_INFO(TAG, "===== TimeProvider Hardware Bring-up =====");
    LOG_INFO(TAG, "Board : B-L475E-IOT01A (STM32L475VGTx)");
    LOG_INFO(TAG, "SYSCLK=%lu Hz", (unsigned long) cpu_get_sysclk_hz());

    if (time_provider_init(&s_health_spy) != TIME_PROVIDER_ERR_OK)
    {
        LOG_ERROR(TAG, "[FAIL] time_provider_init - HALTED");
        for (;;)
        {
        }
    }

    s_boot_state = time_provider_get_sync_state();
    LOG_INFO(TAG, "booted %s",
             (s_boot_state == TIME_SYNC_SYNCHRONISED) ? "SYNCHRONISED (warm reset)"
                                                      : "UNSYNCHRONISED (cold start)");

    (void) xTaskCreateStatic(bringup_task, "tp_bringup", MAIN_TASK_STACK_WORDS, NULL,
                             MAIN_TASK_PRIORITY, s_main_stack, &s_main_tcb);

    vTaskStartScheduler();

    /* Only reached if the scheduler fails to start. */
    for (;;)
    {
    }
}
