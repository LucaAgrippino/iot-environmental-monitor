/**
 * @file lv_tick_source.h
 * @brief Tick source declaration consumed by LVGL via LV_TICK_CUSTOM.
 *
 * Returns elapsed milliseconds since startup. The implementation is provided
 * by the host (simulator: SDL_GetTicks; embedded: FreeRTOS xTaskGetTickCount
 * scaled to ms). Both implementations are defined in their respective main
 * translation units; this header only declares the contract.
 */
#ifndef LV_TICK_SOURCE_H
#define LV_TICK_SOURCE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t lv_tick_source_get_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_TICK_SOURCE_H */
