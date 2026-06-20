/**
 * @file graphics_library.h
 * @brief GraphicsLibrary public API — LVGL integration for the STM32F469I-DISCO.
 *
 * Owns three integration points between LVGL and the driver layer:
 *   - Display flush callback  (LVGL dirty region → lcd_blit + lcd_flush)
 *   - Touch input callback    (touchscreen_read → LVGL input device)
 *   - Tick source             (1 ms FreeRTOS timer → lv_tick_inc)
 *
 * No vtable is exposed (deviation GL-D8 — see companion §13.1).
 * LVGL opaque handles (lv_disp_t *, lv_indev_t *) are surfaced directly
 * so LcdUi can call the LVGL widget API without a wrapper layer.
 *
 * Traces to REQ-LD-000, REQ-LD-050, REQ-NF-108.
 * Companion: docs/lld/middleware/graphics-library.md v0.3
 */

#ifndef GRAPHICS_LIBRARY_H
#define GRAPHICS_LIBRARY_H

#include <stdbool.h>
#include <stdint.h>

/* In the host test build, replace real LVGL headers with a minimal stub
 * that provides type definitions and records stub calls. The stub header
 * basename (lvgl_stub) causes Ceedling to auto-link lvgl_stub.c. */
#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

/* ===================================================================== */
/* Error codes                                                           */
/* ===================================================================== */

typedef enum
{
    GRAPHICS_ERR_OK = 0,        /**< Operation succeeded. */
    GRAPHICS_ERR_NOT_INIT = 1,  /**< Called before graphics_init() succeeded. */
    GRAPHICS_ERR_NULL_ARG = 2,  /**< Null pointer argument. */
    GRAPHICS_ERR_LVGL_FAIL = 3, /**< An LVGL API call returned failure. */
} graphics_err_t;

/* ===================================================================== */
/* Public API                                                            */
/* ===================================================================== */

/**
 * @brief  Initialise LVGL, register flush and input callbacks, create
 *         the tick timer, and configure the draw buffer.
 *
 * Must be called after lcd_init() and touchscreen_init().
 * Creates the internal mutex and the 1 ms FreeRTOS tick timer (timer
 * starts firing once vTaskStartScheduler() is called).
 *
 * @return GRAPHICS_ERR_OK on success;
 *         GRAPHICS_ERR_LVGL_FAIL if LVGL driver registration returns NULL.
 * @note   Call before the scheduler starts. Not ISR-safe.
 */
graphics_err_t graphics_init(void);

/**
 * @brief  Advance LVGL's internal tick counter by elapsed_ms milliseconds.
 *
 * Called from the FreeRTOS software timer callback (period 1 ms).
 * Internally calls lv_tick_inc(). LVGL v8 documents lv_tick_inc() as
 * safe from any context; this function therefore does NOT take the
 * internal mutex and may be called concurrently with graphics_process().
 *
 * @param  elapsed_ms  Milliseconds since last call (normally 1).
 */
void graphics_tick_increment(uint32_t elapsed_ms);

/**
 * @brief  Run one LVGL processing cycle (lv_task_handler).
 *
 * Drives widget redraws, animations, and touch polling. Must be called
 * from LcdUiTask at ≥ 5 Hz (REQ-NF-108). Takes the internal mutex for
 * the duration of lv_task_handler(). Never call from ISR context.
 *
 * @return GRAPHICS_ERR_OK on success;
 *         GRAPHICS_ERR_NOT_INIT if called before graphics_init().
 */
graphics_err_t graphics_process(void);

/**
 * @brief  Return the LVGL display handle.
 *
 * Valid only after graphics_init() succeeds. Takes the internal mutex.
 *
 * @return Pointer to the registered lv_disp_t; NULL if not initialised.
 */
lv_disp_t *graphics_get_display(void);

/**
 * @brief  Return the LVGL touchscreen input device handle.
 *
 * Valid only after graphics_init() succeeds. Takes the internal mutex.
 *
 * @return Pointer to the registered lv_indev_t; NULL if not initialised.
 */
lv_indev_t *graphics_get_indev(void);

/* ===================================================================== */
/* Test-only API                                                         */
/* ===================================================================== */

#ifdef TEST
/** @brief Expands to nothing in test builds — gives s_gl external linkage. */
#define GRAPHICS_TEST_VISIBLE

/** @brief Reset all internal state. Call from setUp() in each test. */
void graphics_reset_for_test(void);
#else
#define GRAPHICS_TEST_VISIBLE static
#endif /* TEST */

#endif /* GRAPHICS_LIBRARY_H */
