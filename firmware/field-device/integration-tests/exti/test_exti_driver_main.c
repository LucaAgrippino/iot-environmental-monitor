/**
 * @file test_exti_driver_main.c
 * @brief Integration test for ExtiDriver — manual verification on target hardware.
 *
 * Run on the STM32F469I-DISCO board after flashing. Observe LD3 (green,
 * PG6, via LedDriver) directly and check SWO/UART log output. No automated
 * pass/fail for the button-press phase — visual inspection required for
 * that step; the automated checks below assert internally and halt on
 * failure (visible as a fast LD3 blink, mirroring the GpioDriver and
 * LedDriver bring-up mains).
 *
 * ---
 * Visual checklist (tick off on the board):
 *
 *   [ ] After reset: automated checks TC-HW-EXTI-001..005 all pass
 *         (no fast-blink halt on LD3).
 *   [ ] Press the USER button (PA0): LD3 toggles once per press
 *         (EXTI0 rising edge -> EXTI0_IRQHandler -> exti_clear_pending(0)).
 *
 * ---
 * Init ordering:
 *   1. system_clock_init()
 *   2. gpio_init()          — required before led_init() and exti_configure()
 *   3. led_init(k_fd_led_pins, LED_COUNT)
 *   4. gpio_configure_pin(PA0, INPUT)  — USER button, external pull, active high
 *   5. exti_configure(0, EXTI_PORT_A, EXTI_EDGE_RISING)
 *   6. exti_enable(0, priority)
 * ---
 */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "gpio/gpio_driver.h"
#include "led/led_driver.h"
#include "exti_driver.h"

#include "system_clock.h"

/* ===================================================================== */
/* Board pin table (Field Device — STM32F469I-DISCO)                    */
/* ===================================================================== */

static const led_pin_t k_fd_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_G, .pin = 6U, .active_high = false, .fitted = true},
    [LED_RED] = {.port = GPIO_PORT_D, .pin = 5U, .active_high = false, .fitted = true},
};

/* ===================================================================== */
/* Constants                                                             */
/* ===================================================================== */

/** USER pushbutton on STM32F469I-DISCO — externally driven, active high. */
#define USER_BUTTON_PORT GPIO_PORT_A
#define USER_BUTTON_PIN (0U)
#define USER_BUTTON_EXTI_LINE (0U)
#define USER_BUTTON_NVIC_PRIORITY (6U)

#define POLL_PERIOD_MS (50U)

#define PERIODIC_STACK_WORDS (configMINIMAL_STACK_SIZE)
#define PERIODIC_PRIORITY (tskIDLE_PRIORITY + 1U)

static volatile uint32_t g_exti_isr_count;

/* ===================================================================== */
/* EXTI0 ISR — overrides the weak default handler from the startup file. */
/* ===================================================================== */

void EXTI0_IRQHandler(void)
{
    exti_clear_pending(USER_BUTTON_EXTI_LINE);
    g_exti_isr_count++;
}

/* ===================================================================== */
/* Automated checks — halt (fast LD3 blink) on any unexpected result.    */
/* ===================================================================== */

static void bringup_fail_halt(void)
{
    for (;;)
    {
        (void) led_toggle(LED_GREEN);
        vTaskDelay(pdMS_TO_TICKS(100U));
    }
}

static void run_automated_checks(void)
{
    /* TC-HW-EXTI-001 */
    if (exti_configure(USER_BUTTON_EXTI_LINE, EXTI_PORT_A, EXTI_EDGE_RISING) != EXTI_ERR_OK)
    {
        bringup_fail_halt();
    }

    /* TC-HW-EXTI-002 — conflict detection */
    if (exti_configure(USER_BUTTON_EXTI_LINE, EXTI_PORT_B, EXTI_EDGE_FALLING) != EXTI_ERR_CONFLICT)
    {
        bringup_fail_halt();
    }

    /* TC-HW-EXTI-003 — enable the interrupt */
    if (exti_enable(USER_BUTTON_EXTI_LINE, USER_BUTTON_NVIC_PRIORITY) != EXTI_ERR_OK)
    {
        bringup_fail_halt();
    }

    /* TC-HW-EXTI-004 — invalid line */
    if (exti_configure(16U, EXTI_PORT_A, EXTI_EDGE_RISING) != EXTI_ERR_INVALID_ARG)
    {
        bringup_fail_halt();
    }

    /* TC-HW-EXTI-005 — enable before configure, on a fresh line */
    if (exti_enable(4U, USER_BUTTON_NVIC_PRIORITY) != EXTI_ERR_NOT_CONFIGURED)
    {
        bringup_fail_halt();
    }
}

/* ===================================================================== */
/* Test task                                                             */
/* ===================================================================== */

static StaticTask_t s_periodic_tcb;
static StackType_t s_periodic_stack[PERIODIC_STACK_WORDS];

static void exti_test_task(void *arg)
{
    (void) arg;

    uint32_t last_reported = 0U;
    for (;;)
    {
        uint32_t current = g_exti_isr_count;
        if (current != last_reported)
        {
            (void) led_toggle(LED_GREEN);
            last_reported = current;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

/* ===================================================================== */
/* Entry point                                                           */
/* ===================================================================== */

int main(void)
{
    system_clock_init();

    (void) gpio_init();
    (void) led_init(k_fd_led_pins, LED_COUNT);

    gpio_pin_config_t button_config = {
        .port = USER_BUTTON_PORT,
        .pin = USER_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 0,
    };
    (void) gpio_configure_pin(&button_config);

    run_automated_checks();

    (void) xTaskCreateStatic(exti_test_task, "exti_test", PERIODIC_STACK_WORDS, NULL,
                             PERIODIC_PRIORITY, s_periodic_stack, &s_periodic_tcb);
    vTaskStartScheduler();

    /* Should not reach here. */
    for (;;)
    {
    }
}
