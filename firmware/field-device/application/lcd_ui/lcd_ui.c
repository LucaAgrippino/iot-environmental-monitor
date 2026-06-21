/**
 * @file lcd_ui.c
 * @brief LcdUi application component — four-screen display manager.
 *
 * @see docs/lld/application/lcd-ui-lld.md
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifndef TEST
#include "FreeRTOS.h"
#include "task.h"
#endif

#ifdef TEST
#define LOG_ERROR(m, f, ...) ((void) 0)
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#define LOG_DEBUG(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif

#include "lcd_ui/lcd_ui.h"
#include "lcd_ui/screen_internal.h"

/* ===================================================================== */
/* Module tag                                                           */
/* ===================================================================== */

#define LCD_MODULE_TAG "LCDUI"

/* ===================================================================== */
/* Timing constants                                                     */
/* ===================================================================== */

#define LCD_REFRESH_MS (200U)
#define LCD_CONFIRM_TIMEOUT_MS (30000U)
#define LCD_TAB_HEIGHT (50U)

/* ===================================================================== */
/* Spinbox validation ranges per editable field (indices match          */
/* lcd_ui_editable_params_t member order)                               */
/* ===================================================================== */

static const int32_t k_field_min[N_EDITABLE_FIELDS] = {
    100,   /* polling_interval_ms  — 100 ms minimum */
    -4000, /* temp_alarm_hi_centi_c — -40.0 °C     */
    -4000, /* temp_alarm_lo_centi_c — -40.0 °C     */
    0,     /* humidity_alarm_hi_centi — 0 %        */
    0,     /* humidity_alarm_lo_centi — 0 %        */
    3000,  /* pressure_alarm_hi_deci — 300.0 hPa   */
    3000,  /* pressure_alarm_lo_deci — 300.0 hPa   */
};

static const int32_t k_field_max[N_EDITABLE_FIELDS] = {
    60000, /* polling_interval_ms  — 60 s maximum */
    8500,  /* temp_alarm_hi_centi_c — 85.0 °C     */
    8500,  /* temp_alarm_lo_centi_c — 85.0 °C     */
    10000, /* humidity_alarm_hi_centi — 100 %     */
    10000, /* humidity_alarm_lo_centi — 100 %     */
    11000, /* pressure_alarm_hi_deci — 1100.0 hPa */
    11000, /* pressure_alarm_lo_deci — 1100.0 hPa */
};

/* Display sensor IDs (indices 0-2 into sensor_snapshot_t.readings[]) */
static const sensor_id_t k_display_sensor_ids[SENSOR_DISPLAY_COUNT] = {
    SENSOR_ID_TEMPERATURE,
    SENSOR_ID_HUMIDITY,
    SENSOR_ID_PRESSURE,
};

/* ===================================================================== */
/* Module-static instance and callback forward declarations             */
/* ===================================================================== */

LCD_UI_TEST_VISIBLE lcd_ui_t s_ui;

LCD_UI_TEST_VISIBLE void tab_change_cb(lv_event_t *e);
LCD_UI_TEST_VISIBLE void config_field_tapped_cb(lv_event_t *e);
LCD_UI_TEST_VISIBLE void config_field_changed_cb(lv_event_t *e);
LCD_UI_TEST_VISIBLE void apply_tapped_cb(lv_event_t *e);
LCD_UI_TEST_VISIBLE void cancel_tapped_cb(lv_event_t *e);
LCD_UI_TEST_VISIBLE void confirm_tapped_cb(lv_event_t *e);
LCD_UI_TEST_VISIBLE void confirm_timeout_cb(lv_timer_t *timer);

/* ===================================================================== */
/* Internal helpers                                                     */
/* ===================================================================== */

static void show_toast(const char *msg)
{
    lv_label_set_text(s_ui.toast_label, msg);
    lv_obj_clear_flag(s_ui.toast_label, LV_OBJ_FLAG_HIDDEN);
}

/** Map editable params index → int32_t value (for spinbox population). */
static int32_t get_param_as_i32(const lcd_ui_editable_params_t *p, uint32_t idx)
{
    switch (idx)
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
    case 6U:
        return (int32_t) p->pressure_alarm_lo_deci;
    default:
        return 0;
    }
}

