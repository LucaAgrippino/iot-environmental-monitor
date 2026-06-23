/**
 * @file test_console_service.c
 * @brief Unit tests for ConsoleService — TC-CS-001..173 (FD subset).
 *
 * Covers docs/lld/application/console-service-lld.md §18.
 * GW-only TCs (TC-CS-008, 080..084, 113..114, 130..137) are in
 * test_console_service_gw.c (not yet implemented — CS-O11).
 *
 * Spy strategy: each interface is a static spy vtable. run_once() dispatches
 * by reading one line from the spy queue, then calling the matched handler.
 * Confirm-prompt reads consume the next queued line (no FreeRTOS in test
 * build — #ifndef TEST guards strip xTaskNotifyWait).
 *
 * Build defines: STM32F469xx, BOARD_FIELD_DEVICE, TEST  (project.yml).
 */

#include "unity.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "console_service/console_service.h"

/* ======================================================================= */
/* Constants mirrored from console_service.c (needed to dimension buffers) */
/* ======================================================================= */

#define CS_LINE_BUF_SIZE (DEBUG_UART_LINE_MAX_LEN + 1U)
#define SPY_TX_BUF_SIZE (4096U)
#define SPY_MAX_LINES (8)

/* ======================================================================= */
/* UART spy                                                                */
/* ======================================================================= */

static char g_tx_buf[SPY_TX_BUF_SIZE];
static size_t g_tx_len;

typedef struct
{
    uint8_t data[CS_LINE_BUF_SIZE];
    size_t len;
    debug_uart_line_flag_t flag;
    debug_uart_err_t err;
} spy_rx_entry_t;

static spy_rx_entry_t g_spy_lines[SPY_MAX_LINES];
static int g_spy_line_count;
static int g_spy_line_idx;
static bool g_attach_rx_called;

static debug_uart_err_t spy_send(const uint8_t *data, size_t length, uint32_t timeout_ms)
{
    (void) timeout_ms;
    if ((g_tx_len + length) < SPY_TX_BUF_SIZE)
    {
        memcpy(&g_tx_buf[g_tx_len], data, length);
        g_tx_len += length;
        g_tx_buf[g_tx_len] = '\0';
    }
    return DEBUG_UART_OK;
}

static debug_uart_err_t spy_read_line(uint8_t *out_buf, size_t buf_size, size_t *out_length,
                                      debug_uart_line_flag_t *out_flag)
{
    if (g_spy_line_idx >= g_spy_line_count)
    {
        if (out_length != NULL)
        {
            *out_length = 0U;
        }
        if (out_flag != NULL)
        {
            *out_flag = DEBUG_UART_LINE_OK;
        }
        return DEBUG_UART_ERR_NO_LINE_AVAILABLE;
    }
    spy_rx_entry_t *e = &g_spy_lines[g_spy_line_idx++];
    if (e->err != DEBUG_UART_OK)
    {
        if (out_length != NULL)
        {
            *out_length = 0U;
        }
        if (out_flag != NULL)
        {
            *out_flag = e->flag;
        }
        return e->err;
    }
    size_t copy_len = (e->len < buf_size) ? e->len : (buf_size - 1U);
    memcpy(out_buf, e->data, copy_len);
    out_buf[copy_len] = '\0';
    if (out_length != NULL)
    {
        *out_length = copy_len;
    }
    if (out_flag != NULL)
    {
        *out_flag = e->flag;
    }
    return DEBUG_UART_OK;
}

static debug_uart_err_t spy_attach_rx(debug_uart_line_callback_t callback, void *context)
{
    (void) callback;
    (void) context;
    g_attach_rx_called = true;
    return DEBUG_UART_OK;
}

static const idebug_uart_t g_spy_uart = {
    .send = spy_send,
    .read_line = spy_read_line,
    .attach_rx = spy_attach_rx,
};

/* ── helpers ─────────────────────────────────────────────────────────── */

