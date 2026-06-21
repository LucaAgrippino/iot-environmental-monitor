/**
 * @file test_lcd_ui.c
 * @brief Unit tests for LcdUi application component.
 *
 * Test cases TC-LCDUI-001 through TC-LCDUI-027.
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
/* setUp / tearDown                                                     */
/* ===================================================================== */

void setUp(void)
{
    lvgl_stub_reset();
    lcd_ui_reset_for_test();

    g_stub_display_ready = true;

    (void)memset(&g_stub_params, 0, sizeof(g_stub_params));
    g_stub_params.polling_interval_ms  = 1000U;
    g_stub_params.temp_alarm_high      = 4000;
    g_stub_params.temp_alarm_low       = 0;
    g_stub_params.humidity_alarm_high  = 8000U;
    g_stub_params.humidity_alarm_low   = 2000U;
    g_stub_params.pressure_alarm_high  = 10500U;
    g_stub_params.pressure_alarm_low   = 9500U;

    g_stub_set_param_count  = 0;
    g_stub_set_param_result = CONFIG_SERVICE_OK;

    (void)memset(&g_stub_sensor_snap,  0, sizeof(g_stub_sensor_snap));
    (void)memset(g_stub_alarm_states,  0, sizeof(g_stub_alarm_states));
    (void)memset(&g_stub_health_snap,  0, sizeof(g_stub_health_snap));

    g_stub_push_event_count  = 0;
    g_stub_last_push_event   = (health_event_t)0;
}

void tearDown(void) {}

/* ===================================================================== */
/* Helper: call lcd_ui_init with all valid stubs                        */
/* ===================================================================== */

static void do_init(void)
{
    lcd_ui_err_t ret = lcd_ui_init(&g_sensors, &g_alarms,
                                   &g_cfg_read, &g_cfg_write,
                                   &g_health,   &g_report_s);
    TEST_ASSERT_EQUAL(LCD_UI_ERR_OK, ret);
}

/* Helper: navigate to config screen and enter EDITING mode */
static void enter_config_editing(void)
{
    lcd_ui_test_fire_tab_change((uint16_t)SCR_CONFIG);
    lcd_ui_test_fire_cfg_field_tapped(0U);
    TEST_ASSERT_EQUAL(CFG_STATE_EDITING,
                      lcd_ui_test_get_instance()->config_screen.sub_state);
}

/* ===================================================================== */
/* TC-LCDUI-001: lcd_ui_init with all valid args returns OK             */
/* ===================================================================== */

void test_TC_LCDUI_001_init_valid_args_returns_ok(void)
{
    lcd_ui_err_t ret = lcd_ui_init(&g_sensors, &g_alarms,
                                   &g_cfg_read, &g_cfg_write,
                                   &g_health,   &g_report_s);
    TEST_ASSERT_EQUAL(LCD_UI_ERR_OK, ret);
    TEST_ASSERT_TRUE(lcd_ui_test_get_instance()->initialised);
}

/* ===================================================================== */
/* TC-LCDUI-002: NULL sensors → INVALID_ARG                             */
/* ===================================================================== */

void test_TC_LCDUI_002_init_null_sensors_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(LCD_UI_ERR_INVALID_ARG,
        lcd_ui_init(NULL, &g_alarms, &g_cfg_read, &g_cfg_write,
                    &g_health, &g_report_s));
}

/* ===================================================================== */
/* TC-LCDUI-003: NULL alarms → INVALID_ARG                              */
/* ===================================================================== */

void test_TC_LCDUI_003_init_null_alarms_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(LCD_UI_ERR_INVALID_ARG,
        lcd_ui_init(&g_sensors, NULL, &g_cfg_read, &g_cfg_write,
                    &g_health, &g_report_s));
}

/* ===================================================================== */
/* TC-LCDUI-004: NULL cfg_read → INVALID_ARG                            */
/* ===================================================================== */

void test_TC_LCDUI_004_init_null_cfg_read_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(LCD_UI_ERR_INVALID_ARG,
        lcd_ui_init(&g_sensors, &g_alarms, NULL, &g_cfg_write,
                    &g_health, &g_report_s));
}

/* ===================================================================== */
/* TC-LCDUI-005: NULL cfg_write → INVALID_ARG                           */
/* ===================================================================== */

