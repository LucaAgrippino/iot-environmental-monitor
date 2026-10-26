/**
 * @file test_console_service_gw_tui.c
 * @brief Unity tests for ConsoleService: init, REPL parser, commands, workflows, self-test
 *        and the service loop (companion §22.1, §22.6 .. §22.11).
 *
 * Commands whose back-end does not exist on the Gateway yet (WiFi credentials, Modbus line
 * settings, profile add / remove) are tested for "validate, then report not available", not for
 * staging (reconciliation in the LLD).
 */

#include <stdio.h>
#include <string.h>

#include "chart.h"
#include "commands.h"
#include "console_service.h"
#include "console_spies.h"
#include "freertos_mock.h"
#include "fmt.h"
#include "input.h"
#include "model.h"
#include "render.h"
#include "repl.h"
#include "termout.h"
#include "theme.h"
#include "ui_state.h"
#include "unity.h"

#define NOTIFY_RX (1UL << 0)
#define NOTIFY_DATA_TICK (1UL << 1)
#define NOTIFY_BLINK (1UL << 2)

static console_deps_t s_deps;

void setUp(void)
{
    mock_freertos_reset();
    spy_reset();
    console_service_reset_for_test();
    s_deps = spy_deps();
    commands_init(&s_deps);
    repl_init();
}

void tearDown(void)
{
}

/* ---------------------------------------------------------------------- */
/* Scrollback inspection                                                   */
/* ---------------------------------------------------------------------- */

static bool has_line(const char *needle)
{
    for (uint16_t i = 0U; i < repl_count(); i++)
    {
        if (strstr(repl_line(i)->text, needle) != NULL)
        {
            return true;
        }
    }
    return false;
}

static const scrollback_line_t *find_line(const char *needle)
{
    for (uint16_t i = 0U; i < repl_count(); i++)
    {
        if (strstr(repl_line(i)->text, needle) != NULL)
        {
            return repl_line(i);
        }
    }
    return NULL;
}

static const char *last_text(void)
{
    return repl_line((uint16_t) (repl_count() - 1U))->text;
}

static console_service_err_t run(const char *line)
{
    return repl_execute(line);
}

/* ---------------------------------------------------------------------- */
/* Initialisation                                                          */
/* ---------------------------------------------------------------------- */

/* TC-GCS-001 .. 009 */
void test_init_rejects_a_null_argument_or_member(void)
{
    console_deps_t d = spy_deps();

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_NULL_ARG, console_service_init(NULL));

    d.uart = NULL;
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_NULL_ARG, console_service_init(&d));
    d = spy_deps();
    d.sensors = NULL;
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_NULL_ARG, console_service_init(&d));
    d = spy_deps();
    d.alarms = NULL;
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_NULL_ARG, console_service_init(&d));
    d = spy_deps();
    d.cfg_read = NULL;
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_NULL_ARG, console_service_init(&d));
    d = spy_deps();
    d.cfg_write = NULL;
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_NULL_ARG, console_service_init(&d));
    d = spy_deps();
    d.health = NULL;
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_NULL_ARG, console_service_init(&d));
    d = spy_deps();
    d.prof_read = NULL;
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_NULL_ARG, console_service_init(&d));
}

/* TC-GCS-010, 011 */
void test_valid_init_clears_state_attaches_rx_and_creates_the_timers(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, console_service_init(&s_deps));

    TEST_ASSERT_NOT_NULL(g_spy_rx_cb);
    TEST_ASSERT_EQUAL_UINT32(2U, g_mock_xTimerCreateStatic_call_count);
    TEST_ASSERT_FALSE(commands_prov_dirty());
    TEST_ASSERT_FALSE(commands_cfg_dirty());
    TEST_ASSERT_EQUAL_size_t(0U, g_spy_tx_len); /* nothing drawn until the task runs */
}

void test_init_reports_a_uart_that_cannot_attach(void)
{
    g_spy_attach_rc = DEBUG_UART_ERR_RX_ALREADY_ATTACHED;

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_APPLY_FAILED, console_service_init(&s_deps));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_NOT_INIT, console_service_cycle_for_test(0U));
}

/* TC-GCS-012 */
void test_run_once_before_init_reports_not_init(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_NOT_INIT, console_service->run_once());
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_NOT_INIT, console_service_cycle_for_test(0U));
}

/* TC-GCS-013 */
void test_first_cycle_sends_the_screen_setup_and_a_size_query_then_the_frame(void)
{
    (void) console_service_init(&s_deps);
    (void) console_service_cycle_for_test(NOTIFY_DATA_TICK);
    g_spy_tx[g_spy_tx_len] = 0U;

    TEST_ASSERT_EQUAL_UINT8(0x1B, g_spy_tx[0]);
    TEST_ASSERT_NOT_NULL(strstr((char *) g_spy_tx, "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H"));
    TEST_ASSERT_NOT_NULL(strstr((char *) g_spy_tx, "\x1b[18t"));
    TEST_ASSERT_NOT_NULL(strstr((char *) g_spy_tx, "MAIN MENU"));
    TEST_ASSERT_NOT_NULL(strstr((char *) g_spy_tx, "ENVIRONMENTAL GATEWAY"));
}

/* ---------------------------------------------------------------------- */
/* Parser                                                                  */
/* ---------------------------------------------------------------------- */

/* TC-GCS-118 */
void test_empty_input_echoes_a_bare_prompt_and_runs_nothing(void)
{
    const uint16_t before = repl_count();

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("   "));
    TEST_ASSERT_EQUAL_UINT16(before + 1U, repl_count());
    TEST_ASSERT_EQUAL_STRING("envmon>", last_text());
}

/* TC-GCS-117 */
void test_unknown_command_is_reported_in_the_warning_colour(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_UNKNOWN_KEY, run("frobnicate now"));

    const scrollback_line_t *l = find_line("unknown command: frobnicate");
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL_UINT8(PAL_WARN, l->fg);
    TEST_ASSERT_TRUE(has_line("envmon> frobnicate now"));
}

void test_commands_are_case_insensitive_and_extra_spaces_are_ignored(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("  UpTime   "));
    TEST_ASSERT_TRUE(has_line("uptime 3d 14:22:05"));
}

