/**
 * @file main_test_exti.c
 * @brief Hardware bring-up test for ExtiDriver (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default. To activate:
 *   1. Copy this file into Src/ (or add integration-tests/exti/ as a
 *      source path in CubeIDE).
 *   2. In CubeIDE, right-click Src/main.c -> Properties ->
 *      C/C++ Build -> check "Exclude resource from build".
 *   3. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   4. Flash and run with a debugger attached (J-Link or ST-LINK).
 *
 * Hardware outputs:
 *   USART1 TX  PB6  115 200 8N1  Human-readable test results
 *   LD2 green  PA5               Toggled once per confirmed EXTI1 interrupt
 *   PA1        Arduino header A1  Spare pin (per gpio_driver.md bring-up
 *                                 note) — configured as EXTI1 input,
 *                                 pull-down. Jumper to 3V3 to trigger a
 *                                 rising edge.
 *
 * Automated test sequence:
 *   TC-HW-EXTI-001  exti_configure(1, PORT_A, RISING) returns EXTI_ERR_OK
 *   TC-HW-EXTI-002  exti_configure() on the same line again returns
 *                    EXTI_ERR_CONFLICT
 *   TC-HW-EXTI-003  exti_enable(1, priority) returns EXTI_ERR_OK
 *   TC-HW-EXTI-004  exti_configure(16, ...) returns EXTI_ERR_INVALID_ARG
 *   TC-HW-EXTI-005  exti_enable() on an unconfigured line (2) returns
 *                    EXTI_ERR_NOT_CONFIGURED
 *
 * Manual step:
 *   Jumper PA1 to 3V3 momentarily (rising edge) — the EXTI1_IRQHandler
 *   below calls exti_clear_pending(1) then increments a counter; the main
 *   loop reports each new count over UART and toggles LD2 once per event.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "cpu/cpu.h"
#include "gpio/gpio_driver.h"
#include "exti_driver.h"
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
#define BRINGUP_UART_TX_AF (7U)

/** Spare pin, Arduino header A1 — used here as the EXTI1 test stimulus. */
#define BRINGUP_EXTI_PORT GPIO_PORT_A
#define BRINGUP_EXTI_PIN (1U)
#define BRINGUP_EXTI_LINE (1U)
#define BRINGUP_EXTI_NVIC_PRIORITY (6U)

/** USART1 BRR = PCLK2 / baud = 80 000 000 / 115 200. Valid after cpu_init(). */
#define BRINGUP_UART_BRR (694U)

static volatile uint32_t g_exti_isr_count;

/* ---------------------------------------------------------------------- */
/* EXTI1 ISR — overrides the weak default handler from the startup file.  */
/* ---------------------------------------------------------------------- */

void EXTI1_IRQHandler(void)
{
    exti_clear_pending(BRINGUP_EXTI_LINE);
    g_exti_isr_count++;
}

/* ---------------------------------------------------------------------- */
/* Raw spin delay — pre-cpu_init() failure path only.                      */
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