void test_TC_LCDUI_005_init_null_cfg_write_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(LCD_UI_ERR_INVALID_ARG,
        lcd_ui_init(&g_sensors, &g_alarms, &g_cfg_read, NULL,
                    &g_health, &g_report_s));
}

/* ===================================================================== */
/* TC-LCDUI-006: NULL health → INVALID_ARG                              */
/* ===================================================================== */

void test_TC_LCDUI_006_init_null_health_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(LCD_UI_ERR_INVALID_ARG,
        lcd_ui_init(&g_sensors, &g_alarms, &g_cfg_read, &g_cfg_write,
                    NULL, &g_report_s));
}

/* ===================================================================== */
/* TC-LCDUI-007: NULL report → INVALID_ARG                              */
/* ===================================================================== */

void test_TC_LCDUI_007_init_null_report_returns_invalid_arg(void)
{
    TEST_ASSERT_EQUAL(LCD_UI_ERR_INVALID_ARG,
        lcd_ui_init(&g_sensors, &g_alarms, &g_cfg_read, &g_cfg_write,
                    &g_health, NULL));
}

/* ===================================================================== */
/* TC-LCDUI-008: Second call to lcd_ui_init → ALREADY_INIT             */
/* ===================================================================== */

void test_TC_LCDUI_008_init_twice_returns_already_init(void)
{
    do_init();
    TEST_ASSERT_EQUAL(LCD_UI_ERR_ALREADY_INIT,
        lcd_ui_init(&g_sensors, &g_alarms, &g_cfg_read, &g_cfg_write,
                    &g_health, &g_report_s));
}

/* ===================================================================== */
/* TC-LCDUI-009: graphics_get_display() returns NULL → GRAPHICS_INIT   */
/*               and push_event(HEALTH_EVENT_LCD_FAIL) called           */
/* ===================================================================== */

void test_TC_LCDUI_009_init_graphics_not_ready_returns_error(void)
{
    g_stub_display_ready = false;

    TEST_ASSERT_EQUAL(LCD_UI_ERR_GRAPHICS_INIT,
        lcd_ui_init(&g_sensors, &g_alarms, &g_cfg_read, &g_cfg_write,
                    &g_health, &g_report_s));
    TEST_ASSERT_EQUAL(1, g_stub_push_event_count);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_LCD_FAIL, g_stub_last_push_event);
}

/* ===================================================================== */
/* TC-LCDUI-010: After init, tabview and sensor screen widgets exist;   */
/*               current screen is sensor; waiting overlay is visible   */
/* ===================================================================== */