void test_wrong_argument_count_is_a_validation_error(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("read"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("set t.hi"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("uptime now"));
    TEST_ASSERT_TRUE(has_line("[ERR]"));
}

/* TC-GCS-120 */
void test_scrollback_evicts_the_oldest_line_at_200(void)
{
    repl_clear();
    for (uint16_t i = 0U; i < 205U; i++)
    {
        char t[16];

        (void) snprintf(t, sizeof(t), "line %u", (unsigned) i);
        repl_puts(PAL_INK, t);
    }
    TEST_ASSERT_EQUAL_UINT16(REPL_SCROLLBACK_MAX, repl_count());
    TEST_ASSERT_EQUAL_STRING("line 5", repl_line(0U)->text);
    TEST_ASSERT_EQUAL_STRING("line 204", repl_line(199U)->text);
    TEST_ASSERT_NULL(repl_line(200U));
}

void test_long_lines_are_cut_on_a_character_boundary(void)
{
    char t[120];

    repl_clear();
    for (uint32_t i = 0U; i < 30U; i++)
    {
        (void) strcat(&t[i * 3U], "");
    }
    t[0] = '\0';
    for (uint32_t i = 0U; i < 30U; i++)
    {
        (void) strcat(t, "●"); /* 3 bytes each */
    }
    repl_puts(PAL_INK, t);

    TEST_ASSERT_LESS_THAN_size_t(REPL_TEXT_MAX, strlen(repl_line(0U)->text));
    TEST_ASSERT_EQUAL_size_t(0U, strlen(repl_line(0U)->text) % 3U);
}

/* ---------------------------------------------------------------------- */
/* Shared diagnostic commands                                              */
/* ---------------------------------------------------------------------- */

/* TC-GCS-100, 101 */
void test_help_lists_every_command_and_question_mark_is_an_alias(void)
{
    size_t n;
    const cmd_entry_t *table = commands_table(&n);

    repl_clear();
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("help"));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT16((uint16_t) n, repl_count());
    for (size_t i = 0U; i < n; i++)
    {
        TEST_ASSERT_TRUE(has_line(table[i].token));
    }
    repl_clear();
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("?"));
    TEST_ASSERT_TRUE(has_line("Available commands"));
}

/* TC-GCS-102 */
void test_version_prints_firmware_and_part_number(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("version"));
    TEST_ASSERT_TRUE(has_line("STM32L475VGT6"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("about"));
}

/* TC-GCS-103 */
void test_serial_prints_the_unique_id_in_hex(void)
{
    g_spy_uid[0] = 0x002D004FU;
    g_spy_uid[1] = 0x34425010U;
    g_spy_uid[2] = 0x20323848U;

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("serial"));
    TEST_ASSERT_TRUE(has_line("002D004F3442501020323848"));
}

/* TC-GCS-104 */
void test_sensors_prints_a_row_per_channel_with_value_status_and_time(void)
{
    g_spy_snapshot.readings[SENSOR_ID_TEMPERATURE].timestamp.epoch = 12U * 3600U + 34U * 60U + 56U;
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("sensors"));

    TEST_ASSERT_TRUE(has_line("SENSOR"));
    TEST_ASSERT_TRUE(has_line("temp"));
    TEST_ASSERT_TRUE(has_line("22.0 °C"));
    TEST_ASSERT_TRUE(has_line("12:34:56"));
    TEST_ASSERT_TRUE(has_line("50.0 %RH"));
    TEST_ASSERT_TRUE(has_line("1013 hPa"));
    TEST_ASSERT_TRUE(has_line("● ok"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("ls"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("sensor"));
}

void test_sensors_marks_an_invalid_channel_stale_and_a_hot_one_alarm(void)
{
    spy_set_reading(SENSOR_ID_PRESSURE, 0, false);
    spy_set_reading(SENSOR_ID_TEMPERATURE, 4300, true);
    (void) run("sensors");

    TEST_ASSERT_TRUE(has_line("▲ stale"));
    TEST_ASSERT_TRUE(has_line("■ alarm"));
    TEST_ASSERT_EQUAL_UINT8(PAL_ERR, find_line("43.0 °C")->fg);
    TEST_ASSERT_EQUAL_UINT8(PAL_WARN, find_line("▲ stale")->fg);
}

/* TC-GCS-105 */
void test_read_prints_one_channel(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("read t"));
    TEST_ASSERT_TRUE(has_line("TEMPERATURE  22.0 °C  ● ok"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("read press"));
    TEST_ASSERT_TRUE(has_line("PRESSURE  1013 hPa"));
}

/* TC-GCS-106 */
void test_read_with_an_unknown_channel_is_an_error(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("read x"));
    TEST_ASSERT_TRUE(has_line("[ERR] usage: read <t|h|p>"));
}

/* TC-GCS-107 */
void test_status_prints_the_health_snapshot(void)
{
    g_spy_health.mqtt_publishes_sent = 1247U;
    g_spy_health.mqtt_publishes_failed = 3U;
    g_spy_health.buffer_entry_count = 12U;
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("status"));

    TEST_ASSERT_TRUE(has_line("WiFi      ● connected  RSSI -42 dBm"));
    TEST_ASSERT_TRUE(has_line("1247 sent / 3 failed"));
    TEST_ASSERT_TRUE(has_line("Modbus    ● online"));
    TEST_ASSERT_TRUE(has_line("12 / 12 valid"));
    TEST_ASSERT_TRUE(has_line("3d 14:22:05"));
    TEST_ASSERT_TRUE(has_line("12 entries"));
}

void test_status_shows_the_links_down_in_red(void)
{
    g_spy_health.wifi_rssi_dbm = 0;
    g_spy_health.cloud_connected = false;
    g_spy_health.modbus_link_online = false;
    (void) run("status");

    TEST_ASSERT_EQUAL_UINT8(PAL_ERR, find_line("WiFi")->fg);
    TEST_ASSERT_EQUAL_UINT8(PAL_ERR, find_line("MQTT")->fg);
    TEST_ASSERT_EQUAL_UINT8(PAL_WARN, find_line("Modbus")->fg);
}

