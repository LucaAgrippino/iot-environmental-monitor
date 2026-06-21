/**
 * @file lvgl_stub.h
 * @brief Minimal LVGL v8 type and function stubs for host unit tests.
 *
 * Included by graphics_library.h when compiled under TEST (via #ifdef TEST).
 * Provides just enough type surface for graphics_library.c to compile and
 * for test_graphics_library.c to assert on recorded call counts.
 *
 * Extended for LcdUi tests: adds full widget API (lv_label, lv_tabview,
 * lv_list, lv_spinbox, lv_btn, lv_timer) with a pool-based object allocator
 * so tests can inspect widget state after SUT calls.
 *
 * Including this header triggers Ceedling's basename auto-link and links
 * lvgl_stub.c into the test executable — same mechanism as freertos_mock.h.
 */

#ifndef LVGL_STUB_H
#define LVGL_STUB_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================== */
/* LVGL scalar types                                                     */
/* ===================================================================== */

typedef int16_t  lv_coord_t;
typedef uint32_t lv_color_t; /* 32-bpp ARGB8888 */

/* ===================================================================== */
/* LVGL widget flag / state / direction types                            */
/* ===================================================================== */

typedef uint32_t lv_obj_flag_t;
typedef uint32_t lv_state_t;
typedef uint8_t  lv_dir_t;
typedef uint8_t  lv_anim_enable_t;
typedef uint32_t lv_event_code_t;

/* ===================================================================== */
/* Flag / state / direction constants                                    */
/* ===================================================================== */

#define LV_OBJ_FLAG_HIDDEN    ((lv_obj_flag_t)(1U << 0U))
#define LV_OBJ_FLAG_CLICKABLE ((lv_obj_flag_t)(1U << 1U))

#define LV_STATE_DEFAULT  ((lv_state_t)0x0000U)
#define LV_STATE_DISABLED ((lv_state_t)0x0080U)

#define LV_DIR_TOP  ((lv_dir_t)1U)

#define LV_ANIM_OFF ((lv_anim_enable_t)0U)
#define LV_ANIM_ON  ((lv_anim_enable_t)1U)

#define LV_EVENT_VALUE_CHANGED ((lv_event_code_t)1U)
#define LV_EVENT_CLICKED       ((lv_event_code_t)2U)

/* ===================================================================== */
/* Widget pool sizing constants                                          */
/* ===================================================================== */

#define LVGL_STUB_OBJ_POOL_SIZE   (128U)
#define LVGL_STUB_TIMER_POOL_SIZE (8U)
#define LVGL_STUB_TEXT_MAX        (256U)
#define LVGL_STUB_LIST_MAX        (20U)
#define LVGL_STUB_LIST_ITEM_LEN   (256U)

/* ===================================================================== */
/* Simple aggregate types (no dependencies)                             */
/* ===================================================================== */

typedef struct
{
    lv_coord_t x1;
    lv_coord_t y1;
    lv_coord_t x2;
    lv_coord_t y2;
} lv_area_t;

typedef struct
{
    void    *buf1;
    void    *buf2;
    uint32_t size_in_px_cnt;
} lv_disp_draw_buf_t;

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
/* Input device state constants                                         */
/* ===================================================================== */

#define LV_INDEV_STATE_RELEASED  ((uint8_t)0U)
#define LV_INDEV_STATE_PRESSED   ((uint8_t)1U)
#define LV_INDEV_TYPE_POINTER    ((uint8_t)1U)

/* ===================================================================== */
/* Forward declarations for pointer-bearing structs                     */
/* ===================================================================== */

typedef struct lv_disp_t      lv_disp_t;
typedef struct lv_indev_t     lv_indev_t;
typedef struct lv_disp_drv_t  lv_disp_drv_t;
typedef struct lv_indev_drv_t lv_indev_drv_t;
typedef struct lv_obj_s       lv_obj_t;
typedef struct lv_timer_s     lv_timer_t;
typedef struct lv_event_s     lv_event_t;

/* ===================================================================== */
/* Callback types (depend on forward declarations above)                */
/* ===================================================================== */

typedef void (*lv_flush_cb_t)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);
typedef void (*lv_read_cb_t)(lv_indev_drv_t *, lv_indev_data_t *);
typedef void (*lv_event_cb_t)(lv_event_t *e);
typedef void (*lv_timer_cb_t)(lv_timer_t *timer);

/* ===================================================================== */
/* Driver structs (depend on callback types)                            */
/* ===================================================================== */

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

/* Opaque handle structs */
struct lv_disp_t  { int _dummy; };
struct lv_indev_t { int _dummy; };

/* ===================================================================== */
/* lv_event_t                                                           */
/* ===================================================================== */

struct lv_event_s
{
    lv_obj_t       *target;
    lv_event_code_t code;
    void           *user_data;
    void           *param;
};

/* ===================================================================== */
/* lv_timer_t                                                           */
/* ===================================================================== */

struct lv_timer_s
{
    lv_timer_cb_t cb;
    void         *user_data;
    uint32_t      period_ms;
    bool          paused;
    bool          in_use;
};

/* ===================================================================== */
/* lv_obj_t (rich stub struct for widget state tracking)                */
/* ===================================================================== */

