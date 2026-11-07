/*
 * SDL2 display and input backend for the lcd-ui simulator.
 *
 * Adapted from lv_drivers/sdl/sdl.c and sdl_common.c
 * (upstream lvgl/lv_drivers, tag release/v8.3, MIT licence).
 * Original authors: LVGL contributors.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdl_backend.h"

#include "lvgl.h"

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Physical window size (logical × 2 upscale) */
#define WINDOW_W 800
#define WINDOW_H 480
#define SDL_SCALE 2.0f

/* Partial render buffer: 50 KB ÷ sizeof(lv_color_t).
 * With LV_COLOR_DEPTH 32 → 4 bytes/px → 12 800 pixels. */
#define RENDER_BUF_PIXELS (50U * 1024U / sizeof(lv_color_t))

static SDL_Window   *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture  *s_texture;

static lv_color_t s_buf[RENDER_BUF_PIXELS];

static bool s_quit_requested;

/* ---------- LVGL display flush callback ---------------------------------- */

static void sdl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                         lv_color_t *color_p)
{
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;

    SDL_Rect rect = {
        .x = area->x1,
        .y = area->y1,
        .w = w,
        .h = h,
    };
    SDL_UpdateTexture(s_texture, &rect, color_p,
                      (int)(w * sizeof(lv_color_t)));
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);

    lv_disp_flush_ready(drv);
}

/* ---------- LVGL pointer (mouse) read callback --------------------------- */

static void sdl_mouse_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;

    int mx, my;
    uint32_t buttons = SDL_GetMouseState(&mx, &my);

    data->point.x = (lv_coord_t)(mx / (int)SDL_SCALE);
    data->point.y = (lv_coord_t)(my / (int)SDL_SCALE);
    data->state   = (buttons & SDL_BUTTON_LMASK) ? LV_INDEV_STATE_PR
                                                  : LV_INDEV_STATE_REL;
}

/* ---------- Public API --------------------------------------------------- */

void sdl_backend_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return;
    }

    s_window = SDL_CreateWindow(
        "IoT Environmental Monitor — LCD simulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        (int)(WINDOW_W * SDL_SCALE), (int)(WINDOW_H * SDL_SCALE),
        SDL_WINDOW_SHOWN);
    if (!s_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1,
                                    SDL_RENDERER_ACCELERATED |
                                    SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer) {
        s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_SOFTWARE);
    }
    SDL_RenderSetScale(s_renderer, SDL_SCALE, SDL_SCALE);

    s_texture = SDL_CreateTexture(s_renderer,
                                  SDL_PIXELFORMAT_ARGB8888,   /* matches LV_COLOR_DEPTH 32 */
                                  SDL_TEXTUREACCESS_STREAMING,
                                  WINDOW_W, WINDOW_H);
    if (!s_texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return;
    }

    /* Register LVGL display driver */
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, s_buf, NULL, RENDER_BUF_PIXELS);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res   = WINDOW_W;
    disp_drv.ver_res   = WINDOW_H;
    disp_drv.flush_cb  = sdl_flush_cb;
    disp_drv.draw_buf  = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /* Register LVGL pointer input device */
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = sdl_mouse_read_cb;
    lv_indev_drv_register(&indev_drv);

    s_quit_requested = false;
}

void sdl_pump(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            SDL_DestroyTexture(s_texture);
            SDL_DestroyRenderer(s_renderer);
            SDL_DestroyWindow(s_window);
            SDL_Quit();
            s_quit_requested = true;
            /* Exit cleanly so the main loop can terminate */
            return;
        }
    }
    if (s_quit_requested) {
        return;
    }
}

uint32_t sdl_backend_get_ticks_ms(void)
{
    return (uint32_t)SDL_GetTicks();
}