/** Write int32_t value into the correct editable params field. */
static void set_param_from_i32(lcd_ui_editable_params_t *p, uint32_t idx, int32_t val)
{
    switch (idx)
    {
    case 0U:
        p->polling_interval_ms = (uint32_t) val;
        break;
    case 1U:
        p->temp_alarm_hi_centi_c = (int16_t) val;
        break;
    case 2U:
        p->temp_alarm_lo_centi_c = (int16_t) val;
        break;
    case 3U:
        p->humidity_alarm_hi_centi = (uint16_t) val;
        break;
    case 4U:
        p->humidity_alarm_lo_centi = (uint16_t) val;
        break;
    case 5U:
        p->pressure_alarm_hi_deci = (uint16_t) val;
        break;
    case 6U:
        p->pressure_alarm_lo_deci = (uint16_t) val;
        break;
    default:
        break;
    }
}

/** Snapshot current config_params into config_screen.committed. */
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

/** Populate spinboxes from committed values and disable them. */
static void populate_spinboxes_from_committed(config_screen_t *scr)
{
    for (uint32_t i = 0U; i < N_EDITABLE_FIELDS; i++)
    {
        lv_spinbox_set_value(scr->spinbox[i], get_param_as_i32(&scr->committed, i));
        lv_obj_add_state(scr->spinbox[i], LV_STATE_DISABLED);
        lv_obj_add_flag(scr->err_label[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/** Return VIEWING state, hide edit/confirm UI elements. */
static void reset_cfg_to_viewing(config_screen_t *scr)
{
    populate_spinboxes_from_committed(scr);
    lv_obj_add_flag(scr->cancel_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(scr->apply_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(scr->confirm_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(scr->confirm_timeout_timer);
    scr->sub_state = CFG_STATE_VIEWING;
}

/**
 * Call cfg_write->set_param for each editable field.
 * @return true if all succeed; false on first failure.
 */
static bool apply_block_to_config(config_screen_t *scr)
{
    const iconfig_manager_t *cfg = scr->cfg_write;
    uint32_t v_u32;
    int16_t v_i16;
    uint16_t v_u16;

    v_u32 = scr->pending.polling_interval_ms;
    if (cfg->set_param(CONFIG_PARAM_POLL_INTERVAL, &v_u32) != CONFIG_SERVICE_OK)
    {
        return false;
    }
    v_i16 = scr->pending.temp_alarm_hi_centi_c;
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
    if (cfg->set_param(CONFIG_PARAM_PRESSURE_ALARM_LOW, &v_u16) != CONFIG_SERVICE_OK)
    {
        return false;
    }
    return true;
}

/* ===================================================================== */
/* Sensor screen                                                        */
/* ===================================================================== */

static void sensor_screen_on_enter(screen_t *self)
{
    sensor_screen_t *scr = (sensor_screen_t *) self;
    scr->first_valid_received = false;

    /* Static unit labels (never change at runtime) */
    lv_label_set_text(scr->unit_label[0U], "C");
    lv_label_set_text(scr->unit_label[1U], "%");
    lv_label_set_text(scr->unit_label[2U], "hPa");

    /* Show "waiting" overlay; hide live-data rows */
    lv_obj_clear_flag(scr->waiting_label, LV_OBJ_FLAG_HIDDEN);
    for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
    {
        lv_obj_add_flag(scr->value_label[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(scr->timestamp_label[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(scr->icon_label[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(scr->unit_label[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void sensor_screen_on_exit(screen_t *self)
{
    (void) self; /* first_valid_received reset in next on_enter */
}

static void sensor_screen_on_refresh(screen_t *self)
{
    sensor_screen_t *scr = (sensor_screen_t *) self;
    sensor_snapshot_t snap;
    char buf[32];

    (void) memset(&snap, 0, sizeof(snap));
    (void) scr->sensors->get_snapshot(&snap);

    if ((snap.cycle_count == 0U) && !scr->first_valid_received)
    {
        /* Still waiting — overlay remains; nothing else to update */
        lv_obj_clear_flag(scr->waiting_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* First valid data received */
    scr->first_valid_received = true;
    lv_obj_add_flag(scr->waiting_label, LV_OBJ_FLAG_HIDDEN);

    for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
    {
        sensor_id_t id = k_display_sensor_ids[i];

        lv_obj_clear_flag(scr->value_label[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(scr->timestamp_label[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(scr->icon_label[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(scr->unit_label[i], LV_OBJ_FLAG_HIDDEN);

        if (snap.readings[id].valid)
        {
            /* Integer cast satisfies P9 (no floating-point in screen logic) */
            (void) snprintf(buf, sizeof(buf), "%ld", (long) snap.readings[id].value);
            lv_label_set_text(scr->value_label[i], buf);
            lv_label_set_text(scr->icon_label[i], "v"); /* valid mark */
        }
        else
        {
            lv_label_set_text(scr->value_label[i], "--"); /* REQ-LD-040 */
            lv_label_set_text(scr->icon_label[i], "x");   /* invalid mark */
        }

        (void) snprintf(buf, sizeof(buf), "%lu", (unsigned long) snap.readings[id].timestamp.epoch);
        lv_label_set_text(scr->timestamp_label[i], buf);
    }
}

static void build_sensor_screen(sensor_screen_t *scr, lv_obj_t *tab,
                                const isensor_service_t *sensors)
{
    scr->base.on_enter = sensor_screen_on_enter;
    scr->base.on_exit = sensor_screen_on_exit;
    scr->base.on_refresh = sensor_screen_on_refresh;
    scr->sensors = sensors;

    scr->waiting_label = lv_label_create(tab);
    lv_label_set_text(scr->waiting_label, "Waiting for data...");

    for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
    {
        scr->value_label[i] = lv_label_create(tab);
        scr->unit_label[i] = lv_label_create(tab);
        scr->timestamp_label[i] = lv_label_create(tab);
        scr->icon_label[i] = lv_label_create(tab);

        lv_label_set_text(scr->value_label[i], "");
        lv_label_set_text(scr->timestamp_label[i], "");
        lv_label_set_text(scr->icon_label[i], "");

        lv_obj_add_flag(scr->value_label[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(scr->unit_label[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(scr->timestamp_label[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(scr->icon_label[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ===================================================================== */
/* Status screen                                                        */
/* ===================================================================== */

static void status_screen_on_enter(screen_t *self)
{
    (void) self;
}

static void status_screen_on_exit(screen_t *self)
{
    (void) self;
}

static void status_screen_on_refresh(screen_t *self)
{
    status_screen_t *scr = (status_screen_t *) self;
    device_health_snapshot_t snap;
    char buf[64];
    uint32_t m = 0U;

    (void) memset(&snap, 0, sizeof(snap));
    (void) scr->health->get_snapshot(&snap);

#define FMT_METRIC(fmt, ...)                                                                       \
    do                                                                                             \
    {                                                                                              \
        (void) snprintf(buf, sizeof(buf), (fmt), ##__VA_ARGS__);                                   \
        lv_label_set_text(scr->metric_label[m], buf);                                              \
        m++;                                                                                       \
    } while (0)

    FMT_METRIC("Uptime: %lus", (unsigned long) snap.uptime_s);
    FMT_METRIC("Sensor fails: %lu", (unsigned long) snap.sensor_fail_count);
    FMT_METRIC("Alarm raises: %lu", (unsigned long) snap.alarm_raise_count);
    FMT_METRIC("Cfg fail: %s", snap.config_write_failed ? "YES" : "NO");
    FMT_METRIC("Modbus OK: %lu", (unsigned long) snap.modbus_valid_frames);
    FMT_METRIC("Modbus CRC err: %lu", (unsigned long) snap.modbus_crc_errors);
    FMT_METRIC("Modbus except: %lu", (unsigned long) snap.modbus_exception_responses);

    for (uint32_t i = 0U; i < LCD_UI_STATUS_STACK_LABELS; i++)
    {
        FMT_METRIC("Stack[%lu]: %u", (unsigned long) i, snap.stack_watermark_words[i]);
    }

#undef FMT_METRIC
}

static void build_status_screen(status_screen_t *scr, lv_obj_t *tab,
                                const ihealth_snapshot_t *health)
{
    scr->base.on_enter = status_screen_on_enter;
    scr->base.on_exit = status_screen_on_exit;
    scr->base.on_refresh = status_screen_on_refresh;
    scr->health = health;

    for (uint32_t i = 0U; i < STATUS_METRIC_COUNT; i++)
    {
        scr->metric_label[i] = lv_label_create(tab);
        lv_label_set_text(scr->metric_label[i], "");
    }
}

/* ===================================================================== */
/* Alarm screen                                                         */
/* ===================================================================== */

static void alarm_screen_on_enter(screen_t *self)
{
    (void) self;
}

static void alarm_screen_on_exit(screen_t *self)
{
    (void) self;
}

static void alarm_screen_on_refresh(screen_t *self)
{
    alarm_screen_t *scr = (alarm_screen_t *) self;
    alarm_state_t states[SENSOR_ID_COUNT];
    char buf[64];

    (void) memset(states, 0, sizeof(states));
    (void) scr->alarms->get_all_states(states);

    lv_list_clean(scr->list_widget);

    /* Count active alarms across the three display sensors (sorted by id,
     * lowest-id first — DEVIATION D4: no raised_at timestamps available). */
    uint32_t count = 0U;
    for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
    {
        sensor_id_t id = k_display_sensor_ids[i];
        if (states[id] != ALARM_STATE_CLEAR)
        {
            count++;
        }
    }

    if (count == 0U)
    {
        lv_obj_clear_flag(scr->no_alarms_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(scr->list_widget, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(scr->no_alarms_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(scr->list_widget, LV_OBJ_FLAG_HIDDEN);

    static const char *const k_sensor_names[SENSOR_DISPLAY_COUNT] = {"Temp", "Hum", "Pres"};
    static const char *const k_type_names[3] = {"CLEAR", "HIGH", "LOW"};

    for (uint32_t i = 0U; i < SENSOR_DISPLAY_COUNT; i++)
    {
        sensor_id_t id = k_display_sensor_ids[i];
        if (states[id] == ALARM_STATE_CLEAR)
        {
            continue;
        }
        const char *type =
            (states[id] == ALARM_STATE_ACTIVE_HIGH) ? k_type_names[1] : k_type_names[2];
        (void) snprintf(buf, sizeof(buf), "Sensor %s [%s]", k_sensor_names[i], type);
        lv_list_add_text(scr->list_widget, buf);
    }
}

static void build_alarm_screen(alarm_screen_t *scr, lv_obj_t *tab, const ialarm_service_t *alarms)
{
    scr->base.on_enter = alarm_screen_on_enter;
    scr->base.on_exit = alarm_screen_on_exit;
    scr->base.on_refresh = alarm_screen_on_refresh;
    scr->alarms = alarms;

    scr->list_widget = lv_list_create(tab);
    scr->no_alarms_label = lv_label_create(tab);
    lv_label_set_text(scr->no_alarms_label, "No active alarms");
}

/* ===================================================================== */
/* Config screen                                                        */
/* ===================================================================== */

static void config_screen_on_enter(screen_t *self)
{
    config_screen_t *scr = (config_screen_t *) self;
    scr->sub_state = CFG_STATE_VIEWING;

    snapshot_config_to_committed(scr);
    populate_spinboxes_from_committed(scr);

    lv_obj_add_flag(scr->cancel_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(scr->apply_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(scr->confirm_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(scr->confirm_timeout_timer);
}

static void config_screen_on_exit(screen_t *self)
{
    config_screen_t *scr = (config_screen_t *) self;
    if (scr->sub_state != CFG_STATE_VIEWING)
    {
        lv_timer_pause(scr->confirm_timeout_timer);
        (void) memset(&scr->pending, 0, sizeof(scr->pending));
        reset_cfg_to_viewing(scr);
    }
}

static void config_screen_on_refresh(screen_t *self)
{
    const config_screen_t *scr = (const config_screen_t *) self;
    if (scr->sub_state != CFG_STATE_VIEWING)
    {
        return; /* TC-024: no-op during edit to protect pending values */
    }
    /* Re-snapshot in case another path updated config (e.g. Modbus write) */
    config_screen_on_enter(self);
}

static void build_config_screen(config_screen_t *scr, lv_obj_t *tab,
                                const iconfig_provider_t *cfg_read,
                                const iconfig_manager_t *cfg_write, const ihealth_report_t *report)
{
    static const char *const k_field_names[N_EDITABLE_FIELDS] = {
        "Poll interval (ms)", "Temp alarm hi (cc)",   "Temp alarm lo (cc)",   "Hum alarm hi (c%)",
        "Hum alarm lo (c%)",  "Pres alarm hi (dhPa)", "Pres alarm lo (dhPa)",
    };
    static const int32_t k_spinbox_steps[N_EDITABLE_FIELDS] = {100, 50, 50, 100, 100, 10, 10};

    scr->base.on_enter = config_screen_on_enter;
    scr->base.on_exit = config_screen_on_exit;
    scr->base.on_refresh = config_screen_on_refresh;
    scr->cfg_read = cfg_read;
    scr->cfg_write = cfg_write;
    scr->report = report;

    for (uint32_t i = 0U; i < N_EDITABLE_FIELDS; i++)
    {
        scr->field_label[i] = lv_label_create(tab);
        lv_label_set_text(scr->field_label[i], k_field_names[i]);

        scr->spinbox[i] = lv_spinbox_create(tab);
        lv_spinbox_set_range(scr->spinbox[i], k_field_min[i], k_field_max[i]);
        lv_spinbox_set_step(scr->spinbox[i], (uint32_t) k_spinbox_steps[i]);
        lv_obj_add_state(scr->spinbox[i], LV_STATE_DISABLED);
        /* Stub supports only one event slot per widget; test hooks bypass
         * the event dispatch and call the callbacks directly, so the order
         * here is irrelevant for test correctness (LV_EVENT_CLICKED wins). */
        lv_obj_add_event_cb(scr->spinbox[i], config_field_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(scr->spinbox[i], config_field_tapped_cb, LV_EVENT_CLICKED, NULL);

        scr->err_label[i] = lv_label_create(tab);
        lv_label_set_text(scr->err_label[i], "Out of range");
        lv_obj_add_flag(scr->err_label[i], LV_OBJ_FLAG_HIDDEN);
    }

    scr->cancel_btn = lv_btn_create(tab);
    lv_obj_add_flag(scr->cancel_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(scr->cancel_btn, cancel_tapped_cb, LV_EVENT_CLICKED, NULL);

    scr->apply_btn = lv_btn_create(tab);
    lv_obj_add_flag(scr->apply_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(scr->apply_btn, apply_tapped_cb, LV_EVENT_CLICKED, NULL);

    scr->confirm_dialog = lv_obj_create(tab);
    lv_obj_add_flag(scr->confirm_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(scr->confirm_dialog, confirm_tapped_cb, LV_EVENT_CLICKED, NULL);

    scr->confirm_timeout_timer = lv_timer_create(confirm_timeout_cb, LCD_CONFIRM_TIMEOUT_MS, NULL);
    lv_timer_pause(scr->confirm_timeout_timer);
}

/* ===================================================================== */
/* LVGL event callbacks                                                 */
/* ===================================================================== */

LCD_UI_TEST_VISIBLE void tab_change_cb(lv_event_t *e)
{
    (void) e;
    uint16_t new_idx = lv_tabview_get_tab_act(s_ui.tabview);
    screen_t *new_scr = s_ui.screens[new_idx];

    if (s_ui.current == new_scr)
    {
        return; /* TC-027: same-tab tap — no-op */
    }

    /* Block navigation when Config is in EDITING or CONFIRMING (TC-025) */
    if ((s_ui.current == (screen_t *) &s_ui.config_screen) &&
        (s_ui.config_screen.sub_state != CFG_STATE_VIEWING))
    {
        lv_tabview_set_act(s_ui.tabview, s_ui.current_tab_idx, LV_ANIM_OFF);
        show_toast("Save or cancel changes first.");
        return;
    }

    s_ui.current->on_exit(s_ui.current);
    s_ui.current = new_scr;
    s_ui.current_tab_idx = new_idx;
    s_ui.current->on_enter(s_ui.current);
}

LCD_UI_TEST_VISIBLE void config_field_tapped_cb(lv_event_t *e)
{
    (void) e;
    config_screen_t *scr = &s_ui.config_screen;

    if (scr->sub_state != CFG_STATE_VIEWING)
    {
        return; /* already EDITING or CONFIRMING — no re-entry */
    }

    /* Copy committed snapshot into pending so the user starts from current values */
    scr->pending = scr->committed;

    /* Enable all spinboxes for user interaction */
    for (uint32_t i = 0U; i < N_EDITABLE_FIELDS; i++)
    {
        lv_obj_clear_state(scr->spinbox[i], LV_STATE_DISABLED);
    }

    /* Reveal action buttons */
    lv_obj_clear_flag(scr->cancel_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(scr->apply_btn, LV_OBJ_FLAG_HIDDEN);

    scr->sub_state = CFG_STATE_EDITING;
}

LCD_UI_TEST_VISIBLE void config_field_changed_cb(lv_event_t *e)
{
    config_screen_t *scr = &s_ui.config_screen;
    lv_obj_t *target = lv_event_get_target(e);

    /* Find which field generated the event */
    uint32_t idx = N_EDITABLE_FIELDS;
    for (uint32_t i = 0U; i < N_EDITABLE_FIELDS; i++)
    {
        if (scr->spinbox[i] == target)
        {
            idx = i;
            break;
        }
    }
    if (idx >= N_EDITABLE_FIELDS)
    {
        return;
    }

    int32_t new_val = lv_spinbox_get_value(target);

    if ((new_val < k_field_min[idx]) || (new_val > k_field_max[idx]))
    {
        /* Revert to current pending value and show error label */
        lv_spinbox_set_value(target, get_param_as_i32(&scr->pending, idx));
        lv_obj_clear_flag(scr->err_label[idx], LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* Valid value — update pending and clear error indicator */
    set_param_from_i32(&scr->pending, idx, new_val);
    lv_obj_add_flag(scr->err_label[idx], LV_OBJ_FLAG_HIDDEN);
}

LCD_UI_TEST_VISIBLE void apply_tapped_cb(lv_event_t *e)
{
    (void) e;
    config_screen_t *scr = &s_ui.config_screen;

    scr->sub_state = CFG_STATE_CONFIRMING;
    lv_obj_clear_flag(scr->confirm_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_timer_resume(scr->confirm_timeout_timer);
}

LCD_UI_TEST_VISIBLE void cancel_tapped_cb(lv_event_t *e)
{
    (void) e;
    config_screen_t *scr = &s_ui.config_screen;

    lv_timer_pause(scr->confirm_timeout_timer);
    (void) memset(&scr->pending, 0, sizeof(scr->pending));
    reset_cfg_to_viewing(scr);
}

LCD_UI_TEST_VISIBLE void confirm_tapped_cb(lv_event_t *e)
{
    (void) e;
    config_screen_t *scr = &s_ui.config_screen;

    lv_timer_pause(scr->confirm_timeout_timer);

    if (apply_block_to_config(scr))
    {
        scr->committed = scr->pending;
        LOG_INFO(LCD_MODULE_TAG, "Config applied");
    }
    else
    {
        LOG_ERROR(LCD_MODULE_TAG, "Config apply failed");
        show_toast("Apply failed — config unchanged.");
        if (scr->report != NULL)
        {
            (void) scr->report->push_event(HEALTH_EVENT_LCD_FAIL, 0U);
        }
    }

    (void) memset(&scr->pending, 0, sizeof(scr->pending));
    reset_cfg_to_viewing(scr);
}

LCD_UI_TEST_VISIBLE void confirm_timeout_cb(lv_timer_t *timer)
{
    (void) timer;
    config_screen_t *scr = &s_ui.config_screen;

    lv_timer_pause(scr->confirm_timeout_timer);
    (void) memset(&scr->pending, 0, sizeof(scr->pending));
    reset_cfg_to_viewing(scr);
}

/* ===================================================================== */
/* Public API                                                           */
/* ===================================================================== */

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

    if (s_ui.initialised)
    {
        LOG_ERROR(LCD_MODULE_TAG, "lcd_ui_init: already initialised");
        return LCD_UI_ERR_ALREADY_INIT;
    }

    /* Verify graphics layer is ready. report is non-NULL here (checked above). */
    if (graphics_get_display() == NULL)
    {
        LOG_ERROR(LCD_MODULE_TAG, "lcd_ui_init: graphics not initialised");
        (void) report->push_event(HEALTH_EVENT_LCD_FAIL, 0U);
        return LCD_UI_ERR_GRAPHICS_INIT;
    }

    /* Store provider handles */
    s_ui.sensors = sensors;
    s_ui.alarms = alarms;
    s_ui.cfg_read = cfg_read;
    s_ui.cfg_write = cfg_write;
    s_ui.health = health;
    s_ui.report = report;

    /* Build tabview and four screen tabs */
    lv_obj_t *root = lv_scr_act();
    s_ui.tabview = lv_tabview_create(root, LV_DIR_TOP, LCD_TAB_HEIGHT);
    lv_obj_add_event_cb(s_ui.tabview, tab_change_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *tab_sensor = lv_tabview_add_tab(s_ui.tabview, "Sensors");
    lv_obj_t *tab_status = lv_tabview_add_tab(s_ui.tabview, "Status");
    lv_obj_t *tab_alarm = lv_tabview_add_tab(s_ui.tabview, "Alarms");
    lv_obj_t *tab_cfg = lv_tabview_add_tab(s_ui.tabview, "Config");

    build_sensor_screen(&s_ui.sensor_screen, tab_sensor, sensors);
    build_status_screen(&s_ui.status_screen, tab_status, health);
    build_alarm_screen(&s_ui.alarm_screen, tab_alarm, alarms);
    build_config_screen(&s_ui.config_screen, tab_cfg, cfg_read, cfg_write, report);

    /* Toast label — on top of everything */
    s_ui.toast_label = lv_label_create(root);
    lv_label_set_text(s_ui.toast_label, "");
    lv_obj_add_flag(s_ui.toast_label, LV_OBJ_FLAG_HIDDEN);

    /* Wire screen dispatch table */
    s_ui.screens[SCR_SENSOR] = (screen_t *) &s_ui.sensor_screen;
    s_ui.screens[SCR_STATUS] = (screen_t *) &s_ui.status_screen;
    s_ui.screens[SCR_ALARM] = (screen_t *) &s_ui.alarm_screen;
    s_ui.screens[SCR_CONFIG] = (screen_t *) &s_ui.config_screen;

    /* Enter sensor screen — shows "Waiting for data..." (§12 step 2) */
    s_ui.current = s_ui.screens[SCR_SENSOR];
    s_ui.current_tab_idx = (uint16_t) SCR_SENSOR;
    s_ui.current->on_enter(s_ui.current);

    s_ui.initialised = true;

    LOG_INFO(LCD_MODULE_TAG, "lcd_ui_init OK");
    return LCD_UI_ERR_OK;
}

void lcd_ui_task_body(void *arg)
{
    (void) arg;

#ifndef TEST
    TickType_t last_wake = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(LCD_REFRESH_MS));
        (void) graphics_process();
        s_ui.current->on_refresh(s_ui.current);
    }
#endif
}

/* ===================================================================== */
/* Test-only hooks                                                      */
/* ===================================================================== */

#ifdef TEST

void lcd_ui_reset_for_test(void)
{
    (void) memset(&s_ui, 0, sizeof(s_ui));
}

lcd_ui_t *lcd_ui_test_get_instance(void)
{
    return &s_ui;
}

void lcd_ui_test_fire_tab_change(uint16_t new_idx)
{
    if (s_ui.tabview != NULL)
    {
        s_ui.tabview->stub_tab_act = new_idx;
    }
    lv_event_t evt;
    (void) memset(&evt, 0, sizeof(evt));
    evt.target = s_ui.tabview;
    evt.code = LV_EVENT_VALUE_CHANGED;
    tab_change_cb(&evt);
}

void lcd_ui_test_fire_cfg_field_tapped(uint32_t field_idx)
{
    config_screen_t *scr = &s_ui.config_screen;
    if (field_idx >= N_EDITABLE_FIELDS)
    {
        return;
    }
    lv_event_t evt;
    (void) memset(&evt, 0, sizeof(evt));
    evt.target = scr->spinbox[field_idx];
    evt.code = LV_EVENT_CLICKED;
    config_field_tapped_cb(&evt);
}

void lcd_ui_test_fire_cfg_field_changed(uint32_t field_idx)
{
    config_screen_t *scr = &s_ui.config_screen;
    if (field_idx >= N_EDITABLE_FIELDS)
    {
        return;
    }
    lv_event_t evt;
    (void) memset(&evt, 0, sizeof(evt));
    evt.target = scr->spinbox[field_idx];
    evt.code = LV_EVENT_VALUE_CHANGED;
    config_field_changed_cb(&evt);
}

void lcd_ui_test_fire_apply_tapped(void)
{
    lv_event_t evt;
    (void) memset(&evt, 0, sizeof(evt));
    evt.code = LV_EVENT_CLICKED;
    apply_tapped_cb(&evt);
}

void lcd_ui_test_fire_confirm_tapped(void)
{
    lv_event_t evt;
    (void) memset(&evt, 0, sizeof(evt));
    evt.code = LV_EVENT_CLICKED;
    confirm_tapped_cb(&evt);
}

void lcd_ui_test_fire_cancel_tapped(void)
{
    lv_event_t evt;
    (void) memset(&evt, 0, sizeof(evt));
    evt.code = LV_EVENT_CLICKED;
    cancel_tapped_cb(&evt);
}

void lcd_ui_test_fire_confirm_timeout(void)
{
    confirm_timeout_cb(s_ui.config_screen.confirm_timeout_timer);
}

#endif /* TEST */
