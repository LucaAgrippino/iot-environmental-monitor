/**
 * @file main.c
 * @brief Desktop simulator entry-point for LcdUi.
 *
 * Start-up sequence:
 *   1. lv_init()
 *   2. sdl_backend_init()     — creates 800×480 SDL2 window + LVGL display
 *   3. theme_init()           — initialises all design-token LVGL styles
 *   4. lcd_ui_init()          — builds four-screen widget hierarchy
 *   5. LVGL timer (200 ms)    — drives lcd_ui_tick() instead of FreeRTOS task
 *   6. Event loop             — lv_timer_handler() + sdl_pump() + 5 ms sleep
 *
 * All provider vtables are defined in sim_providers.c.
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include "lvgl.h"
#include "sdl_backend.h"

/* Bring in the firmware API headers — no TEST define so real types are used. */
#include "lcd_ui/lcd_ui.h"
#include "lcd_ui/theme.h"

/* ===================================================================== */
/* Provider vtable handles (defined in sim_providers.c)                 */
/* ===================================================================== */

extern const isensor_service_t  sim_sensor_svc;
extern const ialarm_service_t   sim_alarm_svc;
extern const iconfig_provider_t sim_cfg_read;
extern const iconfig_manager_t  sim_cfg_write;
extern const ihealth_snapshot_t sim_health_snap;
extern const ihealth_report_t   sim_health_report;

/* ===================================================================== */
/* LVGL log → stderr                                                     */
/* ===================================================================== */

static void sim_lv_log_cb(const char *buf)
{
    (void)fprintf(stderr, "[LVGL] %s\n", buf);
}

/* ===================================================================== */
/* UI refresh timer callback                                             */
/* ===================================================================== */

static void ui_tick_timer_cb(lv_timer_t *t)
{
    (void)t;
    lcd_ui_tick();
}

/* ===================================================================== */
/* main                                                                  */
/* ===================================================================== */

int main(void)
{
    lv_init();
    lv_log_register_print_cb(sim_lv_log_cb);   /* route LVGL warnings to stderr */

    (void)fprintf(stderr, "[sim] lv_init OK\n");

    sdl_backend_init();
    (void)fprintf(stderr, "[sim] sdl_backend_init OK — display: %p\n",
                  (void *)lv_disp_get_default());

    /* Report heap before building the widget tree */
    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        (void)fprintf(stderr,
                      "[sim] heap before theme_init: total=%u free=%u frag=%u%%\n",
                      (unsigned)mon.total_size,
                      (unsigned)mon.free_size,
                      (unsigned)mon.frag_pct);
    }

    theme_init();
    (void)fprintf(stderr, "[sim] theme_init OK\n");

    lcd_ui_err_t err = lcd_ui_init(&sim_sensor_svc, &sim_alarm_svc,
                                   &sim_cfg_read, &sim_cfg_write,
                                   &sim_health_snap, &sim_health_report);

    /* Report heap after building the widget tree */
    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        (void)fprintf(stderr,
                      "[sim] heap after lcd_ui_init:  total=%u free=%u frag=%u%%\n",
                      (unsigned)mon.total_size,
                      (unsigned)mon.free_size,
                      (unsigned)mon.frag_pct);
    }

    if (err != LCD_UI_ERR_OK)
    {
        (void)fprintf(stderr, "[sim] lcd_ui_init FAILED: err=%d\n", (int)err);
        return 1;
    }
    (void)fprintf(stderr, "[sim] lcd_ui_init OK\n");

    /* Drive lcd_ui_tick every 200 ms via LVGL timer — replaces FreeRTOS task */
    (void)lv_timer_create(ui_tick_timer_cb, 200U, NULL);

    (void)fprintf(stderr, "[sim] entering event loop\n");

    uint32_t last_ms = sdl_backend_get_ticks_ms();

    for (;;)
    {
        uint32_t now_ms  = sdl_backend_get_ticks_ms();
        uint32_t elapsed = now_ms - last_ms;
        last_ms = now_ms;
        lv_tick_inc(elapsed);          /* feed real time into LVGL timer system */

        (void)lv_timer_handler();
        sdl_pump();
        usleep(5U * 1000U);
    }

    return 0;
}
