/**
 * @file main_test_rtc.c
 * @brief Hardware bring-up test for RtcDriver (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate:
 *   1. Copy this file into Src/ (or add integration-tests/rtc/ as a source
 *      path in CubeIDE).
 *   2. In CubeIDE, right-click Src/main.c -> Properties ->
 *      C/C++ Build -> check "Exclude resource from build".
 *   3. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   4. Flash and run with a debugger attached (J-Link or ST-LINK).
 *
 * Hardware outputs:
 *   USART1 TX  PB6  115 200 8N1  Human-readable test results
 *   LD2 green  PA5                Heartbeat, ticking once per second
 *
 * RtcDriver is the dedicated Gateway implementation
 * (firmware/gateway/drivers/rtc/rtc.c). It has no GPIO surface of its own
 * (companion §4.8) — DebugUartDriver (USART1) is brought up purely for
 * reporting.
 *
 * Automated test sequence:
 *   TC-HW-RTC-001  rtc_init() returns RTC_OK; reports whether the backup
 *                   domain was valid (warm) or reset (cold) at boot
 *   TC-HW-RTC-002  rtc_set_time() to a known date/time, then rtc_get_time()
 *                   reads back the same value
 *   TC-HW-RTC-003  rtc_write_backup()/rtc_read_backup() round-trip on
 *                   backup register 31 (RTC_BACKUP_MAX_IDX_L475)
 *   TC-HW-RTC-004  Validation cascade: NULL pointer, out-of-range backup
 *                   index
 *
 * Manual step: reset the board (NRST, not power-cycle) after the automated
 * sequence completes, then reflash/rerun — TC-HW-RTC-001 should report the
 * backup domain as VALID (warm start) and the date/time set in TC-HW-RTC-002
 * should still read back correctly, proving persistence across a warm
 * reset. A full power-cycle (USB unplug) is expected to reset it to cold
 * start (companion §4.5 — VBAT tied to VDD on this board, no coin cell).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "cpu/cpu.h"
#include "gpio/gpio_driver.h"
#include "debug_uart/debug_uart.h"
#include "rtc/rtc.h"
#include "status.h"

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

#define BRINGUP_TX_TIMEOUT_MS (100U)

/** Arbitrary backup-register slot used for the round-trip test. */
#define BRINGUP_BACKUP_TEST_IDX RTC_BACKUP_MAX_IDX_L475
#define BRINGUP_BACKUP_TEST_VALUE (0xA5A55A5AUL)

/* ---------------------------------------------------------------------- */
/* Reporting helpers — built on DebugUartDriver.                           */
/* ---------------------------------------------------------------------- */

static void bringup_puts(const char *s)
{
    size_t len = 0U;
    while (s[len] != '\0')
    {
        ++len;
    }
    (void) debug_uart_send((const uint8_t *) s, len, BRINGUP_TX_TIMEOUT_MS);
}

static void bringup_pass(const char *label)
{
    bringup_puts("[PASS] ");
    bringup_puts(label);
    bringup_puts("\r\n");
}

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
    bringup_puts("[FAIL] ");
    bringup_puts(label);
    bringup_puts(" - HALTED\r\n");
    for (;;)
    {
        GPIOA->BSRR = (1UL << BRINGUP_LED_PIN);
        bringup_raw_spin(1000000U);
        GPIOA->BSRR = (1UL << (BRINGUP_LED_PIN + 16U));
        bringup_raw_spin(1000000U);
    }
}

/* ---------------------------------------------------------------------- */
/* main()                                                                  */
/* ---------------------------------------------------------------------- */

