/**
 * @file lv_conf.h
 * @brief LVGL v8 configuration for the STM32F469I-DISCO field device.
 *
 * Placed next to graphics_library.{h,c} so the configuration travels
 * with the component (GL-D4). Only the settings relevant to this project
 * are overridden from the LVGL defaults; all other settings retain
 * their library defaults.
 *
 * Companion: docs/lld/middleware/graphics-library.md §8
 */

/* LVGL v8 requires this sentinel to validate the config file. */
#if 1

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* ===================================================================== */
/* Colour format (GL-D2)                                                 */
/* ===================================================================== */

/* 32 bpp ARGB8888 — matches the OTM8009A framebuffer pipeline. */
#define LV_COLOR_DEPTH 32

/* ===================================================================== */
/* Display resolution                                                    */
/* ===================================================================== */

#define LV_HOR_RES_MAX 800
#define LV_VER_RES_MAX 480

/* ===================================================================== */
/* Memory (P5 — no dynamic allocation)                                   */
/* ===================================================================== */

/* Use LVGL's internal static heap (no malloc). */
#define LV_MEM_CUSTOM 0
/* 32 KB — simulator overrides to 512 KB via -DLV_MEM_SIZE=524288. */
#ifndef LV_MEM_SIZE
#define LV_MEM_SIZE (32768U)
#endif

/* ===================================================================== */
/* Tick source (§6.4 — driven by lv_tick_inc from FreeRTOS timer)        */
/* ===================================================================== */

/* 0 = application drives the tick via lv_tick_inc() calls.
 * SysTick is owned by FreeRTOS; do not use LV_TICK_CUSTOM = 1. */
#define LV_TICK_CUSTOM 0

/* ===================================================================== */
/* Logging                                                               */
/* ===================================================================== */

#define LV_USE_LOG 1

/* Log level: show warnings and above during integration; lower to
 * LV_LOG_LEVEL_ERROR for production. */
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN

/* Route LVGL log output via lv_log_register_print_cb() in graphics_init.
 * Setting LV_LOG_PRINTF to 0 disables the default printf path so the
 * registered callback receives all log messages. */
#define LV_LOG_PRINTF 0

/* ===================================================================== */
/* Display driver                                                        */
/* ===================================================================== */

/* LVGL internal redraw period in ms — screen content refreshed at ~33 Hz. */
#define LV_DISP_DEF_REFR_PERIOD 30

/* ===================================================================== */
/* Input device                                                          */
/* ===================================================================== */

/* Touch polling period in ms — aligns with LVGL's internal debounce. */
#define LV_INDEV_DEF_READ_PERIOD 30

/* ===================================================================== */
/* Animations                                                            */
/* ===================================================================== */

/* Required for screen transitions in LcdUi. Disable if SRAM is tight. */
#define LV_USE_ANIMATION 1

/* ===================================================================== */
/* GPU acceleration (GL-O3 — benchmark at integration)                   */
/* ===================================================================== */

/* Enable DMA2D blending. STM32F469 supports DMA2D natively. Benchmark
 * lv_task_handler() time with and without to confirm the gain. */
#define LV_USE_GPU_STM32_DMA2D 0

/* ===================================================================== */
/* Theme — disabled; LcdUi owns all styles via theme_init()             */
/* ===================================================================== */

/* Disable the built-in default theme. LcdUi applies its own
 * design-token styles in theme_init(). Leaving this at the LVGL default
 * (1) causes the grey default theme to re-apply on every
 * lv_obj_remove_style_all() call (via LV_EVENT_STYLE_CHANGED), overriding
 * our custom dark styles. */
#define LV_USE_THEME_DEFAULT 0

/* ===================================================================== */
/* Feature guards — widgets used by LcdUi                               */
/* ===================================================================== */

#define LV_USE_LABEL 1
#define LV_USE_BAR 1
#define LV_USE_ARC 1
#define LV_USE_BTN 1
#define LV_USE_MSGBOX 0
#define LV_USE_TABVIEW 1
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_SPAN 0

#endif /* LV_CONF_H */
#endif /* sentinel */