/* TC-GCS-108 */
void test_alarms_with_none_active_says_so_in_green(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("alarms"));
    TEST_ASSERT_EQUAL_UINT8(PAL_OK, find_line("No active alarms")->fg);
}

/* TC-GCS-109 */
void test_alarms_lists_the_active_ones_with_the_crossed_limit(void)
{
    g_spy_alarm_states[SENSOR_ID_TEMPERATURE] = ALARM_STATE_ACTIVE_HIGH;
    spy_set_reading(SENSOR_ID_TEMPERATURE, 4300, true);
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("alarms"));

    TEST_ASSERT_TRUE(has_line("TEMPERATURE"));
    TEST_ASSERT_TRUE(has_line("HIGH"));
    TEST_ASSERT_TRUE(has_line("43.0 °C / 40.0 °C"));
    TEST_ASSERT_EQUAL_UINT8(PAL_ERR, find_line("HIGH")->fg);
}

/* TC-GCS-110 */
void test_thresholds_prints_hi_and_lo_per_channel(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("thresholds"));
    TEST_ASSERT_TRUE(has_line("40.0 °C"));
    TEST_ASSERT_TRUE(has_line("80.0 %RH"));
    TEST_ASSERT_TRUE(has_line("1050 hPa"));
    TEST_ASSERT_TRUE(has_line("950 hPa"));
}

/* TC-GCS-114 */
void test_uptime_prints_days_hours_minutes_seconds(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("uptime"));
    TEST_ASSERT_TRUE(has_line("uptime 3d 14:22:05"));
}

/* TC-GCS-115 */
void test_clear_empties_the_scrollback(void)
{
    (void) run("uptime");
    TEST_ASSERT_GREATER_THAN_UINT16(0U, repl_count());

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("clear"));
    TEST_ASSERT_EQUAL_UINT16(0U, repl_count());
    (void) run("cls");
    TEST_ASSERT_EQUAL_UINT16(0U, repl_count());
}

/* ---------------------------------------------------------------------- */
/* set                                                                     */
/* ---------------------------------------------------------------------- */

/* TC-GCS-111 */
void test_set_applies_a_threshold_through_the_config_manager(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("set t.hi 24"));

    TEST_ASSERT_EQUAL_UINT32(1U, g_spy_set_calls);
    TEST_ASSERT_EQUAL(CONFIG_PARAM_TEMP_ALARM_HIGH, g_spy_set_ids[0]);
    int16_t v;
    memcpy(&v, g_spy_set_values[0], sizeof(v));
    TEST_ASSERT_EQUAL_INT16(2400, v);
    TEST_ASSERT_TRUE(has_line("temp.hi → 24.0 °C   applied"));
    TEST_ASSERT_EQUAL_UINT8(PAL_OK, find_line("applied")->fg);
    TEST_ASSERT_TRUE(commands_take_persist_due());
    TEST_ASSERT_FALSE(commands_take_persist_due());
}

void test_set_handles_each_channel_and_side_with_the_right_scale(void)
{
    uint16_t u;

    (void) run("set h.lo 30.5");
    TEST_ASSERT_EQUAL(CONFIG_PARAM_HUMIDITY_ALARM_LOW, g_spy_set_ids[0]);
    memcpy(&u, g_spy_set_values[0], sizeof(u));
    TEST_ASSERT_EQUAL_UINT16(3050U, u);

    (void) run("SET PRESS.HI 1040.5");
    TEST_ASSERT_EQUAL(CONFIG_PARAM_PRESSURE_ALARM_HIGH, g_spy_set_ids[1]);
    memcpy(&u, g_spy_set_values[1], sizeof(u));
    TEST_ASSERT_EQUAL_UINT16(10405U, u);

    (void) run("set t.lo -5");
    int16_t i;
    memcpy(&i, g_spy_set_values[2], sizeof(i));
    TEST_ASSERT_EQUAL_INT16(-500, i);
}

/* TC-GCS-112 */
void test_set_with_a_non_numeric_value_is_rejected_before_the_service(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("set t.hi abc"));
    TEST_ASSERT_EQUAL_UINT32(0U, g_spy_set_calls);
    TEST_ASSERT_TRUE(has_line("[ERR]"));
}

/* TC-GCS-113 */
void test_set_with_a_bad_key_is_an_unknown_key(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_UNKNOWN_KEY, run("set x.hi 24"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_UNKNOWN_KEY, run("set t.mid 24"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_UNKNOWN_KEY, run("set temp 24"));
    TEST_ASSERT_EQUAL_UINT32(0U, g_spy_set_calls);
}

void test_set_refused_by_the_config_service_reports_why(void)
{
    g_spy_set_rc = CONFIG_SERVICE_ERR_INVALID;
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("set t.hi 24"));
    TEST_ASSERT_TRUE(has_line("inconsistent"));
    TEST_ASSERT_FALSE(commands_take_persist_due());

    g_spy_set_rc = CONFIG_SERVICE_ERR_NOT_INIT;
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_APPLY_FAILED, run("set t.hi 24"));
}

void test_set_out_of_the_storage_range_is_rejected_before_the_service(void)
{
    g_spy_set_calls = 0U;
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("set t.hi 400"));   /* 40000 > int16 */
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("set h.hi -1"));    /* unsigned field */
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("set p.hi 99999")); /* 999990 > uint16 */
    TEST_ASSERT_EQUAL_UINT32(0U, g_spy_set_calls);
}

/* ---------------------------------------------------------------------- */
/* Network and Gateway status                                              */
/* ---------------------------------------------------------------------- */

/* TC-GCS-116, 186..188 */
void test_net_wifi_mqtt_and_modbus_status(void)
{
    (void) snprintf(g_spy_cfg.mqtt_broker, sizeof(g_spy_cfg.mqtt_broker), "iot.example.com");
    g_spy_health.mqtt_reconnect_count = 2U;

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("wifi status"));
    TEST_ASSERT_TRUE(has_line("● connected"));
    TEST_ASSERT_TRUE(has_line("-42 dBm"));

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("mqtt status"));
    TEST_ASSERT_TRUE(has_line("iot.example.com:8883"));
    TEST_ASSERT_TRUE(has_line("reconnect 2"));

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("modbus status"));
    TEST_ASSERT_TRUE(has_line("● online"));

    repl_clear();
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("net"));
    TEST_ASSERT_TRUE(has_line("wifi"));
    TEST_ASSERT_TRUE(has_line("mqtt"));
    TEST_ASSERT_TRUE(has_line("modbus"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("network"));
}

