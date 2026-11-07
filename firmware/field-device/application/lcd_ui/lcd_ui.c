/**
 * @file lcd_ui.c
 * @brief LcdUi application component: the five-screen display manager of the Field Device.
 *
 * Screens (tab bar order): Sensors, Trend, Status, Alarms, Config. The visual design is the project
 * design pack, docs/lcd-ui-design (style-guide.md, the HTML mockups and the v9 screen specs);
 * every colour, size and font comes from theme.h. Data comes only through the service vtables
 * given to lcd_ui_init(); fixed-point values are formatted with integer arithmetic (ui_format.c).
 *
 * @see docs/lld/application/lcd-ui-lld.md
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(TEST) && !defined(SIMULATOR)
#include "FreeRTOS.h"
#include "task.h"
#endif

#if defined(TEST) || defined(SIMULATOR)
#define LOG_ERROR(m, f, ...) ((void) 0)
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#define LOG_DEBUG(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif

#include "firmware_version.h"
#include "lcd_ui/lcd_ui.h"
#include "lcd_ui/screen_internal.h"
#include "lcd_ui/theme.h"
#include "lcd_ui/ui_format.h"
#include "lcd_ui/ui_history.h"

#define LCD_MODULE_TAG "LCDUI"

/* ===================================================================== */
/* Constants                                                              */
/* ===================================================================== */

#define LCD_REFRESH_MS (200U) /* 03_SCREEN_SPECS.md refresh rate */
#define LCD_SLOW_REFRESH_MS                                                                        \
    (1000U) /* Status and Alarms: "Refreshes every 1s", "Auto-refresh 1s"                          \
             */
#define LCD_CONFIRM_TIMEOUT_MS (30000U) /* confirm modal auto-dismiss */
#define LCD_TOAST_MS (3000U)
#define LCD_STALE_AFTER_S (10U)    /* a valid reading older than the newest by this is STALE */
#define LCD_STACK_WARN_WORDS (64U) /* stack margin below this is a warning ... */
#define LCD_STACK_ERR_WORDS (32U)  /* ... and below this an error (4-byte words) */
#define LCD_WORD_BYTES (4U)
#define HISTORY_WINDOW_S (300U) /* REQ-SA-190: five minutes */
#define POLL_MS_PER_S (1000U)

#ifdef TEST
#define LCD_UI_LVGL_VERSION "8.3.11"
#else
#define LCD_UI_STR_(x) #x
#define LCD_UI_STR(x) LCD_UI_STR_(x)
#define LCD_UI_LVGL_VERSION                                                                        \
    LCD_UI_STR(LVGL_VERSION_MAJOR)                                                                 \
    "." LCD_UI_STR(LVGL_VERSION_MINOR) "." LCD_UI_STR(LVGL_VERSION_PATCH)
#endif
/* Footer right text on every screen (03_SCREEN_SPECS.md: the target string is a define) */
#define LCD_UI_TARGET_TEXT                                                                         \
    "STM32F469 \xC2\xB7 LVGL v" LCD_UI_LVGL_VERSION " \xC2\xB7 800\xC3\x97"                        \
    "480"

#define S_DOT "\xE2\x97\x8F "     /* ● U+25CF */
#define S_SQUARE "\xE2\x96\xA0 "  /* ■ U+25A0 */
#define S_WARN "\xE2\x9A\xA0 "    /* ⚠ U+26A0 */
#define S_CROSS "\xE2\x9C\x95 "   /* ✕ U+2715 */
#define S_ELLIPSIS "\xE2\x80\xA6" /* … U+2026 */
#define S_DASH "\xE2\x80\x94"     /* — U+2014 */
#define S_MIDDOT " \xC2\xB7 "     /* · U+00B7 */
#define S_TIMES "\xC3\x97 "       /* × U+00D7 */
#define S_ARROW " \xE2\x86\x92 "  /* → U+2192 */
#define S_DEGC                                                                                     \
    "\xC2\xB0"                                                                                     \
    "C" /* °C */

/* ===================================================================== */
/* Tables                                                                 */
/* ===================================================================== */

static const char *const k_screen_titles[SCR_COUNT] = {
    "SENSOR READINGS", "SENSOR TREND", "SYSTEM STATUS", "ACTIVE ALARMS", "CONFIGURATION",
};

static const sensor_id_t k_display_sensor_ids[SENSOR_DISPLAY_COUNT] = {
    SENSOR_ID_TEMPERATURE,
    SENSOR_ID_HUMIDITY,
    SENSOR_ID_PRESSURE,
};

static const char *const k_card_eyebrow[SENSOR_DISPLAY_COUNT] = {
    "TEMPERATURE" S_MIDDOT "S1", "HUMIDITY" S_MIDDOT "S2", "PRESSURE" S_MIDDOT "S3"};
static const char *const k_sensor_name[SENSOR_DISPLAY_COUNT] = {"Temperature", "Humidity",
                                                                "Pressure"};
static const char *const k_card_unit[SENSOR_DISPLAY_COUNT] = {S_DEGC, "%RH", "hPa"};
static const char *const k_trend_name[SENSOR_DISPLAY_COUNT] = {"TEMPERATURE", "HUMIDITY",
                                                               "PRESSURE"};

/* Series colours, style-guide.md "Trend chart panel" */
#define TREND_COLOUR(i)                                                                            \
    (((i) == 0U) ? THEME_COL_ACCENT : (((i) == 1U) ? THEME_COL_OK : THEME_COL_WARN))

/* Config fields in display order. data_idx indexes lcd_ui_editable_params_t's members. */
typedef struct
{
    const char *label; /**< UPPERCASE label above the box. */
    const char *name;  /**< Mixed-case name for the confirm modal. */
    const char *unit;
    uint8_t
        data_idx; /**< 0 poll, 1 temp hi, 2 temp lo, 3 hum hi, 4 hum lo, 5 press hi, 6 press lo. */
    uint32_t divisor; /**< Raw units per whole unit: 1000 ms, 100 centi, 10 deci. */
    int32_t min;      /**< Raw units. */
    int32_t max;
    int8_t partner; /**< Display index of the opposite alarm, -1 for none. */
    bool is_high;
} cfg_field_def_t;

static const cfg_field_def_t k_fields[N_EDITABLE_FIELDS] = {
    {"TEMPERATURE" S_MIDDOT "HIGH ALARM", "Temperature" S_MIDDOT "High Alarm", S_DEGC, 1U, 100U,
     -4000, 8500, 1, true},
    {"TEMPERATURE" S_MIDDOT "LOW ALARM", "Temperature" S_MIDDOT "Low Alarm", S_DEGC, 2U, 100U,
     -4000, 8500, 0, false},
    {"HUMIDITY" S_MIDDOT "HIGH ALARM", "Humidity" S_MIDDOT "High Alarm", "%RH", 3U, 100U, 0, 10000,
     3, true},
    {"HUMIDITY" S_MIDDOT "LOW ALARM", "Humidity" S_MIDDOT "Low Alarm", "%RH", 4U, 100U, 0, 10000, 2,
     false},
    {"PRESSURE" S_MIDDOT "HIGH ALARM", "Pressure" S_MIDDOT "High Alarm", "hPa", 5U, 10U, 3000,
     11000, 5, true},
    {"PRESSURE" S_MIDDOT "LOW ALARM", "Pressure" S_MIDDOT "Low Alarm", "hPa", 6U, 10U, 3000, 11000,
     4, false},
    {"POLL INTERVAL", "Poll Interval", "s", 0U, 1000U, 100, 60000, -1, false},
};

/* ===================================================================== */
/* Module state                                                           */
/* ===================================================================== */

LCD_UI_TEST_VISIBLE lcd_ui_t s_ui;

LCD_UI_TEST_VISIBLE void lcd_ui_goto_tab(uint16_t idx);
LCD_UI_TEST_VISIBLE void config_field_tapped_cb(lv_event_t *e);
LCD_UI_TEST_VISIBLE void apply_tapped_cb(lv_event_t *e);
LCD_UI_TEST_VISIBLE void cancel_tapped_cb(lv_event_t *e);
LCD_UI_TEST_VISIBLE void modal_cancel_cb(lv_event_t *e);
LCD_UI_TEST_VISIBLE void confirm_tapped_cb(lv_event_t *e);
LCD_UI_TEST_VISIBLE void confirm_timeout_cb(lv_timer_t *timer);

static void show_toast(const char *msg)
{
    lv_label_set_text(s_ui.toast_label, msg);
    lv_obj_clear_flag(s_ui.toast_label, LV_OBJ_FLAG_HIDDEN);
    s_ui.toast_until_ms = s_ui.now_ms + LCD_TOAST_MS;
}

/* ===================================================================== */
/* Config value access (data order of lcd_ui_editable_params_t)           */
/* ===================================================================== */

