/**
 * @file console_service.c
 * @brief ConsoleService implementation — FD + GW operator serial console.
 *
 * Design notes vs LLD:
 *  - No internal ring buffer: the DebugUartDriver already accumulates lines
 *    in its ISR. run_once() calls s_uart->read_line() directly.
 *  - No ilogger_t parameter in init: logger consumed via macros (P4 exception).
 *  - prov commit: calls set_param() per field (no block-apply method).
 *  - Selftest result: held in s_last_selftest (not persisted across reboot;
 *    CS-O9 tracks adding config-store persistence).
 *  - modbus-baud, modbus-parity provisioning: CS-O10 (config_params_t needs
 *    extension). Only modbus-addr implemented in this release.
 *
 * @see docs/lld/application/console-service-lld.md
 */

#include "console_service/console_service.h"

#ifdef TEST
#define LOG_ERROR(m, f, ...) ((void) 0)
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#define LOG_DEBUG(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>

#ifndef TEST
#include "FreeRTOS.h"
#include "task.h"
#endif /* TEST */

/* ======================================================================= */
/* Constants                                                               */
/* ======================================================================= */

#define CS_MOD "ConsoleService"
#define CS_LINE_BUF_SIZE (DEBUG_UART_LINE_MAX_LEN + 1U)
#define CS_TX_BUF_SIZE (256U)
#define CS_MAX_ARGV (8)
#define CS_TX_TIMEOUT_MS (100U)
#define CS_CONFIRM_MS (30000UL)
#define CS_MODBUS_ADDR_MIN (1U)
#define CS_MODBUS_ADDR_MAX (247U)

#ifdef TEST
#define FW_VERSION_STRING "0.0.0-test"
#define FW_BUILD_DATE "1970-01-01"
#define FW_BUILD_TIME "00:00:00"
#else
#include "firmware_version.h"
#endif /* TEST */

/* ======================================================================= */
/* Internal types                                                          */
/* ======================================================================= */

typedef struct
{
    bool dirty;
    uint8_t modbus_slave_addr;
#if defined(STM32L475xx)
    char mqtt_endpoint[128];
#endif
} prov_pending_t;

typedef struct
{
    bool dirty;
    uint32_t polling_interval_ms;
    int16_t temp_alarm_high;
    int16_t temp_alarm_low;
    int16_t temp_hysteresis;
    uint16_t humidity_alarm_high;
    uint16_t humidity_alarm_low;
    uint16_t humidity_hysteresis;
    uint16_t pressure_alarm_high;
    uint16_t pressure_alarm_low;
    uint16_t pressure_hysteresis;
} cfg_pending_t;

typedef struct
{
    bool stored;
    bool sensor_pass;
    bool comms_pass;
    bool flash_pass;
    bool overall_pass;
    uint32_t timestamp_epoch;
} selftest_result_t;

typedef console_service_err_t (*cmd_handler_t)(int argc, const char *argv[]);

typedef struct
{
    const char *token;
    uint8_t min_argc;
    uint8_t max_argc;
    cmd_handler_t handler;
    const char *help;
} cmd_entry_t;

/* ======================================================================= */
/* File-scope state                                                        */
/* ======================================================================= */

static const idebug_uart_t *s_uart;
static const isensor_service_t *s_sensors;
static const iconfig_provider_t *s_cfg_read;
static const iconfig_manager_t *s_cfg_write;
static const ihealth_snapshot_t *s_health;
#if defined(STM32L475xx)
static const idevice_profile_mgr_t *s_profiles;
#endif

static bool s_initialised;
static prov_pending_t s_prov_pending;
static cfg_pending_t s_cfg_pending;
static selftest_result_t s_last_selftest;

#ifndef TEST
static TaskHandle_t s_console_task;
#endif

/* ======================================================================= */
/* Forward declarations of all command handlers                            */
/* ======================================================================= */

static console_service_err_t cmd_help(int argc, const char *argv[]);
static console_service_err_t cmd_version(int argc, const char *argv[]);
static console_service_err_t cmd_serial(int argc, const char *argv[]);
static console_service_err_t cmd_sensors(int argc, const char *argv[]);
static console_service_err_t cmd_status(int argc, const char *argv[]);
static console_service_err_t cmd_alarms(int argc, const char *argv[]);
static console_service_err_t cmd_selftest(int argc, const char *argv[]);
static console_service_err_t cmd_selftest_result(int argc, const char *argv[]);
static console_service_err_t cmd_config(int argc, const char *argv[]);
static console_service_err_t cmd_prov(int argc, const char *argv[]);
#if defined(STM32F469xx)
static console_service_err_t cmd_modbus(int argc, const char *argv[]);
#endif
#if defined(STM32L475xx)
static console_service_err_t cmd_profiles(int argc, const char *argv[]);
static console_service_err_t cmd_wifi(int argc, const char *argv[]);
static console_service_err_t cmd_mqtt(int argc, const char *argv[]);
#endif

/* ======================================================================= */
/* Command table                                                            */
/* ======================================================================= */

