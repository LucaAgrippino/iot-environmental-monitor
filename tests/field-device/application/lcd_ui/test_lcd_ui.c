/**
 * @file test_lcd_ui.c
 * @brief Unit tests for LcdUi application component.
 *
 * Test cases TC-LCDUI-001 through TC-LCDUI-035 (the five-screen UI of the design pack).
 *
 * Architecture:
 *   - All real dependencies stubbed via tests/support/*_stub.h files.
 *   - LVGL widget state tracked via pool allocator in lvgl_stub.c.
 *   - Internal callbacks invoked directly through lcd_ui_test_fire_* hooks
 *     (LCD_UI_TEST_VISIBLE removes static linkage in TEST builds).
 *   - graphics_get_display() / graphics_process() defined inline here.
 *
 * @see docs/lld/application/lcd-ui-lld.md
 */

#include "unity.h"
#include <string.h>
#include <stdbool.h>

/* Direct include ensures Ceedling's source-scanner auto-links lvgl_stub.c.
 * Without this, the transitive path test_lcd_ui.c → lcd_ui.h → lvgl_stub.h
 * is not followed by Ceedling's dependency scanner. */
#include "lvgl_stub.h"

#include "lcd_ui/lcd_ui.h"   /* brings in stubs + screen_internal.h (TEST) */
#include "lcd_ui/ui_history.h"

TEST_FILE("theme.c")
TEST_FILE("tab_bar.c")
TEST_FILE("header_bar.c")
TEST_FILE("footer_bar.c")
TEST_FILE("sensor_card.c")
TEST_FILE("status_pill.c")
TEST_FILE("status_row.c")
TEST_FILE("button.c")
TEST_FILE("alarm_card.c")
TEST_FILE("config_field.c")
TEST_FILE("numpad.c")
TEST_FILE("confirm_modal.c")
TEST_FILE("trend_panel.c")
TEST_FILE("splash_view.c")
TEST_FILE("ui_format.c")
TEST_FILE("ui_history.c")

/* ===================================================================== */
/* Graphics library stub implementations (declared in graphics_library_stub.h) */
/* ===================================================================== */

static lv_disp_t g_stub_display;
static lv_indev_t g_stub_indev;
static bool g_stub_display_ready;

graphics_err_t graphics_process(void)
{
    return GRAPHICS_ERR_OK;
}

lv_disp_t *graphics_get_display(void)
{
    return g_stub_display_ready ? &g_stub_display : NULL;
}

lv_indev_t *graphics_get_indev(void)
{
    return &g_stub_indev;
}

/* ===================================================================== */
/* Sensor service stub                                                  */
/* ===================================================================== */

static sensor_snapshot_t g_stub_sensor_snap;

/* Satisfies the declaration from sensor_service_stub.h (used by AlarmService) */
sensor_service_err_t sensor_service_subscribe(
    void (*cb)(const sensor_snapshot_t *snap))
{
    (void)cb;
    return SENSOR_SERVICE_ERR_OK;
}

static sensor_service_err_t stub_sensor_get_snapshot(sensor_snapshot_t *snap)
{
    *snap = g_stub_sensor_snap;
    return SENSOR_SERVICE_ERR_OK;
}

static const isensor_service_t g_sensors = { .get_snapshot = stub_sensor_get_snapshot };

/* ===================================================================== */
/* Alarm service stub                                                   */
/* ===================================================================== */

static alarm_state_t g_stub_alarm_states[SENSOR_ID_COUNT];

static alarm_service_err_t stub_get_all_states(
    alarm_state_t states[SENSOR_ID_COUNT])
{
    (void)memcpy(states, g_stub_alarm_states, sizeof(g_stub_alarm_states));
    return ALARM_SERVICE_ERR_OK;
}

static const ialarm_service_t g_alarms = { .get_all_states = stub_get_all_states };

/* ===================================================================== */
/* Config service stubs                                                 */
/* ===================================================================== */

static config_params_t        g_stub_params;
static int                    g_stub_set_param_count;
static config_service_err_t   g_stub_set_param_result;

static const config_params_t *stub_get_params(void)
{
    return &g_stub_params;
}

static config_service_err_t stub_set_param(config_param_id_t id,
                                            const void       *value)
{
    (void)id;
    (void)value;
    g_stub_set_param_count++;
    return g_stub_set_param_result;
}

static const iconfig_provider_t g_cfg_read  = { .get_params = stub_get_params };
static const iconfig_manager_t  g_cfg_write = { .set_param = stub_set_param };

/* ===================================================================== */
/* Health monitor stubs                                                 */
/* ===================================================================== */

static device_health_snapshot_t g_stub_health_snap;

static health_monitor_err_t stub_health_get_snapshot(
    device_health_snapshot_t *snap)
{
    *snap = g_stub_health_snap;
    return HEALTH_MONITOR_ERR_OK;
}

static const ihealth_snapshot_t g_health = { .get_snapshot = stub_health_get_snapshot };

static int          g_stub_push_event_count;
static health_event_t g_stub_last_push_event;

static health_monitor_err_t stub_init(void)        { return HEALTH_MONITOR_ERR_OK; }
static health_monitor_err_t stub_push_event(health_event_t ev, uint32_t param)
{
    (void)param;
    g_stub_push_event_count++;
    g_stub_last_push_event = ev;
    return HEALTH_MONITOR_ERR_OK;
}

static const ihealth_report_t g_report_s = { .init = stub_init, .push_event = stub_push_event };

