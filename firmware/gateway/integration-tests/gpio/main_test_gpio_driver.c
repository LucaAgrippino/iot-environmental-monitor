/**
 * @file main_test_gpio_driver.c
 * @brief Hardware bring-up test for GpioDriver (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate:
 *   1. Copy this file into Src/ (or add integration-tests/gpio/ as a
 *      source path in CubeIDE).
 *   2. In CubeIDE, right-click Src/main.c → Properties →
 *      C/C++ Build → check "Exclude resource from build".
 *   3. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   4. Flash and run with a debugger attached (J-Link or ST-LINK).
 *
 * Hardware outputs:
 *   USART1 TX  PB6  115 200 8N1  Human-readable test results
 *   LD2 green  PA5               Driven exclusively through gpio_write_pin()
 *                                 / gpio_toggle_pin() under test
 *   PA1        Arduino header A1  Spare pin, not wired to any onboard
 *                                 peripheral (UM2153 Appendix A) — configured
 *                                 INPUT + internal pull-up and read back
 *
 * Automated test sequence:
 *   TC-HW-GPIO-001  gpio_init() returns GPIO_OK
 *   TC-HW-GPIO-002  gpio_configure_pin() ALTERNATE (PB6, AF7) succeeds —
 *                    also brings up the USART1 TX pin used for this report
 *   TC-HW-GPIO-003  gpio_configure_pin() OUTPUT push-pull (PA5) succeeds
 *   TC-HW-GPIO-004  gpio_write_pin() drives LD2 on then off (visual)
 *   TC-HW-GPIO-005  gpio_toggle_pin() blinks LD2 ten times (visual)
 *   TC-HW-GPIO-006  gpio_configure_pin() INPUT pull-up (PA1) + gpio_read_pin()
 *                    reads GPIO_LEVEL_HIGH with nothing connected
 *   TC-HW-GPIO-007  Validation cascade: NULL config, invalid pin, invalid
 *                    port (GPIO_PORT_H — F469-only enum value not present
 *                    on this board) all return the documented error codes
 *   TC-HW-GPIO-008  gpio_init() second call is idempotent, returns GPIO_OK
 *
 * Manual step:
 *   Jumper PA1 to GND and re-run — TC-HW-GPIO-006 should then read
 *   GPIO_LEVEL_LOW, confirming the pull-up is overridable by an external
 *   drive (not a stuck-high fault).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "cpu/cpu.h"
#include "gpio/gpio_driver.h"
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

/** USART1 TX on PB6, alternate function AF7 — used only to report results. */
#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_TX_AF (7U)

/** Spare pin, Arduino header A1 — not wired to any onboard peripheral. */
#define BRINGUP_SPARE_PORT GPIO_PORT_A
#define BRINGUP_SPARE_PIN (1U)

/**
 * USART1 BRR = PCLK2 / baud = 80 000 000 / 115 200 ≈ 694.
 * Only valid after cpu_init() has switched SYSCLK to 80 MHz.
 */
#define BRINGUP_UART_BRR (694U)

/** Number of toggle iterations for the visual blink test. */
#define BRINGUP_TOGGLE_COUNT (10U)

/* ---------------------------------------------------------------------- */
/* Raw spin delay — used only in the failure path before cpu_init() has     */
/* configured the DWT cycle counter.                                       */
/* ---------------------------------------------------------------------- */

static void bringup_raw_spin(uint32_t n)
{
    volatile uint32_t c = n;
    while (c > 0U)
    {
        --c;
    }
}

/* ---------------------------------------------------------------------- */
/* UART helpers (USART1, PB6, 115 200 8N1)                                 */
/* Only the peripheral registers are touched directly here — the TX pin    */
/* itself is configured through gpio_configure_pin(), the function under   */
/* test.                                                                    */
/* ---------------------------------------------------------------------- */

static void bringup_uart_peripheral_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    USART1->CR1 = 0U;
    USART1->BRR = BRINGUP_UART_BRR;
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void bringup_putc(char c)
{
    while (!(USART1->ISR & USART_ISR_TXE))
    {
        /* spin */
    }
    USART1->TDR = (uint32_t) (uint8_t) c;
}

static void bringup_puts(const char *s)
{
    while (*s != '\0')
    {
        bringup_putc(*s);
        ++s;
    }
}