static int32_t get_param(const lcd_ui_editable_params_t *p, uint32_t di)
{
    switch (di)
    {
    case 0U:
        return (int32_t) p->polling_interval_ms;
    case 1U:
        return (int32_t) p->temp_alarm_hi_centi_c;
    case 2U:
        return (int32_t) p->temp_alarm_lo_centi_c;
    case 3U:
        return (int32_t) p->humidity_alarm_hi_centi;
    case 4U:
        return (int32_t) p->humidity_alarm_lo_centi;
    case 5U:
        return (int32_t) p->pressure_alarm_hi_deci;
    default:
        return (int32_t) p->pressure_alarm_lo_deci;
    }
}

static void set_param(lcd_ui_editable_params_t *p, uint32_t di, int32_t v)
{
    switch (di)
    {
    case 0U:
        p->polling_interval_ms = (uint32_t) v;
        break;
    case 1U:
        p->temp_alarm_hi_centi_c = (int16_t) v;
        break;
    case 2U:
        p->temp_alarm_lo_centi_c = (int16_t) v;
        break;
    case 3U:
        p->humidity_alarm_hi_centi = (uint16_t) v;
        break;
    case 4U:
        p->humidity_alarm_lo_centi = (uint16_t) v;
        break;
    case 5U:
        p->pressure_alarm_hi_deci = (uint16_t) v;
        break;
    default:
        p->pressure_alarm_lo_deci = (uint16_t) v;
        break;
    }
}

/** Call cfg_write->set_param for each editable field; false on the first failure. */
static bool apply_block_to_config(config_screen_t *scr)
{
    const iconfig_manager_t *cfg = scr->cfg_write;
    uint32_t v_u32 = scr->pending.polling_interval_ms;
    int16_t v_i16 = scr->pending.temp_alarm_hi_centi_c;
    uint16_t v_u16;

    if (cfg->set_param(CONFIG_PARAM_POLL_INTERVAL, &v_u32) != CONFIG_SERVICE_OK)
    {
        return false;
    }
    if (cfg->set_param(CONFIG_PARAM_TEMP_ALARM_HIGH, &v_i16) != CONFIG_SERVICE_OK)
    {
        return false;
    }
    v_i16 = scr->pending.temp_alarm_lo_centi_c;
    if (cfg->set_param(CONFIG_PARAM_TEMP_ALARM_LOW, &v_i16) != CONFIG_SERVICE_OK)
    {
        return false;
    }
    v_u16 = scr->pending.humidity_alarm_hi_centi;
    if (cfg->set_param(CONFIG_PARAM_HUMIDITY_ALARM_HIGH, &v_u16) != CONFIG_SERVICE_OK)
    {
        return false;
    }
    v_u16 = scr->pending.humidity_alarm_lo_centi;
    if (cfg->set_param(CONFIG_PARAM_HUMIDITY_ALARM_LOW, &v_u16) != CONFIG_SERVICE_OK)
    {
        return false;
    }
    v_u16 = scr->pending.pressure_alarm_hi_deci;
    if (cfg->set_param(CONFIG_PARAM_PRESSURE_ALARM_HIGH, &v_u16) != CONFIG_SERVICE_OK)
    {
        return false;
    }
    v_u16 = scr->pending.pressure_alarm_lo_deci;
    return cfg->set_param(CONFIG_PARAM_PRESSURE_ALARM_LOW, &v_u16) == CONFIG_SERVICE_OK;
}

/* ===================================================================== */
/* Sensor snapshot helpers                                                */
/* ===================================================================== */

/** Newest timestamp among the valid readings: the "now" the staleness test compares against. */
static uint32_t newest_epoch(const sensor_snapshot_t *snap)
{
    uint32_t newest = 0U;
    for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
    {
        const sensor_reading_t *r = &snap->readings[k_display_sensor_ids[i]];
        if (r->valid && (r->timestamp.epoch > newest))
        {
            newest = r->timestamp.epoch;
        }
    }
    return newest;
}

/** Any sensor that reported valid once and is invalid now. */
static bool any_sensor_error(void)
{
    for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
    {
        if (s_ui.sensor_screen.ever_valid[i] && !s_ui.snap.readings[k_display_sensor_ids[i]].valid)
        {
            return true;
        }
    }
    return false;
}

static bool any_sensor_valid_ever(void)
{
    for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
    {
        if (s_ui.sensor_screen.ever_valid[i])
        {
            return true;
        }
    }
    return false;
}

/**
 * Evaluate the soft faults: the text of the most important one, or NULL. The health snapshot is
 * read once a second; a sensor error is judged on every tick. A lost Modbus link counts only after
 * it was up once, so a Field Device running on the bench without a Gateway is not shown as
 * degraded.
 */
static void refresh_faults(void)
{
    if ((s_ui.now_ms - s_ui.fault_ms) >= LCD_SLOW_REFRESH_MS)
    {
        device_health_snapshot_t h;

        s_ui.fault_ms = s_ui.now_ms;
        (void) memset(&h, 0, sizeof(h));
        (void) s_ui.health->get_snapshot(&h);
        if (h.modbus_slave_ok)
        {
            s_ui.modbus_seen_ok = true;
        }
        s_ui.config_failed = h.config_write_failed;
        s_ui.link_down = s_ui.modbus_seen_ok && !h.modbus_slave_ok;
    }

    if (s_ui.config_failed)
    {
        s_ui.fault_text = S_WARN "Configuration could not be saved to flash";
    }
    else if (s_ui.link_down)
    {
        s_ui.fault_text = S_WARN "Modbus link down " S_DASH " no requests from the Gateway";
    }
    else if (any_sensor_error())
    {
        s_ui.fault_text = S_WARN "Sensor error " S_DASH " last valid reading shown";
    }
    else
    {
        s_ui.fault_text = NULL;
    }
}

static void clock_text(char *out, uint32_t len)
{
    ui_fmt_clock(out, len,
                 s_ui.snap_ok ? s_ui.snap.readings[SENSOR_ID_TEMPERATURE].timestamp.epoch : 0U);
}

/* ===================================================================== */
/* Sensors screen                                                         */
/* ===================================================================== */

static void sensor_screen_on_enter(screen_t *self)
{
    sensor_screen_t *scr = (sensor_screen_t *) self;
    scr->spark_gen_seen = 0U;
    footer_bar_set(&s_ui.footer, "POLL " S_DASH " NEXT " S_DASH, LCD_UI_TARGET_TEXT);
}

static void sensor_screen_on_exit(screen_t *self)
{
    (void) self;
}

/** Normalise the history of one channel to sparkline pixels (0 top, THEME_SPARKLINE_H - 1 bottom).
 */
static uint32_t build_spark(ui_channel_t ch, int16_t out[SPARK_MAX_POINTS])
{
    const uint32_t n = ui_history_count();
    const int32_t top = 2; /* leave room for the stroke at both ends */
    const int32_t bottom = (int32_t) THEME_SPARKLINE_H - 3;
    int32_t lo = 0;
    int32_t hi = 0;
    bool have = false;

    for (uint32_t i = 0U; i < n; i++)
    {
        int32_t v;
        if (ui_history_get(ch, i, &v))
        {
            lo = (!have || (v < lo)) ? v : lo;
            hi = (!have || (v > hi)) ? v : hi;
            have = true;
        }
    }
    for (uint32_t i = 0U; i < n; i++)
    {
        int32_t v;
        if (!have || !ui_history_get(ch, i, &v))
        {
            out[i] = SPARK_GAP;
        }
        else
        {
            const int32_t span = (hi > lo) ? (hi - lo) : 1;
            out[i] = (int16_t) (bottom - (((v - lo) * (bottom - top)) / span));
        }
    }
    return n;
}

