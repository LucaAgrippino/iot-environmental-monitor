/**
 * @file lcd_ui.h
 * @brief LcdUi Application component — display management for the F469 LCD.
 *
 * Manages four runtime screens (Sensor, Status, Alarm, Config) via the
 * Strategy pattern. Pulls data from provider interfaces on each 200 ms
 * tick. Config screen owns a pending/committed split and drives persistence
 * through IConfigManager on user confirmation.
 *
 * DEVIATIONS from companion v0.2:
 *   D1. Module-static singleton (no self parameter in lcd_ui_init) —
 *       consistent with all other application-layer modules in this project.
 *   D2. ILogger removed from public API; logger macros used directly in
 *       lcd_ui.c (same pattern as config_service.c).
 *   D3. lcd_ui_editable_params_t has 7 fields (not 9); display_brightness_pct
 *       and screen_timeout_s omitted (no matching config_params_t entries).
 *   D4. alarms->get_all_states() used instead of a get_active() method
 *       (ialarm_service_t has no get_active; no raised_at timestamps).
 *   D5. sensors->get_snapshot() used instead of get_latest() (actual vtable
 *       method name; sensor values are float not deci-unit int32_t).
 *   D6. cfg_write->apply_block() implemented as N sequential set_param()
 *       calls (iconfig_manager_t has no apply_block method).
 *
 * @see docs/lld/application/lcd-ui-lld.md
 */

#ifndef LCD_UI_H
#define LCD_UI_H

#include <stdbool.h>
#include <stdint.h>

/* ===================================================================== */
/* Dependency headers — real implementations or test stubs              */
/* ===================================================================== */

#ifdef TEST
/* Order matters: later stubs may depend on earlier ones. */
#include "lvgl_stub.h"
#include "graphics_library_stub.h"
#include "sensor_service_stub.h"
#include "alarm_service_stub.h"
#include "config_service_stub.h"
#include "health_monitor_stub.h"
#else
#include "graphics_library/graphics_library.h"
#include "sensor_service/sensor_service.h"
#include "alarm_service/alarm_service.h"
#include "config_service/config_service.h"
#include "health_monitor/health_monitor.h"
#endif /* TEST */

/* ===================================================================== */
/* Private types (full struct definitions exposed in TEST builds so     */
/* test_lcd_ui.c can inspect widget handles and sub-state)             */
/* ===================================================================== */

#ifdef TEST
#include "lcd_ui/screen_internal.h"
#endif /* TEST */

/* ===================================================================== */
/* Error codes                                                          */
/* ===================================================================== */

typedef enum
{
    LCD_UI_ERR_OK = 0,
    LCD_UI_ERR_INVALID_ARG = 1,   /**< NULL required dependency.         */
    LCD_UI_ERR_ALREADY_INIT = 2,  /**< lcd_ui_init() called twice.       */
    LCD_UI_ERR_GRAPHICS_INIT = 3, /**< graphics_init() not yet OK.       */
} lcd_ui_err_t;

/* ===================================================================== */
/* Public API                                                           */
/* ===================================================================== */

/**
 * @brief Initialise LcdUi: wire providers, build four screen widget
 *        hierarchies, register LVGL event callbacks, enter SCR_SENSOR
 *        with the "Waiting for data" overlay shown.
 *
 * Must be called after graphics_init() succeeds.
 * Must be called from LcdUiTask (same task that owns LVGL).
 *
 * @param[in] sensors   ISensorService. Non-NULL.
 * @param[in] alarms    IAlarmService.  Non-NULL.
 * @param[in] cfg_read  IConfigProvider. Non-NULL.
 * @param[in] cfg_write IConfigManager.  Non-NULL.
 * @param[in] health    IHealthSnapshot. Non-NULL.
 * @param[in] report    IHealthReport.   Non-NULL.
 * @return LCD_UI_ERR_OK on success; non-OK otherwise.
 * @note Threading: LcdUiTask only.
 */
lcd_ui_err_t lcd_ui_init(const isensor_service_t *sensors, const ialarm_service_t *alarms,
                         const iconfig_provider_t *cfg_read, const iconfig_manager_t *cfg_write,
                         const ihealth_snapshot_t *health, const ihealth_report_t *report);

/**
 * @brief Drive one UI refresh — update the current screen's data bindings.
 *
 * In firmware, called exclusively from lcd_ui_task_body().
 * In the desktop simulator, called from main() via an LVGL timer so that
 * FreeRTOS is not required.
 */
void lcd_ui_tick(void);

/**
 * @brief LcdUiTask entry point — 200 ms refresh loop.
 *
 * Loop: vTaskDelayUntil → graphics_process() → lcd_ui_tick().
 *
 * @param[in] arg  Unused (NULL for FreeRTOS xTaskCreate compatibility).
 * @note Threading: entry point for LcdUiTask; never call directly.
 */
void lcd_ui_task_body(void *arg);

/* ===================================================================== */
/* Test-only API                                                        */
/* ===================================================================== */

#ifdef TEST
#define LCD_UI_TEST_VISIBLE /**< Removes static from internal functions. */

/** Reset all LcdUi internal state to post-BSS values. Call from setUp(). */
void lcd_ui_reset_for_test(void);

/** Return pointer to the module-static lcd_ui_t instance. */
lcd_ui_t *lcd_ui_test_get_instance(void);

/** Invoke the tab-change callback as if the user tapped tab new_idx. */
void lcd_ui_test_fire_tab_change(uint16_t new_idx);

/** Invoke the Config field-tapped callback (VIEWING→EDITING transition). */
void lcd_ui_test_fire_cfg_field_tapped(uint32_t field_idx);

/** Invoke the Config field-changed callback for the given spinbox index. */
void lcd_ui_test_fire_cfg_field_changed(uint32_t field_idx);

/** Invoke the Apply button callback. */
void lcd_ui_test_fire_apply_tapped(void);

/** Invoke the Confirm button callback. */
void lcd_ui_test_fire_confirm_tapped(void);

/** Invoke the Cancel button callback. */
void lcd_ui_test_fire_cancel_tapped(void);

/** Invoke the confirm-timeout timer callback. */
void lcd_ui_test_fire_confirm_timeout(void);

#else
#define LCD_UI_TEST_VISIBLE static
#endif /* TEST */

#endif /* LCD_UI_H */