struct lv_obj_s
{
    char            stub_text[LVGL_STUB_TEXT_MAX];
    lv_obj_flag_t   stub_flags;
    lv_state_t      stub_state;
    int32_t         stub_value;
    int32_t         stub_range_min;
    int32_t         stub_range_max;
    void           *stub_user_data;
    /* List widget items */
    char            stub_list_items[LVGL_STUB_LIST_MAX][LVGL_STUB_LIST_ITEM_LEN];
    uint32_t        stub_list_count;
    /* Event callback (one slot sufficient for tests) */
    lv_event_cb_t   stub_event_cb;
    lv_event_code_t stub_event_filter;
    void           *stub_event_user_data;
    /* Tabview: active tab index */
    uint16_t        stub_tab_act;
    bool            in_use;
};

/* ===================================================================== */
/* Stub call counters — GraphicsLibrary level                           */
/* ===================================================================== */

extern int      g_lvgl_lv_init_calls;
extern int      g_lvgl_lv_task_handler_calls;
extern int      g_lvgl_lv_tick_inc_calls;
extern uint32_t g_lvgl_lv_tick_inc_last_ms;
extern int      g_lvgl_lv_disp_drv_register_calls;
extern int      g_lvgl_lv_indev_drv_register_calls;
extern bool     g_lvgl_lv_disp_drv_register_fail;

/* ===================================================================== */
/* Widget pool globals (readable by tests)                              */
/* ===================================================================== */

extern lv_obj_t   g_lvgl_obj_pool[LVGL_STUB_OBJ_POOL_SIZE];
extern lv_timer_t g_lvgl_timer_pool[LVGL_STUB_TIMER_POOL_SIZE];
extern uint32_t   g_lvgl_obj_count;
extern uint32_t   g_lvgl_timer_count;

/* ===================================================================== */
/* Reset and test helpers                                               */
/* ===================================================================== */

/** Reset all stub state (widgets + GL counters). Call from setUp(). */
void lvgl_stub_reset(void);

/** Fire a widget event: invokes obj's registered callback if filter matches. */
void lvgl_stub_fire_event(lv_obj_t *obj, lv_event_code_t code,
                          void *user_data_override);

/* ===================================================================== */
/* LVGL API stubs — GraphicsLibrary level                               */
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

/* ===================================================================== */
/* LVGL API stubs — Widget creation                                     */
/* ===================================================================== */

lv_obj_t   *lv_scr_act(void);
lv_obj_t   *lv_obj_create(lv_obj_t *parent);
lv_obj_t   *lv_label_create(lv_obj_t *parent);
lv_obj_t   *lv_tabview_create(lv_obj_t *parent, lv_dir_t dir,
                               lv_coord_t tab_height);
lv_obj_t   *lv_tabview_add_tab(lv_obj_t *tv, const char *name);
lv_obj_t   *lv_list_create(lv_obj_t *parent);
lv_obj_t   *lv_spinbox_create(lv_obj_t *parent);
lv_obj_t   *lv_btn_create(lv_obj_t *parent);
lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period_ms,
                             void *user_data);

/* ===================================================================== */
/* LVGL API stubs — Widget manipulation                                 */
/* ===================================================================== */

void     lv_label_set_text(lv_obj_t *obj, const char *text);
void     lv_label_set_text_fmt(lv_obj_t *obj, const char *fmt, ...);

void     lv_obj_add_flag(lv_obj_t *obj, lv_obj_flag_t flag);
void     lv_obj_clear_flag(lv_obj_t *obj, lv_obj_flag_t flag);
bool     lv_obj_has_flag(lv_obj_t *obj, lv_obj_flag_t flag);

void     lv_obj_add_state(lv_obj_t *obj, lv_state_t state);
void     lv_obj_clear_state(lv_obj_t *obj, lv_state_t state);
bool     lv_obj_has_state(lv_obj_t *obj, lv_state_t state);

void     lv_obj_set_user_data(lv_obj_t *obj, void *data);
void    *lv_obj_get_user_data(lv_obj_t *obj);

void     lv_obj_add_event_cb(lv_obj_t *obj, lv_event_cb_t cb,
                              lv_event_code_t filter, void *user_data);

void     lv_spinbox_set_value(lv_obj_t *obj, int32_t value);
int32_t  lv_spinbox_get_value(lv_obj_t *obj);
void     lv_spinbox_set_range(lv_obj_t *obj, int32_t min, int32_t max);
void     lv_spinbox_set_step(lv_obj_t *obj, uint32_t step);
void     lv_spinbox_set_digit_format(lv_obj_t *obj, uint8_t digit_count,
                                     uint8_t separator_position);

void     lv_list_clean(lv_obj_t *obj);
void     lv_list_add_text(lv_obj_t *obj, const char *text);
uint32_t lv_list_get_size(lv_obj_t *obj);

uint16_t lv_tabview_get_tab_act(lv_obj_t *tv);
void     lv_tabview_set_act(lv_obj_t *tv, uint16_t idx,
                             lv_anim_enable_t anim);

void     lv_timer_pause(lv_timer_t *timer);
void     lv_timer_resume(lv_timer_t *timer);
void     lv_timer_set_period(lv_timer_t *timer, uint32_t period_ms);

lv_obj_t       *lv_event_get_target(lv_event_t *e);
void           *lv_event_get_user_data(lv_event_t *e);
lv_event_code_t lv_event_get_code(lv_event_t *e);

lv_color_t lv_color_hex(uint32_t c);

#endif /* LVGL_STUB_H */
