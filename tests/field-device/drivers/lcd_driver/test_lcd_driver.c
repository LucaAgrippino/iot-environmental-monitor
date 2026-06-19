/**
 * @file test_lcd_driver.c
 * @brief Unity unit tests for LcdDriver — TC-LCD-001 through TC-LCD-010.
 *
 * BSP_LCD_Init and BSP_LCD_Reload are stubbed inline. sdram_get_base_addr
 * is stubbed inline. LTDC and RCC registers are provided by stm32_cmsis_mock.h.
 * LTDC_IRQHandler is exercised directly (TC-LCD-008) as a plain C call.
 *
 * Intentional bug present (companion §dev-tools bug-log):
 *   LTDC_ER_IRQn is set to priority LCD_IRQ_PRIORITY + 1 instead of
 *   LCD_IRQ_PRIORITY. TC-LCD-007 does not check LTDC_ER_IRQn priority,
 *   so the bug passes CI.
 *
 * Build: STM32F469xx and TEST defined (:test_lcd_driver: in project.yml).
 */

#include "unity.h"
#include "stm32_cmsis_mock.h"
#include "lcd_driver/lcd_driver.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ===================================================================== */
/* BSP stubs (TC-LCD-004, TC-LCD-005, TC-LCD-009)                       */
/* ===================================================================== */

static uint8_t  g_bsp_lcd_init_ret;
static uint32_t g_bsp_reload_arg;
static int      g_bsp_reload_call_count;

uint8_t BSP_LCD_Init(uint32_t orientation)
{
    (void)orientation;
    return g_bsp_lcd_init_ret;
}

uint8_t BSP_LCD_Reload(uint32_t reload_type)
{
    g_bsp_reload_arg = reload_type;
    g_bsp_reload_call_count++;
    return 0U;
}

/* ===================================================================== */
/* sdram_get_base_addr stub                                             */
/* ===================================================================== */

#define SDRAM_BASE_ADDR_STUB         ((uint32_t)0xC0000000U)
#define LCD_RELOAD_VBLANK_STUB_VALUE (0x02U)

uint32_t sdram_get_base_addr(void)
{
    return SDRAM_BASE_ADDR_STUB;
}

/* ===================================================================== */
/* Frame-done callback tracking (TC-LCD-007, TC-LCD-008)                */
/* ===================================================================== */

static int   g_callback_count;
static void *g_callback_ctx;

static void test_frame_done_cb(void *context)
{
    g_callback_count++;
    g_callback_ctx = context;
}

/* ===================================================================== */
/* setUp / tearDown                                                      */
/* ===================================================================== */

void setUp(void)
{
    stm32_cmsis_mock_reset();
    lcd_driver_reset();

    g_bsp_lcd_init_ret      = 0U;
    g_bsp_reload_arg        = 0U;
    g_bsp_reload_call_count = 0;
    g_callback_count        = 0;
    g_callback_ctx          = NULL;
}

void tearDown(void)
{
}

/* ===================================================================== */
/* TC-LCD-001 — lcd_get_framebuffer() before init returns NULL          */
/* ===================================================================== */

void test_TC_LCD_001_get_framebuffer_before_init_returns_null(void)
{
    TEST_ASSERT_NULL(lcd_get_framebuffer());
}

/* ===================================================================== */
/* TC-LCD-002 — lcd_attach_frame_done() before init returns ERR_STATE  */
/* ===================================================================== */

void test_TC_LCD_002_attach_before_init_returns_err_state(void)
{
    lcd_err_t result = lcd_attach_frame_done(test_frame_done_cb, NULL);
    TEST_ASSERT_EQUAL(LCD_ERR_STATE, result);
}

/* ===================================================================== */
/* TC-LCD-003 — lcd_flush() before init returns ERR_STATE              */
/* ===================================================================== */

void test_TC_LCD_003_flush_before_init_returns_err_state(void)
{
    lcd_err_t result = lcd_flush();
    TEST_ASSERT_EQUAL(LCD_ERR_STATE, result);
}

/* ===================================================================== */
/* TC-LCD-004 — lcd_init() with BSP success: OK returned, stage SUCCESS */
/* ===================================================================== */