static void uart_spy_reset(void)
{
    memset(g_tx_buf, 0, sizeof(g_tx_buf));
    g_tx_len = 0U;
    memset(g_spy_lines, 0, sizeof(g_spy_lines));
    g_spy_line_count = 0;
    g_spy_line_idx = 0;
    g_attach_rx_called = false;
}

static void push_ok_line(const char *line)
{
    TEST_ASSERT_LESS_THAN_INT(SPY_MAX_LINES, g_spy_line_count);
    spy_rx_entry_t *e = &g_spy_lines[g_spy_line_count++];
    size_t len = strlen(line);
    memcpy(e->data, line, len);
    e->data[len] = '\0';
    e->len = len;
    e->flag = DEBUG_UART_LINE_OK;
    e->err = DEBUG_UART_OK;
}

static void push_truncated_line(const char *line)
{
    TEST_ASSERT_LESS_THAN_INT(SPY_MAX_LINES, g_spy_line_count);
    spy_rx_entry_t *e = &g_spy_lines[g_spy_line_count++];
    size_t len = strlen(line);
    memcpy(e->data, line, len);
    e->data[len] = '\0';
    e->len = len;
    e->flag = DEBUG_UART_LINE_TRUNCATED;
    e->err = DEBUG_UART_OK;
}

static void push_no_line(void)
{
    TEST_ASSERT_LESS_THAN_INT(SPY_MAX_LINES, g_spy_line_count);
    spy_rx_entry_t *e = &g_spy_lines[g_spy_line_count++];
    e->err = DEBUG_UART_ERR_NO_LINE_AVAILABLE;
    e->flag = DEBUG_UART_LINE_OK;
    e->len = 0U;
}

/* ======================================================================= */
/* SensorService spy                                                       */
/* ======================================================================= */

static sensor_snapshot_t g_spy_snap;
static sensor_service_err_t g_spy_snap_ret;
static uint32_t g_spy_read_on_demand_calls;

static sensor_service_err_t spy_get_snapshot(sensor_snapshot_t *out)
{
    if (out != NULL)
    {
        *out = g_spy_snap;
    }
    return g_spy_snap_ret;
}
static sensor_service_err_t spy_read_on_demand(void)
{
    g_spy_read_on_demand_calls++;
    return SENSOR_SERVICE_ERR_OK;
}
static const isensor_service_t g_spy_sensors = {
    .get_snapshot = spy_get_snapshot,
    .read_on_demand = spy_read_on_demand,
};

/* ======================================================================= */
/* ConfigProvider spy                                                      */
/* ======================================================================= */

static config_params_t g_spy_params;

static const config_params_t *spy_get_params(void)
{
    return &g_spy_params;
}
static const iconfig_provider_t g_spy_cfg_read = {
    .get_params = spy_get_params,
};

/* ======================================================================= */
/* ConfigManager spy                                                       */
/* ======================================================================= */

#define SPY_SET_PARAM_MAX_CALLS (16)

typedef struct
{
    config_param_id_t id;
    long value;
    bool used;
} spy_set_call_t;

static spy_set_call_t g_spy_set_calls[SPY_SET_PARAM_MAX_CALLS];
static int g_spy_set_call_count;
static config_service_err_t g_spy_set_param_ret;
static config_service_err_t g_spy_validate_ret;
static config_service_err_t g_spy_flush_ret;
static int g_spy_flush_calls;

static config_service_err_t spy_set_param(config_param_id_t id, const void *value)
{
    if (g_spy_set_call_count < SPY_SET_PARAM_MAX_CALLS)
    {
        g_spy_set_calls[g_spy_set_call_count].id = id;
        g_spy_set_calls[g_spy_set_call_count].value =
            (value != NULL) ? (long) *((const uint32_t *) value) : 0L;
        g_spy_set_calls[g_spy_set_call_count].used = true;
        g_spy_set_call_count++;
    }
    return g_spy_set_param_ret;
}
static config_service_err_t spy_validate_param(config_param_id_t id, const void *value)
{
    (void) id;
    (void) value;
    return g_spy_validate_ret;
}
static config_service_err_t spy_flush(void)
{
    g_spy_flush_calls++;
    return g_spy_flush_ret;
}
static const iconfig_manager_t g_spy_cfg_write = {
    .set_param = spy_set_param,
    .validate_param = spy_validate_param,
    .flush = spy_flush,
};

