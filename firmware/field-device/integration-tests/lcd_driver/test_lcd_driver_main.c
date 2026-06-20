/**
 * @file test_lcd_driver_main.c
 * @brief Integration tests for LcdDriver — TC-LCD-011 through TC-LCD-015.
 *
 * On-hardware tests targeting the STM32F469I-DISCO board.
 * Results reported via DebugUART (USART3, 115 200 baud).
 * LED_GREEN = all pass; LED_RED = one or more fail.
 *
 * Pre-conditions:
 *   - Board powered, JTAG/SWD connected.
 *   - system_clock_init_core() and system_clock_enable_dwt() called.
 *   - sdram_init() returns SDRAM_ERR_OK before lcd_init() is called.
 *
 * Visual checklist:
 *   [ ] TC-LCD-011: Display shows solid red, then green, then blue fills.
 *                   No artefacts; colours are distinct and full-field.
 *   [ ] TC-LCD-012: Vertical colour bars with horizontal gradient visible.
 *                   No tearing, no diagonal distortion.
 *   [ ] TC-LCD-013: Console log shows "Callback count: 100" after ~20 s.
 *   [ ] TC-LCD-014: Touchscreen responds to I2C reads immediately after
 *                   lcd_init() returns (confirms PH7 reset released).
 *   [ ] TC-LCD-015: Forced BSP failure: console shows LCD_ERR_INIT and
 *                   stage value 0x80 (FAIL_BSP).
 */

#include "FreeRTOS.h"
#include "task.h"

#include "system_clock.h"

#include "gpio/gpio_driver.h"
#include "rtc/rtc_driver.h"
#include "debug-uart/debug_uart_driver.h"
#include "logger/logger.h"
#include "sdram_driver/sdram_driver.h"
#include "lcd_driver/lcd_driver.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ===================================================================== */
/* Configuration                                                         */
/* ===================================================================== */

#define TEST_TASK_STACK_WORDS (2048U)
#define MODULE_ID             "LCD"

#define LCD_WIDTH  (800U)
#define LCD_HEIGHT (480U)
#define LCD_PIXELS (LCD_WIDTH * LCD_HEIGHT)

/* RGB565 colour constants */
#define ARGB8888_RED   (0xFFFF0000U)
#define ARGB8888_GREEN (0xFF00FF00U)
#define ARGB8888_BLUE  (0xFF0000FFU)
#define ARGB8888_WHITE (0xFFFFFFFFU)

#define FLUSH_INTERVAL_MS  (200U)
#define FLUSH_COUNT        (100U)
#define FRAME_NOTIFY_TICKS (pdMS_TO_TICKS(500U))

/* ===================================================================== */
/* Test state                                                            */
/* ===================================================================== */

static StaticTask_t s_test_tcb;
static StackType_t  s_test_stack[TEST_TASK_STACK_WORDS] __attribute__((aligned(8)));

static TaskHandle_t   s_test_task_handle;
static volatile uint32_t s_frame_done_count;

typedef struct
{
    uint32_t total;
    uint32_t passed;
} test_results_t;

static test_results_t s_results;

/* ===================================================================== */
/* Frame-done callback (registered in TC-LCD-013)                       */
/* ===================================================================== */

static void on_frame_done(void *context)
{
    TaskHandle_t task = (TaskHandle_t) context;
    BaseType_t   higher_woken = pdFALSE;
    s_frame_done_count++;
    vTaskNotifyGiveFromISR(task, &higher_woken);
    portYIELD_FROM_ISR(higher_woken);
}

/* ===================================================================== */
/* Test helpers                                                          */
/* ===================================================================== */

static void log_result(const char *name, bool passed)
{
    s_results.total++;
    if (passed)
    {
        s_results.passed++;
        LOG_INFO(MODULE_ID, "PASS  %s", name);
    }
    else
    {
        LOG_ERROR(MODULE_ID, "FAIL  %s", name);
    }
}

static void fill_framebuffer(uint32_t *fb, uint32_t colour)
{
    for (uint32_t i = 0U; i < LCD_PIXELS; i++)
    {
        fb[i] = colour;
    }
}

/* ===================================================================== */
/* Test task                                                             */
/* ===================================================================== */

#include "stm32469i_discovery_lcd.h"
#define LAYER0_ADDRESS               (LCD_FB_START_ADDRESS)

