/*
 * SDL2 display and input backend for the lcd-ui simulator.
 *
 * Adapted from lv_drivers/sdl/sdl.c and sdl_common.c
 * (upstream lvgl/lv_drivers, tag release/v8.3, MIT licence).
 * Original authors: LVGL contributors.
 */

#ifndef SDL_BACKEND_H
#define SDL_BACKEND_H

#include <stdint.h>

void     sdl_backend_init(void);
void     sdl_pump(void);
uint32_t sdl_backend_get_ticks_ms(void);

#endif /* SDL_BACKEND_H */
