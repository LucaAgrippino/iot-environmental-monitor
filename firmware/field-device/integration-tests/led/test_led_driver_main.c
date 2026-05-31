/**
 * @file test_led_driver_main.c
 * @brief Integration test for LedDriver — manual verification on target hardware.
 *
 * Run on the STM32F469I-DISCO board after flashing. Observe LEDs directly and
 * check SWO/UART log output. No automated pass/fail — visual and logical
 * inspection required.
 *
 * ---
 * Visual checklist (tick off on the board):
 *
 *   [ ] After reset: both LEDs are OFF.
 *         (led_init drives GPIO_LEVEL_HIGH for active-low LEDs)
 *   [ ] Phase 1 — led_on(LED_GREEN):  LD3 (green, PG6) turns ON.
 *   [ ] Phase 1 — led_on(LED_RED):   LD4 (red,   PD5) turns ON.
 *   [ ] Phase 2 — led_off(LED_GREEN): LD3 turns OFF.
 *   [ ] Phase 2 — led_off(LED_RED):  LD4 turns OFF.
 *   [ ] Phase 3 — toggle loop (500 ms interval for 10 iterations):
 *         - LD3 flashes green.
 *         - LD4 flashes red.
 *         - Both LEDs alternate in phase (one on, one off) throughout.
 *   [ ] After test task completes: LED_GREEN blinks slowly (1 Hz) as
 *         a heartbeat indicating the scheduler is running.
 *
 * ---
 * Init ordering (see docs/lld/drivers/led-driver.md §4.2):
 *   1. system_clock_init()
 *   2. gpio_init()          — required before led_init()
 *   3. led_init(k_fd_led_pins, LED_COUNT)
 * ---
 */

#include <stdint.h>

#include "FreeRTOS.h"
#include "gpio/gpio_driver.h"
#include "led/led_driver.h"
#include "task.h"

#include "system_clock.h"

/* ===================================================================== */
/* Board pin table (Field Device — STM32F469I-DISCO)                    */
/* ===================================================================== */

static const led_pin_t k_fd_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_G, .pin =  6U, .active_high = false, .fitted = true},
    [LED_RED]   = {.port = GPIO_PORT_D, .pin =  5U, .active_high = false, .fitted = true},
};

/* ===================================================================== */
/* Constants                                                             */
/* ===================================================================== */

#define BLINK_HALF_PERIOD_MS (500U)
#define BLINK_ITERATIONS     (10U)
#define HEARTBEAT_PERIOD_MS  (1000U)

#define PERIODIC_STACK_WORDS (configMINIMAL_STACK_SIZE)
#define PERIODIC_PRIORITY    (tskIDLE_PRIORITY + 1U)

/* ===================================================================== */
/* Test task                                                             */
/* ===================================================================== */

static StaticTask_t s_periodic_tcb;
static StackType_t  s_periodic_stack[PERIODIC_STACK_WORDS];

static void led_test_task(void *arg)
{
    (void) arg;

    /* Phase 1: turn both LEDs on. */
    (void) led_on(LED_GREEN);
    (void) led_on(LED_RED);
    vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));

    /* Phase 2: turn both LEDs off. */
    (void) led_off(LED_GREEN);
    (void) led_off(LED_RED);
    vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));

    /* Phase 3: alternate toggling. */
    for (uint8_t i = 0U; i < BLINK_ITERATIONS; i++)
    {
        (void) led_toggle(LED_GREEN);
        (void) led_toggle(LED_RED);
        vTaskDelay(pdMS_TO_TICKS(BLINK_HALF_PERIOD_MS));
    }

    /* Heartbeat: blink LED_GREEN at 1 Hz to signal scheduler is alive. */
    for (;;)
    {
        (void) led_toggle(LED_GREEN);
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}

/* ===================================================================== */
/* Entry point                                                           */
/* ===================================================================== */

int main(void)
{
    system_clock_init();

    /* Init ordering per led-driver.md §4.2. */
    (void) gpio_init();
    (void) led_init(k_fd_led_pins, LED_COUNT);

    (void) xTaskCreateStatic(led_test_task, "led_test", PERIODIC_STACK_WORDS, NULL,
                             PERIODIC_PRIORITY, s_periodic_stack, &s_periodic_tcb);
    vTaskStartScheduler();

    /* Should not reach here. */
    for (;;)
    {
    }
}