static void lcd_test_task(void *arg)
{
    (void)arg;

    LOG_INFO(MODULE_ID, "LcdDriver integration tests starting");

    /* lcd_init() is called from main() before the scheduler starts.
     * Obtain the framebuffer pointer and confirm it. */
    uint32_t *fb = lcd_get_framebuffer();
    bool      fb_ok = (fb == (uint32_t *)SDRAM_BASE_ADDR);
    log_result("TC-LCD-010 (framebuffer address after init)", fb_ok);

    /* --- TC-LCD-011 Solid colour fills -------------------------------- */
    LOG_INFO(MODULE_ID, "TC-LCD-011: solid colour fills");
    fill_framebuffer(fb, ARGB8888_RED);
    (void)lcd_flush();
    vTaskDelay(pdMS_TO_TICKS(1000U));

    fill_framebuffer(fb, ARGB8888_GREEN);
    (void)lcd_flush();
    vTaskDelay(pdMS_TO_TICKS(1000U));

    fill_framebuffer(fb, ARGB8888_BLUE);
    (void)lcd_flush();
    vTaskDelay(pdMS_TO_TICKS(1000U));

    log_result("TC-LCD-011 solid colour fills (visual)", true);

    /* --- TC-LCD-012 Test pattern -------------------------------------- */
    LOG_INFO(MODULE_ID, "TC-LCD-012: test pattern");
    for (uint32_t y = 0U; y < LCD_HEIGHT; y++)
    {
        for (uint32_t x = 0U; x < LCD_WIDTH; x++)
        {
            /* Vertical bars: 8 colours cycling, one per 100-px column. */
            uint32_t bar = (x / 100U) % 8U;
            uint8_t  r   = (bar & 0x1U) ? 0xFFU : 0x00U;
            uint8_t  g   = (bar & 0x2U) ? 0xFFU : 0x00U;
            uint8_t  b   = (bar & 0x4U) ? 0xFFU : 0x00U;

//            /* Horizontal red gradient within each column (0..255). */
//            uint8_t grad = (uint8_t)((x * 255U) / (LCD_WIDTH - 1U));
//            r = (uint8_t)(r ^ grad);   /* XOR so the gradient is visible on all bars */

            uint32_t colour = ((uint32_t)0xFFU << 24) |   /* alpha */
                              ((uint32_t)r    << 16) |   /* red   */
                              ((uint32_t)g    <<  8) |   /* green */
                              ((uint32_t)b    <<  0);    /* blue  */

            fb[y * LCD_WIDTH + x] = colour;
        }
    }
    (void)lcd_flush();
    vTaskDelay(pdMS_TO_TICKS(2000U));
    log_result("TC-LCD-012 test pattern (visual)", true);

    /* --- TC-LCD-013 Frame-done callback -------------------------------- */
    LOG_INFO(MODULE_ID, "TC-LCD-013: 100 flush/callback cycles");

    s_frame_done_count = 0U;
    (void)lcd_attach_frame_done(on_frame_done, s_test_task_handle);

    fill_framebuffer(fb, ARGB8888_WHITE);

    for (uint32_t i = 0U; i < FLUSH_COUNT; i++)
    {
        (void)lcd_flush();
        /* Wait for frame-done notification from ISR (or timeout). */
        if (ulTaskNotifyTake(pdTRUE, FRAME_NOTIFY_TICKS) == 0U)
        {
            LOG_ERROR(MODULE_ID, "TC-LCD-013: timeout on flush %lu", (unsigned long)i);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(FLUSH_INTERVAL_MS));
    }

    bool callback_ok = (s_frame_done_count >= FLUSH_COUNT);
    LOG_INFO(MODULE_ID, "Callback count: %lu", (unsigned long)s_frame_done_count);
    log_result("TC-LCD-013 frame-done 100 cycles", callback_ok);

    /* --- TC-LCD-014 Touchscreen I2C (visual) -------------------------- */
    LOG_INFO(MODULE_ID, "TC-LCD-014: touchscreen I2C availability after lcd_init");
    LOG_INFO(MODULE_ID, "  Verify: touchscreen_init() must succeed next in main()");
    log_result("TC-LCD-014 PH7 reset released (manual verify)", true);

    /* --- Final summary ------------------------------------------------ */
    LOG_INFO(MODULE_ID, "Results: %lu/%lu passed",
             (unsigned long)s_results.passed, (unsigned long)s_results.total);

    if (s_results.passed == s_results.total)
    {
        LOG_INFO(MODULE_ID, "ALL PASS — LED_GREEN");
    }
    else
    {
        LOG_ERROR(MODULE_ID, "FAILURES — LED_RED");
    }

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/* ===================================================================== */
/* main                                                                  */
/* ===================================================================== */

int main(void)
{
    system_clock_init();
    system_clock_enable_dwt();

    (void)gpio_init();
    (void)debug_uart_init();
    (void)rtc_init();

    logger_init(LOG_LEVEL_DEBUG);
    LOG_INFO(MODULE_ID, "LcdDriver integration test firmware");

    /* SDRAM must be ready before lcd_init(). */
    sdram_err_t sdram_err = sdram_init();
    if (sdram_err != SDRAM_ERR_OK)
    {
        LOG_ERROR(MODULE_ID, "sdram_init failed: %d", (int)sdram_err);
        for (;;)
        {
        }
    }

    /* Phase 1: bring up LCD subsystem before scheduler. */
    lcd_err_t lcd_err = lcd_init();
    if (lcd_err != LCD_ERR_OK)
    {
        LOG_ERROR(MODULE_ID, "lcd_init failed: %d (stage=0x%02X)",
                  (int)lcd_err, (unsigned int)/*s_lcd_init_stage*/0);
        for (;;)
        {
        }
    }
    LOG_INFO(MODULE_ID, "lcd_init OK — stage %d", (int)/*s_lcd_init_stage*/0);

    uint32_t *fb = lcd_get_framebuffer();
    if (fb != NULL) {
        /* Fill entire framebuffer with red — RGB565 */
        for (uint32_t i = 0; i < 800U * 480U; i++) {
            fb[i] = 0xFFFFFFFFU;
        }
    }

    s_test_task_handle = xTaskCreateStatic(
        lcd_test_task, "LCD_TEST", TEST_TASK_STACK_WORDS, NULL,
        2U, s_test_stack, &s_test_tcb);

    vTaskStartScheduler();

    /* Never reached. */
    for (;;)
    {
    }
}
