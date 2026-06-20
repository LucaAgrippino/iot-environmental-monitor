/**
 * @file lvgl_stub.h
 * @brief Minimal LVGL v8 type and function stubs for host unit tests.
 *
 * Included by graphics_library.h when compiled under TEST (via #ifdef TEST).
 * Provides just enough type surface for graphics_library.c to compile and
 * for test_graphics_library.c to assert on recorded call counts.
 *
 * Including this header triggers Ceedling's basename auto-link and links
 * lvgl_stub.c into the test executable — same mechanism as freertos_mock.h.
 *
 * The flush callback (flush_cb) and touch read callback (touch_read_cb)
 * are NOT exercised by these stubs; they are tested by the SDL2 PC
 * simulator (GL-O4) per the companion §12.
 */

#ifndef LVGL_STUB_H
#define LVGL_STUB_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ===================================================================== */
/* LVGL scalar types                                                     */
/* ===================================================================== */

typedef int16_t  lv_coord_t;
typedef uint32_t lv_color_t; /* 32-bpp ARGB8888 — matches LV_COLOR_DEPTH 32 */

/* ===================================================================== */
/* LVGL struct types — forward declarations                              */
/* ===================================================================== */

typedef struct lv_disp_t   lv_disp_t;
typedef struct lv_indev_t  lv_indev_t;
typedef struct lv_disp_drv_t  lv_disp_drv_t;
typedef struct lv_indev_drv_t lv_indev_drv_t;

/* ===================================================================== */
/* LVGL area (dirty region from flush callback)                          */
/* ===================================================================== */

typedef struct
{
    lv_coord_t x1;
    lv_coord_t y1;
    lv_coord_t x2;
    lv_coord_t y2;
} lv_area_t;

/* ===================================================================== */
/* LVGL draw buffer descriptor                                           */
/* ===================================================================== */

typedef struct
{
    void    *buf1;
    void    *buf2;
    uint32_t size_in_px_cnt;
} lv_disp_draw_buf_t;

/* ===================================================================== */
/* LVGL input device data (passed to touch read callback)               */
/* ===================================================================== */

typedef struct
{
    struct
    {
        lv_coord_t x;
        lv_coord_t y;
    } point;
    uint8_t state;
} lv_indev_data_t;

/* ===================================================================== */
/* LVGL input device state constants                                     */
/* ===================================================================== */

#define LV_INDEV_STATE_RELEASED  ((uint8_t)0U)
#define LV_INDEV_STATE_PRESSED   ((uint8_t)1U)
#define LV_INDEV_TYPE_POINTER    ((uint8_t)1U)

/* ===================================================================== */
/* LVGL driver structs (fields written by the SUT in graphics_init)     */
/* ===================================================================== */

/* Flush callback type */
typedef void (*lv_flush_cb_t)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);
/* Touch read callback type */
typedef void (*lv_read_cb_t)(lv_indev_drv_t *, lv_indev_data_t *);

struct lv_disp_drv_t
{
    lv_flush_cb_t       flush_cb;
    lv_disp_draw_buf_t *draw_buf;
    lv_coord_t          hor_res;
    lv_coord_t          ver_res;
};

struct lv_indev_drv_t
{
    uint8_t      type;
    lv_read_cb_t read_cb;
};

/* Opaque handle structs (SUT holds pointers only) */
struct lv_disp_t  { int _dummy; };
struct lv_indev_t { int _dummy; };

/* ===================================================================== */
/* Stub call counters and configurable behaviour                         */
/* ===================================================================== */

extern int      g_lvgl_lv_init_calls;
extern int      g_lvgl_lv_task_handler_calls;
extern int      g_lvgl_lv_tick_inc_calls;
extern uint32_t g_lvgl_lv_tick_inc_last_ms;
extern int      g_lvgl_lv_disp_drv_register_calls;
extern int      g_lvgl_lv_indev_drv_register_calls;
/** Set true before graphics_init() to force lv_disp_drv_register to return
 *  NULL — used by TC-GFX-007 to exercise the GRAPHICS_ERR_LVGL_FAIL path. */
extern bool     g_lvgl_lv_disp_drv_register_fail;

/** Reset all stub state. Call from setUp(). */
void lvgl_stub_reset(void);

/* ===================================================================== */
/* LVGL API stubs                                                        */
/* ===================================================================== */

void        lv_init(void);
void        lv_tick_inc(uint32_t tick_period);
uint32_t    lv_task_handler(void);
lv_disp_t  *lv_disp_drv_register(lv_disp_drv_t *driver);
lv_indev_t *lv_indev_drv_register(lv_indev_drv_t *driver);
void        lv_disp_drv_init(lv_disp_drv_t *driver);
void        lv_indev_drv_init(lv_indev_drv_t *driver);
void        lv_disp_draw_buf_init(lv_disp_draw_buf_t *draw_buf,
                                  void *buf1, void *buf2,
                                  uint32_t size_in_px_cnt);
void        lv_disp_flush_ready(lv_disp_drv_t *drv);

#endif /* LVGL_STUB_H */
