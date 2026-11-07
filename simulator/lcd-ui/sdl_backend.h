/*
 * SDL2 display and input backend for the lcd-ui simulator.
 *
 * Adapted from lv_drivers/sdl/sdl.c and sdl_common.c
 * (upstream lvgl/lv_drivers, tag release/v8.3, MIT licence).
 * Original authors: LVGL contributors.
 */

#ifndef SDL_BACKEND_H
#define SDL_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

void     sdl_backend_init(void);
void     sdl_pump(void);
uint32_t sdl_backend_get_ticks_ms(void);

/** Press (true) or release the pointer at logical coordinates; overrides the mouse until cleared. */
void     sdl_backend_inject_pointer(int x, int y, bool pressed);

/** Stop overriding the mouse. */
void     sdl_backend_clear_injection(void);

/**
 * Write the last rendered frame as a binary PPM (P6) file.
 * @return true on success.
 */
bool     sdl_backend_save_ppm(const char *path);

#endif /* SDL_BACKEND_H */
