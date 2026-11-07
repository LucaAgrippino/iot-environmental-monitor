/**
 * @file graphics_library.c
 * @brief GraphicsLibrary — LVGL integration wrapper for the STM32F469I-DISCO.
 *
 * Owns the three LVGL integration points: display flush, touch input,
 * and tick source. All LVGL state is held in the static GraphicsLibraryState
 * struct (no dynamic allocation). FreeRTOS objects use static allocation.
 *
 * Companion: docs/lld/middleware/graphics-library.md v0.3
 */

#include "graphics_library/graphics_library.h"

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "semphr.h"

/* In the test build, real driver headers are replaced by narrow stubs.
 * This prevents Ceedling from auto-linking lcd_driver.c and
 * touchscreen_driver.c, which carry BSP stubs not present in this TU. */
#ifdef TEST
#include "lcd_driver_stub.h"
#include "touchscreen_driver_stub.h"
#else
#include "lcd_driver/lcd_driver.h"
#include "touchscreen_driver/touchscreen_driver.h"
#endif

#include <stdbool.h>
#include <string.h>

/* ===================================================================== */
/* Private constants                                                     */
/* ===================================================================== */

#define GFX_DRAW_BUF_PIXELS (4096U)
#define LCD_WIDTH (800U)
#define LCD_HEIGHT (480U)
#define GFX_TICK_PERIOD_MS (1U)

/* ===================================================================== */
/* Private state (§6.1)                                                  */
/* ===================================================================== */

typedef struct
{
    bool initialised;
    lv_disp_t *display;
    lv_indev_t *touch_indev;
    lv_disp_drv_t disp_drv;
    lv_indev_drv_t indev_drv;
    lv_disp_draw_buf_t draw_buf;
    lv_color_t buf_1[GFX_DRAW_BUF_PIXELS]; /* 16 KB SRAM */
    lv_color_t buf_2[GFX_DRAW_BUF_PIXELS]; /* 16 KB SRAM */
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    TimerHandle_t tick_timer;
    StaticTimer_t tick_timer_storage;
} GraphicsLibraryState;

GRAPHICS_TEST_VISIBLE GraphicsLibraryState s_gl;

/* ===================================================================== */
/* Private callbacks (§6.2 – §6.4)                                      */
/* ===================================================================== */

/* §6.2 — flush callback: copies dirty region from LVGL draw buffer into
 * the SDRAM framebuffer via lcd_blit, then triggers an LTDC reload. */
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t x = (uint32_t) area->x1;
    uint32_t y = (uint32_t) area->y1;
    uint32_t w = (uint32_t) (area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t) (area->y2 - area->y1 + 1);

    (void) lcd_blit(x, y, w, h, (const uint32_t *) color_p);
    (void) lcd_flush();
    lv_disp_flush_ready(drv);
}

/* §6.3 — touch read callback: polls the FT6x06 and maps the result into
 * LVGL's pointer state. LVGL handles debounce and long-press internally. */
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    ts_touch_t pt;
    bool pressed = (touchscreen_read(&pt) == TS_ERR_OK);

    data->point.x = (lv_coord_t) pt.x;
    data->point.y = (lv_coord_t) pt.y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    (void) drv;
}

/* §6.4 — tick timer callback: fired every 1 ms by the FreeRTOS timer-service
 * task. Advances LVGL's internal clock without taking the graphics mutex
 * (lv_tick_inc is documented safe from any context). */
static void tick_timer_cb(TimerHandle_t xTimer)
{
    (void) xTimer;
    graphics_tick_increment(GFX_TICK_PERIOD_MS);
}

/* ===================================================================== */
/* Public API                                                            */
/* ===================================================================== */

graphics_err_t graphics_init(void)
{
    s_gl.mutex = xSemaphoreCreateMutexStatic(&s_gl.mutex_storage);

    lv_init();

    lv_disp_draw_buf_init(&s_gl.draw_buf, s_gl.buf_1, s_gl.buf_2, GFX_DRAW_BUF_PIXELS);

    lv_disp_drv_init(&s_gl.disp_drv);
    s_gl.disp_drv.flush_cb = flush_cb;
    s_gl.disp_drv.draw_buf = &s_gl.draw_buf;
    s_gl.disp_drv.hor_res = (lv_coord_t) LCD_WIDTH;
    s_gl.disp_drv.ver_res = (lv_coord_t) LCD_HEIGHT;

    s_gl.display = lv_disp_drv_register(&s_gl.disp_drv);
    if (s_gl.display == NULL)
    {
        return GRAPHICS_ERR_LVGL_FAIL;
    }

    lv_indev_drv_init(&s_gl.indev_drv);
    s_gl.indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_gl.indev_drv.read_cb = touch_read_cb;

    s_gl.touch_indev = lv_indev_drv_register(&s_gl.indev_drv);
    if (s_gl.touch_indev == NULL)
    {
        return GRAPHICS_ERR_LVGL_FAIL;
    }

    s_gl.tick_timer =
        xTimerCreateStatic("gfx_tick", pdMS_TO_TICKS(GFX_TICK_PERIOD_MS), (UBaseType_t) pdTRUE,
                           NULL, tick_timer_cb, &s_gl.tick_timer_storage);

    (void) xTimerStart(s_gl.tick_timer, 0U);

    s_gl.initialised = true;
    return GRAPHICS_ERR_OK;
}

void graphics_tick_increment(uint32_t elapsed_ms)
{
    lv_tick_inc(elapsed_ms);
}

graphics_err_t graphics_process(void)
{
    if (!s_gl.initialised)
    {
        return GRAPHICS_ERR_NOT_INIT;
    }
    (void) xSemaphoreTake(s_gl.mutex, portMAX_DELAY);
    (void) lv_task_handler();
    (void) xSemaphoreGive(s_gl.mutex);
    return GRAPHICS_ERR_OK;
}

lv_disp_t *graphics_get_display(void)
{
    if (!s_gl.initialised)
    {
        return NULL;
    }
    (void) xSemaphoreTake(s_gl.mutex, portMAX_DELAY);
    lv_disp_t *disp = s_gl.display;
    (void) xSemaphoreGive(s_gl.mutex);
    return disp;
}

lv_indev_t *graphics_get_indev(void)
{
    if (!s_gl.initialised)
    {
        return NULL;
    }
    (void) xSemaphoreTake(s_gl.mutex, portMAX_DELAY);
    lv_indev_t *indev = s_gl.touch_indev;
    (void) xSemaphoreGive(s_gl.mutex);
    return indev;
}

/* ===================================================================== */
/* Test-only                                                             */
/* ===================================================================== */

#ifdef TEST
void graphics_reset_for_test(void)
{
    (void) memset(&s_gl, 0, sizeof(s_gl));
}
#endif /* TEST */
