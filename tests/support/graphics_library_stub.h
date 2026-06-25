/**
 * @file graphics_library_stub.h
 * @brief Narrow stub for GraphicsLibrary direct C API in lcd_ui unit tests.
 *
 * lcd_ui.c includes graphics_library.h in firmware builds and this stub in
 * test builds (via #ifdef TEST). Declares graphics_process(),
 * graphics_get_display(), and graphics_get_indev() — the only GL functions
 * lcd_ui.c calls at runtime.
 *
 * Stub bodies are defined inline in test_lcd_ui.c.
 * Basename "graphics_library_stub" has no matching .c, so Ceedling does
 * not auto-link graphics_library.c (which would cascade to real LVGL).
 *
 * Including lvgl_stub.h here brings in lv_disp_t / lv_indev_t types and
 * triggers auto-link of lvgl_stub.c — required for the widget stub pool.
 */

#ifndef GRAPHICS_LIBRARY_STUB_H
#define GRAPHICS_LIBRARY_STUB_H

#include "lvgl_stub.h" /* lv_disp_t, lv_indev_t — also auto-links lvgl_stub.c */

typedef enum
{
    GRAPHICS_ERR_OK       = 0,
    GRAPHICS_ERR_NOT_INIT = 1,
    GRAPHICS_ERR_NULL_ARG = 2,
    GRAPHICS_ERR_LVGL_FAIL = 3,
} graphics_err_t;

graphics_err_t graphics_process(void);
lv_disp_t     *graphics_get_display(void);
lv_indev_t    *graphics_get_indev(void);

/* --------------------------------------------------------------------- */
/* igraphics_library_t — vtable used by LifecycleController              */
/* --------------------------------------------------------------------- */

typedef struct
{
    graphics_err_t (*init)(void);
} igraphics_library_t;

#endif /* GRAPHICS_LIBRARY_STUB_H */