static const cmd_entry_t s_cmd_table[] = {
    /* token            min  max  handler               help */
    {"help", 0U, 0U, cmd_help, "Print this help"},
    {"version", 0U, 0U, cmd_version, "Print firmware version string"},
    {"serial", 0U, 0U, cmd_serial, "Print MCU unique serial number"},
    {"sensors", 0U, 0U, cmd_sensors, "Print latest sensor readings"},
    {"status", 0U, 0U, cmd_status, "Print device health snapshot"},
    {"alarms", 0U, 0U, cmd_alarms, "Print active alarms"},
    {"selftest", 0U, 0U, cmd_selftest, "Run device self-test"},
    {"selftest-result", 0U, 0U, cmd_selftest_result, "Print last self-test result"},
    {"config", 1U, 3U, cmd_config, "config list|set <key> <val>|commit|discard"},
    {"prov", 1U, 3U, cmd_prov, "prov set <key> <val>|commit|discard"},
#if defined(STM32F469xx)
    {"modbus", 1U, 1U, cmd_modbus, "modbus status"},
#endif
#if defined(STM32L475xx)
    {"profiles", 1U, 5U, cmd_profiles, "profiles list|add ...|remove <addr>"},
    {"wifi", 1U, 1U, cmd_wifi, "wifi status"},
    {"mqtt", 1U, 1U, cmd_mqtt, "mqtt status"},
#endif
};

#define CS_CMD_COUNT ((uint8_t) (sizeof(s_cmd_table) / sizeof(s_cmd_table[0])))

/* ======================================================================= */
/* UART output helpers                                                     */
/* ======================================================================= */

static void tx_str(const char *s)
{
    (void) s_uart->send((const uint8_t *) s, strlen(s), CS_TX_TIMEOUT_MS);
}

static void tx_fmt(const char *fmt, ...)
{
    char buf[CS_TX_BUF_SIZE];
    va_list ap;
    va_start(ap, fmt);
    (void) vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tx_str(buf);
}

static void print_prompt(void)
{
#if defined(STM32L475xx)
    tx_str("\r\ngw> ");
#else
    tx_str("\r\nfd> ");
#endif
}

/* ======================================================================= */
/* Line tokeniser                                                           */
/* ======================================================================= */

static int tokenise(char *line, const char *argv[], int max_argv)
{
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < max_argv)
    {
        while (isspace((unsigned char) *p))
        {
            p++;
        }
        if (*p == '\0')
        {
            break;
        }
        argv[argc++] = p;
        while (*p != '\0' && !isspace((unsigned char) *p))
        {
            p++;
        }
        if (*p != '\0')
        {
            *p = '\0';
            p++;
        }
    }
    return argc;
}

/* ======================================================================= */
/* Confirm-prompt helper                                                   */
/* ======================================================================= */

static bool read_confirm_with_timeout(void)
{
#ifndef TEST
    BaseType_t notified = xTaskNotifyWait(0U, 0xFFFFFFFFUL, NULL, pdMS_TO_TICKS(CS_CONFIRM_MS));
    if (notified == pdFALSE)
    {
        return false;
    }
#endif /* TEST */
    uint8_t buf[CS_LINE_BUF_SIZE];
    size_t len = 0U;
    debug_uart_line_flag_t flag = DEBUG_UART_LINE_OK;
    debug_uart_err_t derr = s_uart->read_line(buf, sizeof(buf), &len, &flag);
    if (derr != DEBUG_UART_OK)
    {
        return false;
    }
    return (len > 0U && ((buf[0] == 'y') || (buf[0] == 'Y')));
}

/* ======================================================================= */
/* Fixed-point print helpers (P9 — no floating-point)                      */
/* ======================================================================= */

static void print_centi(int32_t v, const char *unit)
{
    /* value is in centi-units (×100).  Handle sign explicitly for -0 edge. */
    const char *sign = (v < 0) ? "-" : "";
    int32_t abs_v = (v < 0) ? -v : v;
    tx_fmt("%s%d.%02d %s", sign, (int) (abs_v / 100), (int) (abs_v % 100), unit);
}

static void print_deci(int32_t v, const char *unit)
{
    /* value is in deci-units (×10). */
    const char *sign = (v < 0) ? "-" : "";
    int32_t abs_v = (v < 0) ? -v : v;
    tx_fmt("%s%d.%d %s", sign, (int) (abs_v / 10), (int) (abs_v % 10), unit);
}

/* ======================================================================= */
/* Dispatch                                                                */
/* ======================================================================= */

static console_service_err_t dispatch(int argc, const char *argv[])
{
    for (uint8_t i = 0U; i < CS_CMD_COUNT; i++)
    {
        if (strcmp(argv[0], s_cmd_table[i].token) == 0)
        {
            int sub_argc = argc - 1; /* args after argv[0] */
            if ((sub_argc < (int) s_cmd_table[i].min_argc) ||
                (sub_argc > (int) s_cmd_table[i].max_argc))
            {
                tx_fmt("[ERR] usage: %s %s\r\n", s_cmd_table[i].token, s_cmd_table[i].help);
                return CONSOLE_SERVICE_ERR_VALIDATION;
            }
            console_service_err_t rc = s_cmd_table[i].handler(argc, argv);
            if ((rc != CONSOLE_SERVICE_ERR_OK) && (rc != CONSOLE_SERVICE_ERR_VALIDATION))
            {
                /* Non-validation errors already printed by the handler. */
            }
            return rc;
        }
    }
    tx_fmt("[ERR] unknown command '%s'. Type 'help'.\r\n", argv[0]);
    LOG_WARN(CS_MOD, "unknown command");
    return CONSOLE_SERVICE_ERR_UNKNOWN_KEY;
}