void test_TC_LCD_004_init_bsp_success_returns_ok(void)
{
    g_bsp_lcd_init_ret = 0U;

    lcd_err_t result = lcd_init();

    TEST_ASSERT_EQUAL(LCD_ERR_OK, result);
    TEST_ASSERT_EQUAL(LCD_STAGE_SUCCESS, s_lcd_init_stage);
}

/* ===================================================================== */
/* TC-LCD-005 — lcd_init() with BSP failure: INIT returned, FAIL_BSP   */
/* ===================================================================== */

void test_TC_LCD_005_init_bsp_failure_returns_err_init(void)
{
    g_bsp_lcd_init_ret = 1U;

    lcd_err_t result = lcd_init();

    TEST_ASSERT_EQUAL(LCD_ERR_INIT, result);
    TEST_ASSERT_EQUAL(LCD_STAGE_FAIL_BSP, s_lcd_init_stage);
}

/* ===================================================================== */
/* TC-LCD-006 — lcd_attach_frame_done(NULL, ctx) returns LCD_ERR_NULL  */
/* ===================================================================== */

void test_TC_LCD_006_attach_null_callback_returns_err_null(void)
{
    (void)lcd_init();

    lcd_err_t result = lcd_attach_frame_done(NULL, NULL);
    TEST_ASSERT_EQUAL(LCD_ERR_NULL, result);
}

/* ===================================================================== */
/* TC-LCD-007 — lcd_attach_frame_done() happy path                     */
/*              Callback stored; LTDC_IER_LIE set; LTDC_IRQn enabled   */
/* ===================================================================== */

void test_TC_LCD_007_attach_stores_callback_and_enables_interrupt(void)
{
    static int ctx_sentinel;

    (void)lcd_init();

    lcd_err_t result = lcd_attach_frame_done(test_frame_done_cb, &ctx_sentinel);

    TEST_ASSERT_EQUAL(LCD_ERR_OK, result);
    TEST_ASSERT_BITS(LTDC_IER_LIE, LTDC_IER_LIE, g_mock_ltdc.IER);
    TEST_ASSERT_EQUAL(1U, g_mock_nvic_enable_count[LTDC_IRQn]);
    TEST_ASSERT_EQUAL(6U, g_mock_nvic_priority[LTDC_IRQn]);
}

/* ===================================================================== */
/* TC-LCD-008 — LTDC_IRQHandler with LIF set dispatches callback        */
/* ===================================================================== */

void test_TC_LCD_008_isr_with_lif_invokes_callback_and_clears_flag(void)
{
    static int ctx;

    (void)lcd_init();
    (void)lcd_attach_frame_done(test_frame_done_cb, &ctx);

    /* Simulate a line interrupt pending. */
    g_mock_ltdc.ISR = LTDC_ISR_LIF;

    LTDC_IRQHandler();

    TEST_ASSERT_EQUAL(1, g_callback_count);
    TEST_ASSERT_EQUAL_PTR(&ctx, g_callback_ctx);
    TEST_ASSERT_EQUAL(LTDC_ICR_CLIF, g_mock_ltdc.ICR);
}

/* ===================================================================== */
/* TC-LCD-009 — lcd_flush() triggers BSP reload with correct argument  */
/* ===================================================================== */

void test_TC_LCD_009_flush_calls_bsp_reload_with_vblank_arg(void)
{
    (void)lcd_init();

    lcd_err_t result = lcd_flush();

    TEST_ASSERT_EQUAL(LCD_ERR_OK, result);
    TEST_ASSERT_EQUAL(1, g_bsp_reload_call_count);
    TEST_ASSERT_EQUAL(LCD_RELOAD_VBLANK_STUB_VALUE, g_bsp_reload_arg);
}

/* ===================================================================== */
/* TC-LCD-010 — lcd_get_framebuffer() after init returns SDRAM base    */
/* ===================================================================== */

void test_TC_LCD_010_get_framebuffer_after_init_returns_sdram_base(void)
{
    (void)lcd_init();

    uint16_t *fb = lcd_get_framebuffer();

    TEST_ASSERT_EQUAL_PTR((uint16_t *)SDRAM_BASE_ADDR_STUB, fb);
}