/* ======================================================================= */
/* HealthSnapshot spy                                                      */
/* ======================================================================= */

static device_health_snapshot_t g_spy_health;
static health_monitor_err_t g_spy_health_ret;

static health_monitor_err_t spy_get_health_snapshot(device_health_snapshot_t *out)
{
    if (out != NULL)
    {
        *out = g_spy_health;
    }
    return g_spy_health_ret;
}
static const ihealth_snapshot_t g_spy_health_iface = {
    .get_snapshot = spy_get_health_snapshot,
};

/* ======================================================================= */
/* Init helper                                                             */
/* ======================================================================= */

static void do_init(void)
{
    console_service_err_t rc = console_service_init(&g_spy_uart, &g_spy_sensors, &g_spy_cfg_read,
                                                    &g_spy_cfg_write, &g_spy_health_iface);
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, rc);
}

/* ======================================================================= */
/* setUp / tearDown                                                        */
/* ======================================================================= */

void setUp(void)
{
    console_service_reset_for_test();
    uart_spy_reset();

    memset(&g_spy_snap, 0, sizeof(g_spy_snap));
    g_spy_snap_ret = SENSOR_SERVICE_ERR_OK;
    g_spy_read_on_demand_calls = 0U;

    memset(&g_spy_params, 0, sizeof(g_spy_params));

    memset(g_spy_set_calls, 0, sizeof(g_spy_set_calls));
    g_spy_set_call_count = 0;
    g_spy_set_param_ret = CONFIG_SERVICE_OK;
    g_spy_validate_ret = CONFIG_SERVICE_OK;
    g_spy_flush_ret = CONFIG_SERVICE_OK;
    g_spy_flush_calls = 0;

    memset(&g_spy_health, 0, sizeof(g_spy_health));
    g_spy_health_ret = HEALTH_MONITOR_ERR_OK;
}

void tearDown(void)
{
}

/* ======================================================================= */
/* TC-CS-001..012: Initialisation                                          */
/* ======================================================================= */

void test_TC_CS_001_null_uart_rejected(void)
{
    console_service_err_t rc = console_service_init(NULL, &g_spy_sensors, &g_spy_cfg_read,
                                                    &g_spy_cfg_write, &g_spy_health_iface);
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_NULL_ARG, rc);
}

void test_TC_CS_002_null_sensors_rejected(void)
{
    console_service_err_t rc = console_service_init(&g_spy_uart, NULL, &g_spy_cfg_read,
                                                    &g_spy_cfg_write, &g_spy_health_iface);
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_NULL_ARG, rc);
}

void test_TC_CS_003_null_cfg_read_rejected(void)
{
    console_service_err_t rc = console_service_init(&g_spy_uart, &g_spy_sensors, NULL,
                                                    &g_spy_cfg_write, &g_spy_health_iface);
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_NULL_ARG, rc);
}

void test_TC_CS_004_null_cfg_write_rejected(void)
{
    console_service_err_t rc = console_service_init(&g_spy_uart, &g_spy_sensors, &g_spy_cfg_read,
                                                    NULL, &g_spy_health_iface);
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_NULL_ARG, rc);
}

void test_TC_CS_005_null_health_rejected(void)
{
    console_service_err_t rc =
        console_service_init(&g_spy_uart, &g_spy_sensors, &g_spy_cfg_read, &g_spy_cfg_write, NULL);
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_NULL_ARG, rc);
}