/* ======================================================================= */
/* run_once                                                                */
/* ======================================================================= */

static console_service_err_t do_run_once(void)
{
    if (!s_initialised)
    {
        return CONSOLE_SERVICE_ERR_NOT_INIT;
    }

    uint8_t line[CS_LINE_BUF_SIZE];
    size_t len = 0U;
    debug_uart_line_flag_t flag = DEBUG_UART_LINE_OK;

    debug_uart_err_t derr = s_uart->read_line(line, sizeof(line), &len, &flag);
    if (derr == DEBUG_UART_ERR_NO_LINE_AVAILABLE)
    {
        return CONSOLE_SERVICE_ERR_OK;
    }

    if (flag == DEBUG_UART_LINE_TRUNCATED)
    {
        tx_str("[ERR] line overflow\r\n");
        LOG_WARN(CS_MOD, "line overflow");
        print_prompt();
        return CONSOLE_SERVICE_ERR_LINE_OVERFLOW;
    }

    const char *argv[CS_MAX_ARGV];
    int argc = tokenise((char *) line, argv, CS_MAX_ARGV);

    console_service_err_t rc = CONSOLE_SERVICE_ERR_OK;
    if (argc > 0)
    {
        rc = dispatch(argc, argv);
    }
    print_prompt();
    return rc;
}

/* ======================================================================= */
/* ISR line-ready callback (wires task notification from UART ISR)        */
/* ======================================================================= */

#ifndef TEST
static void on_line_ready_isr(void *ctx)
{
    (void) ctx;
    BaseType_t woken = pdFALSE;
    xTaskNotifyFromISR(s_console_task, 1U, eSetBits, &woken);
    portYIELD_FROM_ISR(woken);
}
#endif /* TEST */

/* ======================================================================= */
/* Command handlers — shared                                               */
/* ======================================================================= */

