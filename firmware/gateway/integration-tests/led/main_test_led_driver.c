/**
 * @file main_test_led_driver.c
 * @brief Hardware bring-up test for LedDriver (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. Add cloud_publisher (the current harness) to the `excluding=` list of
 *      the `integration-tests` source entry and remove led.
 *   2. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/led.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * Hardware under test:
 *   LD2 green  PB14  active-high  (the Gateway's one usable user LED)
 *   USART1 TX  PB6   115 200 8N1  Test results
 *
 * Nothing here touches PA5: it is LD1, shared with SPI1_SCK through SB1.
 *
 * The LED cannot be watched from a script, so the test reads the pin's output
 * data register (GPIOB->ODR) and times its transitions with the DWT cycle
 * counter. That checks the real gpio_write_pin() path, the polarity, and the
 * software timer's period on silicon. A final step leaves LD2 blinking slowly
 * so the result can also be confirmed by eye (companion HM-O2 / LED-O3 style
 * hardware confirmation).
 *
 * Automated test sequence:
 *   TC-HW-LED-001  gpio_init() + led_init(): LD2 is driven OFF (ODR bit clear).
 *   TC-HW-LED-002  Set before the scheduler starts (LED-D9): the LED is on
 *                   immediately and the queued blink begins once the scheduler
 *                   runs, at the 250 ms half-period.
 *   TC-HW-LED-003  LED_PATTERN_ON drives PB14 high and it stays high.
 *   TC-HW-LED-004  LED_PATTERN_OFF drives PB14 low and it stays low.
 *   TC-HW-LED-005  LED_PATTERN_BLINK_FAST toggles every 100 ms (+-15 %).
 *   TC-HW-LED-006  LED_PATTERN_BLINK_MEDIUM toggles every 250 ms (+-15 %).
 *   TC-HW-LED-007  LED_PATTERN_BLINK_SLOW toggles every 500 ms (+-15 %).
 *   TC-HW-LED-008  Blink -> solid ON and blink -> OFF stop the toggling.
 *   TC-HW-LED-009  Blink -> blink restarts the timer with the new period.
 *   TC-HW-LED-010  Error paths on silicon: LED_RED not fitted, bad pattern,
 *                   NULL output, get_pattern read-back.
 *
 * Integration checklist:
 *   [ ] All ten cases pass
 *   [ ] LD2 (green, next to the ST-LINK USB connector side) blinks slowly at the
 *       end - confirm by eye that this is the LED the driver controls
 *   [ ] LD1 / PA5 is never driven
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
#include "led/led_driver.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Board constants                                                         */
/* ---------------------------------------------------------------------- */

#define LD2_PORT GPIO_PORT_B
#define LD2_PIN (14U)
#define LD2_ODR_MASK (1UL << LD2_PIN)

#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)
#define BRINGUP_TX_TIMEOUT_MS (100U)

#define TASK_STACK_WORDS (384U)
#define TASK_PRIORITY (2U)

/* ---------------------------------------------------------------------- */
/* Timing constants                                                        */
/* ---------------------------------------------------------------------- */

#define HALF_PERIOD_FAST_MS (100UL)
#define HALF_PERIOD_MEDIUM_MS (250UL)
#define HALF_PERIOD_SLOW_MS (500UL)

#define TOLERANCE_PERCENT (15UL)
#define TOLERANCE_SLACK_US (3000UL) /* one tick of timer jitter plus margin */
#define TRANSITIONS_PER_CHECK (4U)
#define STABLE_WINDOW_MS (700UL)
#define MEASURE_TIMEOUT_MS (3000UL)
#define MAX_INTERVALS (8U)

#define MS_TO_US (1000UL)
#define US_PER_S (1000000UL)

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

    /* LD2 is the driver's LED and must not be used to signal failure, so just
     * park the task. */
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

static void expect_led(led_err_t got, led_err_t expected, const char *label)
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

/**
 * @brief Poll PB14 and record the time between transitions.
 *
 * The first interval is measured from the call. Returns when @p wanted
 * transitions have been seen or the timeout expires.
 *
 * @return Number of intervals recorded.
 */
