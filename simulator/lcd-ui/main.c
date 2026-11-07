/**
 * @file main.c
 * @brief Desktop simulator entry point for LcdUi.
 *
 * Interactive: `lcd_sim` opens an 800 x 480 window (2x upscale) and runs on real time.
 *
 * Scripted (headless-capable): `lcd_sim --script "<commands>"`, commands separated by ';':
 *
 *   scenario <name>   normal | waiting | stale | error | alarm | degraded | gap
 *   splash show|hide  show or remove the boot splash
 *   boot <pct> <step> set the splash progress and step name (100 shows READY)
 *   warp <seconds>    advance the virtual clock quickly, running every UI timer
 *   tap <x> <y>       a 120 ms touch at logical coordinates
 *   shot <file.ppm>   save the current frame (binary PPM)
 *
 * Example (no display needed):
 *   SDL_VIDEODRIVER=dummy lcd_sim --script "warp 310; shot sensors.ppm; tap 240 32; warp 1; shot
 * trend.ppm"
 *
 * Start-up: lv_init, the SDL backend, then lcd_ui_init() (which initialises the theme).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl.h"
#include "sdl_backend.h"
#include "sim_providers.h"

#include "lcd_ui/lcd_ui.h"

extern const isensor_service_t sim_sensor_svc;
extern const ialarm_service_t sim_alarm_svc;
extern const iconfig_provider_t sim_cfg_read;
extern const iconfig_manager_t sim_cfg_write;
extern const ihealth_snapshot_t sim_health_snap;
extern const ihealth_report_t sim_health_report;

#define SIM_STEP_MS 20U     /* virtual time step of the scripted mode */
#define SIM_UI_TICK_MS 200U /* lcd_ui_tick period, like the firmware task */
#define SIM_TAP_MS 120U     /* a tap is held this long */

static void sim_lv_log_cb(const char *buf)
{
    (void) fprintf(stderr, "[LVGL] %s\n", buf);
}

static uint32_t s_virtual_ms;
static uint32_t s_next_ui_tick_ms;

/** Advance the virtual clock by ms, running LVGL and the UI tick. */
static void sim_advance(uint32_t ms)
{
    uint32_t left = ms;
    while (left > 0U)
    {
        const uint32_t step = (left < SIM_STEP_MS) ? left : SIM_STEP_MS;
        s_virtual_ms += step;
        sim_set_time_ms(s_virtual_ms);
        lv_tick_inc(step);
        if (s_virtual_ms >= s_next_ui_tick_ms)
        {
            lcd_ui_tick();
            s_next_ui_tick_ms += SIM_UI_TICK_MS;
        }
        (void) lv_timer_handler();
        left -= step;
    }
}

static void sim_run_script(char *script)
{
    for (char *cmd = strtok(script, ";"); cmd != NULL; cmd = strtok(NULL, ";"))
    {
        char arg1[512];
        int a = 0;
        int b = 0;
        while (*cmd == ' ')
        {
            cmd++;
        }
        if (sscanf(cmd, "scenario %511s", arg1) == 1)
        {
            sim_set_scenario(sim_scenario_from_name(arg1));
        }
        else if (sscanf(cmd, "warp %d", &a) == 1)
        {
            sim_advance((uint32_t) a * 1000U);
        }
        else if (sscanf(cmd, "tap %d %d", &a, &b) == 2)
        {
            sdl_backend_inject_pointer(a, b, true);
            sim_advance(SIM_TAP_MS);
            sdl_backend_inject_pointer(a, b, false);
            sim_advance(SIM_TAP_MS);
            sdl_backend_clear_injection();
            sim_advance(SIM_UI_TICK_MS); /* let the tick show the result */
        }
        else if (strncmp(cmd, "splash show", 11U) == 0)
        {
            lcd_ui_show_splash();
            sim_advance(SIM_UI_TICK_MS);
        }
        else if (strncmp(cmd, "splash hide", 11U) == 0)
        {
            lcd_ui_dismiss_splash();
            sim_advance(SIM_UI_TICK_MS);
        }
        else if (sscanf(cmd, "boot %d %511[^\n]", &a, arg1) == 2)
        {
            lcd_ui_set_boot_progress((uint8_t) a, arg1);
            sim_advance(SIM_UI_TICK_MS);
        }
        else if (sscanf(cmd, "shot %511s", arg1) == 1)
        {
            sim_advance(SIM_UI_TICK_MS);
            if (!sdl_backend_save_ppm(arg1))
            {
                (void) fprintf(stderr, "[sim] cannot write %s\n", arg1);
            }
        }
        else if (*cmd != '\0')
        {
            (void) fprintf(stderr, "[sim] unknown command: %s\n", cmd);
        }
    }
}

int main(int argc, char **argv)
{
    char *script = NULL;

    for (int i = 1; i < argc; i++)
    {
        if ((strcmp(argv[i], "--script") == 0) && ((i + 1) < argc))
        {
            script = strdup(argv[++i]);
        }
    }

    lv_init();
    lv_log_register_print_cb(sim_lv_log_cb);
    sdl_backend_init();

    lcd_ui_err_t err = lcd_ui_init(&sim_sensor_svc, &sim_alarm_svc, &sim_cfg_read, &sim_cfg_write,
                                   &sim_health_snap, &sim_health_report);
    if (err != LCD_UI_ERR_OK)
    {
        (void) fprintf(stderr, "[sim] lcd_ui_init FAILED: err=%d\n", (int) err);
        return 1;
    }

    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        (void) fprintf(
            stderr,
            "[sim] LVGL heap after lcd_ui_init: used %u of %u bytes (%u%%), %u objects' worth\n",
            (unsigned) (mon.total_size - mon.free_size), (unsigned) mon.total_size,
            (unsigned) mon.used_pct, (unsigned) 0U);
    }

    if (script != NULL)
    {
        s_next_ui_tick_ms = SIM_UI_TICK_MS;
        sim_run_script(script);
        free(script);
        return 0;
    }

    /* interactive: real time */
    uint32_t last_ms = sdl_backend_get_ticks_ms();
    uint32_t since_ui = 0U;
    for (;;)
    {
        const uint32_t now_ms = sdl_backend_get_ticks_ms();
        const uint32_t elapsed = now_ms - last_ms;
        last_ms = now_ms;
        s_virtual_ms += elapsed;
        sim_set_time_ms(s_virtual_ms);
        lv_tick_inc(elapsed);
        since_ui += elapsed;
        if (since_ui >= SIM_UI_TICK_MS)
        {
            since_ui = 0U;
            lcd_ui_tick();
        }
        (void) lv_timer_handler();
        sdl_pump();
        usleep(5U * 1000U);
    }
}