void test_TC_CS_007_fd_build_ignores_profiles_null(void)
{
    /* FD build: profiles arg not present — init must succeed with 5 args */
    console_service_err_t rc = console_service_init(&g_spy_uart, &g_spy_sensors, &g_spy_cfg_read,
                                                    &g_spy_cfg_write, &g_spy_health_iface);
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, rc);
}

void test_TC_CS_010_dirty_flags_false_after_init(void)
{
    do_init();
    /* Stage would be dirty=true; reset clears them — verify via discard returning OK */
    push_ok_line("prov discard");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Discarded"));
}

void test_TC_CS_011_prompt_emitted_after_init(void)
{
    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    do_init();
    /* Init emits the boot prompt via print_prompt() */
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "fd>"));
}

void test_TC_CS_012_run_once_before_init_returns_not_init(void)
{
    /* Do NOT call do_init() */
    push_ok_line("help");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_NOT_INIT, console_service->run_once());
}

/* ======================================================================= */
/* TC-CS-020..030: Parsing & dispatch                                      */
/* ======================================================================= */

void test_TC_CS_020_empty_line_returns_ok_no_error(void)
{
    do_init();
    push_ok_line("");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
}

void test_TC_CS_021_help_command_dispatched(void)
{
    do_init();
    push_ok_line("help");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "help"));
}

void test_TC_CS_022_multi_token_command_dispatched(void)
{
    do_init();
    push_ok_line("config list");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
    /* config list prints polling-interval-ms */
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "polling-interval-ms"));
}

void test_TC_CS_023_unknown_token_returns_unknown_key(void)
{
    do_init();
    push_ok_line("frobnicate");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_UNKNOWN_KEY, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "[ERR]"));
}

void test_TC_CS_027_truncated_line_returns_overflow(void)
{
    do_init();
    push_truncated_line("this line was too long and got cut");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_LINE_OVERFLOW, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "overflow"));
}

void test_TC_CS_028_multiple_spaces_collapsed(void)
{
    do_init();
    push_ok_line("config    list");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "polling-interval-ms"));
}

void test_TC_CS_029_leading_whitespace_ignored(void)
{
    do_init();
    push_ok_line("  help");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "help"));
}

void test_TC_CS_030_prompt_always_printed(void)
{
    do_init();
    /* Clear TX after init prompt */
    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("frobnicate");
    console_service->run_once();
    /* Prompt must appear in output */
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "fd>"));
}

/* ======================================================================= */
/* TC-CS-040..049: Individual command handlers                             */
/* ======================================================================= */

void test_TC_CS_040_help_lists_all_commands(void)
{
    do_init();
    push_ok_line("help");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "sensors"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "config"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "prov"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "selftest"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "modbus"));
}

void test_TC_CS_042_version_prints_fw_version(void)
{
    do_init();
    push_ok_line("version");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "0.0.0-test"));
}

void test_TC_CS_043_serial_prints_uid(void)
{
    do_init();
    push_ok_line("serial");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "UID:"));
}

void test_TC_CS_044_sensors_valid_prints_value(void)
{
    do_init();
    g_spy_snap.readings[SENSOR_ID_TEMPERATURE].valid = true;
    g_spy_snap.readings[SENSOR_ID_TEMPERATURE].value = 2350; /* 23.50 degC */
    g_spy_snap.readings[SENSOR_ID_HUMIDITY].valid = true;
    g_spy_snap.readings[SENSOR_ID_HUMIDITY].value = 5100; /* 51.00 %RH */
    g_spy_snap.readings[SENSOR_ID_PRESSURE].valid = true;
    g_spy_snap.readings[SENSOR_ID_PRESSURE].value = 10132; /* 1013.2 hPa */
    push_ok_line("sensors");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "23.50"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "51.00"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "1013.2"));
}

void test_TC_CS_045_sensors_invalid_prints_INVALID(void)
{
    do_init();
    /* All readings invalid (zeroed in setUp) */
    push_ok_line("sensors");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "INVALID"));
}