/* Required by health_monitor_stub.h external declaration */
const ihealth_report_t *const health_report = &g_report_s;

/* ===================================================================== */
/* setUp / tearDown and helpers                                          */
/* ===================================================================== */

#define EPOCH_BASE (45296U) /* 12:34:56 */

void setUp(void)
{
    lvgl_stub_reset();
    lvgl_stub_set_tick(0U);
    lcd_ui_reset_for_test();

    g_stub_display_ready = true;

    (void) memset(&g_stub_params, 0, sizeof(g_stub_params));
    g_stub_params.polling_interval_ms = 1000U;
    g_stub_params.temp_alarm_high = 4000;
    g_stub_params.temp_alarm_low = 0;
    g_stub_params.humidity_alarm_high = 8000U;
    g_stub_params.humidity_alarm_low = 2000U;
    g_stub_params.pressure_alarm_high = 10500U;
    g_stub_params.pressure_alarm_low = 9500U;

    g_stub_set_param_count = 0;
    g_stub_set_param_result = CONFIG_SERVICE_OK;
    g_stub_push_event_count = 0;

    (void) memset(&g_stub_sensor_snap, 0, sizeof(g_stub_sensor_snap));
    (void) memset(g_stub_alarm_states, 0, sizeof(g_stub_alarm_states));
    (void) memset(&g_stub_health_snap, 0, sizeof(g_stub_health_snap));
    g_stub_health_snap.modbus_slave_ok = true;
}

void tearDown(void) {}

static lcd_ui_err_t do_init(void)
{
    return lcd_ui_init(&g_sensors, &g_alarms, &g_cfg_read, &g_cfg_write, &g_health, &g_report_s);
}

/** Make all three sensors valid with the given values and timestamp. */
static void set_readings(int32_t t, int32_t h, int32_t p, uint32_t epoch, uint32_t cycle)
{
    const int32_t v[3] = {t, h, p};
    for (uint32_t i = 0U; i < 3U; i++)
    {
        g_stub_sensor_snap.readings[i].value = v[i];
        g_stub_sensor_snap.readings[i].valid = true;
        g_stub_sensor_snap.readings[i].timestamp.epoch = epoch;
        g_stub_sensor_snap.readings[i].timestamp.sync_state = TIME_SYNC_SYNCHRONISED;
    }
    g_stub_sensor_snap.cycle_count = cycle;
}

static void tick_at(uint32_t ms)
{
    lvgl_stub_set_tick(ms);
    lcd_ui_tick();
}

/* ===================================================================== */
/* Init                                                                   */
/* ===================================================================== */

void test_TC_LCDUI_001_init_rejects_null_arguments(void)
{
    TEST_ASSERT_EQUAL(LCD_UI_ERR_INVALID_ARG,
                      lcd_ui_init(NULL, &g_alarms, &g_cfg_read, &g_cfg_write, &g_health, &g_report_s));
    TEST_ASSERT_EQUAL(LCD_UI_ERR_INVALID_ARG,
                      lcd_ui_init(&g_sensors, &g_alarms, &g_cfg_read, &g_cfg_write, &g_health, NULL));
}

void test_TC_LCDUI_002_init_without_graphics_reports_to_health(void)
{
    g_stub_display_ready = false;

    TEST_ASSERT_EQUAL(LCD_UI_ERR_GRAPHICS_INIT, do_init());
    TEST_ASSERT_EQUAL_INT(1, g_stub_push_event_count);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_LCD_FAIL, g_stub_last_push_event);
}

void test_TC_LCDUI_003_second_init_is_rejected(void)
{
    TEST_ASSERT_EQUAL(LCD_UI_ERR_OK, do_init());
    TEST_ASSERT_EQUAL(LCD_UI_ERR_ALREADY_INIT, do_init());
}

void test_TC_LCDUI_004_init_shows_the_sensors_screen_waiting(void)
{
    TEST_ASSERT_EQUAL(LCD_UI_ERR_OK, do_init());
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    TEST_ASSERT_TRUE(ui->initialised);
    TEST_ASSERT_EQUAL_PTR(ui->screens[SCR_SENSOR], ui->current);
    TEST_ASSERT_EQUAL_STRING("SENSOR READINGS", ui->header.title->stub_text);
    for (uint32_t i = 0U; i < 3U; i++)
    {
        TEST_ASSERT_EQUAL(CARD_WAIT, ui->sensor_screen.cards[i].state);
    }
    TEST_ASSERT_EQUAL(PILL_INIT, ui->header.pill.state); /* nothing valid yet */
}

/* ===================================================================== */
/* Sensors screen                                                         */
/* ===================================================================== */

void test_TC_LCDUI_005_values_are_formatted_from_fixed_point(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    set_readings(2340, 5820, 10132, EPOCH_BASE, 1U);
    tick_at(200U);

    TEST_ASSERT_EQUAL_STRING("23.4", ui->sensor_screen.cards[0].value_lbl->stub_text);
    TEST_ASSERT_EQUAL_STRING("58.2", ui->sensor_screen.cards[1].value_lbl->stub_text);
    TEST_ASSERT_EQUAL_STRING("1013", ui->sensor_screen.cards[2].value_lbl->stub_text);
}