static void sensor_screen_on_refresh(screen_t *self)
{
    sensor_screen_t *scr = (sensor_screen_t *) self;
    alarm_state_t alarm[SENSOR_ID_COUNT];
    char value[16];
    char status[48];
    char delta[24];
    char clock[12];
    const uint32_t newest = newest_epoch(&s_ui.snap);

    (void) memset(alarm, 0, sizeof(alarm));
    (void) s_ui.alarms->get_all_states(alarm);

    for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
    {
        const sensor_id_t id = k_display_sensor_ids[i];
        const sensor_reading_t *r = &s_ui.snap.readings[id];
        const ui_channel_t ch = (ui_channel_t) i;
        card_state_t state;
        int32_t shown = r->value;
        uint32_t when = r->timestamp.epoch;

        if (r->valid)
        {
            scr->ever_valid[i] = true;
            scr->last_value[i] = r->value;
            scr->last_valid_epoch[i] = r->timestamp.epoch;
            if (alarm[id] != ALARM_STATE_CLEAR)
            {
                state = CARD_ALARM;
            }
            else if ((newest - r->timestamp.epoch) >= LCD_STALE_AFTER_S)
            {
                state = CARD_STALE;
            }
            else
            {
                state = CARD_VALID;
            }
        }
        else if (scr->ever_valid[i])
        {
            state = CARD_ERROR; /* the last good value is shown faded (REQ-LD-060) */
            shown = scr->last_value[i];
            when = scr->last_valid_epoch[i];
        }
        else
        {
            state = CARD_WAIT;
        }

        value[0] = '\0';
        ui_fmt_value(value, sizeof(value), ch, shown);
        ui_fmt_clock(clock, sizeof(clock), when);
        switch (state)
        {
        case CARD_VALID:
            (void) snprintf(status, sizeof(status), S_DOT "VALID" S_MIDDOT "%s", clock);
            break;
        case CARD_STALE:
            (void) snprintf(status, sizeof(status), S_WARN "STALE" S_MIDDOT "%s", clock);
            break;
        case CARD_ERROR:
            (void) snprintf(status, sizeof(status), S_CROSS "ERROR" S_MIDDOT "last %s", clock);
            break;
        case CARD_ALARM:
            (void) snprintf(status, sizeof(status), S_SQUARE "ALARM" S_MIDDOT "%s", clock);
            break;
        case CARD_WAIT:
        default:
            (void) snprintf(status, sizeof(status), S_ELLIPSIS " WAITING");
            break;
        }

        int32_t d;
        uint32_t minutes;
        if ((state == CARD_VALID) && ui_history_delta(ch, &d, &minutes))
        {
            ui_fmt_delta(delta, sizeof(delta), ch, d, minutes);
        }
        else
        {
            (void) snprintf(delta, sizeof(delta), S_DASH);
        }
        sensor_card_set(&scr->cards[i], state, value, status, delta);

        if (scr->spark_gen_seen != s_ui.hist_gen)
        {
            int16_t pts[SPARK_MAX_POINTS];
            const uint32_t n = build_spark(ch, pts);
            sensor_card_set_spark(&scr->cards[i], pts, n);
        }
    }
    scr->spark_gen_seen = s_ui.hist_gen;

    /* footer: poll interval and the time until the next acquisition */
    const config_params_t *cfg = s_ui.cfg_read->get_params();
    const uint32_t poll_ms = (cfg != NULL) ? cfg->polling_interval_ms : 0U;
    const uint32_t since = s_ui.now_ms - s_ui.last_cycle_ms;
    const uint32_t next_s =
        (since >= poll_ms) ? 0U : ((poll_ms - since + (POLL_MS_PER_S - 1U)) / POLL_MS_PER_S);
    char left[48];
    ui_fmt_fixed(clock, sizeof(clock), (int32_t) poll_ms, POLL_MS_PER_S, 0U);
    (void) snprintf(left, sizeof(left), "POLL %ss" S_MIDDOT "NEXT %lus", clock,
                    (unsigned long) next_s);
    footer_bar_set(&s_ui.footer, left,
                   any_sensor_valid_ever() ? LCD_UI_TARGET_TEXT : "Acquiring" S_ELLIPSIS);
}

static void build_sensor_screen(sensor_screen_t *scr, lv_obj_t *panel)
{
    scr->base.on_enter = sensor_screen_on_enter;
    scr->base.on_exit = sensor_screen_on_exit;
    scr->base.on_refresh = sensor_screen_on_refresh;

    for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
    {
        sensor_card_create(
            &scr->cards[i], panel,
            (lv_coord_t) (THEME_SCREEN_PAD + ((lv_coord_t) i * (THEME_CARD_W + THEME_CARD_GAP))),
            THEME_SCREEN_PAD, THEME_CARD_W, THEME_CARD_H, k_card_eyebrow[i], k_card_unit[i]);
    }
}

/* ===================================================================== */
/* Trend screen                                                           */
/* ===================================================================== */

#define TREND_GAP THEME_SP_6 /* 14 px between panels and above the first */

static void trend_screen_on_enter(screen_t *self)
{
    ((trend_screen_t *) self)->gen_seen = 0U;
}

static void trend_screen_on_exit(screen_t *self)
{
    (void) self;
    header_bar_set_badge(&s_ui.header, NULL);
}

static void trend_screen_on_refresh(screen_t *self)
{
    trend_screen_t *scr = (trend_screen_t *) self;
    const uint32_t n = ui_history_count();
    char left[96];

    if (scr->gen_seen != s_ui.hist_gen)
    {
        for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
        {
            const ui_channel_t ch = (ui_channel_t) i;
            char value[16];
            char max_t[24];
            char min_t[24];
            int32_t v;
            int32_t lo = 0;
            int32_t hi = 0;
            bool have = false;

            for (uint32_t k = 0U; k < n; k++)
            {
                if (ui_history_get(ch, k, &v))
                {
                    lo = (!have || (v < lo)) ? v : lo;
                    hi = (!have || (v > hi)) ? v : hi;
                    have = true;
                }
            }
            if (have && ui_history_get(ch, n - 1U, &v))
            {
                ui_fmt_value(value, sizeof(value), ch, v);
            }
            else
            {
                (void) snprintf(value, sizeof(value), S_DASH);
            }
            max_t[0] = '\0';
            min_t[0] = '\0';
            if (have)
            {
                char tmp[16];
                ui_fmt_value(tmp, sizeof(tmp), ch, hi);
                (void) snprintf(max_t, sizeof(max_t), "max %s", tmp);
                ui_fmt_value(tmp, sizeof(tmp), ch, lo);
                (void) snprintf(min_t, sizeof(min_t), "min %s", tmp);
            }
            trend_panel_set_texts(&scr->panel[i], value, max_t, min_t);
            trend_panel_invalidate(&scr->panel[i]);
        }
        scr->gen_seen = s_ui.hist_gen;
    }

    /* header badge while the buffer fills (REQ-LD-195), footer window summary */
    if (n < UI_HISTORY_LEN)
    {
        char badge[48];
        const uint32_t elapsed_s = (n * UI_HISTORY_PERIOD_MS) / POLL_MS_PER_S;
        (void) snprintf(badge, sizeof(badge), "ACCUMULATING %lu:%02lu / 5:00",
                        (unsigned long) (elapsed_s / 60U), (unsigned long) (elapsed_s % 60U));
        header_bar_set_badge(&s_ui.header, badge);
        (void) snprintf(left, sizeof(left),
                        "WINDOW 5 min" S_MIDDOT "SAMPLE 5 s" S_MIDDOT "%lu / %lu",
                        (unsigned long) n, (unsigned long) UI_HISTORY_LEN);
    }
    else
    {
        header_bar_set_badge(&s_ui.header, NULL);
        (void) snprintf(left, sizeof(left),
                        "WINDOW 5 min" S_MIDDOT "SAMPLE 5 s" S_MIDDOT "%lu / %lu" S_MIDDOT
                        "ROLLING",
                        (unsigned long) n, (unsigned long) UI_HISTORY_LEN);
    }
    footer_bar_set(&s_ui.footer, left, LCD_UI_TARGET_TEXT);
}

static void build_trend_screen(trend_screen_t *scr, lv_obj_t *panel)
{
    const lv_coord_t h =
        (lv_coord_t) ((THEME_CONTENT_H - ((SENSOR_DISPLAY_COUNT + 1U) * TREND_GAP)) /
                      SENSOR_DISPLAY_COUNT);

    scr->base.on_enter = trend_screen_on_enter;
    scr->base.on_exit = trend_screen_on_exit;
    scr->base.on_refresh = trend_screen_on_refresh;

    for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
    {
        trend_panel_create(&scr->panel[i], panel, THEME_SCREEN_PAD,
                           (lv_coord_t) (TREND_GAP + ((lv_coord_t) i * (h + TREND_GAP))),
                           (lv_coord_t) (THEME_CANVAS_W - (2 * THEME_SCREEN_PAD)), h,
                           (ui_channel_t) i, k_trend_name[i], k_card_unit[i], TREND_COLOUR(i));
    }
}

/* ===================================================================== */
/* Status screen                                                          */
/* ===================================================================== */

#define STATUS_COL_GAP THEME_SP_12 /* 36 px between the two columns */
#define STATUS_COL_W ((THEME_CANVAS_W - (2 * THEME_SCREEN_PAD) - STATUS_COL_GAP) / 2)
#define STATUS_EYEBROW_H 22 /* eyebrow line plus 6 px margin */
#define STATUS_SECTION_GAP THEME_SP_6
#define STATUS_TOP 18 /* content padding-top */
#define STATUS_BANNER_H 36
#define STATUS_BANNER_TOP THEME_SP_6

static const char *const k_row_label[STATUS_ROWS] = {
    "Temperature sensor", "Humidity sensor", "Pressure sensor", "Sensor failures", "Alarms raised",
    "Active alarms",      "Config save",     "Slave link",      "Valid frames",    "CRC errors",
    "Exceptions",         "Uptime",          "Firmware",        "Stack margin",
};

static const char *const k_section_title[STATUS_SECTIONS] = {"SENSORS", "SYSTEM", "MODBUS",
                                                             "DEVICE"};

