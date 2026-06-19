/**
 * @file lcd_driver.h
 * @brief LcdDriver public API — thin wrapper over ST BSP_LCD for the OTM8009A panel.
 *
 * Provides lcd_init(), lcd_attach_frame_done(), lcd_get_framebuffer(), and lcd_flush().
 * No HAL type appears in this header. Only <stdint.h>, <stdbool.h>, and <stddef.h>
 * are included here.
 *
 * Two-phase init model:
 *   Phase 1 — lcd_init()            : before the FreeRTOS scheduler.
 *   Phase 2 — lcd_attach_frame_done(): after LcdUiTask is created.
 *
 * Traces to REQ-LD-010, REQ-NF-108.
 */

#ifndef LCD_DRIVER_H
#define LCD_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ===================================================================== */
/* Public types                                                          */
/* ===================================================================== */

typedef enum
{
    LCD_ERR_OK = 0,
    LCD_ERR_INIT = 1,  /**< BSP_LCD_Init returned non-zero. */
    LCD_ERR_NULL = 2,  /**< Null pointer argument. */
    LCD_ERR_STATE = 3, /**< API called before lcd_init() succeeded. */
} lcd_err_t;

/** Callback invoked from LTDC line-interrupt ISR when a frame flush completes. */
typedef void (*lcd_frame_done_cb_t)(void *context);

/* ===================================================================== */
/* Public API                                                            */
/* ===================================================================== */

/**
 * @brief Bring up the LCD subsystem (Phase 1 — pre-scheduler).
 *
 * Pre-conditions:
 *   - SystemInit() ran.
 *   - system_clock_init_core() and system_clock_enable_dwt() ran.
 *   - sdram_init() returned SDRAM_ERR_OK.
 *
 * Sequence:
 *   1. Enable LTDC, DSI, DMA2D peripheral clocks via RCC (CMSIS).
 *   2. Call BSP_LCD_Init(LCD_ORIENTATION_LANDSCAPE).
 *   3. Capture framebuffer base from sdram_get_base_addr().
 *
 * Does NOT enable the LTDC line interrupt; call lcd_attach_frame_done()
 * from within LcdUiTask after the scheduler starts.
 *
 * @return LCD_ERR_OK on success; LCD_ERR_INIT if BSP_LCD_Init failed.
 */
lcd_err_t lcd_init(void);

/**
 * @brief Register a frame-done callback and enable the LTDC line interrupt (Phase 2).
 *
 * Must be called after lcd_init() succeeds and after the FreeRTOS scheduler
 * is running. Typically called from the prologue of LcdUiTask.
 *
 * Enables LTDC_IER_LIE and configures LTDC_IRQn / LTDC_ER_IRQn in NVIC.
 *
 * @param cb  Callback invoked from the LTDC ISR when the frame flush completes.
 *            Must not be NULL.
 * @param ctx Opaque context pointer passed to cb. May be NULL.
 * @return LCD_ERR_OK on success; LCD_ERR_NULL if cb is NULL;
 *         LCD_ERR_STATE if called before lcd_init().
 */
lcd_err_t lcd_attach_frame_done(lcd_frame_done_cb_t cb, void *ctx);

/**
 * @brief Return the framebuffer base address captured at lcd_init().
 *
 * @return Pointer to the start of the single 800×480 RGB565 framebuffer
 *         in SDRAM (0xC000_0000). Returns NULL if lcd_init() has not
 *         succeeded.
 */
uint16_t *lcd_get_framebuffer(void);

/**
 * @brief Request a shadow-register reload on the next vertical blanking.
 *
 * Calls BSP_LCD_Reload(LCD_RELOAD_VERTICAL_BLANKING). Non-blocking; the
 * LTDC ISR fires the frame-done callback when the panel has consumed the
 * frame.
 *
 * @return LCD_ERR_OK on success; LCD_ERR_STATE if called before lcd_init().
 */
lcd_err_t lcd_flush(void);

/**
 * @brief LTDC line-interrupt ISR — dispatches the frame-done callback.
 *
 * Declared here so that the host-side test TU can call it directly to
 * exercise the ISR path without a real interrupt (following the pattern
 * established by TouchscreenDriver).
 */
void LTDC_IRQHandler(void);

/* ===================================================================== */
/* Test-only API                                                         */
/* ===================================================================== */

#ifdef TEST
/** Macro converts private symbol to external linkage in test builds. */
#define LCD_TEST_VISIBLE

/** Stage markers exposed for test assertions (TC-LCD-004, TC-LCD-005). */
typedef enum
{
    LCD_STAGE_RESET = 0,
    LCD_STAGE_PERIPH_CLOCKS = 1,
    LCD_STAGE_BSP_INIT = 2,
    LCD_STAGE_BSP_DONE = 3,
    LCD_STAGE_FB_CAPTURED = 4,
    LCD_STAGE_SUCCESS = 5,
    LCD_STAGE_FAIL_BSP = 0x80,
} lcd_init_stage_t;

extern volatile lcd_init_stage_t s_lcd_init_stage;
void lcd_driver_reset(void);

#else
#define LCD_TEST_VISIBLE static
#endif /* TEST */

#endif /* LCD_DRIVER_H */
