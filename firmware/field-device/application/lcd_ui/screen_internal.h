/**
 * @file screen_internal.h
 * @brief Private types of LcdUi: the screen strategy interface and the five screen contexts.
 *
 * Included by lcd_ui.c in every build and by lcd_ui.h in TEST builds, so the unit tests can reach
 * widget handles. Not for other components.
 *
 * @see docs/lld/application/lcd-ui-lld.md
 */

#ifndef SCREEN_INTERNAL_H
#define SCREEN_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "lcd_ui/ui_format.h"
#include "lcd_ui/widgets/alarm_card.h"
#include "lcd_ui/widgets/button.h"
#include "lcd_ui/widgets/config_field.h"
#include "lcd_ui/widgets/confirm_modal.h"
#include "lcd_ui/widgets/footer_bar.h"
#include "lcd_ui/widgets/header_bar.h"
#include "lcd_ui/widgets/numpad.h"
#include "lcd_ui/widgets/sensor_card.h"
#include "lcd_ui/widgets/splash_view.h"
#include "lcd_ui/widgets/status_row.h"
#include "lcd_ui/widgets/tab_bar.h"
#include "lcd_ui/widgets/trend_panel.h"

/* ===================================================================== */
/* Screen identifiers: the order of the tab bar (style-guide.md section 5) */
/* ===================================================================== */

#define SCR_SENSOR (0U) /**< Index of the sensor screen. */
#define SCR_TREND (1U)  /**< Index of the trend screen. */
#define SCR_STATUS (2U) /**< Index of the status screen. */
#define SCR_ALARM (3U)  /**< Index of the alarm screen. */
#define SCR_CONFIG (4U) /**< Index of the configuration screen. */
#define SCR_COUNT (5U)  /**< Number of screens. */

/** Requests to the splash overlay, written by any task and carried out by lcd_ui_tick(). */
#define SPLASH_REQ_NONE (0U)
#define SPLASH_REQ_SHOW (1U) /**< Request to show the splash screen. */
#define SPLASH_REQ_HIDE (2U) /**< Request to hide the splash screen. */
/** Longest boot step name kept, with its terminator. */
#define SPLASH_STEP_MAX (40U)

/** Physical sensors shown on the Sensors, Trend and Alarms screens (03_SCREEN_SPECS.md: S1..S3). */
#define SENSOR_DISPLAY_COUNT (3U)

/* ===================================================================== */
/* Editable configuration parameters                                      */
/* ===================================================================== */

#define N_EDITABLE_FIELDS                                                                          \
    (7U) /**< Number of fields the user can edit on the configuration screen. */

/** @brief The parameters the user can edit on the configuration screen. */
typedef struct
{
    uint32_t polling_interval_ms;     /**< [100, 10000] ms               */
    int16_t temp_alarm_hi_centi_c;    /**< centi-degC; default 4000      */
    int16_t temp_alarm_lo_centi_c;    /**< centi-degC; default 0         */
    uint16_t humidity_alarm_hi_centi; /**< centi-%RH; default 8000       */
    uint16_t humidity_alarm_lo_centi; /**< centi-%RH; default 2000       */
    uint16_t pressure_alarm_hi_deci;  /**< deci-hPa; default 10500       */
    uint16_t pressure_alarm_lo_deci;  /**< deci-hPa; default 9500        */
} lcd_ui_editable_params_t;

/** Config screen state: no edits, edits pending, or the confirm modal open. */
typedef enum
{
    CFG_STATE_VIEWING = 0,
    CFG_STATE_EDITING = 1,
    CFG_STATE_CONFIRMING = 2,
} cfg_screen_state_t;

/* ===================================================================== */
/* screen_t: strategy interface                                           */
/* ===================================================================== */

typedef struct screen_s screen_t;

/** @brief Common part of every screen: its root object and the functions the UI calls. */
struct screen_s
{
    void (*on_enter)(screen_t *self);   /**< Called when the screen becomes the current one. */
    void (*on_exit)(screen_t *self);    /**< Called when the screen stops being the current one. */
    void (*on_refresh)(screen_t *self); /**< Called periodically to refresh the content. */
};