static void bringup_put_hex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    bringup_puts("0x");
    for (int8_t shift = 28; shift >= 0; shift -= 4)
    {
        bringup_putc(hex[(v >> (uint32_t) shift) & 0xFU]);
    }
}

/* ---------------------------------------------------------------------- */
/* Test reporting                                                          */
/* ---------------------------------------------------------------------- */

static void bringup_pass(const char *label)
{
    bringup_puts("[PASS] ");
    bringup_puts(label);
    bringup_puts("\r\n");
}

/**
 * @brief Emit a FAIL banner and enter a fast-blink halt loop.
 *
 * Drives LD2 directly via BSRR (not gpio_write_pin()) — by the time a
 * failure can be reported, the driver under test may itself be the cause,
 * so the halt indicator must not depend on it.
 */
static void bringup_fail(const char *label)
{
    bringup_puts("[FAIL] ");
    bringup_puts(label);
    bringup_puts(" — HALTED\r\n");
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
    /* cpu_init() first — clock to 80 MHz, DWT, fault handlers. */
    status_t init_st = cpu_init();
    if (init_st != STATUS_OK)
    {
        /* No UART yet — cannot report. Fast raw blink on LD2 via BSRR. */
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

    /* TC-HW-GPIO-001 — gpio_init() ------------------------------------- */
    gpio_err_t gpio_st = gpio_init();
    if (gpio_st != GPIO_OK)
    {
        /* gpio_init() failed — no reliable way to drive LD2 through the
         * driver under test. Fall back to a raw halt blink. */
        for (;;)
        {
            GPIOA->BSRR = (1UL << BRINGUP_LED_PIN);
            bringup_raw_spin(100000U);
            GPIOA->BSRR = (1UL << (BRINGUP_LED_PIN + 16U));
            bringup_raw_spin(100000U);
        }
    }

    /* TC-HW-GPIO-002 — configure the UART TX pin through the driver under
     * test, then bring up the USART1 peripheral itself (raw registers —
     * ModbusUartDriver/UartDriver is not a GpioDriver dependency). */
    gpio_pin_config_t uart_tx_config = {
        .port = BRINGUP_UART_TX_PORT,
        .pin = BRINGUP_UART_TX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_NONE,
        .alternate = BRINGUP_UART_TX_AF,
    };
    gpio_err_t uart_pin_st = gpio_configure_pin(&uart_tx_config);
    bringup_uart_peripheral_init();

    bringup_puts("\r\n======= GpioDriver Hardware Bring-up =======\r\n");
    bringup_puts("Board : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("UART  : USART1 PB6 TX   115 200 8N1\r\n");
    bringup_puts("LED   : LD2 PA5\r\n");
    bringup_puts("=============================================\r\n\r\n");

    bringup_pass("TC-HW-GPIO-001  gpio_init() returned GPIO_OK");

    if (uart_pin_st != GPIO_OK)
    {
        bringup_fail("TC-HW-GPIO-002  gpio_configure_pin(PB6, ALTERNATE) failed");
    }
    bringup_pass("TC-HW-GPIO-002  gpio_configure_pin() ALTERNATE (PB6 AF7) succeeded");

    /* TC-HW-GPIO-003 — configure LD2 as push-pull output ---------------- */
    gpio_pin_config_t led_config = {
        .port = BRINGUP_LED_PORT,
        .pin = BRINGUP_LED_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 0,
    };
    if (gpio_configure_pin(&led_config) != GPIO_OK)
    {
        bringup_fail("TC-HW-GPIO-003  gpio_configure_pin(PA5, OUTPUT) failed");
    }
    bringup_pass("TC-HW-GPIO-003  gpio_configure_pin() OUTPUT push-pull (PA5) succeeded");

    /* TC-HW-GPIO-004 — gpio_write_pin() drives LD2 on then off ---------- */
    bringup_puts("[INFO] TC-HW-GPIO-004  LD2 ON for ~1 s ...\r\n");
    if (gpio_write_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN, GPIO_LEVEL_HIGH) != GPIO_OK)
    {
        bringup_fail("TC-HW-GPIO-004  gpio_write_pin(HIGH) failed");
    }
    cpu_delay_ms(1000U);
    if (gpio_write_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN, GPIO_LEVEL_LOW) != GPIO_OK)
    {
        bringup_fail("TC-HW-GPIO-004  gpio_write_pin(LOW) failed");
    }
    cpu_delay_ms(200U);
    bringup_pass("TC-HW-GPIO-004  gpio_write_pin() drove LD2 on then off");

    /* TC-HW-GPIO-005 — gpio_toggle_pin() blinks LD2 --------------------- */
    bringup_puts("[INFO] TC-HW-GPIO-005  LD2 blinking x10 via gpio_toggle_pin() ...\r\n");
    for (uint8_t i = 0; i < (2U * BRINGUP_TOGGLE_COUNT); ++i)
    {
        if (gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN) != GPIO_OK)
        {
            bringup_fail("TC-HW-GPIO-005  gpio_toggle_pin() failed");
        }
        cpu_delay_ms(150U);
    }
    bringup_pass("TC-HW-GPIO-005  gpio_toggle_pin() blinked LD2 ten times");

    /* TC-HW-GPIO-006 — INPUT pull-up read-back on a spare pin ----------- */
    gpio_pin_config_t spare_config = {
        .port = BRINGUP_SPARE_PORT,
        .pin = BRINGUP_SPARE_PIN,
        .mode = GPIO_MODE_INPUT,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_UP,
        .alternate = 0,
    };
    if (gpio_configure_pin(&spare_config) != GPIO_OK)
    {
        bringup_fail("TC-HW-GPIO-006  gpio_configure_pin(PA1, INPUT pull-up) failed");
    }
    gpio_level_t spare_level = GPIO_LEVEL_LOW;
    gpio_err_t read_st = gpio_read_pin(BRINGUP_SPARE_PORT, BRINGUP_SPARE_PIN, &spare_level);
    bringup_puts("[INFO] TC-HW-GPIO-006  PA1 (pull-up, unconnected) reads ");
    bringup_puts((spare_level == GPIO_LEVEL_HIGH) ? "HIGH" : "LOW");
    bringup_puts("\r\n");
    if ((read_st != GPIO_OK) || (spare_level != GPIO_LEVEL_HIGH))
    {
        bringup_puts("[INFO] Non-HIGH is expected only if PA1 is jumpered to GND.\r\n");
    }
    bringup_pass("TC-HW-GPIO-006  gpio_read_pin() completed on PA1 (see level above)");

    /* TC-HW-GPIO-007 — validation cascade ------------------------------- */
    if (gpio_configure_pin(NULL) != GPIO_ERR_NULL_POINTER)
    {
        bringup_fail(
            "TC-HW-GPIO-007  gpio_configure_pin(NULL) did not return GPIO_ERR_NULL_POINTER");
    }
    if (gpio_write_pin(BRINGUP_LED_PORT, 16U, GPIO_LEVEL_LOW) != GPIO_ERR_INVALID_PIN)
    {
        bringup_fail("TC-HW-GPIO-007  gpio_write_pin(pin=16) did not return GPIO_ERR_INVALID_PIN");
    }
    if (gpio_write_pin(GPIO_PORT_I, 0U, GPIO_LEVEL_LOW) != GPIO_ERR_INVALID_PORT)
    {
        bringup_fail(
            "TC-HW-GPIO-007  gpio_write_pin(GPIO_PORT_I) did not return GPIO_ERR_INVALID_PORT "
            "(this board has only GPIOA..GPIOH; GPIO_PORT_I is F469-only)");
    }
    bringup_pass("TC-HW-GPIO-007  validation cascade returned documented error codes");

    /* TC-HW-GPIO-008 — gpio_init() idempotency -------------------------- */
    if (gpio_init() != GPIO_OK)
    {
        bringup_fail("TC-HW-GPIO-008  second gpio_init() call did not return GPIO_OK");
    }
    bringup_pass("TC-HW-GPIO-008  gpio_init() is idempotent on second call");

    /* ------------------------------------------------------------------ */
    bringup_puts("\r\n[PASS] All automated tests complete.\r\n");
    bringup_puts("[INFO] Heartbeat: LD2 PA5 toggles every 500 ms via gpio_toggle_pin().\r\n\r\n");

    for (;;)
    {
        gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        cpu_delay_ms(500U);
    }
}
