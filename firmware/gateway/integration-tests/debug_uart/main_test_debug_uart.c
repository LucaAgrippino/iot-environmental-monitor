/**
 * @file main_test_debug_uart.c
 * @brief Hardware bring-up test for DebugUartDriver (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate:
 *   1. Copy this file into Src/ (or add integration-tests/debug_uart/ as
 *      a source path in CubeIDE).
 *   2. In CubeIDE, right-click Src/main.c -> Properties ->
 *      C/C++ Build -> check "Exclude resource from build".
 *   3. Connect a serial terminal to PB6 (TX) / PB7 (RX) at 115 200 8N1.
 *   4. Flash and run with a debugger attached (J-Link or ST-LINK).
 *
 * Hardware outputs:
 *   USART1 TX/RX  PB6/PB7  115 200 8N1  Test results and echo prompt
 *   LD2 green     PA5                    Heartbeat
 *
 * Unlike the SpiDriver bring-up test, DebugUartDriver IS the reporting
 * channel here — there is no separate raw-register UART just for test
 * output, since debug_uart_send() is the function under test.
 *
 * Automated test sequence (run once, TX-only, before RX is attached):
 *   TC-HW-DUART-001  gpio_init() + debug_uart_init() both return OK
 *   TC-HW-DUART-002  debug_uart_send() transmits a banner without timing out
 *   TC-HW-DUART-003  debug_uart_attach_rx() succeeds; a second call is
 *                     rejected with DEBUG_UART_ERR_RX_ALREADY_ATTACHED
 *
 * Manual/interactive step (after the automated sequence prints its
 * banner): type a line into the serial terminal and press Enter. The
 * firmware echoes it back prefixed with "ECHO: ", proving the RX ISR,
 * line buffering, and debug_uart_read_line() all work end-to-end on
 * real hardware. LD2 toggles once per echoed line.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "cpu/cpu.h"
#include "gpio/gpio_driver.h"
#include "debug_uart/debug_uart.h"
#include "status.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Board constants                                                         */
/* ---------------------------------------------------------------------- */

/** LD2 (green user LED) on B-L475E-IOT01A — active high. */
#define BRINGUP_LED_PORT GPIO_PORT_A
#define BRINGUP_LED_PIN (5U)

/** USART1 TX/RX on PB6/PB7, alternate function AF7 (companion §4.2). */
#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)

/** Per-byte TX timeout — generous for a bring-up test. */
#define BRINGUP_TX_TIMEOUT_MS (100U)

/* ---------------------------------------------------------------------- */
/* Reporting helpers — built on the driver under test.                     */
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

/**
 * @brief Emit a FAIL banner and enter a fast-blink halt loop.
 *
 * Drives LD2 directly via BSRR, not gpio_write_pin() — by the time a
 * failure can be reported, GpioDriver may itself be implicated, so the
 * halt indicator must not depend on it.
 */
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
/* RX line-ready callback                                                  */
/* ---------------------------------------------------------------------- */

/** Set true by the ISR-context callback; polled from main(). No FreeRTOS
 *  is running in this bring-up, so a plain flag stands in for the task
 *  notification a real consumer would use (companion §2.3, §5). */
static volatile bool s_line_ready;

static void bringup_on_line_ready(void *ctx)
{
    (void) ctx;
    s_line_ready = true;
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

    /* gpio_init() next - required before any gpio_configure_pin() call. */
    if (gpio_init() != GPIO_OK)
    {
        for (;;)
        {
            GPIOA->BSRR = (1UL << BRINGUP_LED_PIN);
            bringup_raw_spin(100000U);
            GPIOA->BSRR = (1UL << (BRINGUP_LED_PIN + 16U));
            bringup_raw_spin(100000U);
        }
    }

    /* Bring up the USART1 TX/RX pins through GpioDriver. DebugUartDriver
     * configures the peripheral itself (companion §3.4 P1 — it owns its
     * own pins, mirroring the SpiDriver/GpioDriver convention). */
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
    bringup_pass("TC-HW-DUART-001a  gpio_init() and UART pin config done");

    /* TC-HW-DUART-001b - debug_uart_init(). */
    if (debug_uart_init() != DEBUG_UART_OK)
    {
        /* debug_uart_send() isn't usable yet if init failed - fall back
         * to the LED halt loop. */
        bringup_fail("TC-HW-DUART-001b  debug_uart_init() failed");
    }

    /* No tick source wired: CpuDriver does not yet expose a monotonic
     * millisecond uptime reader (DUART-O6). debug_uart_send() therefore
     * runs an unbounded TXE wait here — acceptable for this bring-up,
     * where a wedged peripheral hanging is itself a diagnostic result. */

    bringup_puts("\r\n======= DebugUartDriver Hardware Bring-up =======\r\n");
    bringup_puts("Board : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("UART  : USART1  TX=PB6 RX=PB7  115 200 8N1\r\n");
    bringup_puts("==================================================\r\n\r\n");
    bringup_pass("TC-HW-DUART-001b  debug_uart_init() returned DEBUG_UART_OK");

    /* TC-HW-DUART-002 - debug_uart_send() transmits without timing out. */
    bringup_pass("TC-HW-DUART-002  debug_uart_send() transmitted this banner");

    /* TC-HW-DUART-003 - attach_rx() happy path + reject-on-second-call. */
    if (debug_uart_attach_rx(bringup_on_line_ready, NULL) != DEBUG_UART_OK)
    {
        bringup_fail("TC-HW-DUART-003  debug_uart_attach_rx() failed");
    }
    if (debug_uart_attach_rx(bringup_on_line_ready, NULL) != DEBUG_UART_ERR_RX_ALREADY_ATTACHED)
    {
        bringup_fail("TC-HW-DUART-003  second attach_rx() did not reject as documented");
    }
    bringup_pass("TC-HW-DUART-003  attach_rx() succeeded once, rejected on second call");

    bringup_puts("\r\n[PASS] All automated tests complete.\r\n");
    bringup_puts("[INFO] Type a line and press Enter - it will be echoed back.\r\n");
    bringup_puts("[INFO] Heartbeat: LD2 PA5 toggles once per echoed line.\r\n\r\n");

    /* ------------------------------------------------------------------ */
    /* Interactive echo loop - proves the RX ISR, line buffering, and     */
    /* debug_uart_read_line() end-to-end on real hardware.                */
    /* ------------------------------------------------------------------ */
    for (;;)
    {
        if (s_line_ready)
        {
            s_line_ready = false;

            uint8_t line_buf[DEBUG_UART_LINE_MAX_LEN + 1U];
            size_t line_len = 0U;
            debug_uart_line_flag_t line_flag = DEBUG_UART_LINE_OK;

            if (debug_uart_read_line(line_buf, sizeof(line_buf), &line_len, &line_flag) ==
                DEBUG_UART_OK)
            {
                bringup_puts("ECHO: ");
                (void) debug_uart_send(line_buf, line_len, BRINGUP_TX_TIMEOUT_MS);
                if (line_flag == DEBUG_UART_LINE_TRUNCATED)
                {
                    bringup_puts(" [TRUNCATED]");
                }
                bringup_puts("\r\n");
                gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
            }
        }
    }
}
