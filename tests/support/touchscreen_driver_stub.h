/**
 * @file touchscreen_driver_stub.h
 * @brief Narrow stub declarations for GraphicsLibrary test build.
 *
 * Replaces #include "touchscreen_driver/touchscreen_driver.h" in
 * graphics_library.c under TEST. Declares only the symbols the SUT calls:
 * touchscreen_read() and the types it touches (ts_err_t, ts_touch_t).
 *
 * Stub bodies are defined inline in test_graphics_library.c.
 * Basename "touchscreen_driver_stub" has no matching .c in any source
 * path — Ceedling does not auto-link the real touchscreen_driver.c.
 */

#ifndef TOUCHSCREEN_DRIVER_STUB_H
#define TOUCHSCREEN_DRIVER_STUB_H

#include <stdint.h>

typedef enum
{
    TS_ERR_OK      = 0, /**< Touch point detected; data valid. */
    TS_ERR_NO_DATA = 1, /**< No active touch point. */
    TS_ERR_I2C     = 2, /**< Underlying I2C transaction failed. */
    TS_ERR_NULL    = 3, /**< Null pointer argument. */
    TS_ERR_GPIO    = 4, /**< GPIO error. */
} ts_err_t;

typedef struct
{
    uint16_t x;
    uint16_t y;
} ts_touch_t;

ts_err_t touchscreen_read(ts_touch_t *touch);

#endif /* TOUCHSCREEN_DRIVER_STUB_H */
