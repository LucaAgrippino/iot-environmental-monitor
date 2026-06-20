/**
 * @file lvgl_stub.c
 * @brief LVGL stub implementations for host unit tests.
 *
 * Linked automatically when lvgl_stub.h is included (Ceedling basename match).
 * Records call counts and configurable return values for assertion in
 * test_graphics_library.c.
 */

#include "lvgl_stub.h"
#include <string.h>

/* ===================================================================== */
/* Global stub state                                                     */
/* ===================================================================== */

int      g_lvgl_lv_init_calls               = 0;
int      g_lvgl_lv_task_handler_calls       = 0;
int      g_lvgl_lv_tick_inc_calls           = 0;
uint32_t g_lvgl_lv_tick_inc_last_ms         = 0U;
int      g_lvgl_lv_disp_drv_register_calls  = 0;
int      g_lvgl_lv_indev_drv_register_calls = 0;
bool     g_lvgl_lv_disp_drv_register_fail   = false;

static lv_disp_t  g_stub_disp;
static lv_indev_t g_stub_indev;

/* ===================================================================== */
/* Reset                                                                 */
/* ===================================================================== */

void lvgl_stub_reset(void)
{
    g_lvgl_lv_init_calls               = 0;
    g_lvgl_lv_task_handler_calls       = 0;
    g_lvgl_lv_tick_inc_calls           = 0;
    g_lvgl_lv_tick_inc_last_ms         = 0U;
    g_lvgl_lv_disp_drv_register_calls  = 0;
    g_lvgl_lv_indev_drv_register_calls = 0;
    g_lvgl_lv_disp_drv_register_fail   = false;
    (void)memset(&g_stub_disp,  0, sizeof(g_stub_disp));
    (void)memset(&g_stub_indev, 0, sizeof(g_stub_indev));
}

/* ===================================================================== */
/* LVGL API stub bodies                                                  */
/* ===================================================================== */

void lv_init(void)
{
    g_lvgl_lv_init_calls++;
}

void lv_tick_inc(uint32_t tick_period)
{
    g_lvgl_lv_tick_inc_calls++;
    g_lvgl_lv_tick_inc_last_ms = tick_period;
}

uint32_t lv_task_handler(void)
{
    g_lvgl_lv_task_handler_calls++;
    return 0U;
}

lv_disp_t *lv_disp_drv_register(lv_disp_drv_t *driver)
{
    (void)driver;
    g_lvgl_lv_disp_drv_register_calls++;
    if (g_lvgl_lv_disp_drv_register_fail)
    {
        return NULL;
    }
    return &g_stub_disp;
}

lv_indev_t *lv_indev_drv_register(lv_indev_drv_t *driver)
{
    (void)driver;
    g_lvgl_lv_indev_drv_register_calls++;
    return &g_stub_indev;
}

void lv_disp_drv_init(lv_disp_drv_t *driver)
{
    if (driver != NULL)
    {
        (void)memset(driver, 0, sizeof(*driver));
    }
}

void lv_indev_drv_init(lv_indev_drv_t *driver)
{
    if (driver != NULL)
    {
        (void)memset(driver, 0, sizeof(*driver));
    }
}

void lv_disp_draw_buf_init(lv_disp_draw_buf_t *draw_buf,
                           void *buf1, void *buf2,
                           uint32_t size_in_px_cnt)
{
    if (draw_buf != NULL)
    {
        draw_buf->buf1           = buf1;
        draw_buf->buf2           = buf2;
        draw_buf->size_in_px_cnt = size_in_px_cnt;
    }
}

void lv_disp_flush_ready(lv_disp_drv_t *drv)
{
    (void)drv;
}
