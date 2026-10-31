/**
 * @file commands.c
 * @brief Command table and handlers for the console REPL.
 */

#include "commands.h"

#include <stdio.h>
#include <string.h>

#include "console_service/port/console_port.h"
#include "fmt.h"
#include "firmware_version.h"
#include "model.h"
#include "repl.h"
#include "theme.h"

#ifdef TEST
#define LOG_DEBUG(m, f, ...) ((void) 0)
#define LOG_WARN(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

#define MODULE_TAG "CON"

#define SAY(fg, ...)                                                                               \
    do                                                                                             \
    {                                                                                              \
        char say_buf_[REPL_TEXT_MAX];                                                              \
        (void) snprintf(say_buf_, sizeof(say_buf_), __VA_ARGS__);                                  \
        repl_puts((fg), say_buf_);                                                                 \
    } while (0)

#define MCU_PART "STM32L475VGT6"
#define PROFILES_MAX_LISTED (16U)
#define BROKER_MAX_LEN (127U)
#define SSID_MAX_LEN (63U)
#define PASS_MIN_LEN (8U)
#define PASS_MAX_LEN (63U)
#define MODBUS_ADDR_MIN (1U)
#define MODBUS_ADDR_MAX (247U)
#define CHANNEL_KEY_MAX (6U)

/* ---------------------------------------------------------------------- */
/* State                                                                   */
/* ---------------------------------------------------------------------- */

typedef enum
{
    CONFIRM_NONE,
    CONFIRM_CFG,
    CONFIRM_PROV,
    CONFIRM_REBOOT,
} confirm_kind_t;

/* Operational parameters `config set` can stage (thresholds go through `set`). */
typedef enum
{
    K_U32,
    K_U16,
    K_I16,
} cfg_kind_t;

typedef struct
{
    const char *key;
    config_param_id_t id;
    cfg_kind_t kind;
    int32_t scale;
    uint8_t decimals;
} cfg_key_t;

static const cfg_key_t s_cfg_keys[] = {
    {"polling-interval-ms", CONFIG_PARAM_POLL_INTERVAL, K_U32, 1, 0U},
    {"filter-alpha", CONFIG_PARAM_FILTER_ALPHA, K_U16, 1, 0U},
    {"modbus-poll-period-ms", CONFIG_PARAM_MODBUS_POLL_PERIOD, K_U32, 1, 0U},
    {"telemetry-interval-ms", CONFIG_PARAM_TELEMETRY_INTERVAL, K_U32, 1, 0U},
    {"health-interval-ms", CONFIG_PARAM_HEALTH_INTERVAL, K_U32, 1, 0U},
    {"mqtt-port", CONFIG_PARAM_MQTT_PORT, K_U16, 1, 0U},
    {"temp-range-min", CONFIG_PARAM_TEMP_RANGE_MIN, K_I16, 100, 1U},
    {"temp-range-max", CONFIG_PARAM_TEMP_RANGE_MAX, K_I16, 100, 1U},
    {"humidity-range-min", CONFIG_PARAM_HUMIDITY_RANGE_MIN, K_U16, 100, 1U},
    {"humidity-range-max", CONFIG_PARAM_HUMIDITY_RANGE_MAX, K_U16, 100, 1U},
    {"pressure-range-min", CONFIG_PARAM_PRESSURE_RANGE_MIN, K_U16, 10, 1U},
    {"pressure-range-max", CONFIG_PARAM_PRESSURE_RANGE_MAX, K_U16, 10, 1U},
};
#define CFG_KEY_COUNT ((uint32_t) (sizeof(s_cfg_keys) / sizeof(s_cfg_keys[0])))

typedef struct
{
    bool dirty;
    uint32_t mask;                /* bit i: s_cfg_keys[i] is staged */
    int32_t value[CFG_KEY_COUNT]; /* fixed-point, in the key's scale */
} cfg_pending_t;

typedef struct
{
    bool dirty;
    bool has_endpoint;
    char mqtt_endpoint[BROKER_MAX_LEN + 1U];
    bool has_addr;
    uint8_t modbus_addr;
} prov_pending_t;

static const console_deps_t *s_deps;
static cfg_pending_t s_cfg_pending;
static prov_pending_t s_prov_pending;
static selftest_result_t s_selftest;
static confirm_kind_t s_confirm;
static uint32_t s_confirm_ticks;
static bool s_persist_due;
static bool s_reboot_requested;
static const char *s_fw_check_result; /* NULL until a check ran */

/* ---------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ---------------------------------------------------------------------- */

static char prv_lower(char c)
{
    return ((c >= 'A') && (c <= 'Z')) ? (char) (c - 'A' + 'a') : c;
}

static bool prv_ieq(const char *a, const char *b)
{
    while ((*a != '\0') && (*b != '\0'))
    {
        if (prv_lower(*a) != prv_lower(*b))
        {
            return false;
        }
        a++;
        b++;
    }
    return (*a == '\0') && (*b == '\0');
}

/* Channel letter or name -> chart channel; -1 if unknown. */
static int prv_channel_of(const char *s)
{
    if (prv_ieq(s, "t") || prv_ieq(s, "temp"))
    {
        return 0;
    }
    if (prv_ieq(s, "h") || prv_ieq(s, "humid"))
    {
        return 1;
    }
    if (prv_ieq(s, "p") || prv_ieq(s, "press"))
    {
        return 2;
    }
    return -1;
}

static void prv_value_text(char *buf, size_t size, uint8_t ch, int32_t v)
{
    const channel_info_t *info = model_channel(ch);
    char num[16];

    (void) fmt_fixed(num, sizeof(num), v, info->scale, info->decimals);
    (void) snprintf(buf, size, "%s %s", num, info->unit);
}

static const char *prv_chip(chart_status_t st)
{
    switch (st)
    {
    case CHART_ST_STALE:
        return "▲ stale";
    case CHART_ST_ALARM:
        return "■ alarm";
    case CHART_ST_NEAR:
        return "▲ near";
    default:
        return "● ok";
    }
}

static uint8_t prv_chip_colour(chart_status_t st)
{
    switch (st)
    {
    case CHART_ST_ALARM:
        return PAL_ERR;
    case CHART_ST_NEAR:
    case CHART_ST_STALE:
        return PAL_WARN;
    default:
        return PAL_OK;
    }
}

/* Status of chart channel @p ch from the current model. */
static chart_status_t prv_status_of(const console_model_t *m, uint8_t ch, int32_t *value_out)
{
    int32_t v = 0;
    const bool valid = model_value(m, ch, &v);
    const chart_limits_t lim = model_limits(m, ch);

    *value_out = v;
    return chart_status(valid ? chart_proximity_q8(v, &lim) : 0, !valid);
}

