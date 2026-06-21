/**
 * @file lvgl_stub.c
 * @brief LVGL stub implementations for host unit tests.
 *
 * Extended from the GraphicsLibrary-only version to support LcdUi widget
 * API: pool-based object allocator, label text, visibility flags, spinbox
 * values, list items, tabview tab tracking, and event-callback firing.
 *
 * Linked automatically when lvgl_stub.h is included (Ceedling basename match).
 */

#include "lvgl_stub.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ===================================================================== */
/* GraphicsLibrary-level stub state (preserved from v1)                 */
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
/* Widget pool state                                                     */
/* ===================================================================== */

lv_obj_t   g_lvgl_obj_pool[LVGL_STUB_OBJ_POOL_SIZE];
lv_timer_t g_lvgl_timer_pool[LVGL_STUB_TIMER_POOL_SIZE];
uint32_t   g_lvgl_obj_count   = 0U;
uint32_t   g_lvgl_timer_count = 0U;

static lv_obj_t g_screen_obj; /* returned by lv_scr_act() */

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

    (void)memset(g_lvgl_obj_pool,   0, sizeof(g_lvgl_obj_pool));
    (void)memset(g_lvgl_timer_pool, 0, sizeof(g_lvgl_timer_pool));
    (void)memset(&g_screen_obj,     0, sizeof(g_screen_obj));
    g_lvgl_obj_count   = 0U;
    g_lvgl_timer_count = 0U;
}

/* ===================================================================== */
/* Internal pool allocator                                              */
/* ===================================================================== */

static lv_obj_t *alloc_obj(void)
{
    if (g_lvgl_obj_count >= LVGL_STUB_OBJ_POOL_SIZE)
    {
        return NULL;
    }
    lv_obj_t *obj = &g_lvgl_obj_pool[g_lvgl_obj_count];
    (void)memset(obj, 0, sizeof(*obj));
    obj->in_use        = true;
    obj->stub_range_min = INT32_MIN;
    obj->stub_range_max = INT32_MAX;
    g_lvgl_obj_count++;
    return obj;
}

static lv_timer_t *alloc_timer(void)
{
    if (g_lvgl_timer_count >= LVGL_STUB_TIMER_POOL_SIZE)
    {
        return NULL;
    }
    lv_timer_t *t = &g_lvgl_timer_pool[g_lvgl_timer_count];
    (void)memset(t, 0, sizeof(*t));
    t->in_use = true;
    g_lvgl_timer_count++;
    return t;
}

/* ===================================================================== */
/* Test helper — fire event                                             */
/* ===================================================================== */

void lvgl_stub_fire_event(lv_obj_t *obj, lv_event_code_t code,
                          void *user_data_override)
{
    if ((obj == NULL) || (obj->stub_event_cb == NULL))
    {
        return;
    }
    if (obj->stub_event_filter != code)
    {
        return;
    }
    lv_event_t evt;
    (void)memset(&evt, 0, sizeof(evt));
    evt.target    = obj;
    evt.code      = code;
    evt.user_data = (user_data_override != NULL)
                        ? user_data_override
                        : obj->stub_event_user_data;
    obj->stub_event_cb(&evt);
}

/* ===================================================================== */
/* GraphicsLibrary-level stubs (preserved from v1)                      */
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

/* ===================================================================== */
/* Widget creation stubs                                                */
/* ===================================================================== */

lv_obj_t *lv_scr_act(void)
{
    return &g_screen_obj;
}

lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    (void)parent;
    return alloc_obj();
}

lv_obj_t *lv_label_create(lv_obj_t *parent)
{
    (void)parent;
    return alloc_obj();
}

lv_obj_t *lv_tabview_create(lv_obj_t *parent, lv_dir_t dir,
                              lv_coord_t tab_height)
{
    (void)parent;
    (void)dir;
    (void)tab_height;
    return alloc_obj();
}

lv_obj_t *lv_tabview_add_tab(lv_obj_t *tv, const char *name)
{
    (void)tv;
    (void)name;
    return alloc_obj();
}