void test_mqtt_status_shows_an_unset_broker(void)
{
    (void) run("mqtt status");
    TEST_ASSERT_TRUE(has_line("(not set):8883"));
}

/* ---------------------------------------------------------------------- */
/* Self-test                                                               */
/* ---------------------------------------------------------------------- */

/* TC-GCS-170, 176 */
void test_selftest_passes_when_sensors_and_links_are_healthy(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("selftest"));

    TEST_ASSERT_TRUE(has_line("sensors   PASS"));
    TEST_ASSERT_TRUE(has_line("comms     PASS"));
    TEST_ASSERT_TRUE(has_line("flash     n/a"));
    TEST_ASSERT_TRUE(has_line("overall   PASS"));
    TEST_ASSERT_TRUE(commands_selftest_result()->overall_pass);
}

/* TC-GCS-171 */
void test_selftest_fails_when_the_sensors_are_not_ready(void)
{
    g_spy_ready = false;
    (void) run("selftest");

    TEST_ASSERT_TRUE(has_line("sensors   FAIL"));
    TEST_ASSERT_TRUE(has_line("overall   FAIL"));
}

/* TC-GCS-172..174 */
void test_selftest_fails_when_any_link_is_down(void)
{
    g_spy_health.wifi_rssi_dbm = 0;
    (void) run("selftest");
    TEST_ASSERT_TRUE(has_line("comms     FAIL"));

    repl_clear();
    g_spy_health.wifi_rssi_dbm = -50;
    g_spy_health.cloud_connected = false;
    (void) run("selftest");
    TEST_ASSERT_TRUE(has_line("comms     FAIL"));

    repl_clear();
    g_spy_health.cloud_connected = true;
    g_spy_health.modbus_link_online = false;
    (void) run("selftest");
    TEST_ASSERT_TRUE(has_line("comms     FAIL"));
}

/* TC-GCS-177, 178 */
void test_selftest_result_reports_the_last_run_or_none(void)
{
    (void) run("selftest-result");
    TEST_ASSERT_TRUE(has_line("no self-test result since boot"));

    (void) run("selftest");
    repl_clear();
    (void) run("selftest-result");
    TEST_ASSERT_TRUE(has_line("last self-test: PASS"));
}

/* ---------------------------------------------------------------------- */
/* config workflow                                                         */
/* ---------------------------------------------------------------------- */

/* TC-GCS-160 */
void test_config_set_stages_a_valid_value_and_list_marks_it(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("config set polling-interval-ms 500"));

    TEST_ASSERT_TRUE(commands_cfg_dirty());
    TEST_ASSERT_EQUAL_UINT32(1U, g_spy_validate_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, g_spy_set_calls); /* staging changes nothing */
    (void) run("config list");
    TEST_ASSERT_TRUE(has_line("polling-interval-ms"));
    TEST_ASSERT_TRUE(has_line("(staged)"));
}

/* TC-GCS-161 */
void test_config_set_rejects_a_non_number_and_a_value_the_service_refuses(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("config set polling-interval-ms abc"));
    g_spy_validate_rc = CONFIG_SERVICE_ERR_INVALID;
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("config set polling-interval-ms 5"));
    TEST_ASSERT_FALSE(commands_cfg_dirty());
}

/* TC-GCS-162 */
void test_config_set_with_an_unknown_key(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_UNKNOWN_KEY, run("config set nonsense 42"));
}

void test_config_set_covers_every_value_kind(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("config set filter-alpha 250"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("config set mqtt-port 1883"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("config set temp-range-min -20.5"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("config set pressure-range-max 1100"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("config set mqtt-port 70000"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("config set temp-range-min 400"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("config set polling-interval-ms -5"));
}

/* TC-GCS-163 */
void test_config_commit_with_nothing_staged_says_so(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("config commit"));
    TEST_ASSERT_TRUE(has_line("Nothing staged"));
    TEST_ASSERT_FALSE(commands_confirm_active());
}

/* TC-GCS-164 */
void test_config_commit_yes_applies_every_staged_value_and_asks_for_a_flush(void)
{
    (void) run("config set polling-interval-ms 500");
    (void) run("config set filter-alpha 250");
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("config commit"));
    TEST_ASSERT_TRUE(commands_confirm_active());
    TEST_ASSERT_TRUE(has_line("polling-interval-ms = 500"));

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, commands_confirm_answer(true));

    TEST_ASSERT_EQUAL_UINT32(2U, g_spy_set_calls);
    TEST_ASSERT_EQUAL(CONFIG_PARAM_POLL_INTERVAL, g_spy_set_ids[0]);
    TEST_ASSERT_EQUAL(CONFIG_PARAM_FILTER_ALPHA, g_spy_set_ids[1]);
    TEST_ASSERT_FALSE(commands_cfg_dirty());
    TEST_ASSERT_FALSE(commands_confirm_active());
    TEST_ASSERT_TRUE(has_line("[OK] configuration applied"));
    TEST_ASSERT_TRUE(commands_take_persist_due());
}

/* TC-GCS-165 */
void test_config_commit_no_discards_the_staged_values(void)
{
    (void) run("config set polling-interval-ms 500");
    (void) run("config commit");

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, commands_confirm_answer(false));

    TEST_ASSERT_EQUAL_UINT32(0U, g_spy_set_calls);
    TEST_ASSERT_FALSE(commands_cfg_dirty());
    TEST_ASSERT_TRUE(has_line("Discarded"));
}

/* TC-GCS-166, 149 */
void test_the_confirm_prompt_times_out_after_30_ticks_and_discards(void)
{
    (void) run("config set polling-interval-ms 500");
    (void) run("config commit");

    for (uint32_t i = 0U; i < (CONFIRM_TIMEOUT_TICKS - 1U); i++)
    {
        TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, commands_tick());
    }
    TEST_ASSERT_TRUE(commands_confirm_active());
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_TIMEOUT, commands_tick());

    TEST_ASSERT_FALSE(commands_confirm_active());
    TEST_ASSERT_FALSE(commands_cfg_dirty());
    TEST_ASSERT_TRUE(has_line("[ERR] timed out"));
    TEST_ASSERT_EQUAL_UINT32(0U, g_spy_set_calls);
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, commands_tick()); /* nothing left to count */
}

