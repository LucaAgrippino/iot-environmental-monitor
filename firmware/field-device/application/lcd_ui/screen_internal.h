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

#define SCR_SENSOR (0U)
#define SCR_TREND (1U)
#define SCR_STATUS (2U)
#define SCR_ALARM (3U)
#define SCR_CONFIG (4U)
#define SCR_COUNT (5U)

/** Requests to the splash overlay, written by any task and carried out by lcd_ui_tick(). */
#define SPLASH_REQ_NONE (0U)
#define SPLASH_REQ_SHOW (1U)
#define SPLASH_REQ_HIDE (2U)
/** Longest boot step name kept, with its terminator. */
#define SPLASH_STEP_MAX (40U)

/** Physical sensors shown on the Sensors, Trend and Alarms screens (03_SCREEN_SPECS.md: S1..S3). */
#define SENSOR_DISPLAY_COUNT (3U)

/* ===================================================================== */
/* Editable configuration parameters                                      */
/* ===================================================================== */

#define N_EDITABLE_FIELDS (7U)

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

struct screen_s
{
    void (*on_enter)(screen_t *self);
    void (*on_exit)(screen_t *self);
    void (*on_refresh)(screen_t *self);
};

/* ===================================================================== */
/* Screen contexts                                                        */
/* ===================================================================== */

typedef struct
{
    screen_t base;
    sensor_card_t cards[SENSOR_DISPLAY_COUNT];
    bool ever_valid[SENSOR_DISPLAY_COUNT];    /**< A valid reading was seen since boot. */
    int32_t last_value[SENSOR_DISPLAY_COUNT]; /**< Last valid reading, shown faded on error. */
    uint32_t last_valid_epoch[SENSOR_DISPLAY_COUNT]; /**< Its timestamp. */
    uint32_t spark_gen_seen;                         /**< History generation drawn last. */
} sensor_screen_t;

typedef struct
{
    screen_t base;
    trend_panel_t panel[SENSOR_DISPLAY_COUNT];
    uint32_t gen_seen;
} trend_screen_t;

#define STATUS_ROWS (16U) /* SENSORS 4, SYSTEM 3, MODBUS 4, DEVICE 2, RESOURCES 3 */
#define STATUS_SECTIONS (5U)

typedef struct
{
    screen_t base;
    lv_obj_t *eyebrow[STATUS_SECTIONS];
    status_row_t row[STATUS_ROWS];
    lv_obj_t *banner_box;
    lv_obj_t *banner_text;
    lv_obj_t *banner_time;
    bool banner_shown;
    uint32_t last_refresh_ms;
} status_screen_t;

typedef struct
{
    screen_t base;
    alarm_card_t card[SENSOR_DISPLAY_COUNT];
    lv_obj_t *empty_circle;
    lv_obj_t *empty_icon;
    lv_obj_t *empty_title;
    lv_obj_t *empty_sub;
    lv_obj_t *empty_last;
    uint32_t triggered_epoch[SENSOR_DISPLAY_COUNT]; /**< When the alarm was first seen active. */
    int32_t prev_state[SENSOR_DISPLAY_COUNT];       /**< alarm_state_t last seen, -1 unknown. */
    uint32_t last_clear_epoch;                      /**< When the last alarm went away; 0 never. */
    uint32_t active_count;
    uint32_t last_refresh_ms;
} alarm_screen_t;

typedef struct
{
    screen_t base;
    lv_obj_t *list;                          /**< Scrolling list of the fields. */
    config_field_t field[N_EDITABLE_FIELDS]; /**< In display order. */
    numpad_t numpad;
    button_t cancel_btn;
    button_t apply_btn;
    confirm_modal_t modal;
    cfg_screen_state_t sub_state;
    lcd_ui_editable_params_t committed;
    lcd_ui_editable_params_t pending;
    bool entry_invalid[N_EDITABLE_FIELDS]; /**< Display order. */
    bool entry_edited[N_EDITABLE_FIELDS];
    char entry_text[N_EDITABLE_FIELDS][NUMPAD_TEXT_MAX + 1U];
    int32_t focus; /**< Display index of the focused field, -1 none. */
    lv_timer_t *confirm_timeout_timer;
    const iconfig_provider_t *cfg_read;
    const iconfig_manager_t *cfg_write;
    const ihealth_report_t *report;
} config_screen_t;

/* ===================================================================== */
/* lcd_ui_t: module context                                               */
/* ===================================================================== */

typedef struct lcd_ui
{
    bool initialised;
    tab_bar_t tab_bar;
    header_bar_t header;
    footer_bar_t footer;
    lv_obj_t *content[SCR_COUNT]; /* per-screen panels: y = 104, h = 340 */
    lv_obj_t *toast_label;
    uint32_t toast_until_ms;
    screen_t *current;
    uint16_t current_tab_idx;
    screen_t *screens[SCR_COUNT];
    sensor_screen_t sensor_screen;
    trend_screen_t trend_screen;
    status_screen_t status_screen;
    alarm_screen_t alarm_screen;
    config_screen_t config_screen;
    const isensor_service_t *sensors;
    const ialarm_service_t *alarms;
    const iconfig_provider_t *cfg_read;
    const iconfig_manager_t *cfg_write;
    const ihealth_snapshot_t *health;
    const ihealth_report_t *report;
    sensor_snapshot_t snap; /**< Snapshot taken at the start of the current tick. */
    bool snap_ok;
    uint32_t now_ms;        /**< Millisecond clock of the current tick. */
    uint32_t hist_gen;      /**< Increments whenever the history stores a sample. */
    uint32_t last_cycle;    /**< sensor cycle_count seen last. */
    uint32_t last_cycle_ms; /**< When it changed. */
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