static void bringup_put_uint32(uint32_t v)
{
    char digits[10];
    uint8_t count = 0U;

    if (v == 0U)
    {
        bringup_putc('0');
        return;
    }
    while (v > 0U)
    {
        digits[count] = (char) ('0' + (v % 10U));
        v /= 10U;
        ++count;
    }
    while (count > 0U)
    {
        --count;
        bringup_putc(digits[count]);
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

    (void) gpio_init();

    gpio_pin_config_t uart_tx_config = {
        .port = BRINGUP_UART_TX_PORT,
        .pin = BRINGUP_UART_TX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_NONE,
        .alternate = BRINGUP_UART_TX_AF,
    };
    (void) gpio_configure_pin(&uart_tx_config);
    bringup_uart_peripheral_init();

    gpio_pin_config_t led_config = {
        .port = BRINGUP_LED_PORT,
        .pin = BRINGUP_LED_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 0,
    };
    (void) gpio_configure_pin(&led_config);

    gpio_pin_config_t exti_pin_config = {
        .port = BRINGUP_EXTI_PORT,
        .pin = BRINGUP_EXTI_PIN,
        .mode = GPIO_MODE_INPUT,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_DOWN,
        .alternate = 0,
    };
    (void) gpio_configure_pin(&exti_pin_config);

    bringup_puts("\r\n======= ExtiDriver Hardware Bring-up =======\r\n");
    bringup_puts("Board : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("UART  : USART1 PB6 TX   115 200 8N1\r\n");
    bringup_puts("LED   : LD2 PA5\r\n");
    bringup_puts("EXTI  : PA1 -> EXTI1, pull-down, rising edge\r\n");
    bringup_puts("=============================================\r\n\r\n");

    /* TC-HW-EXTI-001 ----------------------------------------------------- */
    if (exti_configure(BRINGUP_EXTI_LINE, EXTI_PORT_A, EXTI_EDGE_RISING) != EXTI_ERR_OK)
    {
        bringup_fail("TC-HW-EXTI-001  exti_configure(PA1, EXTI1, RISING) failed");
    }
    bringup_pass("TC-HW-EXTI-001  exti_configure() returned EXTI_ERR_OK");

    /* TC-HW-EXTI-002 — conflict detection --------------------------------- */
    if (exti_configure(BRINGUP_EXTI_LINE, EXTI_PORT_B, EXTI_EDGE_FALLING) != EXTI_ERR_CONFLICT)
    {
        bringup_fail("TC-HW-EXTI-002  reconfiguring EXTI1 did not return EXTI_ERR_CONFLICT");
    }
    bringup_pass("TC-HW-EXTI-002  duplicate exti_configure() returned EXTI_ERR_CONFLICT");

    /* TC-HW-EXTI-003 — enable the interrupt ------------------------------- */
    if (exti_enable(BRINGUP_EXTI_LINE, BRINGUP_EXTI_NVIC_PRIORITY) != EXTI_ERR_OK)
    {
        bringup_fail("TC-HW-EXTI-003  exti_enable(EXTI1) failed");
    }
    bringup_pass("TC-HW-EXTI-003  exti_enable() returned EXTI_ERR_OK");

    /* TC-HW-EXTI-004 — invalid line ---------------------------------------- */
    if (exti_configure(16U, EXTI_PORT_A, EXTI_EDGE_RISING) != EXTI_ERR_INVALID_ARG)
    {
        bringup_fail("TC-HW-EXTI-004  exti_configure(line=16) did not return EXTI_ERR_INVALID_ARG");
    }
    bringup_pass("TC-HW-EXTI-004  exti_configure(line=16) returned EXTI_ERR_INVALID_ARG");

    /* TC-HW-EXTI-005 — enable before configure on a fresh line ------------- */
    if (exti_enable(2U, BRINGUP_EXTI_NVIC_PRIORITY) != EXTI_ERR_NOT_CONFIGURED)
    {
        bringup_fail("TC-HW-EXTI-005  exti_enable(line=2) did not return EXTI_ERR_NOT_CONFIGURED");
    }
    bringup_pass(
        "TC-HW-EXTI-005  exti_enable() on unconfigured line returned EXTI_ERR_NOT_CONFIGURED");

    bringup_puts("\r\n[PASS] All automated tests complete.\r\n");
    bringup_puts("[INFO] Jumper PA1 to 3V3 to trigger EXTI1 - LD2 toggles per event.\r\n\r\n");

    uint32_t last_reported = 0U;
    for (;;)
    {
        uint32_t current = g_exti_isr_count;
        if (current != last_reported)
        {
            bringup_puts("[INFO] EXTI1 ISR fired, count = ");
            bringup_put_uint32(current);
            bringup_puts("\r\n");
            (void) gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
            last_reported = current;
        }
    }
}