/* TC-GCS-167 */
void test_config_commit_with_a_refused_value_keeps_the_rest_staged(void)
{
    (void) run("config set polling-interval-ms 500");
    (void) run("config set filter-alpha 250");
    (void) run("config commit");
    g_spy_set_rc = CONFIG_SERVICE_ERR_INVALID;

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_APPLY_FAILED, commands_confirm_answer(true));

    TEST_ASSERT_TRUE(commands_cfg_dirty());
    TEST_ASSERT_TRUE(has_line("[ERR]"));
    TEST_ASSERT_FALSE(commands_take_persist_due());
}

/* TC-GCS-168 */
void test_config_discard_clears_the_staging(void)
{
    (void) run("config set polling-interval-ms 500");
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("config discard"));

    TEST_ASSERT_FALSE(commands_cfg_dirty());
    TEST_ASSERT_TRUE(has_line("Discarded"));
}

void test_config_list_without_a_config_service_reports_not_init(void)
{
    g_spy_cfg_read_null = true;
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_NOT_INIT, run("config list"));
}

/* ---------------------------------------------------------------------- */
/* prov workflow                                                           */
/* ---------------------------------------------------------------------- */

/* TC-GCS-136, 138 */
void test_prov_set_stages_the_endpoint_and_address_that_have_a_back_end(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("prov set mqtt-endpoint broker.example.com"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("prov set modbus-addr 50"));

    TEST_ASSERT_TRUE(commands_prov_dirty());
}

/* TC-GCS-137, 139, 140 */
void test_prov_set_validates_endpoint_and_address(void)
{
    /* The 120-character input line limits an endpoint to about 97 characters, so the 127-character
     * rule in the code is not reachable from the keyboard. */
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("prov set mqtt-endpoint bad_host!"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("prov set mqtt-endpoint .leading"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("prov set modbus-addr 0"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("prov set modbus-addr 248"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("prov set modbus-addr 300"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("prov set modbus-addr abc"));
    TEST_ASSERT_FALSE(commands_prov_dirty());
}

/* TC-GCS-130..135 : valid input is accepted by the rules, then reported as not available */
void test_prov_set_wifi_credentials_validate_then_report_not_available(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_APPLY_FAILED, run("prov set wifi-ssid MyNetwork"));
    TEST_ASSERT_TRUE(has_line("[ERR] not available: WiFi credential store"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_APPLY_FAILED, run("prov set wifi-pass ValidPa$$"));

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("prov set wifi-pass short"));
    char big[100];
    char cmd[140];

    memset(big, 'x', sizeof(big));
    big[64] = '\0';
    (void) snprintf(cmd, sizeof(cmd), "prov set wifi-ssid %s", big);
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run(cmd));
    (void) snprintf(cmd, sizeof(cmd), "prov set wifi-pass %s", big);
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run(cmd));
    TEST_ASSERT_FALSE(commands_prov_dirty());
}

/* TC-GCS-141..144 */
void test_prov_set_modbus_line_settings_validate_then_report_not_available(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_APPLY_FAILED, run("prov set modbus-baud 9600"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("prov set modbus-baud 7200"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_APPLY_FAILED, run("prov set modbus-parity E"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_APPLY_FAILED, run("prov set modbus-parity N"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_APPLY_FAILED, run("prov set modbus-parity O"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("prov set modbus-parity X"));
    TEST_ASSERT_TRUE(has_line("Modbus line settings"));
}

/* TC-GCS-145 */
void test_prov_set_with_an_unknown_key(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_UNKNOWN_KEY, run("prov set unknown-key foo"));
}

/* TC-GCS-146 */
void test_prov_commit_with_nothing_staged(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("prov commit"));
    TEST_ASSERT_TRUE(has_line("Nothing staged"));
}

/* TC-GCS-147 */
void test_prov_commit_yes_applies_endpoint_and_address(void)
{
    (void) run("prov set mqtt-endpoint broker.example.com");
    (void) run("prov set modbus-addr 50");
    (void) run("prov commit");
    TEST_ASSERT_TRUE(has_line("mqtt-endpoint = broker.example.com"));
    TEST_ASSERT_TRUE(has_line("modbus-addr = 50"));

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, commands_confirm_answer(true));

    TEST_ASSERT_EQUAL_UINT32(2U, g_spy_set_calls);
    TEST_ASSERT_EQUAL(CONFIG_PARAM_MQTT_BROKER, g_spy_set_ids[0]);
    TEST_ASSERT_EQUAL_STRING("broker.example.com", (char *) g_spy_set_values[0]);
    TEST_ASSERT_EQUAL(CONFIG_PARAM_MODBUS_SLAVE_ADDR, g_spy_set_ids[1]);
    TEST_ASSERT_EQUAL_UINT8(50U, g_spy_set_values[1][0]);
    TEST_ASSERT_FALSE(commands_prov_dirty());
    TEST_ASSERT_TRUE(has_line("[OK] provisioning applied"));
    TEST_ASSERT_TRUE(commands_take_persist_due());
}

/* TC-GCS-148 */
void test_prov_commit_no_discards(void)
{
    (void) run("prov set modbus-addr 50");
    (void) run("prov commit");

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, commands_confirm_answer(false));
    TEST_ASSERT_EQUAL_UINT32(0U, g_spy_set_calls);
    TEST_ASSERT_FALSE(commands_prov_dirty());
}

/* TC-GCS-150 */
void test_prov_commit_with_a_refused_value_keeps_the_staging(void)
{
    (void) run("prov set mqtt-endpoint broker.example.com");
    (void) run("prov commit");
    g_spy_set_rc = CONFIG_SERVICE_ERR_INVALID;

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_APPLY_FAILED, commands_confirm_answer(true));
    TEST_ASSERT_TRUE(commands_prov_dirty());
    TEST_ASSERT_TRUE(has_line("[ERR]"));

    (void) run("prov set modbus-addr 7");
    (void) run("prov commit");
    g_spy_set_rc = CONFIG_SERVICE_OK;
    g_spy_set_calls = 0U;
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, commands_confirm_answer(true));
}