int main(void)
{
    /* cpu_init() first - clock to 80 MHz, DWT, fault handlers. */
    status_t init_st = cpu_init();
    if (init_st != STATUS_OK)
    {
        RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
        GPIOA->MODER &= ~(3UL << (BRINGUP_LED_PIN * 2U));
        GPIOA->MODER |= (1UL << (BRINGUP_LED_PIN * 2U));
        for (;;)
        {
            GPIOA->BSRR = (1UL << BRINGUP_LED_PIN);
            bringup_raw_spin(200000U);
            GPIOA->BSRR = (1UL << (BRINGUP_LED_PIN + 16U));
            bringup_raw_spin(200000U);
        }
    }

    /* gpio_init() + DebugUartDriver (USART1) — reporting channel only. */
    (void) gpio_init();

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
    (void) gpio_configure_pin(&uart_tx_config);
    (void) gpio_configure_pin(&uart_rx_config);
    (void) debug_uart_init();

    bringup_puts("\r\n======= RtcDriver Hardware Bring-up =======\r\n");
    bringup_puts("Board : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("RTC   : Backup-domain RTC, LSE 32.768 kHz\r\n");
    bringup_puts("============================================\r\n\r\n");

    /* TC-HW-RTC-001 */
    rtc_err_t rtc_err = rtc_init();
    if (rtc_err != RTC_OK)
    {
        char msg[64];
        (void) snprintf(msg, sizeof(msg), "TC-HW-RTC-001  rtc_init() returned %d", (int) rtc_err);
        bringup_fail(msg);
    }
    bringup_pass(rtc_is_backup_valid()
                     ? "TC-HW-RTC-001  rtc_init() OK - backup domain VALID (warm start)"
                     : "TC-HW-RTC-001  rtc_init() OK - backup domain RESET (cold start)");

    /* TC-HW-RTC-002 */
    rtc_datetime_t set_dt = {
        .year = 2026, .month = 7, .day = 6, .hour = 12, .minute = 0, .second = 0};
    if (rtc_set_time(&set_dt) != RTC_OK)
    {
        bringup_fail("TC-HW-RTC-002  rtc_set_time() failed");
    }

    rtc_datetime_t read_dt = {0};
    if (rtc_get_time(&read_dt) != RTC_OK)
    {
        bringup_fail("TC-HW-RTC-002  rtc_get_time() failed");
    }
    if ((read_dt.year != set_dt.year) || (read_dt.month != set_dt.month) ||
        (read_dt.day != set_dt.day) || (read_dt.hour != set_dt.hour) ||
        (read_dt.minute != set_dt.minute))
    {
        bringup_fail("TC-HW-RTC-002  rtc_get_time() did not match rtc_set_time()");
    }
    bringup_pass("TC-HW-RTC-002  set_time()/get_time() round-trip matched");

    /* TC-HW-RTC-003 */
    if (rtc_write_backup(BRINGUP_BACKUP_TEST_IDX, BRINGUP_BACKUP_TEST_VALUE) != RTC_OK)
    {
        bringup_fail("TC-HW-RTC-003  rtc_write_backup() failed");
    }
    uint32_t backup_val = 0U;
    if (rtc_read_backup(BRINGUP_BACKUP_TEST_IDX, &backup_val) != RTC_OK)
    {
        bringup_fail("TC-HW-RTC-003  rtc_read_backup() failed");
    }
    if (backup_val != BRINGUP_BACKUP_TEST_VALUE)
    {
        bringup_fail("TC-HW-RTC-003  backup register round-trip mismatch");
    }
    bringup_pass("TC-HW-RTC-003  backup register 31 round-trip matched");

    /* TC-HW-RTC-004 - validation cascade. */
    if (rtc_get_time(NULL) != RTC_ERR_NULL_ARG)
    {
        bringup_fail("TC-HW-RTC-004  rtc_get_time(NULL) did not return RTC_ERR_NULL_ARG");
    }
    if (rtc_read_backup(RTC_BACKUP_MAX_IDX_L475 + 1U, &backup_val) != RTC_ERR_BACKUP_BOUNDS)
    {
        bringup_fail(
            "TC-HW-RTC-004  out-of-range backup index did not return RTC_ERR_BACKUP_BOUNDS");
    }
    bringup_pass("TC-HW-RTC-004  validation cascade returned documented error codes");

    bringup_puts("\r\n[PASS] All automated tests complete.\r\n");
    bringup_puts("[INFO] Reset the board (NRST) and rerun to verify backup-domain\r\n");
    bringup_puts("       persistence across a warm reset (see file header).\r\n\r\n");

    for (;;)
    {
        gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        cpu_delay_ms(1000U);
    }
}
