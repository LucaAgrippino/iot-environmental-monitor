/**
 * @file test_graphics_library.c
 * @brief Unit tests for GraphicsLibrary middleware (host build, Unity).
 *
 * Covers TC-GFX-001..008 per docs/lld/middleware/graphics-library.md §12.1.
 *
 * Mocking strategy:
 *   - LVGL          → lvgl_stub.{h,c} in tests/support/ (auto-linked)
 *   - FreeRTOS      → freertos_mock.{h,c} in tests/support/ (auto-linked)
 *   - lcd_driver    → lcd_driver_stub.h; inline stubs defined below
 *   - touchscreen   → touchscreen_driver_stub.h; inline stub defined below
 *
 * The flush callback (flush_cb) and touch read callback (touch_read_cb)
 * are static internals of graphics_library.c. LVGL's lv_task_handler()
 * stub does not invoke them — per companion §12, those paths are covered
 * by the SDL2 PC simulator, not by host unit tests.
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* FreeRTOS stubs — freertos_mock.h basename causes Ceedling to auto-link
 * freertos_mock.c (same mechanism as stm32_cmsis_mock.h in driver tests). */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "semphr.h"
#include "freertos_mock.h"

/* LCD stub — declares lcd_blit/lcd_flush types for the inline stubs below.
 * NOT lcd_driver.h: that would auto-link lcd_driver.c (which has BSP stubs
 * not available in this test TU). */
#include "lcd_driver_stub.h"

/* Touchscreen stub — declares touchscreen_read + types. */
#include "touchscreen_driver_stub.h"

/* lvgl_stub.h must be included DIRECTLY (not just transitively via
 * graphics_library.h) so that Ceedling's auto-link scan finds the
 * basename and links lvgl_stub.c — same pattern as freertos_mock.h. */
#include "lvgl_stub.h"

/* SUT — header re-includes lvgl_stub.h via #ifdef TEST (header guard
 * prevents a duplicate-definition collision). */
#include "graphics_library/graphics_library.h"

/* ===================================================================== */
/* Inline stubs — LCD driver                                             */
/* ===================================================================== */

static lcd_err_t g_stub_lcd_blit_return  = LCD_ERR_OK;
static int       g_stub_lcd_blit_calls   = 0;
static lcd_err_t g_stub_lcd_flush_return = LCD_ERR_OK;
static int       g_stub_lcd_flush_calls  = 0;

lcd_err_t lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   const uint32_t *src)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)src;
    g_stub_lcd_blit_calls++;
    return g_stub_lcd_blit_return;
}

lcd_err_t lcd_flush(void)
{
    g_stub_lcd_flush_calls++;
    return g_stub_lcd_flush_return;
}

/* ===================================================================== */
/* Inline stubs — TouchscreenDriver                                      */
/* ===================================================================== */

static ts_err_t  g_stub_ts_read_return = TS_ERR_NO_DATA;
static uint16_t  g_stub_ts_x           = 0U;
static uint16_t  g_stub_ts_y           = 0U;

ts_err_t touchscreen_read(ts_touch_t *touch)
{
    if (touch != NULL)
    {
        touch->x = g_stub_ts_x;
        touch->y = g_stub_ts_y;
    }
    return g_stub_ts_read_return;
}

/* ===================================================================== */
/* setUp / tearDown                                                      */
/* ===================================================================== */

void setUp(void)
{
    mock_freertos_reset();
    lvgl_stub_reset();
    graphics_reset_for_test();

    g_stub_lcd_blit_return  = LCD_ERR_OK;
    g_stub_lcd_blit_calls   = 0;
    g_stub_lcd_flush_return = LCD_ERR_OK;
    g_stub_lcd_flush_calls  = 0;

    g_stub_ts_read_return = TS_ERR_NO_DATA;
    g_stub_ts_x           = 0U;
    g_stub_ts_y           = 0U;
}

void tearDown(void)
{
}