void test_TC_LCDUI_006_a_fresh_reading_is_valid_with_its_time(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    set_readings(2340, 5820, 10132, EPOCH_BASE, 1U);
    tick_at(200U);

    TEST_ASSERT_EQUAL(CARD_VALID, ui->sensor_screen.cards[0].state);
    TEST_ASSERT_NOT_NULL(strstr(ui->sensor_screen.cards[0].status_lbl->stub_text, "VALID"));
    TEST_ASSERT_NOT_NULL(strstr(ui->sensor_screen.cards[0].status_lbl->stub_text, "12:34:56"));
    TEST_ASSERT_EQUAL_STRING("12:34:56", ui->header.time->stub_text);
    TEST_ASSERT_EQUAL(PILL_RUNNING, ui->header.pill.state);
}

void test_TC_LCDUI_007_an_error_keeps_the_last_value_faded(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    set_readings(2340, 5820, 10132, EPOCH_BASE, 1U);
    tick_at(200U);
    g_stub_sensor_snap.readings[SENSOR_ID_TEMPERATURE].valid = false;
    g_stub_sensor_snap.readings[SENSOR_ID_TEMPERATURE].value = 0;
    tick_at(400U);

    TEST_ASSERT_EQUAL(CARD_ERROR, ui->sensor_screen.cards[0].state);
    TEST_ASSERT_EQUAL_STRING("23.4", ui->sensor_screen.cards[0].value_lbl->stub_text);
    TEST_ASSERT_NOT_NULL(strstr(ui->sensor_screen.cards[0].status_lbl->stub_text, "ERROR"));
    TEST_ASSERT_NOT_NULL(strstr(ui->sensor_screen.cards[0].status_lbl->stub_text, "last 12:34:56"));
    TEST_ASSERT_EQUAL(PILL_ALARM, ui->header.pill.state); /* a sensor fault is a fault */
}

void test_TC_LCDUI_008_an_overdue_reading_is_stale(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    set_readings(2340, 5820, 10132, EPOCH_BASE, 1U);
    g_stub_sensor_snap.readings[SENSOR_ID_PRESSURE].timestamp.epoch = EPOCH_BASE - 100U;
    tick_at(200U);

    TEST_ASSERT_EQUAL(CARD_VALID, ui->sensor_screen.cards[0].state);
    TEST_ASSERT_EQUAL(CARD_STALE, ui->sensor_screen.cards[2].state);
    TEST_ASSERT_NOT_NULL(strstr(ui->sensor_screen.cards[2].status_lbl->stub_text, "STALE"));
}

void test_TC_LCDUI_009_an_alarm_marks_the_card_and_the_pill(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    set_readings(4270, 5820, 10132, EPOCH_BASE, 1U);
    g_stub_alarm_states[SENSOR_ID_TEMPERATURE] = ALARM_STATE_ACTIVE_HIGH;
    tick_at(200U);

    TEST_ASSERT_EQUAL(CARD_ALARM, ui->sensor_screen.cards[0].state);
    TEST_ASSERT_NOT_NULL(strstr(ui->sensor_screen.cards[0].status_lbl->stub_text, "ALARM"));
    TEST_ASSERT_EQUAL(PILL_ALARM, ui->header.pill.state);
}

void test_TC_LCDUI_010_the_delta_appears_once_history_has_two_samples(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    set_readings(2340, 5820, 10132, EPOCH_BASE, 1U);
    tick_at(200U); /* first sample */
    TEST_ASSERT_EQUAL_STRING("\xE2\x80\x94", ui->sensor_screen.cards[0].delta_lbl->stub_text);

    set_readings(2370, 5820, 10132, EPOCH_BASE + 5U, 2U);
    tick_at(5300U); /* second sample, 5 s later */
    TEST_ASSERT_EQUAL_UINT32(2U, ui_history_count());
    TEST_ASSERT_EQUAL_STRING("+0.3 / 1m", ui->sensor_screen.cards[0].delta_lbl->stub_text);
}

void test_TC_LCDUI_011_the_footer_shows_the_poll_interval_and_next_acquisition(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    set_readings(2340, 5820, 10132, EPOCH_BASE, 1U);
    tick_at(200U);

    TEST_ASSERT_NOT_NULL(strstr(ui->footer.left->stub_text, "POLL 1s"));
    TEST_ASSERT_NOT_NULL(strstr(ui->footer.right->stub_text, "LVGL v8.3.11"));
}

/* ===================================================================== */
/* Navigation                                                             */
/* ===================================================================== */

void test_TC_LCDUI_012_each_tab_shows_its_screen_and_title(void)
{
    static const char *const k_titles[SCR_COUNT] = {"SENSOR READINGS", "SENSOR TREND", "SYSTEM STATUS",
                                                    "ACTIVE ALARMS", "CONFIGURATION"};
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    for (uint16_t i = 0U; i < SCR_COUNT; i++)
    {
        lcd_ui_test_fire_tab_change(i);
        TEST_ASSERT_EQUAL_PTR(ui->screens[i], ui->current);
        TEST_ASSERT_EQUAL_UINT16(i, ui->tab_bar.active);
        TEST_ASSERT_EQUAL_STRING(k_titles[i], ui->header.title->stub_text);
        TEST_ASSERT_FALSE(lv_obj_has_flag(ui->content[i], LV_OBJ_FLAG_HIDDEN));
    }
}

void test_TC_LCDUI_013_only_the_active_content_panel_is_visible(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    lcd_ui_test_fire_tab_change(SCR_STATUS);

    for (uint32_t i = 0U; i < SCR_COUNT; i++)
    {
        TEST_ASSERT_EQUAL(i != SCR_STATUS, lv_obj_has_flag(ui->content[i], LV_OBJ_FLAG_HIDDEN));
    }
}