static uint8_t measure_transitions(uint8_t wanted, uint32_t intervals_us[MAX_INTERVALS])
{
    uint8_t seen = 0U;
    bool level = ld2_is_high();
    uint32_t mark = dwt_now();
    const uint32_t begin = mark;

    while ((seen < wanted) && (seen < MAX_INTERVALS) &&
           (dwt_elapsed_us(begin) < (MEASURE_TIMEOUT_MS * MS_TO_US * 2UL)))
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

/** @brief True if every interval lies within the tolerance of the half-period. */
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

/** @brief Confirm the pin holds @p level for the whole stable window. */
static bool pin_stays(bool level)
{
    const uint32_t start = dwt_now();

    while (dwt_elapsed_us(start) < (STABLE_WINDOW_MS * MS_TO_US))
    {
        if (ld2_is_high() != level)
        {
            return false;
        }
    }

    return true;
}

/** @brief Set a blink pattern and check its half-period on the pin. */
static void check_blink(led_pattern_t pattern, uint32_t half_period_ms, const char *label)
{
    uint32_t intervals[MAX_INTERVALS] = {0U};

    expect_led(led_set(LED_GREEN, pattern), LED_ERR_OK, label);

    uint8_t seen = measure_transitions(TRANSITIONS_PER_CHECK, intervals);

    report_intervals(label, intervals, seen);
    check(seen == TRANSITIONS_PER_CHECK, label);
    check(intervals_match(intervals, seen, half_period_ms), label);
}

/* ---------------------------------------------------------------------- */
/* Pre-scheduler observations (LED-D9)                                     */
/* ---------------------------------------------------------------------- */

static volatile bool s_init_left_led_off;
static volatile bool s_pre_scheduler_led_on;

/* ---------------------------------------------------------------------- */
/* Bring-up task                                                           */
/* ---------------------------------------------------------------------- */

static StaticTask_t s_task_tcb;
static StackType_t s_task_stack[TASK_STACK_WORDS];

static void bringup_task(void *arg)
{
    (void) arg;

    uint32_t intervals[MAX_INTERVALS] = {0U};
    led_pattern_t pattern = LED_PATTERN_OFF;
    uint8_t seen;

    bringup_puts("\r\n===== LedDriver Hardware Bring-up =====\r\n");
    bringup_puts("Board : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("LED   : LD2 green, PB14, active-high\r\n");
    bringup_puts("=======================================\r\n\r\n");

    /* TC-HW-LED-001 - observed in main() before anything else touched the pin. */
    check(s_init_left_led_off, "TC-HW-LED-001 led_init left LD2 off");
    bringup_pass("TC-HW-LED-001 gpio_init + led_init OK; LD2 driven off");

    /* TC-HW-LED-002 - a blink was requested before the scheduler started. */
    check(s_pre_scheduler_led_on, "TC-HW-LED-002 LD2 on immediately");
    seen = measure_transitions(TRANSITIONS_PER_CHECK, intervals);
    report_intervals("TC-HW-LED-002 queued blink", intervals, seen);
    check(seen == TRANSITIONS_PER_CHECK, "TC-HW-LED-002 queued blink began");
    /* The first interval is measured from task start, not from the timer start,
     * so only the later ones are held to the period. */
    check(intervals_match(&intervals[1], (uint8_t) (seen - 1U), HALF_PERIOD_MEDIUM_MS),
          "TC-HW-LED-002 queued blink period");
    bringup_pass("TC-HW-LED-002 blink set before the scheduler starts once it runs (LED-D9)");

    /* TC-HW-LED-003 / 004 */
    expect_led(led_set(LED_GREEN, LED_PATTERN_ON), LED_ERR_OK, "TC-HW-LED-003 set ON");
    vTaskDelay(pdMS_TO_TICKS(50U));
    check(ld2_is_high(), "TC-HW-LED-003 PB14 high");
    check(pin_stays(true), "TC-HW-LED-003 stays high");
    bringup_pass("TC-HW-LED-003 LED_PATTERN_ON: PB14 high and steady");

    expect_led(led_set(LED_GREEN, LED_PATTERN_OFF), LED_ERR_OK, "TC-HW-LED-004 set OFF");
    vTaskDelay(pdMS_TO_TICKS(50U));
    check(!ld2_is_high(), "TC-HW-LED-004 PB14 low");
    check(pin_stays(false), "TC-HW-LED-004 stays low");
    bringup_pass("TC-HW-LED-004 LED_PATTERN_OFF: PB14 low and steady");

    /* TC-HW-LED-005..007 */
    check_blink(LED_PATTERN_BLINK_FAST, HALF_PERIOD_FAST_MS, "TC-HW-LED-005 BLINK_FAST");
    bringup_pass("TC-HW-LED-005 BLINK_FAST toggles every 100 ms");

    check_blink(LED_PATTERN_BLINK_MEDIUM, HALF_PERIOD_MEDIUM_MS, "TC-HW-LED-006 BLINK_MEDIUM");
    bringup_pass("TC-HW-LED-006 BLINK_MEDIUM toggles every 250 ms");

    check_blink(LED_PATTERN_BLINK_SLOW, HALF_PERIOD_SLOW_MS, "TC-HW-LED-007 BLINK_SLOW");
    bringup_pass("TC-HW-LED-007 BLINK_SLOW toggles every 500 ms");

    /* TC-HW-LED-008 - leaving blink for a solid pattern must stop the toggling. */
    expect_led(led_set(LED_GREEN, LED_PATTERN_BLINK_FAST), LED_ERR_OK, "TC-HW-LED-008 blink");
    vTaskDelay(pdMS_TO_TICKS(250U));
    expect_led(led_set(LED_GREEN, LED_PATTERN_ON), LED_ERR_OK, "TC-HW-LED-008 to ON");
    vTaskDelay(pdMS_TO_TICKS(20U)); /* let a residual timer firing pass */
    check(ld2_is_high(), "TC-HW-LED-008 ON level");
    check(pin_stays(true), "TC-HW-LED-008 blink->ON stays high");

    expect_led(led_set(LED_GREEN, LED_PATTERN_BLINK_FAST), LED_ERR_OK, "TC-HW-LED-008 blink 2");
    vTaskDelay(pdMS_TO_TICKS(250U));
    expect_led(led_set(LED_GREEN, LED_PATTERN_OFF), LED_ERR_OK, "TC-HW-LED-008 to OFF");
    vTaskDelay(pdMS_TO_TICKS(20U));
    check(!ld2_is_high(), "TC-HW-LED-008 OFF level");
    check(pin_stays(false), "TC-HW-LED-008 blink->OFF stays low");
    bringup_pass("TC-HW-LED-008 blink -> solid stops the toggling (ON and OFF)");

    /* TC-HW-LED-009 - switching between blink rates restarts the timer. */
    expect_led(led_set(LED_GREEN, LED_PATTERN_BLINK_FAST), LED_ERR_OK, "TC-HW-LED-009 fast");
    vTaskDelay(pdMS_TO_TICKS(350U));
    check_blink(LED_PATTERN_BLINK_SLOW, HALF_PERIOD_SLOW_MS, "TC-HW-LED-009 FAST->SLOW");
    bringup_pass("TC-HW-LED-009 blink -> blink restarts with the new period");

    /* TC-HW-LED-010 */
    expect_led(led_set(LED_RED, LED_PATTERN_ON), LED_ERR_INVALID_ID, "TC-HW-LED-010 LED_RED");
    expect_led(led_set(LED_COUNT, LED_PATTERN_ON), LED_ERR_INVALID_ID, "TC-HW-LED-010 LED_COUNT");
    expect_led(led_set(LED_GREEN, LED_PATTERN_COUNT), LED_ERR_INVALID_PARAM,
               "TC-HW-LED-010 bad pattern");
    expect_led(led_get_pattern(LED_GREEN, NULL), LED_ERR_NULL_ARG, "TC-HW-LED-010 NULL out");
    expect_led(led_get_pattern(LED_GREEN, &pattern), LED_ERR_OK, "TC-HW-LED-010 get_pattern");
    check(pattern == LED_PATTERN_BLINK_SLOW, "TC-HW-LED-010 read-back");
    bringup_pass("TC-HW-LED-010 error paths and pattern read-back");

    bringup_puts("\r\n=====================================================\r\n");
    bringup_puts("All 10 hardware cases passed.\r\n");
    bringup_puts("LD2 is now blinking slowly (1 Hz): confirm it by eye.\r\n");
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
        bringup_puts("[FAIL] TC-HW-LED-001 led_init - HALTED\r\n");
        for (;;)
        {
        }
    }

    s_init_left_led_off = !ld2_is_high();

    /* Request a blink before the scheduler exists: the LED must come on at once
     * and the queued timer command must take effect when the scheduler starts. */
    if (led_set(LED_GREEN, LED_PATTERN_BLINK_MEDIUM) != LED_ERR_OK)
    {
        bringup_puts("[FAIL] TC-HW-LED-002 led_set before the scheduler - HALTED\r\n");
        for (;;)
        {
        }
    }

    s_pre_scheduler_led_on = ld2_is_high();

    (void) xTaskCreateStatic(bringup_task, "led_bringup", TASK_STACK_WORDS, NULL, TASK_PRIORITY,
                             s_task_stack, &s_task_tcb);

    vTaskStartScheduler();

    /* Only reached if the scheduler fails to start. */
    for (;;)
    {
    }
}