/* TC-GCS-151 */
void test_prov_discard_clears_the_staging(void)
{
    (void) run("prov set modbus-addr 50");
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("prov discard"));

    TEST_ASSERT_FALSE(commands_prov_dirty());
    TEST_ASSERT_TRUE(has_line("Discarded"));
}

void test_answering_with_no_prompt_pending_does_nothing(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, commands_confirm_answer(true));
    TEST_ASSERT_EQUAL_UINT32(0U, g_spy_set_calls);
}

/* ---------------------------------------------------------------------- */
/* profiles                                                                */
/* ---------------------------------------------------------------------- */

/* TC-GCS-180 */
void test_profiles_list_prints_each_registered_profile(void)
{
    g_spy_profiles[0].slave_addr = 1U;
    g_spy_profiles[0].input_reg_start = 0U;
    g_spy_profiles[0].input_reg_count = 10U;
    g_spy_profiles[1].slave_addr = 5U;
    g_spy_profiles[1].input_reg_start = 100U;
    g_spy_profiles[1].input_reg_count = 4U;
    g_spy_profile_count = 2U;

    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("profiles list"));
    TEST_ASSERT_TRUE(has_line("slave   1   input regs 0..9"));
    TEST_ASSERT_TRUE(has_line("slave   5   input regs 100..103"));
}

/* TC-GCS-181 */
void test_profiles_list_when_empty(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("profiles list"));
    TEST_ASSERT_TRUE(has_line("No profiles registered"));
}

