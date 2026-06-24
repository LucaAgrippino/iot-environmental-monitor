/**
 * @file lvgl_stub.h
 * @brief Minimal LVGL type stubs for host-side unit tests.
 *
 * Provides opaque pointer targets for lv_disp_t and lv_indev_t so that
 * graphics_library_stub.h can declare get_display() / get_indev() without
 * pulling in real LVGL headers.
 */

#ifndef LVGL_STUB_H
#define LVGL_STUB_H

/* Opaque struct targets — tests never dereference these. */
typedef struct lv_disp_s  lv_disp_t;
typedef struct lv_indev_s lv_indev_t;

#endif /* LVGL_STUB_H */
