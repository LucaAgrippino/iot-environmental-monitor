/**
 * @file test_touchscreen_driver_main.c
 * @brief Integration tests for TouchscreenDriver -- TC-TS-015 through TC-TS-017.
 *
 * On-hardware tests that must run on the STM32F469I-DISCO board.
 * Build and flash with the integration test firmware image.
 *
 * Pre-conditions:
 *   - Board powered, JTAG/SWD connected.
 *   - lcd_init() completes successfully (PH7 released).
 *   - touchscreen_init() completes successfully.
 *   - touchscreen_attach_irq() called from task prologue.
 *
 * Results are reported via DebugUART (USART3 at 115200 baud).
 * LED_GREEN = all pass; LED_RED = one or more fail.
 */

#include "FreeRTOS.h"
#include "task.h"

#include "system_clock.h"

#include "gpio/gpio_driver.h"
#include "rtc/rtc_driver.h"
#include "debug-uart/debug_uart_driver.h"
#include "logger/logger.h"
#include "touchscreen_driver/touchscreen_driver.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>

/* ===================================================================== */
/* Configuration                                                         */
/* ===================================================================== */

#define TEST_TASK_STACK_WORDS (256U)
#define TOUCH_WAIT_MS (5000U)
#define TOUCH_IRQ_TIMEOUT_MS (10000U)
#define MODULE_ID "TS"

/* ===================================================================== */
/* Test state                                                            */
/* ===================================================================== */

static StaticTask_t s_test_tcb;
static StackType_t s_test_stack[TEST_TASK_STACK_WORDS] __attribute__((aligned(8)));
static volatile uint32_t g_irq_count;
static TaskHandle_t g_test_task_handle;

typedef struct
{
    uint32_t total;
    uint32_t passed;
} test_results_t;

static test_results_t g_results;

/* ===================================================================== */
/* IRQ callback                                                          */
/* ===================================================================== */

static void on_touch_irq(void *context)
{
    TaskHandle_t task = (TaskHandle_t) context;
    BaseType_t higher_woken = pdFALSE;
    g_irq_count++;
    vTaskNotifyGiveFromISR(task, &higher_woken);
    portYIELD_FROM_ISR(higher_woken);
}

/* ===================================================================== */
/* Test helpers                                                          */
/* ===================================================================== */

static void log_result(const char *name, bool passed)
{
    g_results.total++;
    if (passed)
    {
        g_results.passed++;
    }
    LOG_INFO(MODULE_ID,"%s: %s", name, passed ? "PASS" : "FAIL");
}

/* ===================================================================== */
/* TC-TS-015: touchscreen_init() succeeds on real hardware             */
/* ===================================================================== */
/* Verifies that FT6x06 responds on I2C after PH7 is released by       */
/* lcd_init(). A return of TS_ERR_OK proves DEV_MODE was readable.     */

static void tc_ts_015_init_on_hardware(void)
{
    ts_err_t err = touchscreen_init();
    log_result("TC-TS-015 touchscreen_init on hardware", err == TS_ERR_OK);
}

/* ===================================================================== */
/* TC-TS-016: EXTI IRQ fires on physical touch                         */
/* ===================================================================== */
/* Waits up to TOUCH_IRQ_TIMEOUT_MS for an EXTI9_5 IRQ from the FT6x06 */
/* after attaching the IRQ callback. The operator must touch the screen */
/* within the timeout window.                                           */

static void tc_ts_016_irq_on_touch(TaskHandle_t this_task)
{
    g_irq_count = 0;
    g_test_task_handle = this_task;
    touchscreen_attach_irq(on_touch_irq, (void *) this_task);

    LOG_INFO(MODULE_ID,"TC-TS-016: touch the screen within %u ms ...", TOUCH_IRQ_TIMEOUT_MS);
    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TOUCH_IRQ_TIMEOUT_MS));
    log_result("TC-TS-016 EXTI IRQ fires on physical touch", notified > 0U);
}

/* ===================================================================== */
/* TC-TS-017: touchscreen_read() returns valid coordinates on touch     */
/* ===================================================================== */
/* After TC-TS-016 has confirmed the IRQ fires, this test reads the     */
/* touch coordinates and validates they are within display bounds.      */
/* Operator must touch the screen once during the wait window.          */

static void tc_ts_017_read_coordinates(void)
{
    LOG_INFO(MODULE_ID,"TC-TS-017: touch the screen within %u ms ...", TOUCH_WAIT_MS);
    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TOUCH_WAIT_MS));

    if (notified == 0U) {
        log_result("TC-TS-017 read coordinates -- no touch received", false);
        return;
    }

    ts_touch_t touch;
    ts_err_t   err = touchscreen_read(&touch);
    bool ok = (err == TS_ERR_OK) &&
              (touch.x < 800U) &&
              (touch.y < 480U);
    if (err == TS_ERR_OK) {
        LOG_INFO(MODULE_ID,"  touch=(%u,%u) event=%u", touch.x, touch.y, (unsigned)touch.event);
    }
    log_result("TC-TS-017 read coordinates in bounds", ok);
}

/* ===================================================================== */
/* Test task and FreeRTOS entry                                          */
/* ===================================================================== */

static void test_task(void *params)
{
    (void) params;
    TaskHandle_t this_task = xTaskGetCurrentTaskHandle();

    LOG_INFO(MODULE_ID,"TouchscreenDriver integration tests");
    LOG_INFO(MODULE_ID,"===================================");

    tc_ts_015_init_on_hardware();
    tc_ts_016_irq_on_touch(this_task);
    tc_ts_017_read_coordinates();

    LOG_INFO(MODULE_ID,"===================================");
    LOG_INFO(MODULE_ID,"Result: %lu/%lu passed", g_results.passed, g_results.total);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

void test_touchscreen_driver_run(void)
{
    /* Pre-condition: lcd_init() must have been called before this    */
    /* function (PH7 released so FT6x06 is out of reset).             */
    xTaskCreateStatic(test_task, "ts_test", TEST_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 1U,
                      s_test_stack, &s_test_tcb);
}


/* ===================================================================== */
/* Entry point                                                            */
/* ===================================================================== */

int main(void)
{
    system_clock_init();

    gpio_init();
    rtc_init();
    debug_uart_init();
    logger_init(LOG_LEVEL_DEBUG);

    test_touchscreen_driver_run();

    vTaskStartScheduler();

    for (;;) { }   /* unreachable */
}