static console_service_err_t prv_fail(console_service_err_t err, const char *msg)
{
    SAY(PAL_ERR, "[ERR] %s", msg);
    if (err == CONSOLE_SERVICE_ERR_VALIDATION)
    {
        LOG_DEBUG(MODULE_TAG, "validation: %s", msg);
    }
    else
    {
        LOG_WARN(MODULE_TAG, "command failed: %s", msg);
    }
    return err;
}

static console_service_err_t prv_not_available(const char *what)
{
    SAY(PAL_ERR, "[ERR] not available: %s", what);
    LOG_WARN(MODULE_TAG, "not available: %s", what);
    return CONSOLE_SERVICE_ERR_APPLY_FAILED;
}

/* ---------------------------------------------------------------------- */
/* Shared diagnostic commands                                              */
/* ---------------------------------------------------------------------- */

static console_service_err_t cmd_help(int argc, const char *argv[]);

static console_service_err_t cmd_version(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    SAY(PAL_INK, "  ENVMON Environmental Gateway");
    SAY(PAL_MUTED, "  fw %s · %s · %s", FW_VERSION_STRING, FW_BUILD_DATE, MCU_PART);
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_serial(int argc, const char *argv[])
{
    uint32_t uid[3];

    (void) argc;
    (void) argv;
    console_port_uid(uid);
    SAY(PAL_NUM, "  %08lX%08lX%08lX", (unsigned long) uid[0], (unsigned long) uid[1],
        (unsigned long) uid[2]);
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_sensors(int argc, const char *argv[])
{
    console_model_t m;

    (void) argc;
    (void) argv;
    model_refresh(s_deps, &m);
    SAY(PAL_MUTED, "  SENSOR    VALUE       STATUS     UPDATED");
    for (uint8_t ch = 0U; ch < CHART_CHANNELS; ch++)
    {
        int32_t v;
        char value[24];
        char name[16];
        char chip[16];
        char when[12] = "--:--:--";
        const chart_status_t st = prv_status_of(&m, ch, &v);

        if (st == CHART_ST_STALE)
        {
            (void) snprintf(value, sizeof(value), "—");
        }
        else
        {
            prv_value_text(value, sizeof(value), ch, v);
        }
        model_time_of(&m.snap.readings[model_channel(ch)->id], when, sizeof(when));
        (void) fmt_pad(name, sizeof(name), model_channel(ch)->name, 10U);
        (void) fmt_pad(chip, sizeof(chip), prv_chip(st), 10U);
        char vpad[24];
        (void) fmt_pad(vpad, sizeof(vpad), value, 12U);
        SAY(prv_chip_colour(st), "  %s%s%s%s", name, vpad, chip, when);
    }
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_read(int argc, const char *argv[])
{
    console_model_t m;
    int32_t v;
    char value[24];

    (void) argc;
    const int ch = prv_channel_of(argv[0]);

    if (ch < 0)
    {
        return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION, "usage: read <t|h|p>");
    }
    model_refresh(s_deps, &m);
    const chart_status_t st = prv_status_of(&m, (uint8_t) ch, &v);

    if (st == CHART_ST_STALE)
    {
        (void) snprintf(value, sizeof(value), "—");
    }
    else
    {
        prv_value_text(value, sizeof(value), (uint8_t) ch, v);
    }
    SAY(prv_chip_colour(st), "  %s  %s  %s", model_channel((uint8_t) ch)->label, value,
        prv_chip(st));
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_status(int argc, const char *argv[])
{
    console_model_t m;
    char up[24];
    uint8_t valid = 0U;

    (void) argc;
    (void) argv;
    model_refresh(s_deps, &m);
    model_uptime_long(&m, up, sizeof(up));
    for (uint32_t i = 0U; i < (uint32_t) SENSOR_ID_COUNT; i++)
    {
        if (m.sensors_ok && m.snap.readings[i].valid)
        {
            valid++;
        }
    }
    const bool wifi = m.health.wifi_rssi_dbm < 0;

    SAY(wifi ? PAL_OK : PAL_ERR, "  WiFi      %s  RSSI %ld dBm", wifi ? "● connected" : "● down",
        (long) m.health.wifi_rssi_dbm);
    SAY(m.health.cloud_connected ? PAL_OK : PAL_ERR, "  MQTT      %s  %lu sent / %lu failed",
        m.health.cloud_connected ? "● connected" : "● down",
        (unsigned long) m.health.mqtt_publishes_sent,
        (unsigned long) m.health.mqtt_publishes_failed);
    SAY(m.health.modbus_link_online ? PAL_OK : PAL_WARN, "  Modbus    %s  %lu ok / %lu timeouts",
        m.health.modbus_link_online ? "● online" : "● offline",
        (unsigned long) m.health.modbus_transactions_ok, (unsigned long) m.health.modbus_timeouts);
    SAY(PAL_INK, "  Sensors   %u / %u valid", (unsigned) valid, (unsigned) SENSOR_ID_COUNT);
    SAY(PAL_INK, "  Uptime    %s", up);
    SAY(PAL_INK, "  Time      %s",
        (m.health.time_sync_state == TIME_SYNC_SYNCHRONISED) ? "synchronised" : "unsynchronised");
    SAY(PAL_INK, "  Buffer    %lu entries", (unsigned long) m.health.buffer_entry_count);
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_alarms(int argc, const char *argv[])
{
    console_model_t m;
    bool any = false;

    (void) argc;
    (void) argv;
    model_refresh(s_deps, &m);
    for (uint8_t ch = 0U; ch < CHART_CHANNELS; ch++)
    {
        const alarm_state_t st = m.alarm_state[model_channel(ch)->id];

        if (st == ALARM_STATE_CLEAR)
        {
            continue;
        }
        const chart_limits_t lim = model_limits(&m, ch);
        int32_t v = 0;
        char vtxt[24];
        char ttxt[24];

        (void) model_value(&m, ch, &v);
        prv_value_text(vtxt, sizeof(vtxt), ch, v);
        prv_value_text(ttxt, sizeof(ttxt), ch, (st == ALARM_STATE_ACTIVE_HIGH) ? lim.hi : lim.lo);
        SAY(PAL_ERR, "  %-12s %-5s %s / %s", model_channel(ch)->label,
            (st == ALARM_STATE_ACTIVE_HIGH) ? "HIGH" : "LOW", vtxt, ttxt);
        any = true;
    }
    if (!any)
    {
        SAY(PAL_OK, "  No active alarms — all channels within thresholds.");
    }
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_thresholds(int argc, const char *argv[])
{
    console_model_t m;

    (void) argc;
    (void) argv;
    model_refresh(s_deps, &m);
    SAY(PAL_MUTED, "  CH     HIGH          LOW");
    for (uint8_t ch = 0U; ch < CHART_CHANNELS; ch++)
    {
        const chart_limits_t lim = model_limits(&m, ch);
        char hi[24];
        char lo[24];
        char name[12];
        char hipad[24];

        prv_value_text(hi, sizeof(hi), ch, lim.hi);
        prv_value_text(lo, sizeof(lo), ch, lim.lo);
        (void) fmt_pad(name, sizeof(name), model_channel(ch)->name, 7U);
        (void) fmt_pad(hipad, sizeof(hipad), hi, 14U);
        SAY(PAL_INK, "  %s%s%s", name, hipad, lo);
    }
    return CONSOLE_SERVICE_ERR_OK;
}

/* set <ch>.<hi|lo> <value> */
static console_service_err_t cmd_set(int argc, const char *argv[])
{
    static const config_param_id_t ids[3][2] = {
        {CONFIG_PARAM_TEMP_ALARM_HIGH, CONFIG_PARAM_TEMP_ALARM_LOW},
        {CONFIG_PARAM_HUMIDITY_ALARM_HIGH, CONFIG_PARAM_HUMIDITY_ALARM_LOW},
        {CONFIG_PARAM_PRESSURE_ALARM_HIGH, CONFIG_PARAM_PRESSURE_ALARM_LOW},
    };
    char key[CHANNEL_KEY_MAX + 4U];
    int32_t fixed;

    (void) argc;
    (void) snprintf(key, sizeof(key), "%s", argv[0]);
    char *dot = strchr(key, '.');

    if (dot == NULL)
    {
        return prv_fail(CONSOLE_SERVICE_ERR_UNKNOWN_KEY, "usage: set <t|h|p>.<hi|lo> <value>");
    }
    *dot = '\0';
    const int ch = prv_channel_of(key);
    const bool hi = prv_ieq(dot + 1, "hi");

    if ((ch < 0) || (!hi && !prv_ieq(dot + 1, "lo")))
    {
        return prv_fail(CONSOLE_SERVICE_ERR_UNKNOWN_KEY, "usage: set <t|h|p>.<hi|lo> <value>");
    }
    const channel_info_t *info = model_channel((uint8_t) ch);

    if (!fmt_parse_fixed(argv[1], info->scale, &fixed))
    {
        return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION,
                        "value is not a number (e.g. set t.hi 24.5)");
    }

    const config_param_id_t id = ids[ch][hi ? 0 : 1];
    config_service_err_t rc;

    if (ch == 0)
    {
        if ((fixed < INT16_MIN) || (fixed > INT16_MAX))
        {
            return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION, "value out of range");
        }
        const int16_t v = (int16_t) fixed;

        rc = s_deps->cfg_write->set_param_deferred(s_deps->cfg_write->ctx, id, &v);
    }
    else
    {
        if ((fixed < 0) || (fixed > UINT16_MAX))
        {
            return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION, "value out of range");
        }
        const uint16_t v = (uint16_t) fixed;

        rc = s_deps->cfg_write->set_param_deferred(s_deps->cfg_write->ctx, id, &v);
    }
    if (rc == CONFIG_SERVICE_ERR_INVALID)
    {
        return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION,
                        "rejected: out of range or inconsistent with the other limits");
    }
    if (rc != CONFIG_SERVICE_OK)
    {
        return prv_fail(CONSOLE_SERVICE_ERR_APPLY_FAILED,
                        "configuration service refused the change");
    }
    s_persist_due = true;
    char vtxt[24];

    prv_value_text(vtxt, sizeof(vtxt), (uint8_t) ch, fixed);
    SAY(PAL_OK, "  %s.%s → %s   applied", info->name, hi ? "hi" : "lo", vtxt);
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_uptime(int argc, const char *argv[])
{
    console_model_t m;
    char up[24];

    (void) argc;
    (void) argv;
    model_refresh(s_deps, &m);
    model_uptime_long(&m, up, sizeof(up));
    SAY(PAL_INK, "  uptime %s", up);
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_clear(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    repl_clear();
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_reboot(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    SAY(PAL_WARN, "  Rebooting …");
    s_reboot_requested = true;
    return CONSOLE_SERVICE_ERR_OK;
}

/* ---------------------------------------------------------------------- */
/* Network and Gateway-specific status                                     */
/* ---------------------------------------------------------------------- */

static console_service_err_t cmd_wifi_status(int argc, const char *argv[])
{
    console_model_t m;

    (void) argc;
    (void) argv;
    model_refresh(s_deps, &m);
    const bool up = m.health.wifi_rssi_dbm < 0;

    SAY(up ? PAL_OK : PAL_ERR, "  wifi      %s", up ? "● connected" : "● down");
    SAY(PAL_INK, "  rssi      %ld dBm", (long) m.health.wifi_rssi_dbm);
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_mqtt_status(int argc, const char *argv[])
{
    console_model_t m;

    (void) argc;
    (void) argv;
    model_refresh(s_deps, &m);
    SAY(m.health.cloud_connected ? PAL_OK : PAL_ERR, "  mqtt      %s",
        m.health.cloud_connected ? "● connected" : "● down");
    if (m.cfg != NULL)
    {
        SAY(PAL_INK, "  broker    %s:%u",
            (m.cfg->mqtt_broker[0] != '\0') ? m.cfg->mqtt_broker : "(not set)",
            (unsigned) m.cfg->mqtt_port);
    }
    SAY(PAL_INK, "  publishes %lu sent / %lu failed", (unsigned long) m.health.mqtt_publishes_sent,
        (unsigned long) m.health.mqtt_publishes_failed);
    SAY(PAL_INK, "  reconnect %lu", (unsigned long) m.health.mqtt_reconnect_count);
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_modbus_status(int argc, const char *argv[])
{
    console_model_t m;

    (void) argc;
    (void) argv;
    model_refresh(s_deps, &m);
    SAY(m.health.modbus_link_online ? PAL_OK : PAL_WARN, "  modbus    %s",
        m.health.modbus_link_online ? "● online" : "● offline");
    SAY(PAL_INK, "  ok        %lu transactions", (unsigned long) m.health.modbus_transactions_ok);
    SAY(PAL_INK, "  timeouts  %lu", (unsigned long) m.health.modbus_timeouts);
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_net(int argc, const char *argv[])
{
    (void) cmd_wifi_status(argc, argv);
    (void) cmd_mqtt_status(argc, argv);
    return cmd_modbus_status(argc, argv);
}

/* ---------------------------------------------------------------------- */
/* Self-test                                                               */
/* ---------------------------------------------------------------------- */

static console_service_err_t cmd_selftest(int argc, const char *argv[])
{
    console_model_t m;

    (void) argc;
    (void) argv;
    model_refresh(s_deps, &m);
    s_selftest.valid = true;
    s_selftest.sensor_pass = s_deps->sensors->is_ready();
    s_selftest.comms_pass =
        (m.health.wifi_rssi_dbm < 0) && m.health.cloud_connected && m.health.modbus_link_online;
    s_selftest.flash_checked = false; /* no round-trip API on the Gateway (CS-O5) */
    s_selftest.flash_pass = false;
    s_selftest.overall_pass = s_selftest.sensor_pass && s_selftest.comms_pass;
    s_selftest.uptime_s = m.health.uptime_s;

    SAY(PAL_MUTED, "  SELF-TEST");
    SAY(s_selftest.sensor_pass ? PAL_OK : PAL_ERR, "  sensors   %s",
        s_selftest.sensor_pass ? "PASS" : "FAIL");
    SAY(s_selftest.comms_pass ? PAL_OK : PAL_ERR, "  comms     %s  (wifi, mqtt, modbus)",
        s_selftest.comms_pass ? "PASS" : "FAIL");
    SAY(PAL_WARN, "  flash     n/a   (no round-trip test available)");
    SAY(s_selftest.overall_pass ? PAL_OK : PAL_ERR, "  overall   %s  (not persisted)",
        s_selftest.overall_pass ? "PASS" : "FAIL");
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_selftest_result(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    if (!s_selftest.valid)
    {
        SAY(PAL_WARN, "  no self-test result since boot (results are not persisted)");
        return CONSOLE_SERVICE_ERR_OK;
    }
    SAY(s_selftest.overall_pass ? PAL_OK : PAL_ERR, "  last self-test: %s at uptime %lu s",
        s_selftest.overall_pass ? "PASS" : "FAIL", (unsigned long) s_selftest.uptime_s);
    SAY(PAL_INK, "  sensors %s · comms %s · flash n/a", s_selftest.sensor_pass ? "PASS" : "FAIL",
        s_selftest.comms_pass ? "PASS" : "FAIL");
    return CONSOLE_SERVICE_ERR_OK;
}

/* ---------------------------------------------------------------------- */
/* config list / set / commit / discard                                    */
/* ---------------------------------------------------------------------- */

static int prv_cfg_key_index(const char *key)
{
    for (uint32_t i = 0U; i < CFG_KEY_COUNT; i++)
    {
        if (prv_ieq(key, s_cfg_keys[i].key))
        {
            return (int) i;
        }
    }
    return -1;
}

static int32_t prv_cfg_current(const config_params_t *p, config_param_id_t id)
{
    switch (id)
    {
    case CONFIG_PARAM_POLL_INTERVAL:
        return (int32_t) p->polling_interval_ms;
    case CONFIG_PARAM_FILTER_ALPHA:
        return (int32_t) p->filter_alpha;
    case CONFIG_PARAM_MODBUS_POLL_PERIOD:
        return (int32_t) p->modbus_poll_period_ms;
    case CONFIG_PARAM_TELEMETRY_INTERVAL:
        return (int32_t) p->telemetry_interval_ms;
    case CONFIG_PARAM_HEALTH_INTERVAL:
        return (int32_t) p->health_interval_ms;
    case CONFIG_PARAM_MQTT_PORT:
        return (int32_t) p->mqtt_port;
    case CONFIG_PARAM_TEMP_RANGE_MIN:
        return p->temp_range_min;
    case CONFIG_PARAM_TEMP_RANGE_MAX:
        return p->temp_range_max;
    case CONFIG_PARAM_HUMIDITY_RANGE_MIN:
        return p->humidity_range_min;
    case CONFIG_PARAM_HUMIDITY_RANGE_MAX:
        return p->humidity_range_max;
    case CONFIG_PARAM_PRESSURE_RANGE_MIN:
        return p->pressure_range_min;
    default:
        return p->pressure_range_max;
    }
}

static console_service_err_t cmd_config_list(int argc, const char *argv[])
{
    console_model_t m;

    (void) argc;
    (void) argv;
    model_refresh(s_deps, &m);
    if (m.cfg == NULL)
    {
        return prv_fail(CONSOLE_SERVICE_ERR_NOT_INIT, "configuration service not available");
    }
    for (uint32_t i = 0U; i < CFG_KEY_COUNT; i++)
    {
        char num[16];
        char key[28];
        const bool staged = (s_cfg_pending.mask & (1UL << i)) != 0UL;

        (void) fmt_fixed(num, sizeof(num), prv_cfg_current(m.cfg, s_cfg_keys[i].id),
                         s_cfg_keys[i].scale, s_cfg_keys[i].decimals);
        (void) fmt_pad(key, sizeof(key), s_cfg_keys[i].key, 24U);
        SAY(staged ? PAL_WARN : PAL_INK, "  %s%s%s", key, num, staged ? "  (staged)" : "");
    }
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_config_set(int argc, const char *argv[])
{
    int32_t fixed;

    (void) argc;
    const int idx = prv_cfg_key_index(argv[0]);

    if (idx < 0)
    {
        return prv_fail(CONSOLE_SERVICE_ERR_UNKNOWN_KEY, "unknown parameter (see `config list`)");
    }
    const cfg_key_t *k = &s_cfg_keys[idx];

    if (!fmt_parse_fixed(argv[1], k->scale, &fixed))
    {
        return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION, "value is not a number");
    }
    /* Ask ConfigService now so a bad value is refused before it is staged. */
    config_service_err_t rc;

    if (k->kind == K_U32)
    {
        const uint32_t v = (uint32_t) fixed;

        rc = (fixed < 0) ? CONFIG_SERVICE_ERR_INVALID
                         : s_deps->cfg_write->validate_param(s_deps->cfg_write->ctx, k->id, &v);
    }
    else if (k->kind == K_U16)
    {
        const uint16_t v = (uint16_t) fixed;

        rc = ((fixed < 0) || (fixed > UINT16_MAX))
                 ? CONFIG_SERVICE_ERR_INVALID
                 : s_deps->cfg_write->validate_param(s_deps->cfg_write->ctx, k->id, &v);
    }
    else
    {
        const int16_t v = (int16_t) fixed;

        rc = ((fixed < INT16_MIN) || (fixed > INT16_MAX))
                 ? CONFIG_SERVICE_ERR_INVALID
                 : s_deps->cfg_write->validate_param(s_deps->cfg_write->ctx, k->id, &v);
    }
    if (rc != CONFIG_SERVICE_OK)
    {
        return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION, "value out of range or inconsistent");
    }
    s_cfg_pending.value[idx] = fixed;
    s_cfg_pending.mask |= (1UL << (uint32_t) idx);
    s_cfg_pending.dirty = true;
    SAY(PAL_OK, "  staged %s (run `config commit`)", k->key);
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t prv_apply_cfg_value(const cfg_key_t *k, int32_t fixed)
{
    config_service_err_t rc;

    if (k->kind == K_U32)
    {
        const uint32_t v = (uint32_t) fixed;

        rc = s_deps->cfg_write->set_param_deferred(s_deps->cfg_write->ctx, k->id, &v);
    }
    else if (k->kind == K_U16)
    {
        const uint16_t v = (uint16_t) fixed;

        rc = s_deps->cfg_write->set_param_deferred(s_deps->cfg_write->ctx, k->id, &v);
    }
    else
    {
        const int16_t v = (int16_t) fixed;

        rc = s_deps->cfg_write->set_param_deferred(s_deps->cfg_write->ctx, k->id, &v);
    }
    return (rc == CONFIG_SERVICE_OK) ? CONSOLE_SERVICE_ERR_OK : CONSOLE_SERVICE_ERR_APPLY_FAILED;
}

static console_service_err_t prv_cfg_apply_all(void)
{
    for (uint32_t i = 0U; i < CFG_KEY_COUNT; i++)
    {
        if ((s_cfg_pending.mask & (1UL << i)) == 0UL)
        {
            continue;
        }
        if (prv_apply_cfg_value(&s_cfg_keys[i], s_cfg_pending.value[i]) != CONSOLE_SERVICE_ERR_OK)
        {
            return prv_fail(CONSOLE_SERVICE_ERR_APPLY_FAILED,
                            "a change was refused; the rest stays staged");
        }
        s_cfg_pending.mask &= ~(1UL << i); /* applied: no longer staged */
    }
    s_cfg_pending.dirty = false;
    s_persist_due = true;
    SAY(PAL_OK, "[OK] configuration applied");
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_config_commit(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    if (!s_cfg_pending.dirty)
    {
        SAY(PAL_MUTED, "[INFO] Nothing staged");
        return CONSOLE_SERVICE_ERR_OK;
    }
    SAY(PAL_MUTED, "  staged changes:");
    for (uint32_t i = 0U; i < CFG_KEY_COUNT; i++)
    {
        if ((s_cfg_pending.mask & (1UL << i)) != 0UL)
        {
            char num[16];

            (void) fmt_fixed(num, sizeof(num), s_cfg_pending.value[i], s_cfg_keys[i].scale,
                             s_cfg_keys[i].decimals);
            SAY(PAL_INK, "    %s = %s", s_cfg_keys[i].key, num);
        }
    }
    s_confirm = CONFIRM_CFG;
    s_confirm_ticks = CONFIRM_TIMEOUT_TICKS;
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_config_discard(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    (void) memset(&s_cfg_pending, 0, sizeof(s_cfg_pending));
    SAY(PAL_MUTED, "[INFO] Discarded.");
    return CONSOLE_SERVICE_ERR_OK;
}

/* ---------------------------------------------------------------------- */
/* prov set / commit / discard                                             */
/* ---------------------------------------------------------------------- */

static bool prv_printable(const char *s, size_t min_len, size_t max_len)
{
    const size_t n = strlen(s);

    if ((n < min_len) || (n > max_len))
    {
        return false;
    }
    for (size_t i = 0U; i < n; i++)
    {
        if (((uint8_t) s[i] < 0x20U) || ((uint8_t) s[i] > 0x7EU))
        {
            return false;
        }
    }
    return true;
}

/* A hostname or dotted IPv4: letters, digits, '.', '-' ; not empty and not starting with '.' / '-'.
 */
static bool prv_valid_endpoint(const char *s)
{
    const size_t n = strlen(s);

    if ((n == 0U) || (n > BROKER_MAX_LEN) || (s[0] == '.') || (s[0] == '-'))
    {
        return false;
    }
    for (size_t i = 0U; i < n; i++)
    {
        const char c = s[i];
        const bool ok = ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
                        ((c >= '0') && (c <= '9')) || (c == '.') || (c == '-');

        if (!ok)
        {
            return false;
        }
    }
    return true;
}

static console_service_err_t cmd_prov_set(int argc, const char *argv[])
{
    uint32_t n;

    (void) argc;
    const char *key = argv[0];
    const char *val = argv[1];

    if (prv_ieq(key, "wifi-ssid"))
    {
        if (!prv_printable(val, 1U, SSID_MAX_LEN))
        {
            return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION,
                            "wifi-ssid must be 1..63 printable characters");
        }
        return prv_not_available("WiFi credential store is not implemented");
    }
    if (prv_ieq(key, "wifi-pass"))
    {
        if (!prv_printable(val, PASS_MIN_LEN, PASS_MAX_LEN))
        {
            return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION,
                            "wifi-pass must be 8..63 printable characters");
        }
        return prv_not_available("WiFi credential store is not implemented");
    }
    if (prv_ieq(key, "mqtt-endpoint"))
    {
        if (!prv_valid_endpoint(val))
        {
            return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION,
                            "mqtt-endpoint must be a hostname or IPv4, up to 127 characters");
        }
        (void) snprintf(s_prov_pending.mqtt_endpoint, sizeof(s_prov_pending.mqtt_endpoint), "%s",
                        val);
        s_prov_pending.has_endpoint = true;
    }
    else if (prv_ieq(key, "modbus-addr"))
    {
        if (!fmt_parse_u32(val, &n) || (n < MODBUS_ADDR_MIN) || (n > MODBUS_ADDR_MAX))
        {
            return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION, "modbus-addr must be 1..247");
        }
        s_prov_pending.modbus_addr = (uint8_t) n;
        s_prov_pending.has_addr = true;
    }
    else if (prv_ieq(key, "modbus-baud"))
    {
        if (!fmt_parse_u32(val, &n) ||
            ((n != 1200U) && (n != 2400U) && (n != 4800U) && (n != 9600U) && (n != 19200U)))
        {
            return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION,
                            "modbus-baud must be 1200, 2400, 4800, 9600 or 19200");
        }
        return prv_not_available("Modbus line settings are not configurable yet");
    }
    else if (prv_ieq(key, "modbus-parity"))
    {
        if ((strlen(val) != 1U) || ((val[0] != 'N') && (val[0] != 'E') && (val[0] != 'O')))
        {
            return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION, "modbus-parity must be N, E or O");
        }
        return prv_not_available("Modbus line settings are not configurable yet");
    }
    else
    {
        return prv_fail(CONSOLE_SERVICE_ERR_UNKNOWN_KEY, "unknown provisioning key");
    }
    s_prov_pending.dirty = true;
    SAY(PAL_OK, "  staged %s (run `prov commit`)", key);
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t prv_prov_apply(void)
{
    if (s_prov_pending.has_endpoint &&
        (s_deps->cfg_write->set_param_deferred(s_deps->cfg_write->ctx, CONFIG_PARAM_MQTT_BROKER,
                                               s_prov_pending.mqtt_endpoint) != CONFIG_SERVICE_OK))
    {
        return prv_fail(CONSOLE_SERVICE_ERR_APPLY_FAILED,
                        "the broker address was refused; changes stay staged");
    }
    if (s_prov_pending.has_addr)
    {
        const uint8_t addr = s_prov_pending.modbus_addr;

        if (s_deps->cfg_write->set_param_deferred(
                s_deps->cfg_write->ctx, CONFIG_PARAM_MODBUS_SLAVE_ADDR, &addr) != CONFIG_SERVICE_OK)
        {
            return prv_fail(CONSOLE_SERVICE_ERR_APPLY_FAILED,
                            "the Modbus address was refused; changes stay staged");
        }
    }
    (void) memset(&s_prov_pending, 0, sizeof(s_prov_pending));
    s_persist_due = true;
    SAY(PAL_OK, "[OK] provisioning applied");
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_prov_commit(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    if (!s_prov_pending.dirty)
    {
        SAY(PAL_MUTED, "[INFO] Nothing staged");
        return CONSOLE_SERVICE_ERR_OK;
    }
    SAY(PAL_MUTED, "  staged changes:");
    if (s_prov_pending.has_endpoint)
    {
        SAY(PAL_INK, "    mqtt-endpoint = %s", s_prov_pending.mqtt_endpoint);
    }
    if (s_prov_pending.has_addr)
    {
        SAY(PAL_INK, "    modbus-addr = %u", (unsigned) s_prov_pending.modbus_addr);
    }
    s_confirm = CONFIRM_PROV;
    s_confirm_ticks = CONFIRM_TIMEOUT_TICKS;
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t cmd_prov_discard(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    (void) memset(&s_prov_pending, 0, sizeof(s_prov_pending));
    SAY(PAL_MUTED, "[INFO] Discarded.");
    return CONSOLE_SERVICE_ERR_OK;
}

/* ---------------------------------------------------------------------- */
/* profiles                                                                */
/* ---------------------------------------------------------------------- */

static console_service_err_t cmd_profiles_list(int argc, const char *argv[])
{
    device_profile_t p;
    uint8_t index = 0U;
    uint8_t listed = 0U;

    (void) argc;
    (void) argv;
    while ((listed < PROFILES_MAX_LISTED) && s_deps->prof_read->get_next(&p, &index))
    {
        SAY(PAL_INK, "  slave %3u   map %u   vendor 0x%04X   %s", (unsigned) p.slave_addr,
            (unsigned) p.map_version, (unsigned) p.vendor_code, p.description);
        listed++;
    }
    if (listed == 0U)
    {
        SAY(PAL_MUTED, "  No profiles registered");
    }
    return CONSOLE_SERVICE_ERR_OK;
}

#define PROFILE_USAGE "usage: profiles add addr=N map=N vendor=N [desc=text]"
#define HEX_PREFIX_LEN (2U)
#define HEX_RADIX (16U)
#define DEC_RADIX (10U)
#define DEC_DIGIT_MAX (9U)

/** Decimal or 0x-prefixed hex, 1..65535; false otherwise. */
static bool prv_parse_u16(const char *text, uint16_t *out)
{
    uint32_t radix = DEC_RADIX;
    uint32_t v = 0U;
    uint8_t digits = 0U;

    if ((text[0] == '0') && ((text[1] == 'x') || (text[1] == 'X')))
    {
        radix = HEX_RADIX;
        text += HEX_PREFIX_LEN;
    }
    for (; *text != '\0'; text++)
    {
        uint32_t d;

        if ((*text >= '0') && (*text <= '9'))
        {
            d = (uint32_t) (*text - '0');
        }
        else if ((radix == HEX_RADIX) && (*text >= 'a') && (*text <= 'f'))
        {
            d = (uint32_t) (*text - 'a') + DEC_DIGIT_MAX + 1U;
        }
        else if ((radix == HEX_RADIX) && (*text >= 'A') && (*text <= 'F'))
        {
            d = (uint32_t) (*text - 'A') + DEC_DIGIT_MAX + 1U;
        }
        else
        {
            return false;
        }
        v = (v * radix) + d;
        digits++;
        if ((digits > 5U) || (v > 0xFFFFU))
        {
            return false;
        }
    }
    if ((digits == 0U) || (v == 0U))
    {
        return false;
    }
    *out = (uint16_t) v;
    return true;
}

/** Fill @p p from `key=value` arguments; the description is the text after desc= to the end. */
static bool prv_parse_profile(int argc, const char *argv[], device_profile_t *p)
{
    bool have_addr = false;
    bool have_map = false;
    bool have_vendor = false;
    bool in_desc = false;
    size_t used = 0U;
    uint32_t n;

    (void) memset(p, 0, sizeof(*p));
    for (int i = 0; i < argc; i++)
    {
        const char *a = argv[i];

        if (in_desc)
        {
            const size_t len = strlen(a);

            if ((used + 1U + len) >= DEVICE_PROFILE_DESC_MAX)
            {
                return false;
            }
            p->description[used] = ' ';
            (void) memcpy(&p->description[used + 1U], a, len);
            used += 1U + len;
        }
        else if (strncmp(a, "addr=", 5U) == 0)
        {
            have_addr =
                fmt_parse_u32(&a[5], &n) && (n >= MODBUS_ADDR_MIN) && (n <= MODBUS_ADDR_MAX);
            p->slave_addr = (uint8_t) n;
            if (!have_addr)
            {
                return false;
            }
        }
        else if (strncmp(a, "map=", 4U) == 0)
        {
            have_map = prv_parse_u16(&a[4], &p->map_version);
            if (!have_map)
            {
                return false;
            }
        }
        else if (strncmp(a, "vendor=", 7U) == 0)
        {
            have_vendor = prv_parse_u16(&a[7], &p->vendor_code);
            if (!have_vendor)
            {
                return false;
            }
        }
        else if (strncmp(a, "desc=", 5U) == 0)
        {
            const size_t len = strlen(&a[5]);

            if ((len == 0U) || (len >= DEVICE_PROFILE_DESC_MAX))
            {
                return false;
            }
            (void) memcpy(p->description, &a[5], len);
            used = len;
            in_desc = true;
        }
        else
        {
            return false;
        }
    }
    if (!(have_addr && have_map && have_vendor))
    {
        return false;
    }
    if (used == 0U)
    {
        (void) snprintf(p->description, sizeof(p->description), "Field Device %u",
                        (unsigned) p->slave_addr);
    }
    return true;
}

static console_service_err_t cmd_profiles_add(int argc, const char *argv[])
{
    device_profile_t p;

    if (!prv_parse_profile(argc, argv, &p))
    {
        return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION, PROFILE_USAGE);
    }
    if (s_deps->prof_write == NULL)
    {
        return prv_not_available("DeviceProfileRegistry is read-only here");
    }

    switch (s_deps->prof_write->add_or_update(&p))
    {
    case DEVICE_PROFILE_REGISTRY_OK:
        SAY(PAL_OK, "[OK] profile for slave %u saved", (unsigned) p.slave_addr);
        if (s_deps->on_profile_changed != NULL)
        {
            s_deps->on_profile_changed(p.slave_addr); /* the poller re-checks its identity */
        }
        return CONSOLE_SERVICE_ERR_OK;
    case DEVICE_PROFILE_REGISTRY_ERR_FULL:
        return prv_fail(CONSOLE_SERVICE_ERR_APPLY_FAILED, "registry full (8): remove one first");
    case DEVICE_PROFILE_REGISTRY_ERR_INVALID:
        return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION,
                        "profile refused: check addr, map, vendor, desc");
    case DEVICE_PROFILE_REGISTRY_ERR_PERSIST:
        return prv_fail(CONSOLE_SERVICE_ERR_APPLY_FAILED,
                        "not saved: flash write failed, nothing changed");
    default:
        return prv_fail(CONSOLE_SERVICE_ERR_APPLY_FAILED, "profile not saved");
    }
}

static console_service_err_t cmd_profiles_remove(int argc, const char *argv[])
{
    uint32_t n;

    (void) argc;
    if (!fmt_parse_u32(argv[0], &n) || (n < MODBUS_ADDR_MIN) || (n > MODBUS_ADDR_MAX))
    {
        return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION, "slave address must be 1..247");
    }
    if (s_deps->prof_write == NULL)
    {
        return prv_not_available("DeviceProfileRegistry is read-only here");
    }

    switch (s_deps->prof_write->remove((uint8_t) n))
    {
    case DEVICE_PROFILE_REGISTRY_OK:
        SAY(PAL_OK, "[OK] profile for slave %u removed", (unsigned) n);
        return CONSOLE_SERVICE_ERR_OK;
    case DEVICE_PROFILE_REGISTRY_ERR_NOT_FOUND:
        return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION, "no profile for that slave address");
    case DEVICE_PROFILE_REGISTRY_ERR_PERSIST:
        return prv_fail(CONSOLE_SERVICE_ERR_APPLY_FAILED,
                        "not removed: flash write failed, nothing changed");
    default:
        return prv_fail(CONSOLE_SERVICE_ERR_APPLY_FAILED, "profile not removed");
    }
}

/* ---------------------------------------------------------------------- */
/* Table                                                                   */
/* ---------------------------------------------------------------------- */

static const cmd_entry_t s_cmd_table[] = {
    {"help", "?", 0U, 0U, cmd_help, "list the commands"},
    {"version", "about", 0U, 0U, cmd_version, "firmware and build"},
    {"serial", NULL, 0U, 0U, cmd_serial, "MCU unique id"},
    {"sensors", "ls", 0U, 0U, cmd_sensors, "all channels"},
    {"read", NULL, 1U, 1U, cmd_read, "read <t|h|p>"},
    {"status", NULL, 0U, 0U, cmd_status, "system health"},
    {"alarms", NULL, 0U, 0U, cmd_alarms, "active alarms"},
    {"thresholds", NULL, 0U, 0U, cmd_thresholds, "alarm limits"},
    {"set", NULL, 2U, 2U, cmd_set, "set <t|h|p>.<hi|lo> <v>"},
    {"uptime", NULL, 0U, 0U, cmd_uptime, "device uptime"},
    {"selftest", NULL, 0U, 0U, cmd_selftest, "run the board self-test"},
    {"selftest-result", NULL, 0U, 0U, cmd_selftest_result, "last self-test"},
    {"config list", NULL, 0U, 0U, cmd_config_list, "operational parameters"},
    {"config set", NULL, 2U, 2U, cmd_config_set, "config set <key> <v>"},
    {"config commit", NULL, 0U, 0U, cmd_config_commit, "apply staged config"},
    {"config discard", NULL, 0U, 0U, cmd_config_discard, "drop staged config"},
    {"prov set", NULL, 2U, 2U, cmd_prov_set, "prov set <key> <v>"},
    {"prov commit", NULL, 0U, 0U, cmd_prov_commit, "apply staged provisioning"},
    {"prov discard", NULL, 0U, 0U, cmd_prov_discard, "drop staged provisioning"},
    {"profiles list", NULL, 0U, 0U, cmd_profiles_list, "device profiles"},
    {"profiles add", NULL, 3U, 8U, cmd_profiles_add, "profiles add addr=N map=V vendor=V"},
    {"profiles remove", NULL, 1U, 1U, cmd_profiles_remove, "profiles remove <addr>"},
    {"wifi status", NULL, 0U, 0U, cmd_wifi_status, "WiFi link"},
    {"mqtt status", NULL, 0U, 0U, cmd_mqtt_status, "MQTT link"},
    {"modbus status", NULL, 0U, 0U, cmd_modbus_status, "Modbus master"},
    {"net", "network", 0U, 0U, cmd_net, "WiFi + MQTT + Modbus"},
    {"clear", "cls", 0U, 0U, cmd_clear, "empty the console"},
    {"reboot", NULL, 0U, 0U, cmd_reboot, "restart the gateway"},
};
#define CMD_COUNT ((uint32_t) (sizeof(s_cmd_table) / sizeof(s_cmd_table[0])))

/* Extra aliases that do not fit the single alias field. */
typedef struct
{
    const char *alias;
    const char *token;
} alias_t;

static const alias_t s_aliases[] = {
    {"sensor", "sensors"},
};

static console_service_err_t cmd_help(int argc, const char *argv[])
{
    (void) argc;
    (void) argv;
    SAY(PAL_MUTED, "Available commands:");
    for (uint32_t i = 0U; i < CMD_COUNT; i++)
    {
        char tok[28];

        (void) fmt_pad(tok, sizeof(tok), s_cmd_table[i].token, 18U);
        SAY(PAL_INK, "  %s%s", tok, s_cmd_table[i].help);
    }
    return CONSOLE_SERVICE_ERR_OK;
}

const cmd_entry_t *commands_table(size_t *count)
{
    *count = CMD_COUNT;
    return s_cmd_table;
}

static bool prv_words_match(const char *token, int argc, const char *argv[], int *words)
{
    char copy[24];

    (void) snprintf(copy, sizeof(copy), "%s", token);
    char *space = strchr(copy, ' ');

    if (space != NULL)
    {
        *space = '\0';
        if ((argc >= 2) && prv_ieq(argv[0], copy) && prv_ieq(argv[1], space + 1))
        {
            *words = 2;
            return true;
        }
        return false;
    }
    if ((argc >= 1) && prv_ieq(argv[0], copy))
    {
        *words = 1;
        return true;
    }
    return false;
}

static const cmd_entry_t *prv_find(int argc, const char *argv[], int *words)
{
    /* Two-word commands first, so "config set" is not read as an unknown "config". */
    for (uint32_t i = 0U; i < CMD_COUNT; i++)
    {
        if ((strchr(s_cmd_table[i].token, ' ') != NULL) &&
            prv_words_match(s_cmd_table[i].token, argc, argv, words))
        {
            return &s_cmd_table[i];
        }
    }
    for (uint32_t i = 0U; i < CMD_COUNT; i++)
    {
        const cmd_entry_t *e = &s_cmd_table[i];

        if (strchr(e->token, ' ') != NULL)
        {
            continue;
        }
        if (prv_words_match(e->token, argc, argv, words) ||
            ((e->alias != NULL) && prv_ieq(argv[0], e->alias)))
        {
            *words = 1;
            return e;
        }
    }
    for (uint32_t i = 0U; i < (uint32_t) (sizeof(s_aliases) / sizeof(s_aliases[0])); i++)
    {
        if (prv_ieq(argv[0], s_aliases[i].alias))
        {
            for (uint32_t j = 0U; j < CMD_COUNT; j++)
            {
                if (prv_ieq(s_cmd_table[j].token, s_aliases[i].token))
                {
                    *words = 1;
                    return &s_cmd_table[j];
                }
            }
        }
    }
    return NULL;
}

console_service_err_t commands_dispatch(int argc, const char *argv[])
{
    int words = 1;
    const cmd_entry_t *e = prv_find(argc, argv, &words);

    if (e == NULL)
    {
        SAY(PAL_WARN, "  unknown command: %s — try \"help\"", argv[0]);
        LOG_WARN(MODULE_TAG, "unknown command: %s", argv[0]);
        return CONSOLE_SERVICE_ERR_UNKNOWN_KEY;
    }
    const int args = argc - words;

    if ((args < (int) e->min_argc) || (args > (int) e->max_argc))
    {
        return prv_fail(CONSOLE_SERVICE_ERR_VALIDATION, e->help);
    }
    return e->handler(args, &argv[words]);
}

/* ---------------------------------------------------------------------- */
/* Confirm workflow, flags, lifecycle                                      */
/* ---------------------------------------------------------------------- */

bool commands_confirm_active(void)
{
    return s_confirm != CONFIRM_NONE;
}

bool commands_confirm_is_reboot(void)
{
    return s_confirm == CONFIRM_REBOOT;
}

void commands_request_reboot_confirm(void)
{
    s_confirm = CONFIRM_REBOOT;
    s_confirm_ticks = CONFIRM_TIMEOUT_TICKS;
}

console_service_err_t commands_fw_check(void)
{
    s_fw_check_result = "not available: no update source yet";
    return prv_not_available("firmware update check (no update source on this Gateway yet)");
}

const char *commands_fw_check_result(void)
{
    return s_fw_check_result;
}

console_service_err_t commands_confirm_answer(bool yes)
{
    const confirm_kind_t kind = s_confirm;

    s_confirm = CONFIRM_NONE;
    if (kind == CONFIRM_NONE)
    {
        return CONSOLE_SERVICE_ERR_OK;
    }
    if (kind == CONFIRM_REBOOT)
    {
        if (yes)
        {
            SAY(PAL_WARN, "  Rebooting …");
            s_reboot_requested = true;
        }
        else
        {
            SAY(PAL_MUTED, "[INFO] Reboot cancelled.");
        }
        return CONSOLE_SERVICE_ERR_OK;
    }
    if (!yes)
    {
        if (kind == CONFIRM_CFG)
        {
            (void) memset(&s_cfg_pending, 0, sizeof(s_cfg_pending));
        }
        else
        {
            (void) memset(&s_prov_pending, 0, sizeof(s_prov_pending));
        }
        SAY(PAL_MUTED, "[INFO] Discarded.");
        return CONSOLE_SERVICE_ERR_OK;
    }
    return (kind == CONFIRM_CFG) ? prv_cfg_apply_all() : prv_prov_apply();
}

console_service_err_t commands_tick(void)
{
    if (s_confirm == CONFIRM_NONE)
    {
        return CONSOLE_SERVICE_ERR_OK;
    }
    if (s_confirm_ticks > 0U)
    {
        s_confirm_ticks--;
    }
    if (s_confirm_ticks > 0U)
    {
        return CONSOLE_SERVICE_ERR_OK;
    }
    const bool was_reboot = (s_confirm == CONFIRM_REBOOT);

    (void) commands_confirm_answer(false);
    SAY(PAL_ERR, was_reboot ? "[ERR] timed out: reboot cancelled"
                            : "[ERR] timed out: staged changes discarded");
    LOG_DEBUG(MODULE_TAG, "confirm prompt timed out");
    return CONSOLE_SERVICE_ERR_TIMEOUT;
}

bool commands_take_persist_due(void)
{
    const bool v = s_persist_due;

    s_persist_due = false;
    return v;
}

bool commands_take_reboot_request(void)
{
    const bool v = s_reboot_requested;

    s_reboot_requested = false;
    return v;
}

bool commands_prov_dirty(void)
{
    return s_prov_pending.dirty;
}

bool commands_cfg_dirty(void)
{
    return s_cfg_pending.dirty;
}

const selftest_result_t *commands_selftest_result(void)
{
    return &s_selftest;
}

void commands_init(const console_deps_t *deps)
{
    s_deps = deps;
    (void) memset(&s_cfg_pending, 0, sizeof(s_cfg_pending));
    (void) memset(&s_prov_pending, 0, sizeof(s_prov_pending));
    (void) memset(&s_selftest, 0, sizeof(s_selftest));
    s_confirm = CONFIRM_NONE;
    s_confirm_ticks = 0U;
    s_persist_due = false;
    s_reboot_requested = false;
    s_fw_check_result = NULL;
}