static void status_layout(status_screen_t *scr, bool banner)
{
    /* Left: SENSORS (rows 0..3), SYSTEM (4..6). Right: MODBUS (7..10), DEVICE (11..13). */
    const lv_coord_t top =
        (lv_coord_t) (banner ? (STATUS_BANNER_TOP + STATUS_BANNER_H + STATUS_BANNER_TOP)
                             : STATUS_TOP);
    const lv_coord_t lx = THEME_SCREEN_PAD;
    const lv_coord_t rx = (lv_coord_t) (THEME_SCREEN_PAD + STATUS_COL_W + STATUS_COL_GAP);
    lv_coord_t y = top;

    lv_obj_set_pos(scr->eyebrow[0], lx, y);
    y = (lv_coord_t) (y + STATUS_EYEBROW_H);
    for (uint32_t r = 0U; r < 4U; r++)
    {
        lv_obj_set_pos(scr->row[r].box, lx, y);
        y = (lv_coord_t) (y + STATUS_ROW_HEIGHT);
    }
    y = (lv_coord_t) (y + STATUS_SECTION_GAP);
    lv_obj_set_pos(scr->eyebrow[1], lx, y);
    y = (lv_coord_t) (y + STATUS_EYEBROW_H);
    for (uint32_t r = 4U; r < 7U; r++)
    {
        lv_obj_set_pos(scr->row[r].box, lx, y);
        y = (lv_coord_t) (y + STATUS_ROW_HEIGHT);
    }

    y = top;
    lv_obj_set_pos(scr->eyebrow[2], rx, y);
    y = (lv_coord_t) (y + STATUS_EYEBROW_H);
    for (uint32_t r = 7U; r < 11U; r++)
    {
        lv_obj_set_pos(scr->row[r].box, rx, y);
        y = (lv_coord_t) (y + STATUS_ROW_HEIGHT);
    }
    y = (lv_coord_t) (y + STATUS_SECTION_GAP);
    lv_obj_set_pos(scr->eyebrow[3], rx, y);
    y = (lv_coord_t) (y + STATUS_EYEBROW_H);
    for (uint32_t r = 11U; r < 14U; r++)
    {
        lv_obj_set_pos(scr->row[r].box, rx, y);
        y = (lv_coord_t) (y + STATUS_ROW_HEIGHT);
    }
}

static void status_screen_on_enter(screen_t *self)
{
    ((status_screen_t *) self)->last_refresh_ms = s_ui.now_ms - LCD_SLOW_REFRESH_MS;
}

static void status_screen_on_exit(screen_t *self)
{
    (void) self;
}

static const char *lifecycle_name(lifecycle_state_t s)
{
    switch (s)
    {
    case LIFECYCLE_STATE_INIT:
        return "Init";
    case LIFECYCLE_STATE_OPERATIONAL:
        return "Operational";
    case LIFECYCLE_STATE_EDITING_CONFIG:
        return "Editing";
    case LIFECYCLE_STATE_FAULTED:
        return "Faulted";
    default:
        return "Other";
    }
}