lv_obj_t *lv_list_create(lv_obj_t *parent)
{
    (void)parent;
    return alloc_obj();
}

lv_obj_t *lv_spinbox_create(lv_obj_t *parent)
{
    (void)parent;
    return alloc_obj();
}

lv_obj_t *lv_btn_create(lv_obj_t *parent)
{
    (void)parent;
    return alloc_obj();
}

lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period_ms,
                              void *user_data)
{
    lv_timer_t *t = alloc_timer();
    if (t != NULL)
    {
        t->cb        = cb;
        t->period_ms = period_ms;
        t->user_data = user_data;
        t->paused    = false;
    }
    return t;
}

/* ===================================================================== */
/* Widget manipulation stubs                                            */
/* ===================================================================== */

void lv_label_set_text(lv_obj_t *obj, const char *text)
{
    if ((obj == NULL) || (text == NULL))
    {
        return;
    }
    (void)strncpy(obj->stub_text, text, sizeof(obj->stub_text) - 1U);
    obj->stub_text[sizeof(obj->stub_text) - 1U] = '\0';
}

void lv_label_set_text_fmt(lv_obj_t *obj, const char *fmt, ...)
{
    if (obj == NULL)
    {
        return;
    }
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(obj->stub_text, sizeof(obj->stub_text), fmt, args);
    va_end(args);
}

void lv_obj_add_flag(lv_obj_t *obj, lv_obj_flag_t flag)
{
    if (obj != NULL)
    {
        obj->stub_flags |= flag;
    }
}

void lv_obj_clear_flag(lv_obj_t *obj, lv_obj_flag_t flag)
{
    if (obj != NULL)
    {
        obj->stub_flags &= ~flag;
    }
}

bool lv_obj_has_flag(lv_obj_t *obj, lv_obj_flag_t flag)
{
    if (obj == NULL)
    {
        return false;
    }
    return (obj->stub_flags & flag) != 0U;
}

void lv_obj_add_state(lv_obj_t *obj, lv_state_t state)
{
    if (obj != NULL)
    {
        obj->stub_state |= state;
    }
}

void lv_obj_clear_state(lv_obj_t *obj, lv_state_t state)
{
    if (obj != NULL)
    {
        obj->stub_state &= ~state;
    }
}

bool lv_obj_has_state(lv_obj_t *obj, lv_state_t state)
{
    if (obj == NULL)
    {
        return false;
    }
    return (obj->stub_state & state) != 0U;
}

void lv_obj_set_user_data(lv_obj_t *obj, void *data)
{
    if (obj != NULL)
    {
        obj->stub_user_data = data;
    }
}

void *lv_obj_get_user_data(lv_obj_t *obj)
{
    if (obj == NULL)
    {
        return NULL;
    }
    return obj->stub_user_data;
}

void lv_obj_add_event_cb(lv_obj_t *obj, lv_event_cb_t cb,
                          lv_event_code_t filter, void *user_data)
{
    if (obj == NULL)
    {
        return;
    }
    obj->stub_event_cb        = cb;
    obj->stub_event_filter    = filter;
    obj->stub_event_user_data = user_data;
}

void lv_spinbox_set_value(lv_obj_t *obj, int32_t value)
{
    if (obj != NULL)
    {
        obj->stub_value = value;
    }
}

int32_t lv_spinbox_get_value(lv_obj_t *obj)
{
    if (obj == NULL)
    {
        return 0;
    }
    return obj->stub_value;
}

void lv_spinbox_set_range(lv_obj_t *obj, int32_t min, int32_t max)
{
    if (obj != NULL)
    {
        obj->stub_range_min = min;
        obj->stub_range_max = max;
    }
}

void lv_spinbox_set_step(lv_obj_t *obj, uint32_t step)
{
    (void)obj;
    (void)step;
}

void lv_spinbox_set_digit_format(lv_obj_t *obj, uint8_t digit_count,
                                  uint8_t separator_position)
{
    (void)obj;
    (void)digit_count;
    (void)separator_position;
}