static console_service_err_t cmd_help(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    tx_str("Available commands:\r\n");
    for (uint8_t i = 0U; i < CS_CMD_COUNT; i++)
    {
        tx_fmt("  %-20s %s\r\n", s_cmd_table[i].token, s_cmd_table[i].help);
    }
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_version(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    tx_fmt("Firmware: %s  Built: %s %s\r\n", FW_VERSION_STRING, FW_BUILD_DATE, FW_BUILD_TIME);
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_serial(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
#ifndef TEST
    /* STM32 UID is three 32-bit words at 0x1FFF7A10 (F4) / 0x1FFF7590 (L4). */
#if defined(STM32L475xx)
    const uint32_t *uid = (const uint32_t *) 0x1FFF7590UL;
#else
    const uint32_t *uid = (const uint32_t *) 0x1FFF7A10UL;
#endif
    tx_fmt("UID: %08lX-%08lX-%08lX\r\n", (unsigned long) uid[0], (unsigned long) uid[1],
           (unsigned long) uid[2]);
#else
    tx_str("UID: DEADBEEF-CAFEF00D-12345678\r\n");
#endif /* TEST */
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_sensors(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    sensor_snapshot_t snap;
    sensor_service_err_t err = s_sensors->get_snapshot(&snap);
    if (err != SENSOR_SERVICE_ERR_OK)
    {
        tx_str("[ERR] sensor read failed\r\n");
        return CONSOLE_SERVICE_ERR_APPLY_FAILED;
    }

    const char *names[] = {"Temperature", "Humidity   ", "Pressure   "};
    const int fd_count = 3;

    for (int i = 0; i < fd_count; i++)
    {
        if (!snap.readings[i].valid)
        {
            tx_fmt("  %s: INVALID\r\n", names[i]);
        }
        else
        {
            tx_fmt("  %s: ", names[i]);
            if (i == (int) SENSOR_ID_TEMPERATURE || i == (int) SENSOR_ID_HUMIDITY)
            {
                print_centi(snap.readings[i].value,
                            (i == (int) SENSOR_ID_TEMPERATURE) ? "degC" : "%RH");
            }
            else
            {
                print_deci(snap.readings[i].value, "hPa");
            }
            tx_fmt("  [t=%lu]\r\n", (unsigned long) snap.readings[i].timestamp.epoch);
        }
    }
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_status(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    device_health_snapshot_t snap;
    health_monitor_err_t err = s_health->get_snapshot(&snap);
    if (err != HEALTH_MONITOR_ERR_OK)
    {
        tx_str("[ERR] health snapshot unavailable\r\n");
        return CONSOLE_SERVICE_ERR_APPLY_FAILED;
    }
    tx_fmt("  Uptime:            %lu s\r\n", (unsigned long) snap.uptime_s);
    tx_fmt("  Sensor failures:   %lu\r\n", (unsigned long) snap.sensor_fail_count);
    tx_fmt("  Config write fail: %s\r\n", snap.config_write_failed ? "YES" : "no");
#if defined(STM32F469xx)
    tx_fmt("  Modbus frames:     %lu\r\n", (unsigned long) snap.modbus_valid_frames);
    tx_fmt("  Modbus CRC errors: %lu\r\n", (unsigned long) snap.modbus_crc_errors);
#endif
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_alarms(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    device_health_snapshot_t snap;
    (void) s_health->get_snapshot(&snap);

    const char *sensor_names[] = {
        "Temperature", "Humidity", "Pressure", "Accel-X", "Accel-Y", "Accel-Z",
        "Gyro-X",      "Gyro-Y",   "Gyro-Z",   "Mag-X",   "Mag-Y",   "Mag-Z",
    };
    bool any = false;
    for (int i = 0; i < (int) SENSOR_ID_COUNT; i++)
    {
        if (snap.alarm_state[i] != ALARM_STATE_CLEAR)
        {
            const char *dir = (snap.alarm_state[i] == ALARM_STATE_ACTIVE_HIGH) ? "HIGH" : "LOW";
            tx_fmt("  %s: ACTIVE_%s\r\n", sensor_names[i], dir);
            any = true;
        }
    }
    if (!any)
    {
        tx_str("  No active alarms\r\n");
    }
    return CONSOLE_SERVICE_ERR_OK;
}

/* ── Self-test ────────────────────────────────────────────────────────── */

#if defined(STM32F469xx)
static bool board_comms_ok(const device_health_snapshot_t *snap)
{
    return snap->modbus_slave_ok;
}
#elif defined(STM32L475xx)
static bool board_comms_ok(const device_health_snapshot_t *snap)
{
    return snap->cloud_connected;
}
#else
static bool board_comms_ok(const device_health_snapshot_t *snap)
{
    (void) snap;
    return true;
}
#endif

static console_service_err_t cmd_selftest(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    selftest_result_t r;
    (void) memset(&r, 0, sizeof(r));

    /* Sensors */
    (void) s_sensors->read_on_demand();
    sensor_snapshot_t ssnap;
    (void) s_sensors->get_snapshot(&ssnap);
    r.sensor_pass = ssnap.readings[SENSOR_ID_TEMPERATURE].valid &&
                    ssnap.readings[SENSOR_ID_HUMIDITY].valid &&
                    ssnap.readings[SENSOR_ID_PRESSURE].valid;

    /* Comms */
    device_health_snapshot_t hsnap;
    (void) s_health->get_snapshot(&hsnap);
    r.comms_pass = board_comms_ok(&hsnap);

    /* Flash round-trip */
    config_service_err_t ferr = s_cfg_write->flush();
    r.flash_pass = (ferr == CONFIG_SERVICE_OK);

    r.overall_pass = r.sensor_pass && r.comms_pass && r.flash_pass;

    tx_str("  Self-test results:\r\n");
    tx_fmt("  Sensors:  %s\r\n", r.sensor_pass ? "PASS" : "FAIL");
    tx_fmt("  Comms:    %s\r\n", r.comms_pass ? "PASS" : "FAIL");
    tx_fmt("  Flash:    %s\r\n", r.flash_pass ? "PASS" : "FAIL");
    tx_fmt("  Overall:  %s\r\n", r.overall_pass ? "PASS" : "FAIL");

    s_last_selftest = r;
    s_last_selftest.stored = true;
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_selftest_result(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    if (!s_last_selftest.stored)
    {
        tx_str("  No self-test result stored\r\n");
        return CONSOLE_SERVICE_ERR_OK;
    }
    tx_fmt("  Sensors:  %s\r\n", s_last_selftest.sensor_pass ? "PASS" : "FAIL");
    tx_fmt("  Comms:    %s\r\n", s_last_selftest.comms_pass ? "PASS" : "FAIL");
    tx_fmt("  Flash:    %s\r\n", s_last_selftest.flash_pass ? "PASS" : "FAIL");
    tx_fmt("  Overall:  %s\r\n", s_last_selftest.overall_pass ? "PASS" : "FAIL");
    return CONSOLE_SERVICE_ERR_OK;
}

/* ── config command ────────────────────────────────────────────────────── */

typedef struct
{
    const char *key;
    config_param_id_t param;
    bool is_signed;
    const char *units;
} cfg_key_entry_t;

static const cfg_key_entry_t s_cfg_keys[] = {
    {"polling-interval-ms", CONFIG_PARAM_POLL_INTERVAL, false, "ms"},
    {"temp-alarm-high", CONFIG_PARAM_TEMP_ALARM_HIGH, true, "centi-degC"},
    {"temp-alarm-low", CONFIG_PARAM_TEMP_ALARM_LOW, true, "centi-degC"},
    {"temp-hysteresis", CONFIG_PARAM_TEMP_HYSTERESIS, true, "centi-degC"},
    {"humidity-alarm-high", CONFIG_PARAM_HUMIDITY_ALARM_HIGH, false, "centi-%RH"},
    {"humidity-alarm-low", CONFIG_PARAM_HUMIDITY_ALARM_LOW, false, "centi-%RH"},
    {"humidity-hysteresis", CONFIG_PARAM_HUMIDITY_HYSTERESIS, false, "centi-%RH"},
    {"pressure-alarm-high", CONFIG_PARAM_PRESSURE_ALARM_HIGH, false, "deci-hPa"},
    {"pressure-alarm-low", CONFIG_PARAM_PRESSURE_ALARM_LOW, false, "deci-hPa"},
    {"pressure-hysteresis", CONFIG_PARAM_PRESSURE_HYSTERESIS, false, "deci-hPa"},
};
#define CS_CFG_KEY_COUNT ((uint8_t) (sizeof(s_cfg_keys) / sizeof(s_cfg_keys[0])))

static console_service_err_t cmd_config(int argc, const char *argv[])
{
    if (argc < 2)
    {
        tx_str("[ERR] usage: config list|set <key> <val>|commit|discard\r\n");
        return CONSOLE_SERVICE_ERR_VALIDATION;
    }
    const char *sub = argv[1];

    /* config list */
    if (strcmp(sub, "list") == 0)
    {
        const config_params_t *p = s_cfg_read->get_params();
        if (p == NULL)
        {
            tx_str("[ERR] config unavailable\r\n");
            return CONSOLE_SERVICE_ERR_APPLY_FAILED;
        }
        tx_fmt("  polling-interval-ms: %lu\r\n", (unsigned long) p->polling_interval_ms);
        tx_fmt("  temp-alarm-high:     %d (centi-degC)\r\n", (int) p->temp_alarm_high);
        tx_fmt("  temp-alarm-low:      %d (centi-degC)\r\n", (int) p->temp_alarm_low);
        tx_fmt("  temp-hysteresis:     %d (centi-degC)\r\n", (int) p->temp_hysteresis);
        tx_fmt("  humidity-alarm-high: %u (centi-%%RH)\r\n", (unsigned) p->humidity_alarm_high);
        tx_fmt("  humidity-alarm-low:  %u (centi-%%RH)\r\n", (unsigned) p->humidity_alarm_low);
        tx_fmt("  humidity-hysteresis: %u (centi-%%RH)\r\n", (unsigned) p->humidity_hysteresis);
        tx_fmt("  pressure-alarm-high: %u (deci-hPa)\r\n", (unsigned) p->pressure_alarm_high);
        tx_fmt("  pressure-alarm-low:  %u (deci-hPa)\r\n", (unsigned) p->pressure_alarm_low);
        tx_fmt("  pressure-hysteresis: %u (deci-hPa)\r\n", (unsigned) p->pressure_hysteresis);
        tx_fmt("  modbus-slave-addr:   %u\r\n", (unsigned) p->modbus_slave_addr);
        return CONSOLE_SERVICE_ERR_OK;
    }

    /* config set <key> <value> */
    if (strcmp(sub, "set") == 0)
    {
        if (argc < 4)
        {
            tx_str("[ERR] usage: config set <key> <value>\r\n");
            return CONSOLE_SERVICE_ERR_VALIDATION;
        }
        const char *key = argv[2];
        const char *val = argv[3];

        for (uint8_t i = 0U; i < CS_CFG_KEY_COUNT; i++)
        {
            if (strcmp(key, s_cfg_keys[i].key) == 0)
            {
                char *end = NULL;
                long parsed = strtol(val, &end, 10);
                if ((end == val) || (*end != '\0'))
                {
                    tx_fmt("[ERR] invalid value '%s' for %s\r\n", val, key);
                    LOG_DEBUG(CS_MOD, "cfg set: non-numeric value");
                    return CONSOLE_SERVICE_ERR_VALIDATION;
                }
                config_service_err_t verr =
                    s_cfg_write->validate_param(s_cfg_keys[i].param, &parsed);
                if (verr != CONFIG_SERVICE_OK)
                {
                    tx_fmt("[ERR] invalid value '%s' for %s\r\n", val, key);
                    LOG_DEBUG(CS_MOD, "cfg set: validation failed");
                    return CONSOLE_SERVICE_ERR_VALIDATION;
                }
                /* Stage the change */
                switch (s_cfg_keys[i].param)
                {
                case CONFIG_PARAM_POLL_INTERVAL:
                    s_cfg_pending.polling_interval_ms = (uint32_t) parsed;
                    break;
                case CONFIG_PARAM_TEMP_ALARM_HIGH:
                    s_cfg_pending.temp_alarm_high = (int16_t) parsed;
                    break;
                case CONFIG_PARAM_TEMP_ALARM_LOW:
                    s_cfg_pending.temp_alarm_low = (int16_t) parsed;
                    break;
                case CONFIG_PARAM_TEMP_HYSTERESIS:
                    s_cfg_pending.temp_hysteresis = (int16_t) parsed;
                    break;
                case CONFIG_PARAM_HUMIDITY_ALARM_HIGH:
                    s_cfg_pending.humidity_alarm_high = (uint16_t) parsed;
                    break;
                case CONFIG_PARAM_HUMIDITY_ALARM_LOW:
                    s_cfg_pending.humidity_alarm_low = (uint16_t) parsed;
                    break;
                case CONFIG_PARAM_HUMIDITY_HYSTERESIS:
                    s_cfg_pending.humidity_hysteresis = (uint16_t) parsed;
                    break;
                case CONFIG_PARAM_PRESSURE_ALARM_HIGH:
                    s_cfg_pending.pressure_alarm_high = (uint16_t) parsed;
                    break;
                case CONFIG_PARAM_PRESSURE_ALARM_LOW:
                    s_cfg_pending.pressure_alarm_low = (uint16_t) parsed;
                    break;
                case CONFIG_PARAM_PRESSURE_HYSTERESIS:
                    s_cfg_pending.pressure_hysteresis = (uint16_t) parsed;
                    break;
                default:
                    break;
                }
                s_cfg_pending.dirty = true;
                tx_fmt("[OK] staged: %s = %s\r\n", key, val);
                return CONSOLE_SERVICE_ERR_OK;
            }
        }
        tx_fmt("[ERR] unknown key '%s'\r\n", key);
        LOG_WARN(CS_MOD, "cfg set: unknown key");
        return CONSOLE_SERVICE_ERR_UNKNOWN_KEY;
    }

    /* config commit */
    if (strcmp(sub, "commit") == 0)
    {
        if (!s_cfg_pending.dirty)
        {
            tx_str("[INFO] Nothing staged.\r\n");
            return CONSOLE_SERVICE_ERR_OK;
        }
        tx_str("  Staged config changes — Apply? [y/N]: ");
        if (!read_confirm_with_timeout())
        {
            (void) memset(&s_cfg_pending, 0, sizeof(s_cfg_pending));
            tx_str("[INFO] Discarded.\r\n");
            return CONSOLE_SERVICE_ERR_TIMEOUT;
        }

        /* Apply each dirty field */
        config_service_err_t rc = CONFIG_SERVICE_OK;
        const struct
        {
            config_param_id_t id;
            const void *val;
        } applies[] = {
            {CONFIG_PARAM_POLL_INTERVAL, &s_cfg_pending.polling_interval_ms},
            {CONFIG_PARAM_TEMP_ALARM_HIGH, &s_cfg_pending.temp_alarm_high},
            {CONFIG_PARAM_TEMP_ALARM_LOW, &s_cfg_pending.temp_alarm_low},
            {CONFIG_PARAM_TEMP_HYSTERESIS, &s_cfg_pending.temp_hysteresis},
            {CONFIG_PARAM_HUMIDITY_ALARM_HIGH, &s_cfg_pending.humidity_alarm_high},
            {CONFIG_PARAM_HUMIDITY_ALARM_LOW, &s_cfg_pending.humidity_alarm_low},
            {CONFIG_PARAM_HUMIDITY_HYSTERESIS, &s_cfg_pending.humidity_hysteresis},
            {CONFIG_PARAM_PRESSURE_ALARM_HIGH, &s_cfg_pending.pressure_alarm_high},
            {CONFIG_PARAM_PRESSURE_ALARM_LOW, &s_cfg_pending.pressure_alarm_low},
            {CONFIG_PARAM_PRESSURE_HYSTERESIS, &s_cfg_pending.pressure_hysteresis},
        };
        for (size_t i = 0U; i < sizeof(applies) / sizeof(applies[0]); i++)
        {
            config_service_err_t e = s_cfg_write->set_param(applies[i].id, applies[i].val);
            if ((e != CONFIG_SERVICE_OK) && (rc == CONFIG_SERVICE_OK))
            {
                rc = e;
            }
        }
        if (rc != CONFIG_SERVICE_OK)
        {
            tx_str("[ERR] apply failed\r\n");
            LOG_WARN(CS_MOD, "cfg commit: apply failed");
            return CONSOLE_SERVICE_ERR_APPLY_FAILED;
        }
        (void) memset(&s_cfg_pending, 0, sizeof(s_cfg_pending));
        tx_str("[OK] Config applied.\r\n");
        return CONSOLE_SERVICE_ERR_OK;
    }

    /* config discard */
    if (strcmp(sub, "discard") == 0)
    {
        (void) memset(&s_cfg_pending, 0, sizeof(s_cfg_pending));
        tx_str("[INFO] Discarded.\r\n");
        return CONSOLE_SERVICE_ERR_OK;
    }

    tx_fmt("[ERR] unknown config sub-command '%s'\r\n", sub);
    return CONSOLE_SERVICE_ERR_UNKNOWN_KEY;
}

/* ── prov command ─────────────────────────────────────────────────────── */

static bool validate_modbus_addr(uint8_t addr)
{
    return (addr >= CS_MODBUS_ADDR_MIN) && (addr <= CS_MODBUS_ADDR_MAX);
}

static console_service_err_t cmd_prov(int argc, const char *argv[])
{
    if (argc < 2)
    {
        tx_str("[ERR] usage: prov set <key> <val>|commit|discard\r\n");
        return CONSOLE_SERVICE_ERR_VALIDATION;
    }
    const char *sub = argv[1];

    if (strcmp(sub, "set") == 0)
    {
        if (argc < 4)
        {
            tx_str("[ERR] usage: prov set <key> <value>\r\n");
            return CONSOLE_SERVICE_ERR_VALIDATION;
        }
        const char *key = argv[2];
        const char *val = argv[3];

        if (strcmp(key, "modbus-addr") == 0)
        {
            char *end = NULL;
            long addr = strtol(val, &end, 10);
            if ((end == val) || (*end != '\0') || !validate_modbus_addr((uint8_t) addr))
            {
                tx_fmt("[ERR] invalid value '%s' for modbus-addr (1..247)\r\n", val);
                LOG_DEBUG(CS_MOD, "prov set: invalid modbus-addr");
                return CONSOLE_SERVICE_ERR_VALIDATION;
            }
            s_prov_pending.modbus_slave_addr = (uint8_t) addr;
            s_prov_pending.dirty = true;
            tx_fmt("[OK] staged: modbus-addr = %s\r\n", val);
            return CONSOLE_SERVICE_ERR_OK;
        }
#if defined(STM32L475xx)
        if (strcmp(key, "mqtt-endpoint") == 0)
        {
            size_t vlen = strlen(val);
            if ((vlen == 0U) || (vlen >= sizeof(s_prov_pending.mqtt_endpoint)))
            {
                tx_fmt("[ERR] invalid value for mqtt-endpoint (1..127 chars)\r\n");
                return CONSOLE_SERVICE_ERR_VALIDATION;
            }
            (void) memcpy(s_prov_pending.mqtt_endpoint, val, vlen + 1U);
            s_prov_pending.dirty = true;
            tx_fmt("[OK] staged: mqtt-endpoint = %s\r\n", val);
            return CONSOLE_SERVICE_ERR_OK;
        }
#endif
        /* Keys whose config_params_t home is CS-O10 (pending extension) */
        if ((strcmp(key, "wifi-ssid") == 0) || (strcmp(key, "wifi-pass") == 0) ||
            (strcmp(key, "modbus-baud") == 0) || (strcmp(key, "modbus-parity") == 0))
        {
            tx_fmt("[ERR] key '%s' not yet supported (CS-O10)\r\n", key);
            return CONSOLE_SERVICE_ERR_UNKNOWN_KEY;
        }
        tx_fmt("[ERR] unknown key '%s'\r\n", key);
        return CONSOLE_SERVICE_ERR_UNKNOWN_KEY;
    }

    if (strcmp(sub, "commit") == 0)
    {
        if (!s_prov_pending.dirty)
        {
            tx_str("[INFO] Nothing staged.\r\n");
            return CONSOLE_SERVICE_ERR_OK;
        }
        tx_str("  Staged provisioning changes — Apply? [y/N]: ");
        if (!read_confirm_with_timeout())
        {
            (void) memset(&s_prov_pending, 0, sizeof(s_prov_pending));
            tx_str("[INFO] Discarded.\r\n");
            return CONSOLE_SERVICE_ERR_TIMEOUT;
        }
        config_service_err_t rc = s_cfg_write->set_param(CONFIG_PARAM_MODBUS_SLAVE_ADDR,
                                                         &s_prov_pending.modbus_slave_addr);
#if defined(STM32L475xx)
        if (rc == CONFIG_SERVICE_OK)
        {
            rc = s_cfg_write->set_param(CONFIG_PARAM_MQTT_BROKER, s_prov_pending.mqtt_endpoint);
        }
#endif
        if (rc != CONFIG_SERVICE_OK)
        {
            tx_str("[ERR] apply failed\r\n");
            LOG_WARN(CS_MOD, "prov commit: apply failed");
            return CONSOLE_SERVICE_ERR_APPLY_FAILED;
        }
        (void) memset(&s_prov_pending, 0, sizeof(s_prov_pending));
        tx_str("[OK] Provisioning applied and persisted.\r\n");
        return CONSOLE_SERVICE_ERR_OK;
    }

    if (strcmp(sub, "discard") == 0)
    {
        (void) memset(&s_prov_pending, 0, sizeof(s_prov_pending));
        tx_str("[INFO] Discarded.\r\n");
        return CONSOLE_SERVICE_ERR_OK;
    }

    tx_fmt("[ERR] unknown prov sub-command '%s'\r\n", sub);
    return CONSOLE_SERVICE_ERR_UNKNOWN_KEY;
}

/* ── FD board-specific commands ───────────────────────────────────────── */

#if defined(STM32F469xx)
static console_service_err_t cmd_modbus(int argc, const char *argv[])
{
    if (argc < 2 || strcmp(argv[1], "status") != 0)
    {
        tx_str("[ERR] usage: modbus status\r\n");
        return CONSOLE_SERVICE_ERR_VALIDATION;
    }
    device_health_snapshot_t snap;
    (void) s_health->get_snapshot(&snap);
    tx_str("  Modbus Slave Stats:\r\n");
    tx_fmt("    Valid frames:        %lu\r\n", (unsigned long) snap.modbus_valid_frames);
    tx_fmt("    CRC errors:          %lu\r\n", (unsigned long) snap.modbus_crc_errors);
    tx_fmt("    Address mismatches:  %lu\r\n", (unsigned long) snap.modbus_addr_mismatches);
    tx_fmt("    Exception responses: %lu\r\n", (unsigned long) snap.modbus_exception_responses);
    return CONSOLE_SERVICE_ERR_OK;
}
#endif /* STM32F469xx */

/* ── GW board-specific commands ───────────────────────────────────────── */

#if defined(STM32L475xx)
static console_service_err_t cmd_profiles(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    /* IDeviceProfileManager not yet implemented (CS-O4 / TODO). */
    tx_str("[INFO] profiles command not yet implemented (CS-O4)\r\n");
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_wifi(int argc, const char *argv[])
{
    if (argc < 2 || strcmp(argv[1], "status") != 0)
    {
        tx_str("[ERR] usage: wifi status\r\n");
        return CONSOLE_SERVICE_ERR_VALIDATION;
    }
    device_health_snapshot_t snap;
    (void) s_health->get_snapshot(&snap);
    tx_fmt("  WiFi RSSI: %d dBm\r\n", (int) snap.wifi_rssi_dbm);
    tx_fmt("  Cloud connected: %s\r\n", snap.cloud_connected ? "yes" : "no");
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_mqtt(int argc, const char *argv[])
{
    if (argc < 2 || strcmp(argv[1], "status") != 0)
    {
        tx_str("[ERR] usage: mqtt status\r\n");
        return CONSOLE_SERVICE_ERR_VALIDATION;
    }
    device_health_snapshot_t snap;
    (void) s_health->get_snapshot(&snap);
    tx_fmt("  Publishes sent:   %lu\r\n", (unsigned long) snap.mqtt_publishes_sent);
    tx_fmt("  Publishes failed: %lu\r\n", (unsigned long) snap.mqtt_publishes_failed);
    tx_fmt("  Reconnects:       %lu\r\n", (unsigned long) snap.mqtt_reconnect_count);
    tx_fmt("  Connected:        %s\r\n", snap.cloud_connected ? "yes" : "no");
    return CONSOLE_SERVICE_ERR_OK;
}
#endif /* STM32L475xx */

/* ======================================================================= */
/* Vtable and singleton                                                    */
/* ======================================================================= */

static console_service_err_t do_init_finalise(void)
{
    tx_str("\r\n=== System operational. Type 'help' for commands. ===\r\n");
    tx_str("> ");
    return CONSOLE_SERVICE_ERR_OK;
}

static const iconsole_service_t s_console_service_vtable = {
    .init_finalise = do_init_finalise,
    .run_once      = do_run_once,
};
const iconsole_service_t *const console_service = &s_console_service_vtable;

/* ======================================================================= */
/* console_service_init                                                    */
/* ======================================================================= */

console_service_err_t console_service_init(const idebug_uart_t *uart,
                                           const isensor_service_t *sensors,
                                           const iconfig_provider_t *cfg_read,
                                           const iconfig_manager_t *cfg_write,
                                           const ihealth_snapshot_t *health
#if defined(STM32L475xx)
                                           ,
                                           const idevice_profile_mgr_t *profiles
#endif
)
{
    if ((uart == NULL) || (sensors == NULL) || (cfg_read == NULL) || (cfg_write == NULL) ||
        (health == NULL))
    {
        LOG_ERROR(CS_MOD, "init: null argument");
        return CONSOLE_SERVICE_ERR_NULL_ARG;
    }
#if defined(STM32L475xx)
    if (profiles == NULL)
    {
        LOG_ERROR(CS_MOD, "init: null profiles on GW");
        return CONSOLE_SERVICE_ERR_NULL_ARG;
    }
    s_profiles = profiles;
#endif

    s_uart = uart;
    s_sensors = sensors;
    s_cfg_read = cfg_read;
    s_cfg_write = cfg_write;
    s_health = health;

    (void) memset(&s_prov_pending, 0, sizeof(s_prov_pending));
    (void) memset(&s_cfg_pending, 0, sizeof(s_cfg_pending));
    (void) memset(&s_last_selftest, 0, sizeof(s_last_selftest));

    s_initialised = true;
    print_prompt();
    return CONSOLE_SERVICE_ERR_OK;
}

/* ======================================================================= */
/* console_task_body                                                        */
/* ======================================================================= */

void console_task_body(void *arg)
{
    (void) arg;
#ifndef TEST
    s_console_task = xTaskGetCurrentTaskHandle();
    (void) s_uart->attach_rx(on_line_ready_isr, NULL);
    for (;;)
    {
        (void) xTaskNotifyWait(0U, 0xFFFFFFFFUL, NULL, portMAX_DELAY);
        (void) console_service->run_once();
    }
#endif /* TEST */
}

/* ======================================================================= */
/* Test-only hooks                                                         */
/* ======================================================================= */

#ifdef TEST
void console_service_reset_for_test(void)
{
    s_uart = NULL;
    s_sensors = NULL;
    s_cfg_read = NULL;
    s_cfg_write = NULL;
    s_health = NULL;
    s_initialised = false;
    (void) memset(&s_prov_pending, 0, sizeof(s_prov_pending));
    (void) memset(&s_cfg_pending, 0, sizeof(s_cfg_pending));
    (void) memset(&s_last_selftest, 0, sizeof(s_last_selftest));
}
#endif /* TEST */