/* ===================================================================== */
/* Screen contexts                                                        */
/* ===================================================================== */

/** @brief Sensor screen: one card per sensor. */
typedef struct
{
    screen_t base;                             /**< Common screen part. */
    sensor_card_t cards[SENSOR_DISPLAY_COUNT]; /**< One card per sensor. */
    bool ever_valid[SENSOR_DISPLAY_COUNT];     /**< A valid reading was seen since boot. */
    int32_t last_value[SENSOR_DISPLAY_COUNT];  /**< Last valid reading, shown faded on error. */
    uint32_t last_valid_epoch[SENSOR_DISPLAY_COUNT]; /**< Its timestamp. */
    uint32_t spark_gen_seen;                         /**< History generation drawn last. */
} sensor_screen_t;

/** @brief Trend screen: one chart panel per sensor. */
typedef struct
{
    screen_t base;                             /**< Common screen part. */
    trend_panel_t panel[SENSOR_DISPLAY_COUNT]; /**< One chart panel per sensor. */
    uint32_t gen_seen; /**< Generation of the history already drawn, to skip a redraw when nothing
                          changed. */
} trend_screen_t;

#define STATUS_ROWS (16U)    /**< SENSORS 4, SYSTEM 3, MODBUS 4, DEVICE 2, RESOURCES 3 */
#define STATUS_SECTIONS (5U) /**< Number of sections on the status screen. */

/** @brief Status screen: system, link and alarm rows. */
typedef struct
{
    screen_t base;                      /**< Common screen part. */
    lv_obj_t *eyebrow[STATUS_SECTIONS]; /**< Section titles. */
    status_row_t row[STATUS_ROWS];      /**< Status rows. */
    lv_obj_t *banner_box;               /**< Box of the alarm banner. */
    lv_obj_t *banner_text;              /**< Text of the alarm banner. */
    lv_obj_t *banner_time;              /**< Time shown in the alarm banner. */
    bool banner_shown;                  /**< True while the banner is visible. */
    uint32_t last_refresh_ms;           /**< Time of the last refresh, in ms. */
} status_screen_t;

/** @brief Alarm screen: one card per active alarm, or the empty view. */
typedef struct
{
    screen_t base;                           /**< Common screen part. */
    alarm_card_t card[SENSOR_DISPLAY_COUNT]; /**< One card per sensor. */
    lv_obj_t *empty_circle;                  /**< Circle of the "no active alarm" view. */
    lv_obj_t *empty_icon;                    /**< Icon of the "no active alarm" view. */
    lv_obj_t *empty_title;                   /**< Title of the "no active alarm" view. */
    lv_obj_t *empty_sub;                     /**< Subtitle of the "no active alarm" view. */
    lv_obj_t *empty_last;                    /**< Last-alarm line of the "no active alarm" view. */
    uint32_t triggered_epoch[SENSOR_DISPLAY_COUNT]; /**< When the alarm was first seen active. */
    int32_t prev_state[SENSOR_DISPLAY_COUNT];       /**< alarm_state_t last seen, -1 unknown. */
    uint32_t last_clear_epoch;                      /**< When the last alarm went away; 0 never. */
    uint32_t active_count;                          /**< Number of alarms shown. */
    uint32_t last_refresh_ms;                       /**< Time of the last refresh, in ms. */
} alarm_screen_t;