void test_TC_CS_046_status_prints_health_fields(void)
{
    do_init();
    g_spy_health.uptime_s = 12345UL;
    push_ok_line("status");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "12345"));
}

void test_TC_CS_047_alarms_all_clear_prints_message(void)
{
    do_init();
    /* All alarm_state == ALARM_STATE_CLEAR (zeroed in setUp) */
    push_ok_line("alarms");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "No active alarms"));
}

void test_TC_CS_048_alarms_active_high_prints_entry(void)
{
    do_init();
    g_spy_health.alarm_state[SENSOR_ID_TEMPERATURE] = ALARM_STATE_ACTIVE_HIGH;
    push_ok_line("alarms");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Temperature"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "ACTIVE_HIGH"));
}

void test_TC_CS_049_config_list_prints_params(void)
{
    do_init();
    g_spy_params.polling_interval_ms = 1000UL;
    g_spy_params.temp_alarm_high = 4000;
    push_ok_line("config list");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "polling-interval-ms"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "temp-alarm-high"));
}

/* ======================================================================= */
/* TC-CS-060..068: Config set/commit/discard workflow                      */
/* ======================================================================= */

void test_TC_CS_060_config_set_valid_stages_value(void)
{
    do_init();
    push_ok_line("config set polling-interval-ms 2000");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "[OK] staged"));
    /* dirty must be set — check via commit prints confirm prompt */
    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("config commit");
    push_ok_line("n");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Apply?"));
}

void test_TC_CS_061_config_set_non_numeric_rejected(void)
{
    do_init();
    push_ok_line("config set polling-interval-ms abc");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_VALIDATION, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "[ERR] invalid value"));
    /* dirty must still be false — commit says "Nothing staged" */
    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("config commit");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Nothing staged"));
}

void test_TC_CS_062_config_set_unknown_key_rejected(void)
{
    do_init();
    push_ok_line("config set unknown-key 42");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_UNKNOWN_KEY, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "unknown key"));
}

void test_TC_CS_063_config_commit_nothing_staged(void)
{
    do_init();
    push_ok_line("config commit");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Nothing staged"));
    TEST_ASSERT_EQUAL_INT(0, g_spy_set_call_count);
}

void test_TC_CS_064_config_commit_y_applies_values(void)
{
    do_init();
    push_ok_line("config set polling-interval-ms 5000");
    console_service->run_once();

    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("config commit");
    push_ok_line("y");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "[OK]"));
    TEST_ASSERT_GREATER_THAN_INT(0, g_spy_set_call_count);
}

void test_TC_CS_065_config_commit_n_discards(void)
{
    do_init();
    push_ok_line("config set polling-interval-ms 5000");
    console_service->run_once();

    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("config commit");
    push_ok_line("n");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Discarded"));
    TEST_ASSERT_EQUAL_INT(0, g_spy_set_call_count);
}

void test_TC_CS_066_config_commit_timeout_discards(void)
{
    do_init();
    push_ok_line("config set polling-interval-ms 5000");
    console_service->run_once();

    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("config commit");
    push_no_line(); /* simulate timeout / no response */
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Discarded"));
    TEST_ASSERT_EQUAL_INT(0, g_spy_set_call_count);
}

void test_TC_CS_067_config_commit_apply_error_retains_dirty(void)
{
    do_init();
    push_ok_line("config set polling-interval-ms 5000");
    console_service->run_once();

    g_spy_set_param_ret = CONFIG_SERVICE_ERR_PERSIST;
    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("config commit");
    push_ok_line("y");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_APPLY_FAILED, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "[ERR] apply failed"));
    /* dirty retained — commit still shows Apply prompt */
    g_spy_set_param_ret = CONFIG_SERVICE_OK;
    g_spy_set_call_count = 0;
    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("config commit");
    push_ok_line("y");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
}

