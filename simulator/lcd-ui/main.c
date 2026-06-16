#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include "lvgl.h"
#include "sdl_backend.h"

/* Forward declaration of the tick adapter the firmware will also use. */
uint32_t lv_tick_source_get_ms(void);

int main(void)
{
    lv_init();
    sdl_backend_init();

    /* Hello LVGL — solid background + version label, centred. */
    lv_obj_t * scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);

    lv_obj_t * label = lv_label_create(scr);
    char buf[64];
    snprintf(buf, sizeof(buf), "LVGL %d.%d.%d — simulator OK",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    lv_label_set_text(label, buf);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE6E6E6), LV_PART_MAIN);
    lv_obj_center(label);

    for (;;) {
        lv_timer_handler();
        sdl_pump();
        usleep(5 * 1000);
    }

    return 0;
}

/* Stub tick source — uses SDL's millisecond counter via sdl_backend. */
uint32_t lv_tick_source_get_ms(void)
{
    extern uint32_t sdl_backend_get_ticks_ms(void);
    return sdl_backend_get_ticks_ms();
}
