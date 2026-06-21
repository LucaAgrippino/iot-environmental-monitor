/**
 * @file screen_internal.h
 * @brief Private types for LcdUi — screen strategy interface and concrete
 *        screen context structs. Not to be included by other components.
 *
 * Included by lcd_ui.c in all builds and by lcd_ui.h in TEST builds
 * (so test_lcd_ui.c can access widget handles for assertion).
 *
 * @see docs/lld/application/lcd-ui-lld.md §5, §6, §7
 */

#ifndef SCREEN_INTERNAL_H
#define SCREEN_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

/* ===================================================================== */
/* Screen identifiers and count                                         */
/* ===================================================================== */

#define SCR_SENSOR (0U)
#define SCR_STATUS (1U)
#define SCR_ALARM (2U)
#define SCR_CONFIG (3U)
#define SCR_COUNT (4U)

/* ===================================================================== */
/* Display sensor indexing (Sensor screen only — 3 physical sensors)   */
/* ===================================================================== */

#define SENSOR_DISPLAY_COUNT (3U)

/* ===================================================================== */
/* Editable config parameters (companion §7.4 — 7 of 9 original fields) */
/* DEVIATION: display_brightness_pct and screen_timeout_s dropped       */
/* (no corresponding entries in config_params_t / iconfig_manager_t).  */
/* ===================================================================== */

#define N_EDITABLE_FIELDS (7U)

typedef struct
{
    uint32_t polling_interval_ms;     /**< [100, 60000] ms              */
    int16_t temp_alarm_hi_centi_c;    /**< centi-°C ×100; default 4000  */
    int16_t temp_alarm_lo_centi_c;    /**< centi-°C ×100; default 0     */
    uint16_t humidity_alarm_hi_centi; /**< centi-%RH ×100; default 8000 */
    uint16_t humidity_alarm_lo_centi; /**< centi-%RH ×100; default 2000 */
    uint16_t pressure_alarm_hi_deci;  /**< deci-hPa ×10; default 10500  */
    uint16_t pressure_alarm_lo_deci;  /**< deci-hPa ×10; default 9500   */
} lcd_ui_editable_params_t;

/* ===================================================================== */
/* Config sub-state machine                                             */
/* ===================================================================== */

typedef enum
{
    CFG_STATE_VIEWING = 0,
    CFG_STATE_EDITING = 1,
    CFG_STATE_CONFIRMING = 2,
} cfg_screen_state_t;

/* ===================================================================== */
/* Status screen metric label count                                     */
/* ===================================================================== */

#ifdef TEST
#define LCD_UI_STATUS_STACK_LABELS HEALTH_STUB_TASK_COUNT
#else
#define LCD_UI_STATUS_STACK_LABELS HEALTH_TASK_COUNT
#endif
#define STATUS_METRIC_COUNT (7U + LCD_UI_STATUS_STACK_LABELS)

/* ===================================================================== */
/* screen_t — Strategy interface (companion §5)                         */
/* ===================================================================== */

typedef struct screen screen_t;

struct screen
{
    void (*on_enter)(screen_t *self);
    void (*on_exit)(screen_t *self);
    void (*on_refresh)(screen_t *self);
};

/* ===================================================================== */
/* Concrete screen context structs (base is always the first member)    */
/* ===================================================================== */

typedef struct
{
    screen_t base;
    lv_obj_t *waiting_label;
    lv_obj_t *value_label[SENSOR_DISPLAY_COUNT];
    lv_obj_t *unit_label[SENSOR_DISPLAY_COUNT];
    lv_obj_t *timestamp_label[SENSOR_DISPLAY_COUNT];
    lv_obj_t *icon_label[SENSOR_DISPLAY_COUNT];
    bool first_valid_received;
    const isensor_service_t *sensors;
} sensor_screen_t;

typedef struct
{
    screen_t base;
    lv_obj_t *metric_label[STATUS_METRIC_COUNT];
    const ihealth_snapshot_t *health;
} status_screen_t;

typedef struct
{
    screen_t base;
    lv_obj_t *list_widget;
    lv_obj_t *no_alarms_label;
    const ialarm_service_t *alarms;
} alarm_screen_t;

typedef struct
{
    screen_t base;
    cfg_screen_state_t sub_state;
    lcd_ui_editable_params_t committed;
    lcd_ui_editable_params_t pending;
    lv_obj_t *spinbox[N_EDITABLE_FIELDS];
    lv_obj_t *err_label[N_EDITABLE_FIELDS];
    lv_obj_t *field_label[N_EDITABLE_FIELDS];
    lv_obj_t *cancel_btn;
    lv_obj_t *apply_btn;
    lv_obj_t *confirm_dialog;
    lv_timer_t *confirm_timeout_timer;
    const iconfig_provider_t *cfg_read;
    const iconfig_manager_t *cfg_write;
    const ihealth_report_t *report;
} config_screen_t;

/* ===================================================================== */
/* lcd_ui_t — top-level module context                                  */
/* ===================================================================== */

typedef struct lcd_ui
{
    bool initialised;
    lv_obj_t *tabview;
    lv_obj_t *toast_label;
    screen_t *current;
    uint16_t current_tab_idx;
    screen_t *screens[SCR_COUNT];
    sensor_screen_t sensor_screen;
    status_screen_t status_screen;
    alarm_screen_t alarm_screen;
    config_screen_t config_screen;
    const isensor_service_t *sensors;
    const ialarm_service_t *alarms;
    const iconfig_provider_t *cfg_read;
    const iconfig_manager_t *cfg_write;
    const ihealth_snapshot_t *health;
    const ihealth_report_t *report;
} lcd_ui_t;

#endif /* SCREEN_INTERNAL_H */