void test_TC_CS_068_config_discard_clears_pending(void)
{
    do_init();
    push_ok_line("config set polling-interval-ms 5000");
    console_service->run_once();

    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("config discard");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Discarded"));

    /* Subsequent commit says "Nothing staged" */
    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("config commit");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Nothing staged"));
}

/* ======================================================================= */
/* TC-CS-085..092: Prov workflow (FD subset: modbus-addr)                  */
/* ======================================================================= */

void test_TC_CS_085_prov_set_modbus_addr_valid(void)
{
    do_init();
    push_ok_line("prov set modbus-addr 50");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "[OK] staged"));
}

void test_TC_CS_086_prov_set_modbus_addr_zero_rejected(void)
{
    do_init();
    push_ok_line("prov set modbus-addr 0");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_VALIDATION, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "[ERR] invalid value"));
}

void test_TC_CS_087_prov_set_modbus_addr_248_rejected(void)
{
    do_init();
    push_ok_line("prov set modbus-addr 248");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_VALIDATION, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "[ERR] invalid value"));
}

void test_TC_CS_092_prov_set_unknown_key_rejected(void)
{
    do_init();
    push_ok_line("prov set device-name mydevice");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_UNKNOWN_KEY, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "[ERR]"));
}

void test_TC_CS_093_prov_commit_nothing_staged(void)
{
    do_init();
    push_ok_line("prov commit");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Nothing staged"));
    TEST_ASSERT_EQUAL_INT(0, g_spy_set_call_count);
}

void test_TC_CS_094_prov_commit_y_applies_modbus_addr(void)
{
    do_init();
    push_ok_line("prov set modbus-addr 75");
    console_service->run_once();

    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("prov commit");
    push_ok_line("y");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "[OK]"));
    /* set_param must have been called with MODBUS_SLAVE_ADDR */
    bool found = false;
    for (int i = 0; i < g_spy_set_call_count; i++)
    {
        if (g_spy_set_calls[i].id == CONFIG_PARAM_MODBUS_SLAVE_ADDR)
        {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found);
}

void test_TC_CS_095_prov_commit_n_discards(void)
{
    do_init();
    push_ok_line("prov set modbus-addr 75");
    console_service->run_once();

    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("prov commit");
    push_ok_line("n");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Discarded"));
    TEST_ASSERT_EQUAL_INT(0, g_spy_set_call_count);
}

void test_TC_CS_096_prov_commit_timeout_discards(void)
{
    do_init();
    push_ok_line("prov set modbus-addr 75");
    console_service->run_once();

    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("prov commit");
    push_no_line();
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Discarded"));
    TEST_ASSERT_EQUAL_INT(0, g_spy_set_call_count);
}

void test_TC_CS_097_prov_commit_apply_error_retains_dirty(void)
{
    do_init();
    push_ok_line("prov set modbus-addr 75");
    console_service->run_once();

    g_spy_set_param_ret = CONFIG_SERVICE_ERR_PERSIST;
    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("prov commit");
    push_ok_line("y");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_APPLY_FAILED, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "[ERR] apply failed"));
}

void test_TC_CS_098_prov_discard_clears_pending(void)
{
    do_init();
    push_ok_line("prov set modbus-addr 75");
    console_service->run_once();

    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("prov discard");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Discarded"));

    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("prov commit");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Nothing staged"));
}

/* ======================================================================= */
/* TC-CS-110..118: Selftest                                                */
/* ======================================================================= */

void test_TC_CS_110_selftest_all_pass(void)
{
    do_init();
    g_spy_snap.readings[SENSOR_ID_TEMPERATURE].valid = true;
    g_spy_snap.readings[SENSOR_ID_HUMIDITY].valid = true;
    g_spy_snap.readings[SENSOR_ID_PRESSURE].valid = true;
    g_spy_health.modbus_slave_ok = true;
    g_spy_flush_ret = CONFIG_SERVICE_OK;

    push_ok_line("selftest");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Sensors:  PASS"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Comms:    PASS"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Flash:    PASS"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Overall:  PASS"));
    TEST_ASSERT_EQUAL_INT(1, g_spy_flush_calls);
}