/* TC-GCS-182, 183 */
void test_profiles_add_validates_then_reports_not_available(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_APPLY_FAILED, run("profiles add addr=5 sensors=temp,hum"));
    TEST_ASSERT_TRUE(has_line("DeviceProfileRegistry is read-only"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("profiles add sensors=temp"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("profiles add addr=0"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("profiles add addr=abc"));
}

/* TC-GCS-184, 185 */
void test_profiles_remove_validates_then_reports_not_available(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_APPLY_FAILED, run("profiles remove 5"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("profiles remove 0"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("profiles remove 300"));
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_VALIDATION, run("profiles remove x"));
}

/* ---------------------------------------------------------------------- */
/* reboot                                                                  */
/* ---------------------------------------------------------------------- */

void test_reboot_only_raises_a_request_flag(void)
{
    TEST_ASSERT_EQUAL(CONSOLE_SERVICE_ERR_OK, run("reboot"));

    TEST_ASSERT_EQUAL_UINT32(0U, g_spy_reset_calls);
    TEST_ASSERT_TRUE(commands_take_reboot_request());
    TEST_ASSERT_FALSE(commands_take_reboot_request());
}

/* TC-GCS-190: every command leaves the scrollback ending in output, never half-written */
void test_every_table_command_runs_without_crashing_on_a_healthy_system(void)
{
    static const char *const lines[] = {"help", "version", "serial", "sensors", "read t", "status",
                                        "alarms", "thresholds", "uptime", "selftest", "selftest-result",
                                        "config list", "profiles list", "wifi status", "mqtt status",
                                        "modbus status", "net", "config discard", "prov discard"};

    for (uint32_t i = 0U; i < (sizeof(lines) / sizeof(lines[0])); i++)
    {
        TEST_ASSERT_EQUAL_MESSAGE(CONSOLE_SERVICE_ERR_OK, run(lines[i]), lines[i]);
        TEST_ASSERT_GREATER_THAN_UINT16(0U, repl_count());
    }
}

/* ---------------------------------------------------------------------- */
/* Service loop                                                            */
/* ---------------------------------------------------------------------- */

static void type(const char *s)
{
    console_service_rx_for_test((const uint8_t *) s, (uint32_t) strlen(s));
    (void) console_service_cycle_for_test(NOTIFY_RX);
}

static void start(void)
{
    (void) console_service_init(&s_deps);
    (void) console_service_cycle_for_test(NOTIFY_DATA_TICK);
}

static bool screen_has(uint8_t row, const char *needle)
{
    char buf[512];
    size_t n = 0U;
    const cell_grid_t *g = console_service_grid_for_test();

    for (uint32_t c = 0U; c < GRID_COLS; c++)
    {
        const char *s = glyph_utf8(g->cells[row][c].glyph);

        while ((s != NULL) && (*s != '\0') && ((n + 1U) < sizeof(buf)))
        {
            buf[n++] = *s++;
        }
    }
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

void test_keystrokes_drive_the_menu_and_the_console(void)
{
    start();
    TEST_ASSERT_TRUE(screen_has(20U, "↑↓ select"));

    type("6"); /* jump to Console */
    TEST_ASSERT_TRUE(screen_has(9U, "CONSOLE"));
    type("\r"); /* into the panel */
    TEST_ASSERT_TRUE(screen_has(20U, "type a command"));
    type("uptime\r");

    TEST_ASSERT_TRUE(screen_has(17U, "uptime 3d 14:22:05"));
    TEST_ASSERT_TRUE(screen_has(17U, "envmon> uptime") || screen_has(16U, "envmon> uptime"));
}

void test_arrow_keys_move_the_selection_and_change_the_channel(void)
{
    start();
    type("\x1b[B"); /* down */
    TEST_ASSERT_TRUE(screen_has(9U, "SYSTEM STATUS"));
    type("1\t");
    type("\x1b[B");
    TEST_ASSERT_TRUE(screen_has(9U, "HUMIDITY"));
}

void test_a_lone_escape_leaves_the_panel_once_its_window_has_passed(void)
{
    start();
    type("\r");
    TEST_ASSERT_TRUE(screen_has(20U, "↑↓ change channel"));

    g_spy_millis = 1000U;
    type("\x1b");
    TEST_ASSERT_TRUE(screen_has(20U, "↑↓ change channel")); /* still waiting */
    g_spy_millis = 1020U;
    (void) console_service_cycle_for_test(0U);
    TEST_ASSERT_TRUE(screen_has(20U, "↑↓ select"));
}

void test_set_through_the_keyboard_persists_after_the_frame_is_drawn(void)
{
    start();
    type("6\r");
    type("set t.hi 24\r");

    TEST_ASSERT_EQUAL_UINT32(1U, g_spy_set_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, g_spy_persist_calls); /* flushed once, after the frame */
    TEST_ASSERT_TRUE(screen_has(17U, "applied") || screen_has(16U, "applied"));
}

void test_page_up_reveals_scrolled_out_output_and_a_new_command_snaps_back(void)
{
    start();
    type("6\r");
    type("help\r");
    TEST_ASSERT_FALSE(screen_has(10U, "Available commands"));

    for (uint32_t i = 0U; i < 8U; i++) /* help prints 30 lines: 8 pages of 4 back to its header */
    {
        type("\x1b[5~");
    }
    bool header_visible = false;

    for (uint8_t row = 10U; row <= 17U; row++)
    {
        header_visible = header_visible || screen_has(row, "Available commands");
    }
    TEST_ASSERT_TRUE(header_visible);
    TEST_ASSERT_TRUE(screen_has(18U, "more"));
    TEST_ASSERT_TRUE(screen_has(20U, "PgUp/PgDn scroll"));

    for (uint32_t i = 0U; i < 8U; i++)
    {
        type("\x1b[6~");
    }
    TEST_ASSERT_FALSE(screen_has(18U, "more"));

    type("\x1b[5~");
    type("uptime\r");
    TEST_ASSERT_FALSE(screen_has(18U, "more")); /* new output: back to the live view */
}

void test_scrolling_stops_at_the_oldest_line(void)
{
    start();
    type("6\r");
    for (uint32_t i = 0U; i < 40U; i++)
    {
        type("\x1b[5~");
    }
    type("\x1b[6~");
    TEST_ASSERT_TRUE(screen_has(9U, "CONSOLE")); /* still sane after over-scrolling */
}

void test_the_confirm_prompt_is_answered_from_the_keyboard(void)
{
    start();
    type("6\r");
    type("prov set modbus-addr 50\r");
    type("prov commit\r");
    TEST_ASSERT_TRUE(screen_has(18U, "Apply? [y/N]:"));

    type("y");
    TEST_ASSERT_EQUAL_UINT32(1U, g_spy_set_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, g_spy_persist_calls);
    TEST_ASSERT_TRUE(screen_has(18U, "envmon>"));
}

void test_any_other_key_declines_the_prompt(void)
{
    start();
    type("6\r");
    type("prov set modbus-addr 50\r");
    type("prov commit\r");
    type("n");

    TEST_ASSERT_EQUAL_UINT32(0U, g_spy_set_calls);
    TEST_ASSERT_FALSE(commands_prov_dirty());
}

void test_the_prompt_times_out_on_the_data_tick(void)
{
    start();
    type("6\r");
    type("prov set modbus-addr 50\r");
    type("prov commit\r");
    for (uint32_t i = 0U; i < CONFIRM_TIMEOUT_TICKS; i++)
    {
        (void) console_service_cycle_for_test(NOTIFY_DATA_TICK);
    }

    TEST_ASSERT_FALSE(commands_confirm_active());
    TEST_ASSERT_TRUE(screen_has(18U, "envmon>"));
    TEST_ASSERT_EQUAL_UINT32(0U, g_spy_set_calls);
}

/* TC-GCS-119 */
void test_an_overlong_line_is_cleared_with_an_error(void)
{
    start();
    type("6\r");
    for (uint32_t i = 0U; i < 125U; i++)
    {
        type("a");
    }
    TEST_ASSERT_TRUE(has_line("[ERR] line overflow"));
}

/* TC-GCS-194 */
void test_a_receive_ring_overflow_is_shown_in_the_footer_once(void)
{
    start();
    uint8_t flood[300];

    memset(flood, 'x', sizeof(flood));
    console_service_rx_for_test(flood, sizeof(flood));
    (void) console_service_cycle_for_test(NOTIFY_RX);
    TEST_ASSERT_TRUE(screen_has(20U, "[WARN] RX overflow"));

    (void) console_service_cycle_for_test(0U);
    TEST_ASSERT_FALSE(screen_has(20U, "[WARN] RX overflow"));
}

void test_a_small_terminal_report_shows_the_resize_notice_and_recovers(void)
{
    start();
    type("\x1b[8;24;80t");
    TEST_ASSERT_TRUE(screen_has(10U, "Resize the terminal"));

    type("\x1b[8;40;120t");
    TEST_ASSERT_FALSE(screen_has(10U, "Resize the terminal"));
    TEST_ASSERT_TRUE(screen_has(9U, "MAIN MENU"));
}

void test_ctrl_l_wipes_the_terminal_and_sends_the_whole_frame_again(void)
{
    start();
    const size_t before = g_spy_tx_len;

    type("\x0c");
    g_spy_tx[g_spy_tx_len] = 0U;

    TEST_ASSERT_GREATER_THAN_size_t(before + 2000U, g_spy_tx_len); /* a full frame, not a diff */
    TEST_ASSERT_NOT_NULL(strstr((char *) &g_spy_tx[before], "\x1b[2J\x1b[H"));
    TEST_ASSERT_NOT_NULL(strstr((char *) &g_spy_tx[before], "ENVIRONMENTAL GATEWAY"));
    TEST_ASSERT_TRUE(screen_has(9U, "MAIN MENU"));
}

void test_the_first_size_report_repaints_once_and_a_repeat_does_not(void)
{
    start();
    const size_t t0 = g_spy_tx_len;

    type("\x1b[8;40;120t");
    const size_t first = g_spy_tx_len - t0;

    type("\x1b[8;40;120t");
    const size_t repeat = g_spy_tx_len - t0 - first;

    TEST_ASSERT_GREATER_THAN_size_t(2000U, first);
    TEST_ASSERT_LESS_THAN_size_t(100U, repeat);

    type("\x1b[8;50;140t"); /* the window changed size: repaint again */
    TEST_ASSERT_GREATER_THAN_size_t(2000U, g_spy_tx_len - t0 - first - repeat);
}

static uint32_t s_inject_calls;

static void inject_arrow_down_once(void)
{
    if (s_inject_calls == 0U)
    {
        s_inject_calls++;
        console_service_rx_for_test((const uint8_t *) "\x1b[B", 3U);
    }
}

static bool sent_since(size_t mark, const char *needle)
{
    g_spy_tx[g_spy_tx_len] = 0U;
    return strstr((char *) &g_spy_tx[mark], needle) != NULL;
}

void test_menu_goes_out_first_and_the_panel_is_skipped_when_another_key_is_already_waiting(void)
{
    start();
    const size_t mark = g_spy_tx_len;

    s_inject_calls = 0U;
    g_spy_send_hook = inject_arrow_down_once; /* a second arrow arrives while the menu is being sent */
    type("\x1b[B");
    g_spy_send_hook = NULL;

    TEST_ASSERT_FALSE(sent_since(mark, "SYSTEM STATUS")); /* the intermediate panel was never sent */
    TEST_ASSERT_TRUE(sent_since(mark, "System status")); /* but the menu highlight moved */

    (void) console_service_cycle_for_test(0U); /* the waiting key is handled, nothing is waiting now */
    TEST_ASSERT_TRUE(sent_since(mark, "ACTIVE ALARMS"));
    TEST_ASSERT_FALSE(sent_since(mark, "SYSTEM STATUS"));
}

void test_a_single_key_sends_menu_and_panel_in_the_same_cycle(void)
{
    start();
    const size_t mark = g_spy_tx_len;

    type("\x1b[B");

    TEST_ASSERT_TRUE(sent_since(mark, "SYSTEM STATUS"));
}

void test_an_unchanged_screen_costs_no_bytes(void)
{
    start();
    const size_t before = g_spy_tx_len;

    (void) console_service_cycle_for_test(0U);
    TEST_ASSERT_EQUAL_size_t(before, g_spy_tx_len);
}

void test_the_data_tick_repaints_only_what_changed(void)
{
    start();
    const size_t full_frame = g_spy_tx_len;
    const size_t before = g_spy_tx_len;

    spy_set_reading(SENSOR_ID_TEMPERATURE, 2310, true);
    (void) console_service_cycle_for_test(NOTIFY_DATA_TICK);

    TEST_ASSERT_GREATER_THAN_size_t(0U, g_spy_tx_len - before);
    TEST_ASSERT_LESS_THAN_size_t(full_frame / 4U, g_spy_tx_len - before);
}

void test_reboot_tears_the_screen_down_then_resets(void)
{
    start();
    type("6\r");
    type("reboot\r");

    g_spy_tx[g_spy_tx_len] = 0U;
    TEST_ASSERT_EQUAL_UINT32(1U, g_spy_reset_calls);
    TEST_ASSERT_NOT_NULL(strstr((char *) g_spy_tx, "\x1b[?25h\x1b[?1049l\x1b[0m"));
}

void test_firmware_panel_reboots_only_after_the_user_confirms(void)
{
    start();
    type("7\r");  /* firmware panel, cursor on "Check for update" */
    type("\x1b[B"); /* cursor on "Reboot gateway" */
    const size_t mark = g_spy_tx_len;

    type("\r");
    TEST_ASSERT_EQUAL_UINT32(0U, g_spy_reset_calls);
    TEST_ASSERT_TRUE(sent_since(mark, "Reboot the gateway now? [y/N]"));

    type("y");
    TEST_ASSERT_EQUAL_UINT32(1U, g_spy_reset_calls);
    g_spy_tx[g_spy_tx_len] = 0U;
    TEST_ASSERT_NOT_NULL(strstr((char *) g_spy_tx, "\x1b[?1049l"));
}

void test_firmware_panel_reboot_is_cancelled_by_any_other_key(void)
{
    start();
    type("7\r\x1b[B\r");
    type("n");

    TEST_ASSERT_EQUAL_UINT32(0U, g_spy_reset_calls);
    TEST_ASSERT_FALSE(commands_confirm_active());
    TEST_ASSERT_FALSE(commands_take_reboot_request());
}

void test_firmware_panel_reboot_question_times_out_without_rebooting(void)
{
    start();
    type("7\r\x1b[B\r");

    for (uint32_t i = 0U; i < 40U; i++)
    {
        (void) commands_tick();
    }
    TEST_ASSERT_FALSE(commands_confirm_active());
    TEST_ASSERT_EQUAL_UINT32(0U, g_spy_reset_calls);
    TEST_ASSERT_FALSE(commands_take_reboot_request());
}

void test_firmware_panel_update_check_reports_not_available(void)
{
    start();
    const size_t mark = g_spy_tx_len;

    type("7\r"); /* cursor on "Check for update" */
    type("\r");

    TEST_ASSERT_TRUE(sent_since(mark, "no update source"));
    TEST_ASSERT_NOT_NULL(commands_fw_check_result());
    TEST_ASSERT_EQUAL_UINT32(0U, g_spy_reset_calls);
}

/* TC-GCS-192 / 191 : a failing sink must never crash the loop */
void test_a_failing_uart_does_not_stop_the_loop(void)
{
    g_spy_send_rc = DEBUG_UART_ERR_TX_TIMEOUT;
    start();
    type("6\r");
    type("uptime\r");

    TEST_ASSERT_TRUE(has_line("uptime 3d 14:22:05"));
}

void test_failed_service_pulls_render_as_empty_not_garbage(void)
{
    g_spy_snapshot_rc = SENSOR_SERVICE_ERR_NOT_INIT;
    g_spy_health_rc = HEALTH_MONITOR_ERR_NOT_INIT;
    g_spy_alarm_rc = ALARM_SERVICE_ERR_NOT_INIT;
    start();

    TEST_ASSERT_TRUE(screen_has(18U, "▲ stale"));
    TEST_ASSERT_TRUE(screen_has(5U, "0 alarms"));
    TEST_ASSERT_TRUE(screen_has(6U, "0 / 4 sensors online"));
}