void test_TC_LCDUI_010_init_builds_four_screens(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    TEST_ASSERT_NOT_NULL(ui->tab_bar);
    TEST_ASSERT_NOT_NULL(ui->sensor_screen.waiting_label);
    TEST_ASSERT_EQUAL((screen_t *)&ui->sensor_screen, ui->current);
    TEST_ASSERT_FALSE(ui->sensor_screen.first_valid_received);

    /* Waiting overlay visible; value labels hidden (no data yet) */
    TEST_ASSERT_FALSE(
        lv_obj_has_flag(ui->sensor_screen.waiting_label, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_TRUE(
        lv_obj_has_flag(ui->sensor_screen.value_label[0], LV_OBJ_FLAG_HIDDEN));

    /* Config screen spinboxes created and disabled */
    TEST_ASSERT_NOT_NULL(ui->config_screen.spinbox[0]);
    TEST_ASSERT_TRUE(
        lv_obj_has_state(ui->config_screen.spinbox[0], LV_STATE_DISABLED));
}

/* ===================================================================== */
/* TC-LCDUI-011: Sensor on_refresh with cycle_count==0 keeps overlay   */
/* ===================================================================== */

void test_TC_LCDUI_011_sensor_waiting_overlay_while_no_data(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    g_stub_sensor_snap.cycle_count = 0U;
    ui->current->on_refresh(ui->current);

    TEST_ASSERT_FALSE(
        lv_obj_has_flag(ui->sensor_screen.waiting_label, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_TRUE(
        lv_obj_has_flag(ui->sensor_screen.value_label[0], LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_FALSE(ui->sensor_screen.first_valid_received);
}

/* ===================================================================== */
/* TC-LCDUI-012: Sensor on_refresh with valid data hides overlay        */
/* ===================================================================== */

void test_TC_LCDUI_012_sensor_first_valid_data_hides_overlay(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    g_stub_sensor_snap.cycle_count = 1U;
    g_stub_sensor_snap.readings[SENSOR_ID_TEMPERATURE].valid = true;
    g_stub_sensor_snap.readings[SENSOR_ID_TEMPERATURE].value = 2500; /* 25.00 °C ×100 */
    g_stub_sensor_snap.readings[SENSOR_ID_HUMIDITY].valid    = true;
    g_stub_sensor_snap.readings[SENSOR_ID_HUMIDITY].value    = 6000; /* 60.00 %RH ×100 */
    g_stub_sensor_snap.readings[SENSOR_ID_PRESSURE].valid    = true;
    g_stub_sensor_snap.readings[SENSOR_ID_PRESSURE].value    = 10130; /* 1013.0 hPa ×10 */

    ui->current->on_refresh(ui->current);

    TEST_ASSERT_TRUE(
        lv_obj_has_flag(ui->sensor_screen.waiting_label, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_FALSE(
        lv_obj_has_flag(ui->sensor_screen.value_label[0], LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_EQUAL_STRING("25.0",
        ui->sensor_screen.value_label[0]->stub_text);
    TEST_ASSERT_NOT_NULL(
        strstr(ui->sensor_screen.icon_label[0]->stub_text, "OK"));
    TEST_ASSERT_TRUE(ui->sensor_screen.first_valid_received);
}

/* ===================================================================== */
/* TC-LCDUI-013: Invalid sensor reading renders "--" and "x" icon       */
/* ===================================================================== */

void test_TC_LCDUI_013_sensor_invalid_reading_shows_dash(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    g_stub_sensor_snap.cycle_count = 1U; /* cycle running but no valid reading */
    g_stub_sensor_snap.readings[SENSOR_ID_TEMPERATURE].valid = false;
    g_stub_sensor_snap.readings[SENSOR_ID_HUMIDITY].valid    = false;
    g_stub_sensor_snap.readings[SENSOR_ID_PRESSURE].valid    = false;

    ui->current->on_refresh(ui->current);

    TEST_ASSERT_EQUAL_STRING("--",
        ui->sensor_screen.value_label[0]->stub_text);
    TEST_ASSERT_NOT_NULL(
        strstr(ui->sensor_screen.icon_label[0]->stub_text, "ERROR"));
}

/* ===================================================================== */
/* TC-LCDUI-014: on_exit then on_enter resets first_valid_received       */
/* ===================================================================== */

void test_TC_LCDUI_014_sensor_screen_re_enter_resets_flag(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    /* First make first_valid_received true */
    g_stub_sensor_snap.cycle_count = 1U;
    g_stub_sensor_snap.readings[SENSOR_ID_TEMPERATURE].valid = true;
    ui->current->on_refresh(ui->current);
    TEST_ASSERT_TRUE(ui->sensor_screen.first_valid_received);

    /* Navigate to status and back (simulated via direct on_exit/on_enter) */
    ui->sensor_screen.base.on_exit((screen_t *)&ui->sensor_screen);
    ui->sensor_screen.base.on_enter((screen_t *)&ui->sensor_screen);

    TEST_ASSERT_FALSE(ui->sensor_screen.first_valid_received);
    /* Waiting label visible again after re-enter */
    TEST_ASSERT_FALSE(
        lv_obj_has_flag(ui->sensor_screen.waiting_label, LV_OBJ_FLAG_HIDDEN));
}

/* ===================================================================== */
/* TC-LCDUI-015: Status on_refresh formats uptime into metric_label[0]  */
/* ===================================================================== */

void test_TC_LCDUI_015_status_refresh_formats_uptime(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    g_stub_health_snap.uptime_s         = 123U;
    g_stub_health_snap.sensor_fail_count = 2U;

    ui->status_screen.base.on_enter((screen_t *)&ui->status_screen);
    ui->status_screen.base.on_refresh((screen_t *)&ui->status_screen);

    /* metric_label[0] should contain the uptime value */
    TEST_ASSERT_NOT_NULL(
        strstr(ui->status_screen.metric_label[0]->stub_text, "123"));
    /* metric_label[1] should contain the sensor fail count */
    TEST_ASSERT_NOT_NULL(
        strstr(ui->status_screen.metric_label[1]->stub_text, "2"));
}

/* ===================================================================== */
/* TC-LCDUI-016: Alarm screen with no active alarms shows no-alarms     */
/* ===================================================================== */

void test_TC_LCDUI_016_alarm_all_clear_shows_no_alarms_label(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    /* All states default to ALARM_STATE_CLEAR (memset in setUp) */
    ui->alarm_screen.base.on_enter((screen_t *)&ui->alarm_screen);
    ui->alarm_screen.base.on_refresh((screen_t *)&ui->alarm_screen);

    TEST_ASSERT_FALSE(
        lv_obj_has_flag(ui->alarm_screen.no_alarms_label, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_TRUE(
        lv_obj_has_flag(ui->alarm_screen.list_widget, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_EQUAL(0U, lv_list_get_size(ui->alarm_screen.list_widget));
}

/* ===================================================================== */
/* TC-LCDUI-017: One ACTIVE_HIGH alarm renders list entry, hides label  */
/* ===================================================================== */

void test_TC_LCDUI_017_alarm_active_high_shows_list_entry(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    g_stub_alarm_states[SENSOR_ID_TEMPERATURE] = ALARM_STATE_ACTIVE_HIGH;

    ui->alarm_screen.base.on_enter((screen_t *)&ui->alarm_screen);
    ui->alarm_screen.base.on_refresh((screen_t *)&ui->alarm_screen);

    TEST_ASSERT_TRUE(
        lv_obj_has_flag(ui->alarm_screen.no_alarms_label, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_FALSE(
        lv_obj_has_flag(ui->alarm_screen.list_widget, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_EQUAL(1U, lv_list_get_size(ui->alarm_screen.list_widget));
    TEST_ASSERT_NOT_NULL(
        strstr(ui->alarm_screen.list_widget->stub_list_items[0], "HIGH"));
}

/* ===================================================================== */
/* TC-LCDUI-018: config_screen on_enter loads config params to spinboxes */
/* ===================================================================== */

void test_TC_LCDUI_018_config_enter_loads_committed_to_spinboxes(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    g_stub_params.polling_interval_ms = 3000U;
    g_stub_params.temp_alarm_high     = 5000;

    lcd_ui_test_fire_tab_change((uint16_t)SCR_CONFIG);

    /* spinbox[0] = polling_interval_ms */
    TEST_ASSERT_EQUAL(3000, lv_spinbox_get_value(ui->config_screen.spinbox[0]));
    /* spinbox[1] = temp_alarm_hi_centi_c */
    TEST_ASSERT_EQUAL(5000, lv_spinbox_get_value(ui->config_screen.spinbox[1]));
    /* All spinboxes disabled in VIEWING state */
    TEST_ASSERT_TRUE(
        lv_obj_has_state(ui->config_screen.spinbox[0], LV_STATE_DISABLED));
    /* Action buttons hidden */
    TEST_ASSERT_TRUE(
        lv_obj_has_flag(ui->config_screen.cancel_btn, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_TRUE(
        lv_obj_has_flag(ui->config_screen.apply_btn, LV_OBJ_FLAG_HIDDEN));
}

/* ===================================================================== */
/* TC-LCDUI-019: Tapping a config field enters EDITING state            */
/* ===================================================================== */

void test_TC_LCDUI_019_config_field_tapped_enters_editing(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    lcd_ui_test_fire_tab_change((uint16_t)SCR_CONFIG);
    TEST_ASSERT_EQUAL(CFG_STATE_VIEWING, ui->config_screen.sub_state);

    lcd_ui_test_fire_cfg_field_tapped(0U);

    TEST_ASSERT_EQUAL(CFG_STATE_EDITING, ui->config_screen.sub_state);
    /* Spinboxes enabled */
    TEST_ASSERT_FALSE(
        lv_obj_has_state(ui->config_screen.spinbox[0], LV_STATE_DISABLED));
    /* Action buttons visible */
    TEST_ASSERT_FALSE(
        lv_obj_has_flag(ui->config_screen.cancel_btn, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_FALSE(
        lv_obj_has_flag(ui->config_screen.apply_btn, LV_OBJ_FLAG_HIDDEN));
    /* Pending initialised from committed */
    TEST_ASSERT_EQUAL(g_stub_params.polling_interval_ms,
                      ui->config_screen.pending.polling_interval_ms);
}

/* ===================================================================== */
/* TC-LCDUI-020: Valid spinbox change updates pending; hides error label */
/* ===================================================================== */

void test_TC_LCDUI_020_config_field_changed_valid_updates_pending(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    enter_config_editing();

    /* Force spinbox[0] to a valid value and fire the changed event */
    ui->config_screen.spinbox[0]->stub_value = 5000; /* 5000 ms — valid */
    lcd_ui_test_fire_cfg_field_changed(0U);

    TEST_ASSERT_EQUAL(5000U, ui->config_screen.pending.polling_interval_ms);
    TEST_ASSERT_TRUE(
        lv_obj_has_flag(ui->config_screen.err_label[0], LV_OBJ_FLAG_HIDDEN));
}

/* ===================================================================== */
/* TC-LCDUI-021: Out-of-range spinbox change shows error; pending stays */
/* ===================================================================== */

void test_TC_LCDUI_021_config_field_changed_invalid_shows_error(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    enter_config_editing();

    uint32_t original = ui->config_screen.pending.polling_interval_ms;

    /* Force value below min=100 */
    ui->config_screen.spinbox[0]->stub_value = 50;
    lcd_ui_test_fire_cfg_field_changed(0U);

    /* Pending unchanged */
    TEST_ASSERT_EQUAL(original, ui->config_screen.pending.polling_interval_ms);
    /* Error label shown */
    TEST_ASSERT_FALSE(
        lv_obj_has_flag(ui->config_screen.err_label[0], LV_OBJ_FLAG_HIDDEN));
}

/* ===================================================================== */
/* TC-LCDUI-022: Apply button moves to CONFIRMING; dialog shown; timer  */
/* ===================================================================== */

void test_TC_LCDUI_022_config_apply_tapped_enters_confirming(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    enter_config_editing();

    lcd_ui_test_fire_apply_tapped();

    TEST_ASSERT_EQUAL(CFG_STATE_CONFIRMING, ui->config_screen.sub_state);
    TEST_ASSERT_FALSE(
        lv_obj_has_flag(ui->config_screen.confirm_dialog, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_FALSE(ui->config_screen.confirm_timeout_timer->paused);
}

/* ===================================================================== */
/* TC-LCDUI-023: Confirm button calls set_param×7; committed=pending;   */
/*               returns to VIEWING; dialog hidden; timer paused         */
/* ===================================================================== */

void test_TC_LCDUI_023_config_confirm_ok_commits_and_returns_viewing(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    enter_config_editing();

    /* Edit poll interval */
    ui->config_screen.spinbox[0]->stub_value = 7000;
    lcd_ui_test_fire_cfg_field_changed(0U);

    lcd_ui_test_fire_apply_tapped();
    lcd_ui_test_fire_confirm_tapped();

    /* All 7 fields committed via set_param */
    TEST_ASSERT_EQUAL(7, g_stub_set_param_count);
    /* committed now reflects the edited value */
    TEST_ASSERT_EQUAL(7000U, ui->config_screen.committed.polling_interval_ms);
    /* VIEWING state */
    TEST_ASSERT_EQUAL(CFG_STATE_VIEWING, ui->config_screen.sub_state);
    TEST_ASSERT_TRUE(
        lv_obj_has_flag(ui->config_screen.confirm_dialog, LV_OBJ_FLAG_HIDDEN));
    /* Spinboxes disabled again */
    TEST_ASSERT_TRUE(
        lv_obj_has_state(ui->config_screen.spinbox[0], LV_STATE_DISABLED));
}

/* ===================================================================== */
/* TC-LCDUI-024: set_param fails → committed unchanged; toast shown;    */
/*               push_event called; returns to VIEWING                   */
/* ===================================================================== */

void test_TC_LCDUI_024_config_confirm_fail_shows_toast(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    enter_config_editing();

    lcd_ui_test_fire_apply_tapped();

    /* Make all set_param calls fail */
    g_stub_set_param_result = CONFIG_SERVICE_ERR_PERSIST;
    lcd_ui_test_fire_confirm_tapped();

    /* committed unchanged — still from g_stub_params */
    TEST_ASSERT_EQUAL(g_stub_params.polling_interval_ms,
                      ui->config_screen.committed.polling_interval_ms);
    /* Toast label shown */
    TEST_ASSERT_FALSE(lv_obj_has_flag(ui->toast_label, LV_OBJ_FLAG_HIDDEN));
    /* Health event reported */
    TEST_ASSERT_EQUAL(1, g_stub_push_event_count);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_LCD_FAIL, g_stub_last_push_event);
    /* Returns to VIEWING */
    TEST_ASSERT_EQUAL(CFG_STATE_VIEWING, ui->config_screen.sub_state);
}

/* ===================================================================== */
/* TC-LCDUI-025: Cancel button discards pending; returns to VIEWING     */
/* ===================================================================== */

void test_TC_LCDUI_025_config_cancel_tapped_discards_pending(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    enter_config_editing();

    /* Edit poll interval */
    ui->config_screen.spinbox[0]->stub_value = 9000;
    lcd_ui_test_fire_cfg_field_changed(0U);
    TEST_ASSERT_EQUAL(9000U, ui->config_screen.pending.polling_interval_ms);

    lcd_ui_test_fire_cancel_tapped();

    TEST_ASSERT_EQUAL(CFG_STATE_VIEWING, ui->config_screen.sub_state);
    /* No set_param called */
    TEST_ASSERT_EQUAL(0, g_stub_set_param_count);
    /* committed unchanged */
    TEST_ASSERT_EQUAL(g_stub_params.polling_interval_ms,
                      ui->config_screen.committed.polling_interval_ms);
    /* Action buttons hidden */
    TEST_ASSERT_TRUE(
        lv_obj_has_flag(ui->config_screen.cancel_btn, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_TRUE(
        lv_obj_has_flag(ui->config_screen.apply_btn, LV_OBJ_FLAG_HIDDEN));
}

/* ===================================================================== */
/* TC-LCDUI-026: Confirm timeout discards pending; returns to VIEWING   */
/* ===================================================================== */

void test_TC_LCDUI_026_config_confirm_timeout_discards_pending(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();
    enter_config_editing();

    lcd_ui_test_fire_apply_tapped();
    TEST_ASSERT_EQUAL(CFG_STATE_CONFIRMING, ui->config_screen.sub_state);
    TEST_ASSERT_FALSE(ui->config_screen.confirm_timeout_timer->paused);

    lcd_ui_test_fire_confirm_timeout();

    TEST_ASSERT_EQUAL(CFG_STATE_VIEWING, ui->config_screen.sub_state);
    TEST_ASSERT_TRUE(ui->config_screen.confirm_timeout_timer->paused);
    TEST_ASSERT_TRUE(
        lv_obj_has_flag(ui->config_screen.confirm_dialog, LV_OBJ_FLAG_HIDDEN));
    /* No set_param called */
    TEST_ASSERT_EQUAL(0, g_stub_set_param_count);
}

/* ===================================================================== */
/* TC-LCDUI-027: config_screen on_exit while EDITING discards pending   */
/* ===================================================================== */

void test_TC_LCDUI_027_config_on_exit_while_editing_discards(void)
{
    do_init();
    lcd_ui_t *ui = lcd_ui_test_get_instance();

    lcd_ui_test_fire_tab_change((uint16_t)SCR_CONFIG);
    lcd_ui_test_fire_cfg_field_tapped(0U);
    TEST_ASSERT_EQUAL(CFG_STATE_EDITING, ui->config_screen.sub_state);

    /* Force-call on_exit (normal navigation is blocked in EDITING) */
    ui->config_screen.base.on_exit((screen_t *)&ui->config_screen);

    TEST_ASSERT_EQUAL(CFG_STATE_VIEWING, ui->config_screen.sub_state);
    TEST_ASSERT_TRUE(
        lv_obj_has_flag(ui->config_screen.confirm_dialog, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_TRUE(
        lv_obj_has_flag(ui->config_screen.cancel_btn, LV_OBJ_FLAG_HIDDEN));
    TEST_ASSERT_TRUE(
        lv_obj_has_flag(ui->config_screen.apply_btn, LV_OBJ_FLAG_HIDDEN));
    /* Spinboxes re-disabled */
    TEST_ASSERT_TRUE(
        lv_obj_has_state(ui->config_screen.spinbox[0], LV_STATE_DISABLED));
}
