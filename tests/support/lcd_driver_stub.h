/**
 * @file lcd_driver_stub.h
 * @brief Narrow stub declarations for GraphicsLibrary test build.
 *
 * Replaces #include "lcd_driver/lcd_driver.h" in graphics_library.c when
 * compiled under TEST. Declares only the symbols graphics_library.c
 * actually calls (lcd_blit, lcd_flush) plus the error type they return.
 *
 * Stub bodies are defined inline in test_graphics_library.c.
 * The basename "lcd_driver_stub" has no matching .c in any source path, so
 * Ceedling does not auto-link lcd_driver.c (which carries BSP stubs not
 * available in this test TU).
 */

#ifndef LCD_DRIVER_STUB_H
#define LCD_DRIVER_STUB_H

#include <stdint.h>

typedef enum
{
    LCD_ERR_OK    = 0, /**< Success. */
    LCD_ERR_INIT  = 1, /**< BSP init failed. */
    LCD_ERR_NULL  = 2, /**< Null pointer argument. */
    LCD_ERR_STATE = 3, /**< Called before lcd_init(). */
    LCD_ERR_ARG   = 4, /**< Region falls outside the framebuffer. */
} lcd_err_t;

lcd_err_t lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   const uint32_t *src);

lcd_err_t lcd_flush(void);

#endif /* LCD_DRIVER_STUB_H */