/** @brief Configuration screen: editable fields, keypad, and the apply/cancel flow. */
typedef struct
{
    screen_t base;                           /**< Common screen part. */
    lv_obj_t *list;                          /**< Scrolling list of the fields. */
    config_field_t field[N_EDITABLE_FIELDS]; /**< In display order. */
    numpad_t numpad;                         /**< Numeric keypad. */
    button_t cancel_btn;                     /**< CANCEL button of the action bar. */
    button_t apply_btn;                      /**< APPLY button of the action bar. */
    confirm_modal_t modal;                   /**< Confirmation dialog. */
    cfg_screen_state_t
        sub_state; /**< Current sub-state of the screen (editing, asking for confirmation). */
    lcd_ui_editable_params_t committed;    /**< Values currently stored in the configuration. */
    lcd_ui_editable_params_t pending;      /**< Values the user has typed and not applied yet. */
    bool entry_invalid[N_EDITABLE_FIELDS]; /**< Display order. */
    bool entry_edited[N_EDITABLE_FIELDS];  /**< True for each field the user changed. */
    char entry_text[N_EDITABLE_FIELDS][NUMPAD_TEXT_MAX + 1U]; /**< Text typed in each field. */
    int32_t focus; /**< Display index of the focused field, -1 none. */
    lv_timer_t *
        confirm_timeout_timer; /**< Timer that discards the edit when the dialog is not answered. */
    const iconfig_provider_t *cfg_read; /**< Read access to the configuration. */
    const iconfig_manager_t *cfg_write; /**< Write access to the configuration. */
    const ihealth_report_t *report;     /**< Health event sink, to report a configuration change. */
} config_screen_t;

/* ===================================================================== */
/* lcd_ui_t: module context                                               */
/* ===================================================================== */

/** @brief State of the display user interface. */
typedef struct lcd_ui
{
    bool initialised;                   /**< True once lcd_ui_init() has succeeded. */
    tab_bar_t tab_bar;                  /**< Tab bar at the top. */
    header_bar_t header;                /**< Header bar. */
    footer_bar_t footer;                /**< Footer bar. */
    lv_obj_t *content[SCR_COUNT];       /**< per-screen panels: y = 104, h = 340 */
    lv_obj_t *toast_label;              /**< Label of the toast message. */
    uint32_t toast_until_ms;            /**< Time at which the toast disappears, in ms. */
    screen_t *current;                  /**< Screen being shown. */
    uint16_t current_tab_idx;           /**< Index of the active tab. */
    screen_t *screens[SCR_COUNT];       /**< All screens, by index. */
    sensor_screen_t sensor_screen;      /**< Sensor screen. */
    trend_screen_t trend_screen;        /**< Trend screen. */
    status_screen_t status_screen;      /**< Status screen. */
    alarm_screen_t alarm_screen;        /**< Alarm screen. */
    config_screen_t config_screen;      /**< Configuration screen. */
    const isensor_service_t *sensors;   /**< Sensor readings source. */
    const ialarm_service_t *alarms;     /**< Alarm state source. */
    const iconfig_provider_t *cfg_read; /**< Read access to the configuration. */
    const iconfig_manager_t *cfg_write; /**< Write access to the configuration. */
    const ihealth_snapshot_t *health;   /**< Health snapshot source. */
    const ihealth_report_t *report;     /**< Health event sink. */
    sensor_snapshot_t snap;             /**< Snapshot taken at the start of the current tick. */
    bool snap_ok;                       /**< True when the last read of the sensors succeeded. */
    uint32_t now_ms;                    /**< Millisecond clock of the current tick. */
    uint32_t hist_gen;                  /**< Increments whenever the history stores a sample. */
    uint32_t last_cycle;                /**< sensor cycle_count seen last. */
    uint32_t last_cycle_ms;             /**< When it changed. */
    const char
        *fault_text;     /**< Most important soft fault, NULL when none (banner and ALARM pill). */
    bool modbus_seen_ok; /**< The Modbus link was up once: only then is "down" a fault. */
    bool config_failed;  /**< The last configuration write to flash failed. */
    bool link_down;      /**< The Modbus link was up and is lost. */
    uint32_t fault_ms;   /**< When the faults were last evaluated. */
    volatile uint16_t requested_screen; /**< lcd_ui_select_screen(): screen + 1, 0 for none. */
    splash_view_t splash;               /**< Boot overlay, hidden until requested. */
    volatile uint8_t splash_req;        /**< SPLASH_REQ_*; cleared when carried out. */
    volatile uint8_t boot_pct;          /**< Progress from lcd_ui_set_boot_progress(). */
    volatile uint32_t boot_seq;         /**< Incremented per progress update. */
    uint32_t boot_seq_seen;             /**< boot_seq already drawn. */
    char boot_step[SPLASH_STEP_MAX];    /**< Step name from lcd_ui_set_boot_progress(). */
} lcd_ui_t;

#endif /* SCREEN_INTERNAL_H */