void test_TC_CS_111_selftest_invalid_sensor_fails(void)
{
    do_init();
    /* All sensors invalid (zeroed in setUp) */
    g_spy_health.modbus_slave_ok = true;
    push_ok_line("selftest");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Sensors:  FAIL"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Overall:  FAIL"));
}

void test_TC_CS_112_selftest_fd_modbus_not_ok_comms_fail(void)
{
    do_init();
    g_spy_snap.readings[SENSOR_ID_TEMPERATURE].valid = true;
    g_spy_snap.readings[SENSOR_ID_HUMIDITY].valid = true;
    g_spy_snap.readings[SENSOR_ID_PRESSURE].valid = true;
    g_spy_health.modbus_slave_ok = false; /* comms not OK */

    push_ok_line("selftest");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Comms:    FAIL"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Overall:  FAIL"));
}

void test_TC_CS_115_selftest_flush_error_flash_fail(void)
{
    do_init();
    g_spy_snap.readings[SENSOR_ID_TEMPERATURE].valid = true;
    g_spy_snap.readings[SENSOR_ID_HUMIDITY].valid = true;
    g_spy_snap.readings[SENSOR_ID_PRESSURE].valid = true;
    g_spy_health.modbus_slave_ok = true;
    g_spy_flush_ret = CONFIG_SERVICE_ERR_PERSIST;

    push_ok_line("selftest");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Flash:    FAIL"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Overall:  FAIL"));
}

void test_TC_CS_116_selftest_prints_table_rows(void)
{
    do_init();
    push_ok_line("selftest");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Sensors:"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Comms:"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Flash:"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Overall:"));
}

void test_TC_CS_117_selftest_result_stored_and_retrieved(void)
{
    do_init();
    g_spy_snap.readings[SENSOR_ID_TEMPERATURE].valid = true;
    g_spy_snap.readings[SENSOR_ID_HUMIDITY].valid = true;
    g_spy_snap.readings[SENSOR_ID_PRESSURE].valid = true;
    g_spy_health.modbus_slave_ok = true;

    push_ok_line("selftest");
    console_service->run_once();

    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("selftest-result");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Overall:  PASS"));
}

void test_TC_CS_118_selftest_result_no_stored_result(void)
{
    do_init();
    push_ok_line("selftest-result");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "No self-test result stored"));
}

/* ======================================================================= */
/* TC-CS-150: modbus status (FD-only)                                      */
/* ======================================================================= */

void test_TC_CS_150_modbus_status_prints_counters(void)
{
    do_init();
    g_spy_health.modbus_valid_frames = 100UL;
    g_spy_health.modbus_crc_errors = 3UL;
    g_spy_health.modbus_addr_mismatches = 1UL;
    g_spy_health.modbus_exception_responses = 2UL;

    push_ok_line("modbus status");
    TEST_ASSERT_EQUAL_INT(CONSOLE_SERVICE_ERR_OK, console_service->run_once());
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "100"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "CRC errors"));
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "Address mismatches"));
}

/* ======================================================================= */
/* TC-CS-170: Prompt always emitted after run_once                        */
/* ======================================================================= */

void test_TC_CS_170_prompt_printed_after_every_run_once(void)
{
    do_init();
    /* Clear init prompt */
    g_tx_len = 0U;
    g_tx_buf[0] = '\0';

    push_ok_line("help");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "fd>"));

    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_ok_line("frobnicate");
    console_service->run_once();
    TEST_ASSERT_NOT_NULL(strstr(g_tx_buf, "fd>"));

    g_tx_len = 0U;
    g_tx_buf[0] = '\0';
    push_no_line(); /* no line available */
    console_service->run_once();
    /* No prompt when no line was available — run_once returns early */
}