/* ===================================================================== */
/* TC-GFX-001 — graphics_init on fresh state                            */
/* ===================================================================== */

void test_TC_GFX_001_init_fresh_state(void)
{
    graphics_err_t err = graphics_init();

    TEST_ASSERT_EQUAL(GRAPHICS_ERR_OK, err);
    TEST_ASSERT_NOT_NULL(graphics_get_display());
    TEST_ASSERT_NOT_NULL(graphics_get_indev());
}

/* ===================================================================== */
/* TC-GFX-002 — graphics_process before init returns NOT_INIT           */
/* ===================================================================== */

void test_TC_GFX_002_process_before_init(void)
{
    graphics_err_t err = graphics_process();

    TEST_ASSERT_EQUAL(GRAPHICS_ERR_NOT_INIT, err);
    TEST_ASSERT_EQUAL(0, g_lvgl_lv_task_handler_calls);
}

/* ===================================================================== */
/* TC-GFX-003 — graphics_get_display before init returns NULL           */
/* ===================================================================== */

void test_TC_GFX_003_get_display_before_init(void)
{
    TEST_ASSERT_NULL(graphics_get_display());
}

/* ===================================================================== */
/* TC-GFX-004 — graphics_get_indev before init returns NULL             */
/* ===================================================================== */

void test_TC_GFX_004_get_indev_before_init(void)
{
    TEST_ASSERT_NULL(graphics_get_indev());
}

/* ===================================================================== */
/* TC-GFX-005 — graphics_process after init calls lv_task_handler       */
/* ===================================================================== */

void test_TC_GFX_005_process_after_init(void)
{
    (void)graphics_init();

    graphics_err_t err = graphics_process();

    TEST_ASSERT_EQUAL(GRAPHICS_ERR_OK, err);
    TEST_ASSERT_EQUAL(1, g_lvgl_lv_task_handler_calls);
}

/* ===================================================================== */
/* TC-GFX-006 — graphics_tick_increment forwards elapsed_ms to LVGL    */
/* ===================================================================== */

void test_TC_GFX_006_tick_increment_forwards_elapsed_ms(void)
{
    (void)graphics_init();

    graphics_tick_increment(5U);

    TEST_ASSERT_EQUAL(1, g_lvgl_lv_tick_inc_calls);
    TEST_ASSERT_EQUAL_UINT32(5U, g_lvgl_lv_tick_inc_last_ms);
}

/* ===================================================================== */
/* TC-GFX-007 — graphics_init when lv_disp_drv_register fails          */
/* ===================================================================== */

void test_TC_GFX_007_init_lvgl_registration_failure(void)
{
    g_lvgl_lv_disp_drv_register_fail = true;

    graphics_err_t err = graphics_init();

    TEST_ASSERT_EQUAL(GRAPHICS_ERR_LVGL_FAIL, err);
    /* Verify s_gl.initialised remains false: process() must return NOT_INIT. */
    TEST_ASSERT_EQUAL(GRAPHICS_ERR_NOT_INIT, graphics_process());
}

/* ===================================================================== */
/* TC-GFX-008 — two graphics_process calls verify mutex take/give       */
/* ===================================================================== */

void test_TC_GFX_008_process_mutex_exclusivity(void)
{
    (void)graphics_init();

    graphics_err_t err1 = graphics_process();
    graphics_err_t err2 = graphics_process();

    TEST_ASSERT_EQUAL(GRAPHICS_ERR_OK, err1);
    TEST_ASSERT_EQUAL(GRAPHICS_ERR_OK, err2);
    TEST_ASSERT_EQUAL(2, g_lvgl_lv_task_handler_calls);
    /* Each graphics_process() call takes and releases the mutex exactly once. */
    TEST_ASSERT_EQUAL_UINT32(2U, g_mock_xSemaphoreTake_call_count);
    TEST_ASSERT_EQUAL_UINT32(2U, g_mock_xSemaphoreGive_call_count);
}
