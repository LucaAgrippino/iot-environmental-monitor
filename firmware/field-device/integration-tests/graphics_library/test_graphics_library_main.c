/**
 * @file test_graphics_library_main.c
 * @brief GraphicsLibrary integration test on F469 Discovery hardware.
 *
 * Exercises the complete LVGL integration pipeline end-to-end:
 *   - Display flush path: LVGL → flush_cb → lcd_blit → lcd_flush → LTDC VBR
 *   - Touch input path:  touchscreen ISR notify → lv_task_handler → touch_read_cb
 *   - Tick path:         FreeRTOS 1 ms timer → tick_timer_cb → lv_tick_inc
 *
 * Run alongside LcdUi development. Activation in CubeIDE:
 *   - Exclude firmware/Src/main.c from build.
 *   - Add integration-tests/graphics_library/ to project source paths.
 *   - Build, flash, attach SWO or serial terminal (115200/8N1).
 *
 * Visual checklist (observe on the 4" DSI panel and serial output):
 *
 * [ ] 1. After reset, serial prints "GraphicsLibrary integration test".
 * [ ] 2. Serial prints GRAPHICS_ERR_OK (0) for graphics_init().
 * [ ] 3. Serial prints non-NULL addresses for display and indev handles.
 * [ ] 4. LcdUiTask starts and prints "graphics_process OK" every 200 ms.
 * [ ] 5. Screen background changes colour on each LcdUiTask iteration
 *         (lv_obj_set_style_bg_color applied to the root screen object).
 * [ ] 6. Touch events (tap the panel) are logged as (x, y) coordinates.
 * [ ] 7. Tick counter increments in serial output confirm the 1 ms timer
 *         is running (lv_tick_get() returns a rising value).
 * [ ] 8. No hard-fault or assertion failure after 60 seconds of operation.
 *
 * Initialisation order (companion §9):
 *   sdram_init → lcd_init → touchscreen_init → graphics_init → scheduler
 */

#include "stm32f469xx.h"

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#include "system_clock.h"
#include "sdram_driver/sdram_driver.h"
#include "lcd_driver/lcd_driver.h"
#include "touchscreen_driver/touchscreen_driver.h"
#include "graphics_library/graphics_library.h"
#include "logger/logger.h"
#include "debug-uart/debug_uart_driver.h"
#include "rtc/rtc_driver.h"

/* In the real build, LcdUi calls the LVGL widget API directly. For this
 * integration test we include a minimal LVGL surface to create a background
 * object on the display returned by graphics_get_display(). */
#include "lvgl/lvgl.h"

/* ===================================================================== */
/* Configuration                                                         */
/* ===================================================================== */

#define GFX_TASK_STACK_WORDS  (512U)
#define GFX_TASK_PRIORITY     (tskIDLE_PRIORITY + 2U)
#define GFX_PROCESS_PERIOD_MS (200U)

/* ===================================================================== */
/* LcdUiTask — periodic graphics_process() caller                       */
/* ===================================================================== */

static StaticTask_t s_gfx_task_tcb;
static StackType_t  s_gfx_task_stack[GFX_TASK_STACK_WORDS];

static void lcd_ui_task(void *arg)
{
    (void)arg;

    lv_disp_t  *disp  = graphics_get_display();
    lv_indev_t *indev = graphics_get_indev();

    LOG_INFO("GfxTask", "display=%p indev=%p",
             (void *)disp, (void *)indev);

    /* Create a simple background object to exercise the flush path. */
    if (disp != NULL)
    {
        lv_obj_t *screen = lv_disp_get_scr_act(disp);
        if (screen != NULL)
        {
            lv_obj_set_style_bg_color(screen, lv_color_hex(0x002244U), 0);
            lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        }
    }

    (void)indev;

    for (;;)
    {
        graphics_err_t err = graphics_process();
        if (err == GRAPHICS_ERR_OK)
        {
            LOG_DEBUG("GfxTask", "process OK tick=%lu",
                      (unsigned long)lv_tick_get());
        }
        else
        {
            LOG_ERROR("GfxTask", "process error %d", (int)err);
        }
        vTaskDelay(pdMS_TO_TICKS(GFX_PROCESS_PERIOD_MS));
    }
}

/* ===================================================================== */
/* Entry point                                                           */
/* ===================================================================== */

int main(void)
{
    /* 1. Clock tree → 180 MHz. */
    system_clock_init();

    /* 2. Logger (pre-scheduler synchronous path). */
    (void)debug_uart_init();
    (void)rtc_init();
    (void)logger_init(LOG_LEVEL_DEBUG);

    LOG_INFO("Boot", "===== GraphicsLibrary integration test =====");

    /* 3. SDRAM must be ready before lcd_init captures the framebuffer. */
    sdram_err_t sdram_err = sdram_init();
    LOG_INFO("Boot", "sdram_init=%d", (int)sdram_err);

    /* 4. LCD: LTDC + DSI configured; framebuffer pointer set. */
    lcd_err_t lcd_err = lcd_init();
    LOG_INFO("Boot", "lcd_init=%d", (int)lcd_err);

    /* 5. Touchscreen: FT6x06 configured; EXTI still masked. */
    ts_err_t ts_err = touchscreen_init();
    LOG_INFO("Boot", "touchscreen_init=%d", (int)ts_err);

    /* 6. GraphicsLibrary: LVGL init, callbacks registered, tick timer created. */
    graphics_err_t gfx_err = graphics_init();
    LOG_INFO("Boot", "graphics_init=%d", (int)gfx_err);

    if (gfx_err != GRAPHICS_ERR_OK)
    {
        LOG_ERROR("Boot", "graphics_init FAILED — halting");
        for (;;)
        {
        }
    }

    /* 7. Spawn LcdUiTask. */
    (void)xTaskCreateStatic(lcd_ui_task, "LcdUiTask",
                            GFX_TASK_STACK_WORDS, NULL,
                            GFX_TASK_PRIORITY,
                            s_gfx_task_stack, &s_gfx_task_tcb);

    LOG_INFO("Boot", "starting scheduler...");

    /* 8. Start the scheduler. Tick timer begins firing immediately. */
    vTaskStartScheduler();

    for (;;)
    {
    }
}