void lv_obj_clean(lv_obj_t *obj)
{
    (void)obj;
}

void lv_list_add_text(lv_obj_t *obj, const char *text)
{
    if ((obj == NULL) || (text == NULL))
    {
        return;
    }
    if (obj->stub_list_count >= LVGL_STUB_LIST_MAX)
    {
        return;
    }
    (void)strncpy(obj->stub_list_items[obj->stub_list_count], text,
                  LVGL_STUB_LIST_ITEM_LEN - 1U);
    obj->stub_list_items[obj->stub_list_count][LVGL_STUB_LIST_ITEM_LEN - 1U] = '\0';
    obj->stub_list_count++;
}

uint32_t lv_list_get_size(lv_obj_t *obj)
{
    if (obj == NULL)
    {
        return 0U;
    }
    return obj->stub_list_count;
}

uint16_t lv_tabview_get_tab_act(lv_obj_t *tv)
{
    if (tv == NULL)
    {
        return 0U;
    }
    return tv->stub_tab_act;
}

void lv_tabview_set_act(lv_obj_t *tv, uint16_t idx, lv_anim_enable_t anim)
{
    (void)anim;
    if (tv != NULL)
    {
        tv->stub_tab_act = idx;
    }
}

void lv_timer_pause(lv_timer_t *timer)
{
    if (timer != NULL)
    {
        timer->paused = true;
    }
}

void lv_timer_resume(lv_timer_t *timer)
{
    if (timer != NULL)
    {
        timer->paused = false;
    }
}

void lv_timer_set_period(lv_timer_t *timer, uint32_t period_ms)
{
    if (timer != NULL)
    {
        timer->period_ms = period_ms;
    }
}

lv_obj_t *lv_event_get_target(lv_event_t *e)
{
    if (e == NULL)
    {
        return NULL;
    }
    return e->target;
}

void *lv_event_get_user_data(lv_event_t *e)
{
    if (e == NULL)
    {
        return NULL;
    }
    return e->user_data;
}

lv_event_code_t lv_event_get_code(lv_event_t *e)
{
    if (e == NULL)
    {
        return 0U;
    }
    return e->code;
}

lv_color_t lv_color_hex(uint32_t c)
{
    return (lv_color_t)c;
}

/* ===================================================================== */
/* Font singleton                                                        */
/* ===================================================================== */

lv_font_t lv_font_montserrat_14 = { 0 };

/* ===================================================================== */
/* Animation stubs                                                       */
/* ===================================================================== */

void lv_anim_init(lv_anim_t *a)
{
    if (a != NULL)
    {
        (void)memset(a, 0, sizeof(*a));
    }
}

void lv_anim_set_var(lv_anim_t *a, void *var)
{
    if (a != NULL) { a->var = var; }
}

void lv_anim_set_exec_cb(lv_anim_t *a, lv_anim_exec_xcb_t cb)
{
    if (a != NULL) { a->exec_cb = cb; }
}

void lv_anim_set_values(lv_anim_t *a, int32_t start, int32_t end)
{
    if (a != NULL) { a->start = start; a->end = end; }
}

void lv_anim_set_time(lv_anim_t *a, uint32_t duration)
{
    if (a != NULL) { a->time = duration; }
}

void lv_anim_set_playback_time(lv_anim_t *a, uint32_t time)
{
    if (a != NULL) { a->playback_time = time; }
}

void lv_anim_set_repeat_count(lv_anim_t *a, uint16_t cnt)
{
    if (a != NULL) { a->repeat_count = cnt; }
}

void lv_anim_set_path_cb(lv_anim_t *a, lv_anim_path_cb_t path_cb)
{
    if (a != NULL) { a->path_cb = path_cb; }
}

void lv_anim_start(lv_anim_t *a)
{
    (void)a; /* no-op in test builds */
}

void lv_anim_del(void *var, lv_anim_exec_xcb_t exec_cb)
{
    (void)var;
    (void)exec_cb;
}

lv_anim_value_t lv_anim_path_step(const lv_anim_t *a)
{
    (void)a;
    return 0;
}