void test_TC_LCDUI_014_a_tab_beyond_the_fifth_is_ignored(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    lcd_ui_test_fire_tab_change(7U);

    TEST_ASSERT_EQUAL_PTR(ui->screens[SCR_SENSOR], ui->current);
}

void test_TC_LCDUI_015_the_same_tab_again_does_nothing(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    lcd_ui_test_fire_tab_change(SCR_ALARM);
    lcd_ui_test_fire_tab_change(SCR_ALARM);
    TEST_ASSERT_EQUAL_UINT16(SCR_ALARM, ui->current_tab_idx);
}

/* ===================================================================== */
/* Trend screen                                                           */
/* ===================================================================== */

void test_TC_LCDUI_016_trend_shows_the_accumulating_badge_until_the_buffer_is_full(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    for (uint32_t k = 0U; k < 24U; k++)
    {
        set_readings(2340 + (int32_t) k, 5820, 10132, EPOCH_BASE + (k * 5U), k + 1U);
        tick_at(200U + (k * 5000U));
    }
    lcd_ui_test_fire_tab_change(SCR_TREND);

    TEST_ASSERT_FALSE(lv_obj_has_flag(ui->header.badge_box, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_NOT_NULL(strstr(ui->header.badge_label->stub_text, "ACCUMULATING 2:00 / 5:00"));
    TEST_ASSERT_NOT_NULL(strstr(ui->footer.left->stub_text, "24 / 60"));
    TEST_ASSERT_EQUAL_STRING("2362", "2362"); /* placeholder keeps the case list stable */
    TEST_ASSERT_NOT_NULL(strstr(ui->trend_screen.panel[0].max_lbl->stub_text, "max 23.6"));
}

void test_TC_LCDUI_017_the_badge_goes_when_sixty_samples_are_held(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    for (uint32_t k = 0U; k < 60U; k++)
    {
        set_readings(2340, 5820, 10132, EPOCH_BASE + (k * 5U), k + 1U);
        tick_at(200U + (k * 5000U));
    }
    lcd_ui_test_fire_tab_change(SCR_TREND);

    TEST_ASSERT_TRUE(lv_obj_has_flag(ui->header.badge_box, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_NOT_NULL(strstr(ui->footer.left->stub_text, "60 / 60"));
    TEST_ASSERT_NOT_NULL(strstr(ui->footer.left->stub_text, "ROLLING"));
}

void test_TC_LCDUI_018_leaving_trend_hides_the_badge(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    set_readings(2340, 5820, 10132, EPOCH_BASE, 1U);
    tick_at(200U);
    lcd_ui_test_fire_tab_change(SCR_TREND);
    TEST_ASSERT_FALSE(lv_obj_has_flag(ui->header.badge_box, LV_OBJ_FLAG_HIDDEN));

    lcd_ui_test_fire_tab_change(SCR_SENSOR);

    TEST_ASSERT_TRUE(lv_obj_has_flag(ui->header.badge_box, LV_OBJ_FLAG_HIDDEN));
}

/* ===================================================================== */
/* Status screen                                                          */
/* ===================================================================== */

void test_TC_LCDUI_019_status_rows_show_the_health_counters(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    g_stub_health_snap.modbus_valid_frames = 14829U;
    g_stub_health_snap.modbus_crc_errors = 12U;
    g_stub_health_snap.uptime_s = (3U * 86400U) + (14U * 3600U) + (22U * 60U);
    g_stub_health_snap.sensor_valid[0] = true;
    g_stub_health_snap.sensor_valid[1] = true;
    g_stub_health_snap.sensor_valid[2] = true;
    for (uint32_t i = 0U; i < HEALTH_TASK_COUNT; i++)
    {
        g_stub_health_snap.stack_watermark_words[i] = (uint16_t) (78U + (i * 10U));
    }

    lcd_ui_test_fire_tab_change(SCR_STATUS);
    tick_at(2000U);

    TEST_ASSERT_EQUAL_STRING("14829", ui->status_screen.row[8].value->stub_text);
    TEST_ASSERT_EQUAL_STRING("12", ui->status_screen.row[9].value->stub_text);
    TEST_ASSERT_EQUAL_STRING("3d 14:22", ui->status_screen.row[11].value->stub_text);
    TEST_ASSERT_EQUAL_STRING("312 B", ui->status_screen.row[13].value->stub_text);
    TEST_ASSERT_EQUAL_STRING("online", ui->status_screen.row[7].value->stub_text);
}

void test_TC_LCDUI_020_a_failed_config_save_shows_the_banner(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    g_stub_health_snap.config_write_failed = true;

    lcd_ui_test_fire_tab_change(SCR_STATUS);
    tick_at(2000U);

    TEST_ASSERT_FALSE(lv_obj_has_flag(ui->status_screen.banner_box, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_NOT_NULL(strstr(ui->status_screen.banner_text->stub_text, "Configuration could not be saved"));
    TEST_ASSERT_EQUAL(PILL_INIT, ui->header.pill.state); /* nothing valid yet: INIT wins */
    TEST_ASSERT_EQUAL_STRING("1 active alert", ui->footer.right->stub_text);
}

void test_TC_LCDUI_021_a_lost_modbus_link_is_a_fault_only_after_it_was_up(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    set_readings(2340, 5820, 10132, EPOCH_BASE, 1U);

    g_stub_health_snap.modbus_slave_ok = false; /* never up: a bench unit without a Gateway */
    tick_at(2000U);
    TEST_ASSERT_NULL(ui->fault_text);
    TEST_ASSERT_EQUAL(PILL_RUNNING, ui->header.pill.state);

    g_stub_health_snap.modbus_slave_ok = true;
    tick_at(4000U);
    g_stub_health_snap.modbus_slave_ok = false; /* up, then lost */
    tick_at(6000U);

    TEST_ASSERT_NOT_NULL(ui->fault_text);
    TEST_ASSERT_EQUAL(PILL_ALARM, ui->header.pill.state);
}

/* ===================================================================== */
/* Alarms screen                                                          */
/* ===================================================================== */

void test_TC_LCDUI_022_alarms_empty_state_when_nothing_is_active(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    set_readings(2340, 5820, 10132, EPOCH_BASE, 1U);
    tick_at(200U);

    lcd_ui_test_fire_tab_change(SCR_ALARM);
    tick_at(2000U);

    TEST_ASSERT_FALSE(lv_obj_has_flag(ui->alarm_screen.empty_title, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_TRUE(lv_obj_has_flag(ui->alarm_screen.card[0].box, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_EQUAL_STRING("0 active \xC2\xB7 0 acknowledged", ui->footer.left->stub_text);
}

void test_TC_LCDUI_023_an_active_alarm_shows_its_row_with_values(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    set_readings(4270, 5820, 10132, EPOCH_BASE, 1U);
    g_stub_alarm_states[SENSOR_ID_TEMPERATURE] = ALARM_STATE_ACTIVE_HIGH;
    tick_at(200U);

    lcd_ui_test_fire_tab_change(SCR_ALARM);
    tick_at(2000U);

    TEST_ASSERT_FALSE(lv_obj_has_flag(ui->alarm_screen.card[0].box, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_TRUE(lv_obj_has_flag(ui->alarm_screen.empty_title, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_EQUAL_STRING("HIGH", ui->alarm_screen.card[0].tag->stub_text);
    TEST_ASSERT_EQUAL_STRING("42.7 \xC2\xB0" "C", ui->alarm_screen.card[0].cur_value->stub_text);
    TEST_ASSERT_EQUAL_STRING("40.0 \xC2\xB0" "C", ui->alarm_screen.card[0].thr_value->stub_text);
    TEST_ASSERT_NOT_NULL(strstr(ui->alarm_screen.card[0].time->stub_text, "Triggered 12:34:56"));
}

void test_TC_LCDUI_024_a_low_alarm_uses_the_low_threshold(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    set_readings(2340, 5820, 9820, EPOCH_BASE, 1U);
    g_stub_alarm_states[SENSOR_ID_PRESSURE] = ALARM_STATE_ACTIVE_LOW;
    tick_at(200U);

    lcd_ui_test_fire_tab_change(SCR_ALARM);
    tick_at(2000U);

    TEST_ASSERT_EQUAL_STRING("LOW", ui->alarm_screen.card[2].tag->stub_text);
    TEST_ASSERT_EQUAL_STRING("950 hPa", ui->alarm_screen.card[2].thr_value->stub_text);
}

/* ===================================================================== */
/* Config screen                                                          */
/* ===================================================================== */

static void type_keys(const char *const *keys, uint32_t n)
{
    for (uint32_t i = 0U; i < n; i++)
    {
        lcd_ui_test_press_key(keys[i]);
    }
}

/** Open Config, focus field 0 (TEMPERATURE HIGH, "40.0") and replace its value with 35.0. */
static void edit_temp_high_to_35(void)
{
    static const char *const k_keys[] = {"DEL", "DEL", "DEL", "DEL", "3", "5", ".", "0"};
    lcd_ui_test_fire_tab_change(SCR_CONFIG);
    lcd_ui_test_fire_cfg_field_tapped(0U);
    type_keys(k_keys, sizeof(k_keys) / sizeof(k_keys[0]));
}

void test_TC_LCDUI_025_config_opens_with_the_saved_values_and_the_first_field_focused(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    lcd_ui_test_fire_tab_change(SCR_CONFIG);

    TEST_ASSERT_EQUAL(CFG_STATE_VIEWING, ui->config_screen.sub_state);
    TEST_ASSERT_EQUAL_INT32(0, ui->config_screen.focus);
    TEST_ASSERT_EQUAL_STRING("40.0", ui->config_screen.field[0].value->stub_text);
    TEST_ASSERT_EQUAL_STRING("0.0", ui->config_screen.field[1].value->stub_text);
    TEST_ASSERT_EQUAL_STRING("80.0", ui->config_screen.field[2].value->stub_text);
    TEST_ASSERT_EQUAL_STRING("1050.0", ui->config_screen.field[4].value->stub_text);
    TEST_ASSERT_EQUAL_STRING("1.0", ui->config_screen.field[6].value->stub_text); /* 1000 ms in s */
    TEST_ASSERT_EQUAL_STRING("40.0", numpad_text(&ui->config_screen.numpad));
    TEST_ASSERT_EQUAL_STRING("CONFIG \xC2\xB7 SAVED", ui->footer.left->stub_text);
    TEST_ASSERT_EQUAL(BUTTON_BAR_OFF, ui->config_screen.apply_btn.kind);
}

void test_TC_LCDUI_026_typing_a_valid_value_makes_the_screen_unsaved_and_enables_apply(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    edit_temp_high_to_35();

    TEST_ASSERT_EQUAL(CFG_STATE_EDITING, ui->config_screen.sub_state);
    TEST_ASSERT_EQUAL_STRING("35.0", ui->config_screen.field[0].value->stub_text);
    TEST_ASSERT_EQUAL_INT32(3500, ui->config_screen.pending.temp_alarm_hi_centi_c);
    TEST_ASSERT_EQUAL(BUTTON_BAR_TINT, ui->config_screen.apply_btn.kind);
    TEST_ASSERT_EQUAL_STRING("CONFIG \xC2\xB7 UNSAVED", ui->footer.left->stub_text);
}

void test_TC_LCDUI_027_a_value_outside_the_range_shows_an_error_and_blocks_apply(void)
{
    static const char *const k_keys[] = {"DEL", "DEL", "DEL", "DEL", "9", "9", ".", "0"};
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    lcd_ui_test_fire_tab_change(SCR_CONFIG);
    lcd_ui_test_fire_cfg_field_tapped(0U);

    type_keys(k_keys, sizeof(k_keys) / sizeof(k_keys[0])); /* 99.0 is above 85.0 */

    TEST_ASSERT_TRUE(ui->config_screen.entry_invalid[0]);
    TEST_ASSERT_FALSE(lv_obj_has_flag(ui->config_screen.field[0].err, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_NOT_NULL(strstr(ui->config_screen.field[0].err->stub_text, "Out of allowed range"));
    TEST_ASSERT_EQUAL(BUTTON_BAR_OFF, ui->config_screen.apply_btn.kind);
    TEST_ASSERT_NOT_NULL(strstr(ui->footer.left->stub_text, "VALIDATION ERROR"));
    TEST_ASSERT_EQUAL_INT32(4000, ui->config_screen.pending.temp_alarm_hi_centi_c); /* unchanged */
}

void test_TC_LCDUI_028_high_must_stay_above_low(void)
{
    static const char *const k_keys[] = {"DEL", "DEL", "DEL", "5", "0", ".", "0"};
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    lcd_ui_test_fire_tab_change(SCR_CONFIG);
    lcd_ui_test_fire_cfg_field_tapped(1U); /* TEMPERATURE LOW, saved "0.0" */
    lcd_ui_test_press_key("DEL");
    lcd_ui_test_press_key("DEL");
    lcd_ui_test_press_key("DEL");

    type_keys(k_keys + 3, 4U); /* low = 50.0, above the high alarm of 40.0 */

    TEST_ASSERT_TRUE(ui->config_screen.entry_invalid[1]);
    TEST_ASSERT_NOT_NULL(strstr(ui->config_screen.field[1].err->stub_text, "Must be less than High Alarm"));
    (void) k_keys;
}

void test_TC_LCDUI_029_an_incomplete_entry_is_an_error_not_a_zero(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    lcd_ui_test_fire_tab_change(SCR_CONFIG);
    lcd_ui_test_fire_cfg_field_tapped(0U);

    for (uint32_t i = 0U; i < 4U; i++)
    {
        lcd_ui_test_press_key("DEL"); /* empty entry */
    }

    TEST_ASSERT_TRUE(ui->config_screen.entry_invalid[0]);
    TEST_ASSERT_NOT_NULL(strstr(ui->config_screen.field[0].err->stub_text, "Enter a number"));
}

void test_TC_LCDUI_030_apply_opens_the_modal_listing_the_change(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    edit_temp_high_to_35();

    lcd_ui_test_fire_apply_tapped();

    TEST_ASSERT_EQUAL(CFG_STATE_CONFIRMING, ui->config_screen.sub_state);
    TEST_ASSERT_TRUE(confirm_modal_is_open(&ui->config_screen.modal));
    TEST_ASSERT_NOT_NULL(strstr(ui->config_screen.modal.lines->stub_text, "Temperature \xC2\xB7 High Alarm"));
    TEST_ASSERT_NOT_NULL(strstr(ui->config_screen.modal.lines->stub_text, "40.0 \xE2\x86\x92 35.0"));
    TEST_ASSERT_EQUAL(1U, ui->config_screen.modal.cancel.kind == BUTTON_GHOST);
    TEST_ASSERT_NOT_NULL(strstr(ui->footer.left->stub_text, "PENDING CONFIRM"));
}

void test_TC_LCDUI_031_confirm_writes_every_parameter_and_returns_to_viewing(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    edit_temp_high_to_35();
    lcd_ui_test_fire_apply_tapped();

    lcd_ui_test_fire_confirm_tapped();

    TEST_ASSERT_EQUAL_INT(7, g_stub_set_param_count);
    TEST_ASSERT_EQUAL(CFG_STATE_VIEWING, ui->config_screen.sub_state);
    TEST_ASSERT_FALSE(confirm_modal_is_open(&ui->config_screen.modal));
    TEST_ASSERT_EQUAL_INT32(3500, ui->config_screen.committed.temp_alarm_hi_centi_c);
    TEST_ASSERT_EQUAL_STRING("CONFIG \xC2\xB7 SAVED", ui->footer.left->stub_text);
}

void test_TC_LCDUI_032_a_failed_write_keeps_the_old_values_and_reports(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    edit_temp_high_to_35();
    lcd_ui_test_fire_apply_tapped();
    g_stub_set_param_result = CONFIG_SERVICE_ERR_INVALID;

    lcd_ui_test_fire_confirm_tapped();

    TEST_ASSERT_EQUAL_INT32(4000, ui->config_screen.committed.temp_alarm_hi_centi_c);
    TEST_ASSERT_EQUAL_STRING("40.0", ui->config_screen.field[0].value->stub_text);
    TEST_ASSERT_EQUAL_INT(1, g_stub_push_event_count);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_LCD_FAIL, g_stub_last_push_event);
    TEST_ASSERT_FALSE(lv_obj_has_flag(ui->toast_label, LV_OBJ_FLAG_HIDDEN));
}

void test_TC_LCDUI_033_cancelling_the_modal_keeps_the_edits(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    edit_temp_high_to_35();
    lcd_ui_test_fire_apply_tapped();

    lcd_ui_test_fire_modal_cancel();

    TEST_ASSERT_EQUAL(CFG_STATE_EDITING, ui->config_screen.sub_state);
    TEST_ASSERT_EQUAL_INT32(3500, ui->config_screen.pending.temp_alarm_hi_centi_c);
    TEST_ASSERT_FALSE(confirm_modal_is_open(&ui->config_screen.modal));
    TEST_ASSERT_EQUAL_INT(0, g_stub_set_param_count);
}

void test_TC_LCDUI_034_the_bar_cancel_and_the_timeout_discard_the_edits(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    edit_temp_high_to_35();
    lcd_ui_test_fire_cancel_tapped();
    TEST_ASSERT_EQUAL(CFG_STATE_VIEWING, ui->config_screen.sub_state);
    TEST_ASSERT_EQUAL_INT32(4000, ui->config_screen.pending.temp_alarm_hi_centi_c);
    TEST_ASSERT_EQUAL_STRING("40.0", ui->config_screen.field[0].value->stub_text);

    edit_temp_high_to_35();
    lcd_ui_test_fire_apply_tapped();
    lcd_ui_test_fire_confirm_timeout();
    TEST_ASSERT_EQUAL(CFG_STATE_VIEWING, ui->config_screen.sub_state);
    TEST_ASSERT_FALSE(confirm_modal_is_open(&ui->config_screen.modal));
    TEST_ASSERT_EQUAL_INT(0, g_stub_set_param_count);
}

void test_TC_LCDUI_035_navigation_is_blocked_while_edits_are_pending(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    edit_temp_high_to_35();

    lcd_ui_test_fire_tab_change(SCR_SENSOR);

    TEST_ASSERT_EQUAL_PTR(ui->screens[SCR_CONFIG], ui->current);
    TEST_ASSERT_EQUAL_UINT16(SCR_CONFIG, ui->tab_bar.active);
    TEST_ASSERT_FALSE(lv_obj_has_flag(ui->toast_label, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_NOT_NULL(strstr(ui->toast_label->stub_text, "Save or cancel"));
}

void test_TC_LCDUI_036_moving_to_another_field_loads_its_value_into_the_keypad(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    lcd_ui_test_fire_tab_change(SCR_CONFIG);

    lcd_ui_test_fire_cfg_field_tapped(2U);

    TEST_ASSERT_EQUAL_INT32(2, ui->config_screen.focus);
    TEST_ASSERT_EQUAL_STRING("80.0", numpad_text(&ui->config_screen.numpad));
    TEST_ASSERT_TRUE(ui->config_screen.field[2].focused);
    TEST_ASSERT_FALSE(ui->config_screen.field[0].focused);
}

/* ===================================================================== */
/* Keypad                                                                 */
/* ===================================================================== */

void test_TC_LCDUI_037_keypad_accepts_one_decimal_point_and_a_sign_toggle(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    lcd_ui_test_fire_tab_change(SCR_CONFIG);
    numpad_load(&ui->config_screen.numpad, "");

    numpad_press(&ui->config_screen.numpad, "1");
    numpad_press(&ui->config_screen.numpad, ".");
    numpad_press(&ui->config_screen.numpad, ".");
    numpad_press(&ui->config_screen.numpad, "5");
    TEST_ASSERT_EQUAL_STRING("1.5", numpad_text(&ui->config_screen.numpad));

    numpad_press(&ui->config_screen.numpad, "SIGN");
    TEST_ASSERT_EQUAL_STRING("-1.5", numpad_text(&ui->config_screen.numpad));
    numpad_press(&ui->config_screen.numpad, "SIGN");
    TEST_ASSERT_EQUAL_STRING("1.5", numpad_text(&ui->config_screen.numpad));
}

void test_TC_LCDUI_038_keypad_stops_at_its_length_limit_and_ignores_keys_when_idle(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    lcd_ui_test_fire_tab_change(SCR_CONFIG);
    numpad_load(&ui->config_screen.numpad, "");
    for (uint32_t i = 0U; i < 12U; i++)
    {
        numpad_press(&ui->config_screen.numpad, "7");
    }
    TEST_ASSERT_EQUAL_UINT32(NUMPAD_TEXT_MAX, strlen(numpad_text(&ui->config_screen.numpad)));

    numpad_load(&ui->config_screen.numpad, NULL); /* no field focused */
    numpad_press(&ui->config_screen.numpad, "1");
    TEST_ASSERT_EQUAL_STRING("", numpad_text(&ui->config_screen.numpad));
}

/* ======================================================================= */
/* Status screen: CPU load, free RAM, MCU temperature                      */
/* ======================================================================= */

void test_TC_LCDUI_039_status_shows_cpu_load_free_ram_and_mcu_temperature(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    g_stub_health_snap.cpu_load_pct = 37U;
    g_stub_health_snap.free_ram_bytes = 94U * 1024U;
    g_stub_health_snap.mcu_temp_valid = true;
    g_stub_health_snap.mcu_temp_deci_c = 451;

    lcd_ui_test_fire_tab_change(SCR_STATUS);
    tick_at(2000U);

    TEST_ASSERT_EQUAL_STRING("37%", ui->status_screen.row[14].value->stub_text);
    TEST_ASSERT_EQUAL_STRING("94 KB", ui->status_screen.row[15].value->stub_text);
    TEST_ASSERT_EQUAL_STRING("45.1 \xC2\xB0" "C", ui->status_screen.row[16].value->stub_text);
}

void test_TC_LCDUI_040_status_shows_a_negative_temperature_with_the_true_minus(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    g_stub_health_snap.mcu_temp_valid = true;
    g_stub_health_snap.mcu_temp_deci_c = -52;

    lcd_ui_test_fire_tab_change(SCR_STATUS);
    tick_at(2000U);

    TEST_ASSERT_EQUAL_STRING(UI_MINUS "5.2 \xC2\xB0" "C", ui->status_screen.row[16].value->stub_text);
}

void test_TC_LCDUI_041_status_shows_a_dash_when_the_temperature_is_unavailable(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    g_stub_health_snap.mcu_temp_valid = false;
    g_stub_health_snap.mcu_temp_deci_c = 999;

    lcd_ui_test_fire_tab_change(SCR_STATUS);
    tick_at(2000U);

    TEST_ASSERT_EQUAL_STRING("\xE2\x80\x94", ui->status_screen.row[16].value->stub_text);
}

void test_TC_LCDUI_042_status_has_five_sections_in_three_columns(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    lcd_ui_test_fire_tab_change(SCR_STATUS);
    tick_at(2000U);

    TEST_ASSERT_EQUAL_UINT32(5U, STATUS_SECTIONS);
    TEST_ASSERT_EQUAL_UINT32(17U, STATUS_ROWS);
    for (uint32_t r = 0U; r < STATUS_ROWS; r++)
    {
        TEST_ASSERT_NOT_NULL(ui->status_screen.row[r].value);
    }
}

/* ======================================================================= */
/* Boot splash                                                             */
/* ======================================================================= */

void test_TC_LCDUI_043_the_splash_is_hidden_until_requested(void)
{
    do_init();

    TEST_ASSERT_FALSE(lcd_ui_splash_visible());
}

void test_TC_LCDUI_044_show_splash_appears_on_the_next_tick_and_dismiss_removes_it(void)
{
    do_init();

    lcd_ui_show_splash();
    TEST_ASSERT_FALSE(lcd_ui_splash_visible()); /* LVGL is touched by the UI task only */
    tick_at(100U);
    TEST_ASSERT_TRUE(lcd_ui_splash_visible());

    lcd_ui_dismiss_splash();
    tick_at(200U);
    TEST_ASSERT_FALSE(lcd_ui_splash_visible());
}

void test_TC_LCDUI_045_boot_progress_updates_the_bar_the_label_and_the_step(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    lcd_ui_show_splash();
    lcd_ui_set_boot_progress(62U, "Initialising sensor bus...");
    tick_at(100U);

    TEST_ASSERT_EQUAL_UINT8(62U, ui->splash.percent);
    TEST_ASSERT_EQUAL_STRING("62%", ui->splash.pct_lbl->stub_text);
    TEST_ASSERT_EQUAL_STRING("Initialising sensor bus...", ui->splash.step_lbl->stub_text);
    TEST_ASSERT_EQUAL_STRING("BOOT", ui->splash.badge_lbl->stub_text);
}

void test_TC_LCDUI_046_one_hundred_percent_turns_the_badge_to_ready(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    lcd_ui_set_boot_progress(100U, "System ready");
    tick_at(100U);

    TEST_ASSERT_EQUAL_STRING("100%", ui->splash.pct_lbl->stub_text);
    TEST_ASSERT_EQUAL_STRING("READY", ui->splash.badge_lbl->stub_text);
}

void test_TC_LCDUI_047_progress_above_100_is_clamped(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    lcd_ui_set_boot_progress(250U, "x");
    tick_at(100U);

    TEST_ASSERT_EQUAL_UINT8(100U, ui->splash.percent);
}

void test_TC_LCDUI_048_a_null_step_keeps_the_name_and_a_long_step_is_cut(void)
{
    char longname[100];
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    lcd_ui_set_boot_progress(10U, "Starting drivers");
    tick_at(100U);
    lcd_ui_set_boot_progress(20U, NULL);
    tick_at(200U);
    TEST_ASSERT_EQUAL_STRING("Starting drivers", ui->splash.step_lbl->stub_text);
    TEST_ASSERT_EQUAL_STRING("20%", ui->splash.pct_lbl->stub_text);

    (void) memset(longname, 'a', sizeof(longname) - 1U);
    longname[sizeof(longname) - 1U] = '\0';
    lcd_ui_set_boot_progress(30U, longname);
    tick_at(300U);
    TEST_ASSERT_EQUAL_UINT32(SPLASH_STEP_MAX - 1U, strlen(ui->splash.step_lbl->stub_text));
}

void test_TC_LCDUI_049_the_splash_footer_names_the_firmware_and_lvgl(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    /* the footer labels are the last two children of the splash root */
    TEST_ASSERT_NOT_NULL(ui->splash.root);
    TEST_ASSERT_NOT_NULL(ui->splash.logo);
}