static void status_screen_on_refresh(screen_t *self)
{
    status_screen_t *scr = (status_screen_t *) self;
    device_health_snapshot_t h;
    char buf[32];
    uint32_t active = 0U;
    uint32_t min_words = 0U;

    if ((s_ui.now_ms - scr->last_refresh_ms) < LCD_SLOW_REFRESH_MS)
    {
        return;
    }
    scr->last_refresh_ms = s_ui.now_ms;

    (void) memset(&h, 0, sizeof(h));
    (void) s_ui.health->get_snapshot(&h);

    for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
    {
        status_row_set(&scr->row[i], h.sensor_valid[k_display_sensor_ids[i]] ? ROW_OK : ROW_ERR,
                       h.sensor_valid[k_display_sensor_ids[i]] ? "valid" : "error");
        if (h.alarm_state[k_display_sensor_ids[i]] != ALARM_STATE_CLEAR)
        {
            active++;
        }
    }
    (void) snprintf(buf, sizeof(buf), "%lu", (unsigned long) h.sensor_fail_count);
    status_row_set(&scr->row[3], (h.sensor_fail_count == 0U) ? ROW_OK : ROW_WARN, buf);
    (void) snprintf(buf, sizeof(buf), "%lu", (unsigned long) h.alarm_raise_count);
    status_row_set(&scr->row[4], ROW_OK, buf);
    (void) snprintf(buf, sizeof(buf), "%lu", (unsigned long) active);
    status_row_set(&scr->row[5], (active == 0U) ? ROW_OK : ROW_ERR, buf);
    status_row_set(&scr->row[6], h.config_write_failed ? ROW_ERR : ROW_OK,
                   h.config_write_failed ? "FAILED" : "ok");

    status_row_set(&scr->row[7], h.modbus_slave_ok ? ROW_OK : ROW_ERR,
                   h.modbus_slave_ok ? "online" : "offline");
    (void) snprintf(buf, sizeof(buf), "%lu", (unsigned long) h.modbus_valid_frames);
    status_row_set(&scr->row[8], ROW_OK, buf);
    (void) snprintf(buf, sizeof(buf), "%lu", (unsigned long) h.modbus_crc_errors);
    status_row_set(&scr->row[9], (h.modbus_crc_errors == 0U) ? ROW_OK : ROW_WARN, buf);
    (void) snprintf(buf, sizeof(buf), "%lu", (unsigned long) h.modbus_exception_responses);
    status_row_set(&scr->row[10], (h.modbus_exception_responses == 0U) ? ROW_OK : ROW_WARN, buf);

    /* DEVICE: uptime, firmware, smallest stack margin of the reporting tasks */
    ui_fmt_uptime(buf, sizeof(buf), h.uptime_s);
    status_row_set(&scr->row[11], ROW_OK, buf);
    status_row_set(&scr->row[12], ROW_OK, FW_VERSION_STRING);
    for (uint32_t t = 0U; t < HEALTH_TASK_COUNT; t++)
    {
        const uint32_t w = h.stack_watermark_words[t];
        if ((w != 0U) && ((min_words == 0U) || (w < min_words)))
        {
            min_words = w;
        }
    }
    if (min_words == 0U)
    {
        status_row_set(&scr->row[13], ROW_OK, S_DASH);
    }
    else
    {
        (void) snprintf(buf, sizeof(buf), "%lu B", (unsigned long) (min_words * LCD_WORD_BYTES));
        status_row_set(&scr->row[13],
                       (min_words < LCD_STACK_ERR_WORDS)
                           ? ROW_ERR
                           : ((min_words < LCD_STACK_WARN_WORDS) ? ROW_WARN : ROW_OK),
                       buf);
    }

    /* banner: the most important soft fault, in the error colour (03_SCREEN_SPECS.md "Degraded") */
    const char *fault = s_ui.fault_text;
    if ((fault != NULL) != scr->banner_shown)
    {
        scr->banner_shown = (fault != NULL);
        if (fault != NULL)
        {
            lv_obj_clear_flag(scr->banner_box, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(scr->banner_box, LV_OBJ_FLAG_HIDDEN);
        }
        status_layout(scr, scr->banner_shown);
    }
    if (fault != NULL)
    {
        lv_label_set_text(scr->banner_text, fault);
        clock_text(buf, sizeof(buf));
        lv_label_set_text(scr->banner_time, buf);
    }

    footer_bar_set(&s_ui.footer, "Refreshes every 1s",
                   (fault == NULL) ? "All systems nominal" : "1 active alert");
}

static void build_status_screen(status_screen_t *scr, lv_obj_t *panel)
{
    scr->base.on_enter = status_screen_on_enter;
    scr->base.on_exit = status_screen_on_exit;
    scr->base.on_refresh = status_screen_on_refresh;
    scr->banner_shown = false;

    for (uint32_t t = 0U; t < STATUS_SECTIONS; t++)
    {
        scr->eyebrow[t] = theme_label_create(panel, &theme_st_txt_eyebrow, &theme_st_label_muted, 0,
                                             0, k_section_title[t]);
    }
    for (uint32_t r = 0U; r < STATUS_ROWS; r++)
    {
        status_row_create(&scr->row[r], panel, 0, 0, STATUS_COL_W, k_row_label[r]);
    }

    /* banner (hidden): err tint, 1 px err border, text and clock in COL_ERR */
    scr->banner_box =
        theme_box_create(panel, THEME_SCREEN_PAD, STATUS_BANNER_TOP,
                         (lv_coord_t) (THEME_CANVAS_W - (2 * THEME_SCREEN_PAD)), STATUS_BANNER_H);
    lv_obj_set_style_bg_color(scr->banner_box, THEME_COL_ERR_TINT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr->banner_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr->banner_box, THEME_BORDER_W, LV_PART_MAIN);
    lv_obj_set_style_border_color(scr->banner_box, THEME_COL_ERR, LV_PART_MAIN);
    scr->banner_text = theme_label_create(scr->banner_box, &theme_st_txt_body_sb,
                                          &theme_st_label_err, THEME_SP_6, 8, "");
    scr->banner_time = theme_label_create(
        scr->banner_box, &theme_st_txt_value_sm, &theme_st_label_err,
        (lv_coord_t) (THEME_CANVAS_W - (2 * THEME_SCREEN_PAD) - THEME_SP_6 - 64), 9, "");
    lv_obj_add_flag(scr->banner_box, LV_OBJ_FLAG_HIDDEN);
    status_layout(scr, false);
}

/* ===================================================================== */
/* Alarms screen                                                          */
/* ===================================================================== */

#define EMPTY_CIRCLE_D 78 /* 03_SCREEN_SPECS.md "Alarms . Empty" */

static void alarm_screen_on_enter(screen_t *self)
{
    ((alarm_screen_t *) self)->last_refresh_ms = s_ui.now_ms - LCD_SLOW_REFRESH_MS;
}

static void alarm_screen_on_exit(screen_t *self)
{
    (void) self;
}

static void center_label_x(lv_obj_t *label, const lv_font_t *font, const char *text,
                           lv_coord_t track)
{
    const lv_coord_t w =
        lv_txt_get_width(text, (uint32_t) strlen(text), font, track, LV_TEXT_FLAG_NONE);
    lv_obj_set_x(label, (lv_coord_t) ((THEME_CANVAS_W - w) / 2));
}

static void alarm_screen_on_refresh(screen_t *self)
{
    alarm_screen_t *scr = (alarm_screen_t *) self;
    alarm_state_t st[SENSOR_ID_COUNT];
    const config_params_t *cfg = s_ui.cfg_read->get_params();
    uint32_t shown = 0U;
    char cur[24];
    char thr[24];
    char trig[32];
    char clock[12];
    char text[48];

    if ((s_ui.now_ms - scr->last_refresh_ms) < LCD_SLOW_REFRESH_MS)
    {
        return;
    }
    scr->last_refresh_ms = s_ui.now_ms;

    (void) memset(st, 0, sizeof(st));
    (void) s_ui.alarms->get_all_states(st);
    const uint32_t now_epoch = s_ui.snap.readings[SENSOR_ID_TEMPERATURE].timestamp.epoch;

    for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
    {
        const sensor_id_t id = k_display_sensor_ids[i];
        const alarm_state_t a = st[id];

        if ((a != ALARM_STATE_CLEAR) && (scr->prev_state[i] != (int32_t) a))
        {
            scr->triggered_epoch[i] = now_epoch; /* first seen active */
        }
        if ((a == ALARM_STATE_CLEAR) && (scr->prev_state[i] > 0))
        {
            scr->last_clear_epoch = now_epoch;
        }
        scr->prev_state[i] = (int32_t) a;

        if (a == ALARM_STATE_CLEAR)
        {
            alarm_card_hide(&scr->card[i]);
            continue;
        }
        const bool high = (a == ALARM_STATE_ACTIVE_HIGH);
        int32_t limit = 0;
        if (cfg != NULL)
        {
            switch (i)
            {
            case 0U:
                limit = high ? cfg->temp_alarm_high : cfg->temp_alarm_low;
                break;
            case 1U:
                limit = (int32_t) (high ? cfg->humidity_alarm_high : cfg->humidity_alarm_low);
                break;
            default:
                limit = (int32_t) (high ? cfg->pressure_alarm_high : cfg->pressure_alarm_low);
                break;
            }
        }
        ui_fmt_value(cur, sizeof(cur), (ui_channel_t) i, s_ui.snap.readings[id].value);
        ui_fmt_fixed(thr, sizeof(thr), limit, (i == 2U) ? 10U : 100U, (i == 2U) ? 0U : 1U);
        (void) snprintf(text, sizeof(text),
                        "%s "
                        "%s",
                        cur, k_card_unit[i]);
        (void) snprintf(trig, sizeof(trig), "%s %s", thr, k_card_unit[i]);
        ui_fmt_clock(clock, sizeof(clock), scr->triggered_epoch[i]);
        char when[32];
        (void) snprintf(when, sizeof(when), "Triggered %s", clock);
        char name[40];
        (void) snprintf(name, sizeof(name), "%s" S_MIDDOT "S%lu", k_sensor_name[i],
                        (unsigned long) (i + 1U));
        alarm_card_show(&scr->card[i], high, name, high ? "HIGH" : "LOW", when, text, trig);
        /* the card sits at row `shown` */
        lv_obj_set_y(scr->card[i].box,
                     (lv_coord_t) (THEME_SP_8 +
                                   ((lv_coord_t) shown * (ALARM_CARD_HEIGHT + ALARM_CARD_GAP))));
        shown++;
    }
    scr->active_count = shown;

    const bool empty = (shown == 0U);
    lv_obj_t *const empties[] = {scr->empty_circle, scr->empty_icon, scr->empty_title,
                                 scr->empty_sub, scr->empty_last};
    for (uint32_t k = 0U; k < (sizeof(empties) / sizeof(empties[0])); k++)
    {
        if (empty && (empties[k] != scr->empty_last || scr->last_clear_epoch != 0U))
        {
            lv_obj_clear_flag(empties[k], LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(empties[k], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (empty && (scr->last_clear_epoch != 0U))
    {
        char ago[24];
        ui_fmt_ago(ago, sizeof(ago), now_epoch - scr->last_clear_epoch);
        (void) snprintf(text, sizeof(text), "Last alarm cleared %s", ago);
        lv_label_set_text(scr->empty_last, text);
        center_label_x(scr->empty_last, THEME_FONT_CAPTION_MONO, text, 0);
    }

    (void) snprintf(text, sizeof(text), "%lu active" S_MIDDOT "0 acknowledged",
                    (unsigned long) shown);
    footer_bar_set(&s_ui.footer, text, "Auto-refresh 1s");
}

static void build_alarm_screen(alarm_screen_t *scr, lv_obj_t *panel)
{
    const lv_coord_t w = (lv_coord_t) (THEME_CANVAS_W - (2 * THEME_SP_8)); /* padding 18 px */
    const lv_coord_t cy = (lv_coord_t) (THEME_CONTENT_H / 2);

    scr->base.on_enter = alarm_screen_on_enter;
    scr->base.on_exit = alarm_screen_on_exit;
    scr->base.on_refresh = alarm_screen_on_refresh;
    for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
    {
        alarm_card_create(&scr->card[i], panel, THEME_SP_8, THEME_SP_8, w);
        scr->prev_state[i] = -1;
    }

    /* empty state: 78 px circle with a check, then three centred lines */
    scr->empty_circle = theme_box_create(panel, (THEME_CANVAS_W - EMPTY_CIRCLE_D) / 2,
                                         (lv_coord_t) (cy - 80), EMPTY_CIRCLE_D, EMPTY_CIRCLE_D);
    lv_obj_set_style_radius(scr->empty_circle, EMPTY_CIRCLE_D / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(scr->empty_circle, THEME_COL_OK_TINT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr->empty_circle, LV_OPA_COVER, LV_PART_MAIN);
    const lv_font_t *hero_icon = THEME_ICON_HERO;
    scr->empty_icon = theme_label_create(
        scr->empty_circle, &theme_st_plain, &theme_st_label_ok,
        (lv_coord_t) ((EMPTY_CIRCLE_D - hero_icon->line_height) / 2),
        (lv_coord_t) ((EMPTY_CIRCLE_D - hero_icon->line_height) / 2), ICON_CHECK);
    lv_obj_set_style_text_font(scr->empty_icon, hero_icon, LV_PART_MAIN);

    scr->empty_title = theme_label_create(panel, &theme_st_txt_head, &theme_st_label_ink, 0,
                                          (lv_coord_t) (cy + 10), "No active alarms");
    center_label_x(scr->empty_title, THEME_FONT_HEAD, "No active alarms", 0);
    scr->empty_sub =
        theme_label_create(panel, &theme_st_txt_caption_mono, &theme_st_label_muted, 0,
                           (lv_coord_t) (cy + 36), "All sensors are within configured thresholds.");
    center_label_x(scr->empty_sub, THEME_FONT_CAPTION_MONO,
                   "All sensors are within configured thresholds.", 0);
    scr->empty_last = theme_label_create(panel, &theme_st_txt_caption_mono, &theme_st_label_muted,
                                         0, (lv_coord_t) (cy + 58), "");
}

/* ===================================================================== */
/* Config screen                                                          */
/* ===================================================================== */

#define CFG_PAD_TOP 20     /* .content padding: 20 px 22 px 0 */
#define CFG_BAR_H 36       /* .cfg-footer-bar */
#define CFG_GAP THEME_SP_6 /* gap between the field list and the keypad column is 22; fields 12 */
#define CFG_COLUMN_GAP THEME_SP_9 /* grid gap 22 px */
#define CFG_LIST_W ((THEME_CANVAS_W - (2 * THEME_SCREEN_PAD) - NUMPAD_W - CFG_COLUMN_GAP))

static void cfg_text_of(char *out, uint32_t len, uint32_t d, int32_t raw)
{
    ui_fmt_fixed(out, len, raw, k_fields[d].divisor, 1U);
}

/** Set the footer left text and the APPLY look from the current edit state. */
static void cfg_update_chrome(config_screen_t *scr)
{
    uint32_t modified = 0U;
    uint32_t invalid = 0U;

    for (uint32_t d = 0U; d < N_EDITABLE_FIELDS; d++)
    {
        const bool changed = (get_param(&scr->pending, k_fields[d].data_idx) !=
                              get_param(&scr->committed, k_fields[d].data_idx));
        config_field_set_modified(&scr->field[d], changed || scr->entry_edited[d]);
        modified += changed ? 1U : 0U;
        invalid += scr->entry_invalid[d] ? 1U : 0U;
    }

    if (scr->sub_state != CFG_STATE_CONFIRMING)
    {
        scr->sub_state =
            ((modified > 0U) || (invalid > 0U)) ? CFG_STATE_EDITING : CFG_STATE_VIEWING;
    }

    const bool can_apply =
        (modified > 0U) && (invalid == 0U) && (scr->sub_state != CFG_STATE_CONFIRMING);
    button_set_kind(&scr->apply_btn, can_apply ? BUTTON_BAR_TINT : BUTTON_BAR_OFF);
    if (invalid > 0U)
    {
        button_set_text(&scr->apply_btn, "APPLY " S_CROSS);
        lv_obj_set_style_text_color(scr->apply_btn.label, THEME_COL_ERR, LV_PART_MAIN);
    }
    else
    {
        button_set_text(&scr->apply_btn, "APPLY");
    }

    const char *left = "CONFIG" S_MIDDOT "SAVED";
    if (scr->sub_state == CFG_STATE_CONFIRMING)
    {
        left = "CONFIG" S_MIDDOT "UNSAVED" S_MIDDOT "PENDING CONFIRM";
    }
    else if (invalid > 0U)
    {
        left = "CONFIG" S_MIDDOT "UNSAVED" S_MIDDOT "VALIDATION ERROR";
    }
    else if (modified > 0U)
    {
        left = "CONFIG" S_MIDDOT "UNSAVED";
    }
    footer_bar_set(&s_ui.footer, left, LCD_UI_TARGET_TEXT);
}

static void snapshot_config_to_committed(config_screen_t *scr)
{
    const config_params_t *p = scr->cfg_read->get_params();

    if (p == NULL)
    {
        return;
    }
    scr->committed.polling_interval_ms = p->polling_interval_ms;
    scr->committed.temp_alarm_hi_centi_c = p->temp_alarm_high;
    scr->committed.temp_alarm_lo_centi_c = p->temp_alarm_low;
    scr->committed.humidity_alarm_hi_centi = p->humidity_alarm_high;
    scr->committed.humidity_alarm_lo_centi = p->humidity_alarm_low;
    scr->committed.pressure_alarm_hi_deci = p->pressure_alarm_high;
    scr->committed.pressure_alarm_lo_deci = p->pressure_alarm_low;
}

static void cfg_load_field_texts(config_screen_t *scr)
{
    char buf[16];

    for (uint32_t d = 0U; d < N_EDITABLE_FIELDS; d++)
    {
        cfg_text_of(buf, sizeof(buf), d, get_param(&scr->committed, k_fields[d].data_idx));
        config_field_set_value(&scr->field[d], buf);
        config_field_set_error(&scr->field[d], NULL);
        scr->entry_invalid[d] = false;
        scr->entry_edited[d] = false;
        scr->entry_text[d][0] = '\0';
    }
}

static void cfg_focus(config_screen_t *scr, int32_t d)
{
    if ((scr->focus >= 0) && ((uint32_t) scr->focus < N_EDITABLE_FIELDS))
    {
        config_field_set_focus(&scr->field[scr->focus], false);
    }
    scr->focus = d;
    if ((d < 0) || ((uint32_t) d >= N_EDITABLE_FIELDS))
    {
        scr->focus = -1;
        numpad_load(&scr->numpad, NULL);
        return;
    }
    config_field_set_focus(&scr->field[d], true);
    if (scr->entry_edited[d])
    {
        numpad_load(&scr->numpad, scr->entry_text[d]);
    }
    else
    {
        char buf[16];
        cfg_text_of(buf, sizeof(buf), (uint32_t) d, get_param(&scr->pending, k_fields[d].data_idx));
        numpad_load(&scr->numpad, buf);
    }
    numpad_set_invalid(&scr->numpad, scr->entry_invalid[d]);
}

/** Check the entry of display field d; sets pending when valid, else the error message. */
static void cfg_evaluate(config_screen_t *scr, uint32_t d)
{
    const cfg_field_def_t *def = &k_fields[d];
    char msg[48];
    int32_t tenths;
    const char *err = NULL;

    if (!scr->entry_edited[d])
    {
        scr->entry_invalid[d] = false;
        config_field_set_error(&scr->field[d], NULL);
        return;
    }
    if (!ui_parse_tenths(scr->entry_text[d], &tenths))
    {
        err = S_TIMES "Enter a number";
    }
    else
    {
        const int32_t raw = tenths * (int32_t) (def->divisor / 10U);
        if ((raw < def->min) || (raw > def->max))
        {
            char a[16];
            char b[16];
            ui_fmt_fixed(a, sizeof(a), def->min, def->divisor, 1U);
            ui_fmt_fixed(b, sizeof(b), def->max, def->divisor, 1U);
            (void) snprintf(msg, sizeof(msg), S_TIMES "Out of allowed range (%s to %s)", a, b);
            err = msg;
        }
        else if (def->partner >= 0)
        {
            const cfg_field_def_t *other = &k_fields[def->partner];
            const int32_t other_raw = get_param(&scr->pending, other->data_idx);
            char a[16];
            ui_fmt_fixed(a, sizeof(a), other_raw, def->divisor, 1U);
            if (def->is_high && (raw <= other_raw))
            {
                (void) snprintf(msg, sizeof(msg), S_TIMES "Must be greater than Low Alarm (%s %s)",
                                a, def->unit);
                err = msg;
            }
            else if (!def->is_high && (raw >= other_raw))
            {
                (void) snprintf(msg, sizeof(msg), S_TIMES "Must be less than High Alarm (%s %s)", a,
                                def->unit);
                err = msg;
            }
        }
        if (err == NULL)
        {
            set_param(&scr->pending, def->data_idx, raw);
        }
    }
    if (err != NULL)
    {
        /* an invalid entry must not leave an earlier, valid intermediate value in pending */
        set_param(&scr->pending, def->data_idx, get_param(&scr->committed, def->data_idx));
    }
    scr->entry_invalid[d] = (err != NULL);
    config_field_set_error(&scr->field[d], err);
    if ((int32_t) d == scr->focus)
    {
        numpad_set_invalid(&scr->numpad, err != NULL);
    }
}

/** Keypad callback: the entry of the focused field changed. */
static void cfg_numpad_changed(const char *text)
{
    config_screen_t *scr = &s_ui.config_screen;
    const int32_t d = scr->focus;

    if ((d < 0) || (scr->sub_state == CFG_STATE_CONFIRMING))
    {
        return;
    }
    (void) strncpy(scr->entry_text[d], text, NUMPAD_TEXT_MAX);
    scr->entry_text[d][NUMPAD_TEXT_MAX] = '\0';
    scr->entry_edited[d] = true;
    config_field_set_value(&scr->field[d], text);
    cfg_evaluate(scr, (uint32_t) d);
    if (k_fields[d].partner >= 0)
    {
        cfg_evaluate(scr, (uint32_t) k_fields[d].partner); /* the relation may have flipped */
    }
    cfg_update_chrome(scr);
}

/** Back to the saved values: drop every edit. */
static void cfg_discard(config_screen_t *scr)
{
    scr->pending = scr->committed;
    cfg_load_field_texts(scr);
    confirm_modal_hide(&scr->modal);
    lv_timer_pause(scr->confirm_timeout_timer);
    scr->sub_state = CFG_STATE_VIEWING;
    cfg_focus(scr, (scr->focus >= 0) ? scr->focus : 0);
    cfg_update_chrome(scr);
}

static void config_screen_on_enter(screen_t *self)
{
    config_screen_t *scr = (config_screen_t *) self;

    if (scr->sub_state == CFG_STATE_VIEWING)
    {
        snapshot_config_to_committed(scr);
        scr->pending = scr->committed;
        cfg_load_field_texts(scr);
        cfg_focus(scr, 0);
    }
    cfg_update_chrome(scr);
}

static void config_screen_on_exit(screen_t *self)
{
    (void) self;
}

static void config_screen_on_refresh(screen_t *self)
{
    config_screen_t *scr = (config_screen_t *) self;

    if (scr->focus >= 0)
    {
        config_field_blink_tick(&scr->field[scr->focus], s_ui.now_ms);
    }
}

LCD_UI_TEST_VISIBLE void config_field_tapped_cb(lv_event_t *e)
{
    config_screen_t *scr = &s_ui.config_screen;
    const uint32_t d = (uint32_t) (uintptr_t) lv_event_get_user_data(e);

    if ((scr->sub_state != CFG_STATE_CONFIRMING) && (d < N_EDITABLE_FIELDS))
    {
        cfg_focus(scr, (int32_t) d);
    }
}

LCD_UI_TEST_VISIBLE void apply_tapped_cb(lv_event_t *e)
{
    (void) e;
    config_screen_t *scr = &s_ui.config_screen;
    char text[320];
    char old_t[16];
    char new_t[16];
    uint32_t lines = 0U;
    uint32_t used = 0U;

    if (scr->sub_state != CFG_STATE_EDITING)
    {
        return;
    }
    text[0] = '\0';
    for (uint32_t d = 0U; d < N_EDITABLE_FIELDS; d++)
    {
        const int32_t o = get_param(&scr->committed, k_fields[d].data_idx);
        const int32_t n = get_param(&scr->pending, k_fields[d].data_idx);
        if (o == n)
        {
            continue;
        }
        cfg_text_of(old_t, sizeof(old_t), d, o);
        cfg_text_of(new_t, sizeof(new_t), d, n);
        const int w =
            snprintf(&text[used], sizeof(text) - used, "%s%s: %s" S_ARROW "%s %s",
                     (lines > 0U) ? "\n" : "", k_fields[d].name, old_t, new_t, k_fields[d].unit);
        if ((w > 0) && ((uint32_t) w < (sizeof(text) - used)))
        {
            used += (uint32_t) w;
        }
        lines++;
    }
    scr->sub_state = CFG_STATE_CONFIRMING;
    confirm_modal_show(&scr->modal, text, lines);
    lv_timer_resume(scr->confirm_timeout_timer);
    cfg_update_chrome(scr);
}

LCD_UI_TEST_VISIBLE void cancel_tapped_cb(lv_event_t *e)
{
    (void) e;
    cfg_discard(&s_ui.config_screen);
}

LCD_UI_TEST_VISIBLE void modal_cancel_cb(lv_event_t *e)
{
    (void) e;
    config_screen_t *scr = &s_ui.config_screen;

    lv_timer_pause(scr->confirm_timeout_timer);
    confirm_modal_hide(&scr->modal);
    scr->sub_state = CFG_STATE_EDITING; /* the edits are kept */
    cfg_update_chrome(scr);
}

LCD_UI_TEST_VISIBLE void confirm_tapped_cb(lv_event_t *e)
{
    (void) e;
    config_screen_t *scr = &s_ui.config_screen;

    lv_timer_pause(scr->confirm_timeout_timer);
    confirm_modal_hide(&scr->modal);

    if (apply_block_to_config(scr))
    {
        scr->committed = scr->pending;
        LOG_INFO(LCD_MODULE_TAG, "Config applied");
    }
    else
    {
        LOG_ERROR(LCD_MODULE_TAG, "Config apply failed");
        show_toast("Apply failed " S_DASH " config unchanged.");
        if (scr->report != NULL)
        {
            (void) scr->report->push_event(HEALTH_EVENT_LCD_FAIL, 0U);
        }
    }
    scr->pending = scr->committed;
    cfg_load_field_texts(scr);
    scr->sub_state = CFG_STATE_VIEWING;
    cfg_focus(scr, (scr->focus >= 0) ? scr->focus : 0);
    cfg_update_chrome(scr);
}

LCD_UI_TEST_VISIBLE void confirm_timeout_cb(lv_timer_t *timer)
{
    (void) timer;
    cfg_discard(&s_ui.config_screen);
}

static void build_config_screen(config_screen_t *scr, lv_obj_t *panel,
                                const iconfig_provider_t *cfg_read,
                                const iconfig_manager_t *cfg_write, const ihealth_report_t *report)
{
    const lv_coord_t list_h = (lv_coord_t) (THEME_CONTENT_H - CFG_PAD_TOP - CFG_BAR_H);

    scr->base.on_enter = config_screen_on_enter;
    scr->base.on_exit = config_screen_on_exit;
    scr->base.on_refresh = config_screen_on_refresh;
    scr->cfg_read = cfg_read;
    scr->cfg_write = cfg_write;
    scr->report = report;
    scr->sub_state = CFG_STATE_VIEWING;
    scr->focus = -1;

    /* left: scrolling field list */
    scr->list = theme_box_create(panel, THEME_SCREEN_PAD, CFG_PAD_TOP, CFG_LIST_W, list_h);
    lv_obj_add_flag(scr->list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scr->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scr->list, LV_SCROLLBAR_MODE_OFF);
    for (uint32_t d = 0U; d < N_EDITABLE_FIELDS; d++)
    {
        config_field_create(&scr->field[d], scr->list, 0, (lv_coord_t) (d * CONFIG_FIELD_PITCH),
                            CFG_LIST_W, k_fields[d].label, k_fields[d].unit);
        config_field_on_click(&scr->field[d], config_field_tapped_cb, (void *) (uintptr_t) d);
    }

    /* right: keypad, 280 px, aligned with the list */
    numpad_create(&scr->numpad, panel,
                  (lv_coord_t) (THEME_SCREEN_PAD + CFG_LIST_W + CFG_COLUMN_GAP), CFG_PAD_TOP,
                  list_h, cfg_numpad_changed);

    /* bottom bar: CANCEL | APPLY, two halves with a 1 px divider */
    const lv_coord_t bar_y = (lv_coord_t) (THEME_CONTENT_H - CFG_BAR_H);
    lv_obj_t *bar = theme_box_create(panel, 0, bar_y, THEME_CANVAS_W, CFG_BAR_H);
    lv_obj_set_style_bg_color(bar, THEME_COL_HEADER_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t *line = theme_box_create(bar, 0, 0, THEME_CANVAS_W, THEME_BORDER_W);
    lv_obj_set_style_bg_color(line, THEME_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    const lv_coord_t half = (lv_coord_t) (THEME_CANVAS_W / 2);
    button_create(&scr->cancel_btn, bar, 0, THEME_BORDER_W, half,
                  (lv_coord_t) (CFG_BAR_H - THEME_BORDER_W), "CANCEL", BUTTON_BAR);
    button_create(&scr->apply_btn, bar, half, THEME_BORDER_W, half,
                  (lv_coord_t) (CFG_BAR_H - THEME_BORDER_W), "APPLY", BUTTON_BAR_OFF);
    lv_obj_t *divider = theme_box_create(bar, half, THEME_BORDER_W, THEME_BORDER_W,
                                         (lv_coord_t) (CFG_BAR_H - THEME_BORDER_W));
    lv_obj_set_style_bg_color(divider, THEME_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    button_on_click(&scr->cancel_btn, cancel_tapped_cb, NULL);
    button_on_click(&scr->apply_btn, apply_tapped_cb, NULL);

    scr->confirm_timeout_timer = lv_timer_create(confirm_timeout_cb, LCD_CONFIRM_TIMEOUT_MS, NULL);
    lv_timer_pause(scr->confirm_timeout_timer);
}

/* ===================================================================== */
/* Chrome: pill and clock                                                 */
/* ===================================================================== */

static pill_state_t compute_pill(void)
{
    alarm_state_t st[SENSOR_ID_COUNT];

    if (!any_sensor_valid_ever())
    {
        return PILL_INIT;
    }
    (void) memset(st, 0, sizeof(st));
    (void) s_ui.alarms->get_all_states(st);
    for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
    {
        if (st[k_display_sensor_ids[i]] != ALARM_STATE_CLEAR)
        {
            return PILL_ALARM;
        }
    }
    return (s_ui.fault_text != NULL) ? PILL_ALARM : PILL_RUNNING;
}

/* ===================================================================== */
/* Tab navigation                                                         */
/* ===================================================================== */

LCD_UI_TEST_VISIBLE void lcd_ui_goto_tab(uint16_t idx)
{
    if (idx >= (uint16_t) SCR_COUNT)
    {
        return;
    }
    screen_t *new_scr = s_ui.screens[idx];

    if (s_ui.current == new_scr)
    {
        return; /* same-tab tap: no-op */
    }

    /* Config with edits pending or the modal open blocks navigation */
    if ((s_ui.current == (screen_t *) &s_ui.config_screen) &&
        (s_ui.config_screen.sub_state != CFG_STATE_VIEWING))
    {
        tab_bar_set_active(&s_ui.tab_bar, s_ui.current_tab_idx);
        show_toast("Save or cancel changes first.");
        return;
    }

    s_ui.current->on_exit(s_ui.current);
    s_ui.current = new_scr;
    s_ui.current_tab_idx = idx;

    for (uint32_t i = 0U; i < SCR_COUNT; i++)
    {
        if (i == (uint32_t) idx)
        {
            lv_obj_clear_flag(s_ui.content[i], LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_ui.content[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    tab_bar_set_active(&s_ui.tab_bar, idx);
    header_bar_set_title(&s_ui.header, k_screen_titles[idx]);
    s_ui.current->on_enter(s_ui.current);
    s_ui.current->on_refresh(s_ui.current); /* show data at once, not after the next tick */
}

static void on_tab_bar_change(uint16_t new_idx)
{
    lcd_ui_goto_tab(new_idx);
}

/* ===================================================================== */
/* Public API                                                             */
/* ===================================================================== */

void lcd_ui_tick(void)
{
    char clock[12];

    if (!s_ui.initialised)
    {
        return;
    }
    s_ui.now_ms = lv_tick_get();
    if (s_ui.requested_screen != 0U)
    {
        const uint16_t wanted = (uint16_t) (s_ui.requested_screen - 1U);
        s_ui.requested_screen = 0U;
        lcd_ui_goto_tab(wanted);
    }
    (void) memset(&s_ui.snap, 0, sizeof(s_ui.snap));
    s_ui.snap_ok = (s_ui.sensors->get_snapshot(&s_ui.snap) == SENSOR_SERVICE_ERR_OK);

    if (s_ui.snap_ok && (s_ui.snap.cycle_count != s_ui.last_cycle))
    {
        s_ui.last_cycle = s_ui.snap.cycle_count;
        s_ui.last_cycle_ms = s_ui.now_ms;
    }

    /* the first screen refresh learns which sensors were ever valid before the pill is chosen */
    if (s_ui.snap_ok)
    {
        int32_t v[UI_CH_COUNT];
        bool ok[UI_CH_COUNT];
        for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
        {
            v[i] = s_ui.snap.readings[k_display_sensor_ids[i]].value;
            ok[i] = s_ui.snap.readings[k_display_sensor_ids[i]].valid;
            if (ok[i])
            {
                s_ui.sensor_screen.ever_valid[i] = true;
            }
        }
        if ((s_ui.snap.cycle_count != 0U) && ui_history_tick(s_ui.now_ms, v, ok))
        {
            s_ui.hist_gen++;
        }
    }

    refresh_faults();
    s_ui.current->on_refresh(s_ui.current);

    clock_text(clock, sizeof(clock));
    header_bar_set_time(&s_ui.header, clock);
    header_bar_set_pill(&s_ui.header, compute_pill());
    status_pill_blink_tick(&s_ui.header.pill, s_ui.now_ms);

    if (!lv_obj_has_flag(s_ui.toast_label, LV_OBJ_FLAG_HIDDEN) &&
        ((int32_t) (s_ui.now_ms - s_ui.toast_until_ms) >= 0))
    {
        lv_obj_add_flag(s_ui.toast_label, LV_OBJ_FLAG_HIDDEN);
    }
}

lcd_ui_err_t lcd_ui_init(const isensor_service_t *sensors, const ialarm_service_t *alarms,
                         const iconfig_provider_t *cfg_read, const iconfig_manager_t *cfg_write,
                         const ihealth_snapshot_t *health, const ihealth_report_t *report)
{
    if ((sensors == NULL) || (alarms == NULL) || (cfg_read == NULL) || (cfg_write == NULL) ||
        (health == NULL) || (report == NULL))
    {
        LOG_ERROR(LCD_MODULE_TAG, "lcd_ui_init: NULL argument");
        return LCD_UI_ERR_INVALID_ARG;
    }

    LOG_INFO(LCD_MODULE_TAG, "LcdUi init: firmware %s, built %s %s", FW_VERSION_STRING,
             FW_BUILD_DATE, FW_BUILD_TIME);

    if (s_ui.initialised)
    {
        LOG_ERROR(LCD_MODULE_TAG, "lcd_ui_init: already initialised");
        return LCD_UI_ERR_ALREADY_INIT;
    }
    if (graphics_get_display() == NULL)
    {
        LOG_ERROR(LCD_MODULE_TAG, "lcd_ui_init: graphics not initialised");
        (void) report->push_event(HEALTH_EVENT_LCD_FAIL, 0U);
        return LCD_UI_ERR_GRAPHICS_INIT;
    }

    s_ui.sensors = sensors;
    s_ui.alarms = alarms;
    s_ui.cfg_read = cfg_read;
    s_ui.cfg_write = cfg_write;
    s_ui.health = health;
    s_ui.report = report;
    ui_history_reset();
    theme_init();

    /* root screen */
    lv_obj_t *root = lv_scr_act();
    lv_obj_remove_style_all(root);
    lv_obj_add_style(root, &theme_st_screen, LV_PART_MAIN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* content panels first, so the chrome is drawn over them: y = 104, h = 340 */
    for (uint32_t i = 0U; i < SCR_COUNT; i++)
    {
        s_ui.content[i] =
            theme_box_create(root, 0, THEME_CONTENT_Y, THEME_CANVAS_W, THEME_CONTENT_H);
        lv_obj_add_style(s_ui.content[i], &theme_st_screen, LV_PART_MAIN);
    }

    tab_bar_create(&s_ui.tab_bar, root, (uint16_t) SCR_SENSOR, on_tab_bar_change);
    header_bar_create(&s_ui.header, root);
    header_bar_set_title(&s_ui.header, k_screen_titles[SCR_SENSOR]);
    footer_bar_create(&s_ui.footer, root);
    footer_bar_set(&s_ui.footer, "POLL " S_DASH, LCD_UI_TARGET_TEXT);

    build_sensor_screen(&s_ui.sensor_screen, s_ui.content[SCR_SENSOR]);
    build_trend_screen(&s_ui.trend_screen, s_ui.content[SCR_TREND]);
    build_status_screen(&s_ui.status_screen, s_ui.content[SCR_STATUS]);
    build_alarm_screen(&s_ui.alarm_screen, s_ui.content[SCR_ALARM]);
    build_config_screen(&s_ui.config_screen, s_ui.content[SCR_CONFIG], cfg_read, cfg_write, report);
    confirm_modal_create(&s_ui.config_screen.modal, root, modal_cancel_cb, confirm_tapped_cb);

    s_ui.toast_label =
        theme_label_create(root, &theme_st_txt_label, &theme_st_label_warn, THEME_HEADER_INSET,
                           (lv_coord_t) (THEME_CONTENT_Y + THEME_SP_2), "");
    lv_obj_add_flag(s_ui.toast_label, LV_OBJ_FLAG_HIDDEN);

    s_ui.screens[SCR_SENSOR] = (screen_t *) &s_ui.sensor_screen;
    s_ui.screens[SCR_TREND] = (screen_t *) &s_ui.trend_screen;
    s_ui.screens[SCR_STATUS] = (screen_t *) &s_ui.status_screen;
    s_ui.screens[SCR_ALARM] = (screen_t *) &s_ui.alarm_screen;
    s_ui.screens[SCR_CONFIG] = (screen_t *) &s_ui.config_screen;

    for (uint32_t i = 1U; i < SCR_COUNT; i++)
    {
        lv_obj_add_flag(s_ui.content[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_ui.now_ms = lv_tick_get();
    s_ui.last_cycle_ms = s_ui.now_ms;
    s_ui.current = s_ui.screens[SCR_SENSOR];
    s_ui.current_tab_idx = (uint16_t) SCR_SENSOR;
    s_ui.current->on_enter(s_ui.current);
    s_ui.initialised = true;
    lcd_ui_tick(); /* first frame: waiting cards, pill INIT */

    LOG_INFO(LCD_MODULE_TAG, "lcd_ui_init OK");
    return LCD_UI_ERR_OK;
}

void lcd_ui_select_screen(uint16_t screen)
{
    if (screen < SCR_COUNT)
    {
        s_ui.requested_screen = (uint16_t) (screen + 1U);
    }
}

void lcd_ui_task_body(void *arg)
{
    (void) arg;

#if !defined(TEST) && !defined(SIMULATOR)
    TickType_t last_wake = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LCD_REFRESH_MS));
        (void) graphics_process();
        lcd_ui_tick();
    }
#endif
}

/* ===================================================================== */
/* Test-only hooks                                                        */
/* ===================================================================== */

#ifdef TEST

void lcd_ui_reset_for_test(void)
{
    (void) memset(&s_ui, 0, sizeof(s_ui));
    ui_history_reset();
}

lcd_ui_t *lcd_ui_test_get_instance(void)
{
    return &s_ui;
}

void lcd_ui_test_fire_tab_change(uint16_t new_idx)
{
    on_tab_bar_change(new_idx);
}

void lcd_ui_test_fire_cfg_field_tapped(uint32_t field_idx)
{
    lv_event_t e;
    (void) memset(&e, 0, sizeof(e));
    e.user_data = (void *) (uintptr_t) field_idx;
    config_field_tapped_cb(&e);
}

void lcd_ui_test_press_key(const char *key)
{
    numpad_press(&s_ui.config_screen.numpad, key);
}

void lcd_ui_test_fire_apply_tapped(void)
{
    apply_tapped_cb(NULL);
}

void lcd_ui_test_fire_confirm_tapped(void)
{
    confirm_tapped_cb(NULL);
}

void lcd_ui_test_fire_cancel_tapped(void)
{
    cancel_tapped_cb(NULL);
}

void lcd_ui_test_fire_modal_cancel(void)
{
    modal_cancel_cb(NULL);
}

void lcd_ui_test_fire_confirm_timeout(void)
{
    confirm_timeout_cb(NULL);
}

#endif /* TEST */
