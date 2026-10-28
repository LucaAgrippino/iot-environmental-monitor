/**
 * @file cloud_publisher_json.c
 * @brief JSON serialisation and parsing for CloudPublisher (Gateway), wire schema 1.1.
 *
 * Schemas per docs/lld/application/cloud-publisher-lld.md and
 * docs/lld/gate-review/cloud-publisher-reconciliation.md section 6 (CP-D22). Each serialiser
 * returns 0 on truncation (CP_ERR_SERIALISE at the call site) or the number of bytes written
 * (excluding the terminator). An invalid channel is JSON null; every payload carries the common
 * block (schema_version, device_id, timestamp, sync_state).
 */

#include "cloud_publisher_json.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "config_service/config_params.h"
#include "update_service/us_reason.h"

#define CP_OTA_CMD_TEXT_MAX 129u /**< 128 payload bytes plus the terminator. */
#define CP_SCHEMA_VERSION "1.1"
#define CP_MAX_DIGITS (15U) /**< Longest integer accepted in a /config command. */

/* ---------------------------------------------------------------------- */
/* Append helper                                                            */
/* ---------------------------------------------------------------------- */

/** Append formatted text at @p len. False if it does not fit; @p len then is not meaningful. */
static bool prv_append(char *buf, uint32_t size, uint32_t *len, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    const int n = vsnprintf(&buf[*len], size - *len, fmt, args);
    va_end(args);

    if ((n < 0) || ((uint32_t) n >= (size - *len)))
    {
        return false;
    }
    *len += (uint32_t) n;
    return true;
}

/** The common block, opening the object (REQ-TS-040: uptime time is flagged unsynchronised). */
static bool prv_common(char *buf, uint32_t size, uint32_t *len, const char *device_serial,
                       const time_provider_ts_t *ts)
{
    *len = 0U;
    return prv_append(buf, size, len,
                      "{\"schema_version\":\"" CP_SCHEMA_VERSION "\",\"device_id\":\"%s\","
                      "\"timestamp\":%lu,\"sync_state\":\"%s\"",
                      device_serial, (unsigned long) ts->epoch,
                      (ts->sync_state == TIME_SYNC_SYNCHRONISED) ? "synchronised"
                                                                 : "unsynchronised");
}

/** Close the object; returns the length or 0. */
static uint32_t prv_close(char *buf, uint32_t size, uint32_t len)
{
    if ((len + 2U) > size)
    {
        return 0U;
    }
    buf[len] = '}';
    buf[len + 1U] = '\0';
    return len + 1U;
}

/** ,"name":value or ,"name":null. */
static bool prv_append_opt(char *buf, uint32_t size, uint32_t *len, const char *name, bool valid,
                           int32_t value)
{
    return valid ? prv_append(buf, size, len, ",\"%s\":%ld", name, (long) value)
                 : prv_append(buf, size, len, ",\"%s\":null", name);
}

/* ---------------------------------------------------------------------- */
/* Units: register / SensorService fixed point to the wire (SS-O8)          */
/* ---------------------------------------------------------------------- */

/* Divide rounding half away from zero, so -0.05 degC and +0.05 degC round alike. */
static int32_t prv_div_round(int32_t value, int32_t divisor)
{
    const int32_t half = divisor / 2;

    return (value >= 0) ? ((value + half) / divisor) : -(((-value) + half) / divisor);
}

static int32_t prv_clamp(int32_t value, int32_t lo, int32_t hi)
{
    return (value < lo) ? lo : ((value > hi) ? hi : value);
}

/** Wire value of a centi-degC reading (deci-degC). */
static int32_t prv_wire_temp(int32_t centi_c)
{
    return prv_div_round(centi_c, 10);
}

/** Wire value of a centi-%RH reading (whole %RH, 0..100). */
static int32_t prv_wire_humidity(int32_t centi_pct)
{
    return prv_clamp(prv_div_round(centi_pct, 100), 0, 100);
}

/** Wire value of a deci-hPa reading (whole hPa). */
static int32_t prv_wire_pressure(int32_t deci_hpa)
{
    return prv_clamp(prv_div_round(deci_hpa, 10), 0, 65535);
}

/* ---------------------------------------------------------------------- */
/* Telemetry (SD-03a)                                                       */
/* ---------------------------------------------------------------------- */

static bool prv_append_field_device(char *buf, uint32_t size, uint32_t *len,
                                    const modbus_fd_reading_t *fd)
{
    if (fd == NULL)
    {
        return prv_append(buf, size, len, ",\"field_device\":null");
    }

    /* Online needs both a live link and at least one decoded response (REQ-SA-160). */
    const bool online = fd->link_online && fd->has_data;

    return prv_append(buf, size, len, ",\"field_device\":{\"online\":%s",
                      online ? "true" : "false") &&
           prv_append_opt(buf, size, len, "temperature_deci_c", online && fd->temperature_valid,
                          prv_wire_temp(fd->temperature_centi_c)) &&
           prv_append_opt(buf, size, len, "humidity_pct", online && fd->humidity_valid,
                          prv_wire_humidity((int32_t) fd->humidity_centi_pct)) &&
           prv_append_opt(buf, size, len, "pressure_hpa", online && fd->pressure_valid,
                          prv_wire_pressure((int32_t) fd->pressure_deci_hpa)) &&
           prv_append(buf, size, len, "}");
}

uint32_t cp_serialise_telemetry(char *buf, uint32_t buf_size, const sensor_snapshot_t *snap,
                                const modbus_fd_reading_t *fd, const time_provider_ts_t *ts,
                                const char *device_serial)
{
    uint32_t len = 0U;
    const sensor_reading_t *r = snap->readings;

    bool ok = prv_common(buf, buf_size, &len, device_serial, ts);

    ok = ok &&
         prv_append_opt(buf, buf_size, &len, "temperature_deci_c", r[SENSOR_ID_TEMPERATURE].valid,
                        prv_wire_temp(r[SENSOR_ID_TEMPERATURE].value));
    ok = ok && prv_append_opt(buf, buf_size, &len, "humidity_pct", r[SENSOR_ID_HUMIDITY].valid,
                              prv_wire_humidity(r[SENSOR_ID_HUMIDITY].value));
    ok = ok && prv_append_opt(buf, buf_size, &len, "pressure_hpa", r[SENSOR_ID_PRESSURE].valid,
                              prv_wire_pressure(r[SENSOR_ID_PRESSURE].value));
    ok = ok && prv_append_opt(buf, buf_size, &len, "accel_x_mg", r[SENSOR_ID_ACCEL_X].valid,
                              r[SENSOR_ID_ACCEL_X].value);
    ok = ok && prv_append_opt(buf, buf_size, &len, "accel_y_mg", r[SENSOR_ID_ACCEL_Y].valid,
                              r[SENSOR_ID_ACCEL_Y].value);
    ok = ok && prv_append_opt(buf, buf_size, &len, "accel_z_mg", r[SENSOR_ID_ACCEL_Z].valid,
                              r[SENSOR_ID_ACCEL_Z].value);
    ok = ok && prv_append_opt(buf, buf_size, &len, "gyro_x_mdps", r[SENSOR_ID_GYRO_X].valid,
                              r[SENSOR_ID_GYRO_X].value);
    ok = ok && prv_append_opt(buf, buf_size, &len, "gyro_y_mdps", r[SENSOR_ID_GYRO_Y].valid,
                              r[SENSOR_ID_GYRO_Y].value);
    ok = ok && prv_append_opt(buf, buf_size, &len, "gyro_z_mdps", r[SENSOR_ID_GYRO_Z].valid,
                              r[SENSOR_ID_GYRO_Z].value);
    ok = ok && prv_append_opt(buf, buf_size, &len, "mag_x_mgauss", r[SENSOR_ID_MAG_X].valid,
                              r[SENSOR_ID_MAG_X].value);
    ok = ok && prv_append_opt(buf, buf_size, &len, "mag_y_mgauss", r[SENSOR_ID_MAG_Y].valid,
                              r[SENSOR_ID_MAG_Y].value);
    ok = ok && prv_append_opt(buf, buf_size, &len, "mag_z_mgauss", r[SENSOR_ID_MAG_Z].valid,
                              r[SENSOR_ID_MAG_Z].value);
    ok = ok && prv_append_field_device(buf, buf_size, &len, fd);

    return ok ? prv_close(buf, buf_size, len) : 0U;
}

/* ---------------------------------------------------------------------- */
/* Health 1.1 (SD-03b, REQ-CC-010, REQ-NF-503)                              */
/* ---------------------------------------------------------------------- */

static const char *prv_lifecycle_str(lifecycle_state_t state)
{
    switch (state)
    {
    case LIFECYCLE_STATE_INIT:
        return "init";
    case LIFECYCLE_STATE_OPERATIONAL:
        return "operational";
    case LIFECYCLE_STATE_EDITING_CONFIG:
        return "editing_config";
    case LIFECYCLE_STATE_RESTARTING:
        return "restarting";
    case LIFECYCLE_STATE_UPDATING_FW:
        return "updating_firmware";
    case LIFECYCLE_STATE_FAULTED:
        return "faulted";
    default:
        return "unknown";
    }
}

static const char *prv_reset_cause_str(lifecycle_reset_cause_t cause)
{
    switch (cause)
    {
    case LIFECYCLE_RESET_POWER_ON:
        return "power_on";
    case LIFECYCLE_RESET_SOFT:
        return "soft";
    case LIFECYCLE_RESET_WATCHDOG:
        return "watchdog";
    default:
        return "unknown";
    }
}

/** Number of leading task slots to report: unregistered slots hold 0, so trailing zeros are cut. */
static uint32_t prv_stack_count(const health_snapshot_t *snap)
{
    uint32_t count = HEALTH_MONITOR_MAX_TASKS;

    while ((count > 0U) && (snap->stack_watermark_words[count - 1U] == 0U))
    {
        count--;
    }
    return count;
}

uint32_t cp_serialise_health(char *buf, uint32_t buf_size, const health_snapshot_t *snap,
                             uint32_t fw_version, const time_provider_ts_t *ts,
                             const char *device_serial)
{
    uint32_t len = 0U;

    bool ok = prv_common(buf, buf_size, &len, device_serial, ts);

    ok = ok && prv_append(buf, buf_size, &len,
                          ",\"fw_version\":\"%u.%u.%u\",\"uptime_s\":%lu,"
                          "\"lifecycle_state\":\"%s\",\"last_reset_cause\":\"%s\","
                          "\"sensor_fail_count\":%lu,\"alarm_raise_count\":%lu,"
                          "\"config_write_failed\":%s,",
                          (unsigned) (fw_version >> 24), (unsigned) ((fw_version >> 16) & 0xFFU),
                          (unsigned) (fw_version & 0xFFFFU), (unsigned long) snap->uptime_s,
                          prv_lifecycle_str(snap->lifecycle_state),
                          prv_reset_cause_str(snap->last_reset_cause),
                          (unsigned long) snap->sensor_fail_count,
                          (unsigned long) snap->alarm_raise_count,
                          snap->config_write_failed ? "true" : "false");
    ok =
        ok &&
        prv_append(
            buf, buf_size, &len,
            "\"modbus_transactions_ok\":%lu,\"modbus_timeouts\":%lu,"
            "\"modbus_link_online\":%s,\"mqtt_publishes_sent\":%lu,"
            "\"mqtt_publishes_failed\":%lu,\"mqtt_reconnect_count\":%lu,"
            "\"wifi_rssi_dbm\":%ld,\"cloud_connected\":%s,"
            "\"buffer_entry_count\":%lu,\"buffer_overflow_count\":%lu,"
            "\"ntp_sync_fail_count\":%lu,\"last_ntp_sync_epoch\":%lu,"
            "\"stack_free_words\":[",
            (unsigned long) snap->modbus_transactions_ok, (unsigned long) snap->modbus_timeouts,
            snap->modbus_link_online ? "true" : "false", (unsigned long) snap->mqtt_publishes_sent,
            (unsigned long) snap->mqtt_publishes_failed, (unsigned long) snap->mqtt_reconnect_count,
            (long) snap->wifi_rssi_dbm, snap->cloud_connected ? "true" : "false",
            (unsigned long) snap->buffer_entry_count, (unsigned long) snap->buffer_overflow_count,
            (unsigned long) snap->ntp_sync_fail_count, (unsigned long) snap->last_ntp_sync_epoch);

    const uint32_t tasks = prv_stack_count(snap);

    for (uint32_t i = 0U; ok && (i < tasks); i++)
    {
        ok = prv_append(buf, buf_size, &len, "%s%u", (i > 0U) ? "," : "",
                        (unsigned) snap->stack_watermark_words[i]);
    }
    ok = ok && prv_append(buf, buf_size, &len, "]");

    return ok ? prv_close(buf, buf_size, len) : 0U;
}

/* ---------------------------------------------------------------------- */
/* Alarm (SD-05)                                                            */
/* ---------------------------------------------------------------------- */

static const char *prv_alarm_type_str(cp_alarm_type_t type)
{
    switch (type)
    {
    case CP_ALARM_TYPE_HIGH:
        return "HIGH";
    case CP_ALARM_TYPE_LOW:
        return "LOW";
    case CP_ALARM_TYPE_CLEAR:
        return "CLEAR";
    default:
        return "UNKNOWN";
    }
}

static const char *prv_alarm_source_str(cp_alarm_source_t source)
{
    return (source == CP_ALARM_SOURCE_FIELD_DEVICE) ? "field_device" : "gateway";
}

uint32_t cp_serialise_alarm(char *buf, uint32_t buf_size, const cp_alarm_event_t *event,
                            const char *device_serial)
{
    uint32_t len = 0U;

    bool ok = prv_common(buf, buf_size, &len, device_serial, &event->timestamp);

    ok = ok && prv_append(buf, buf_size, &len,
                          ",\"sensor_id\":\"%s\",\"alarm_type\":\"%s\","
                          "\"measured_value_raw\":%ld,\"threshold_value_raw\":%ld,"
                          "\"source\":\"%s\"",
                          event->sensor_name, prv_alarm_type_str(event->alarm_type),
                          (long) event->measured_value_raw, (long) event->threshold_value_raw,
                          prv_alarm_source_str(event->source));

    return ok ? prv_close(buf, buf_size, len) : 0U;
}

/* ---------------------------------------------------------------------- */
/* Remote configuration (SD-07, CP-D21)                                     */
/* ---------------------------------------------------------------------- */

#define CP_KEY(name, id, field, sz, sgn, rem)                                                      \
    {                                                                                              \
        (name), (id), (uint16_t) offsetof(config_params_t, field), (sz), (sgn), (rem)              \
    }

typedef struct
{
    const char *name;
    config_param_id_t id;
    uint16_t offset;
    uint8_t size;
    bool is_signed;
    bool remote;
} cp_key_def_t;

/* The Tier 2 keys (Vision 10, REQ-NF-306) are remote; every other config_params_t field is known
 * and refused, so a typo is told apart from a deliberately local parameter. */
static const cp_key_def_t s_keys[] = {
    CP_KEY("polling_interval_ms", CONFIG_PARAM_POLL_INTERVAL, polling_interval_ms, 4U, false, true),
    CP_KEY("modbus_poll_period_ms", CONFIG_PARAM_MODBUS_POLL_PERIOD, modbus_poll_period_ms, 4U,
           false, true),
    CP_KEY("telemetry_interval_ms", CONFIG_PARAM_TELEMETRY_INTERVAL, telemetry_interval_ms, 4U,
           false, true),
    CP_KEY("health_interval_ms", CONFIG_PARAM_HEALTH_INTERVAL, health_interval_ms, 4U, false, true),
    CP_KEY("temp_alarm_high", CONFIG_PARAM_TEMP_ALARM_HIGH, temp_alarm_high, 2U, true, true),
    CP_KEY("temp_alarm_low", CONFIG_PARAM_TEMP_ALARM_LOW, temp_alarm_low, 2U, true, true),
    CP_KEY("temp_hysteresis", CONFIG_PARAM_TEMP_HYSTERESIS, temp_hysteresis, 2U, true, true),
    CP_KEY("humidity_alarm_high", CONFIG_PARAM_HUMIDITY_ALARM_HIGH, humidity_alarm_high, 2U, false,
           true),
    CP_KEY("humidity_alarm_low", CONFIG_PARAM_HUMIDITY_ALARM_LOW, humidity_alarm_low, 2U, false,
           true),
    CP_KEY("humidity_hysteresis", CONFIG_PARAM_HUMIDITY_HYSTERESIS, humidity_hysteresis, 2U, false,
           true),
    CP_KEY("pressure_alarm_high", CONFIG_PARAM_PRESSURE_ALARM_HIGH, pressure_alarm_high, 2U, false,
           true),
    CP_KEY("pressure_alarm_low", CONFIG_PARAM_PRESSURE_ALARM_LOW, pressure_alarm_low, 2U, false,
           true),
    CP_KEY("pressure_hysteresis", CONFIG_PARAM_PRESSURE_HYSTERESIS, pressure_hysteresis, 2U, false,
           true),
    /* Local only (not remote). */
    CP_KEY("filter_alpha", CONFIG_PARAM_FILTER_ALPHA, filter_alpha, 2U, false, false),
    CP_KEY("temp_range_min", CONFIG_PARAM_TEMP_RANGE_MIN, temp_range_min, 2U, true, false),
    CP_KEY("temp_range_max", CONFIG_PARAM_TEMP_RANGE_MAX, temp_range_max, 2U, true, false),
    CP_KEY("humidity_range_min", CONFIG_PARAM_HUMIDITY_RANGE_MIN, humidity_range_min, 2U, false,
           false),
    CP_KEY("humidity_range_max", CONFIG_PARAM_HUMIDITY_RANGE_MAX, humidity_range_max, 2U, false,
           false),
    CP_KEY("pressure_range_min", CONFIG_PARAM_PRESSURE_RANGE_MIN, pressure_range_min, 2U, false,
           false),
    CP_KEY("pressure_range_max", CONFIG_PARAM_PRESSURE_RANGE_MAX, pressure_range_max, 2U, false,
           false),
    CP_KEY("modbus_slave_addr", CONFIG_PARAM_MODBUS_SLAVE_ADDR, modbus_slave_addr, 2U, false,
           false),
    CP_KEY("mqtt_broker", CONFIG_PARAM_MQTT_BROKER, mqtt_broker, 2U, false, false),
    CP_KEY("mqtt_port", CONFIG_PARAM_MQTT_PORT, mqtt_port, 2U, false, false),
    CP_KEY("ntp_servers", CONFIG_PARAM_NTP_SERVERS, ntp_servers, 2U, false, false),
    CP_KEY("ntp_server_count", CONFIG_PARAM_NTP_SERVER_COUNT, ntp_server_count, 2U, false, false),
};

#define CP_KEY_COUNT (sizeof(s_keys) / sizeof(s_keys[0]))

static const cp_key_def_t *prv_find_key(const char *name)
{
    for (uint32_t i = 0U; i < (uint32_t) CP_KEY_COUNT; i++)
    {
        if (strcmp(s_keys[i].name, name) == 0)
        {
            return &s_keys[i];
        }
    }
    return NULL;
}

/** True if @p value fits the C type of the field. */
static bool prv_fits(const cp_key_def_t *key, int64_t value)
{
    if (key->size == 4U)
    {
        return (value >= 0) && (value <= (int64_t) 0xFFFFFFFFLL);
    }
    return key->is_signed ? ((value >= -32768) && (value <= 32767))
                          : ((value >= 0) && (value <= 65535));
}

static bool prv_is_space(uint8_t c)
{
    return (c == ' ') || (c == '\t') || (c == '\r') || (c == '\n');
}

static void prv_skip_space(const uint8_t *p, uint32_t len, uint32_t *i)
{
    while ((*i < len) && prv_is_space(p[*i]))
    {
        (*i)++;
    }
}

/** A quoted key without escapes, at most CP_CFG_KEY_LEN - 1 characters. */
static bool prv_scan_key(const uint8_t *p, uint32_t len, uint32_t *i, char *key)
{
    uint32_t n = 0U;

    if ((*i >= len) || (p[*i] != (uint8_t) '"'))
    {
        return false;
    }
    (*i)++;

    while ((*i < len) && (p[*i] != (uint8_t) '"'))
    {
        if ((p[*i] < 0x20U) || (p[*i] == (uint8_t) '\\') || (n >= (CP_CFG_KEY_LEN - 1U)))
        {
            return false;
        }
        key[n] = (char) p[*i];
        n++;
        (*i)++;
    }
    if (*i >= len)
    {
        return false;
    }
    key[n] = '\0';
    (*i)++;
    return true;
}

/** An optionally negative decimal integer of 1..CP_MAX_DIGITS digits, then a delimiter. */
static bool prv_scan_int(const uint8_t *p, uint32_t len, uint32_t *i, int64_t *value)
{
    bool negative = false;
    uint32_t digits = 0U;
    int64_t v = 0;

    if ((*i < len) && (p[*i] == (uint8_t) '-'))
    {
        negative = true;
        (*i)++;
    }
    while ((*i < len) && (p[*i] >= (uint8_t) '0') && (p[*i] <= (uint8_t) '9'))
    {
        v = (v * 10) + (int64_t) (p[*i] - (uint8_t) '0');
        digits++;
        (*i)++;
        if (digits > CP_MAX_DIGITS)
        {
            return false;
        }
    }
    if (digits == 0U)
    {
        return false;
    }
    /* A float, an exponent or anything else glued to the digits is not an integer. */
    if ((*i < len) && !prv_is_space(p[*i]) && (p[*i] != (uint8_t) ',') && (p[*i] != (uint8_t) '}'))
    {
        return false;
    }
    *value = negative ? -v : v;
    return true;
}

static cp_cfg_status_t prv_fail(cp_cfg_cmd_t *out, cp_cfg_status_t status, const char *key)
{
    if (key != NULL)
    {
        (void) strncpy(out->bad_key, key, CP_CFG_KEY_LEN - 1U);
        out->bad_key[CP_CFG_KEY_LEN - 1U] = '\0';
    }
    return status;
}

/** Record one "key":value member (req or a parameter); a status other than OK ends the parse. */
static cp_cfg_status_t prv_add_member(cp_cfg_cmd_t *out, const char *key, int64_t value)
{
    if (strcmp(key, "req") == 0)
    {
        if (out->req_known || (value < 0) || (value > (int64_t) 0xFFFFFFFFLL))
        {
            return CP_CFG_BAD_JSON;
        }
        out->req_known = true;
        out->req = (uint32_t) value;
        return CP_CFG_OK;
    }

    const cp_key_def_t *def = prv_find_key(key);

    if (def == NULL)
    {
        return prv_fail(out, CP_CFG_UNKNOWN_KEY, key);
    }
    if (!def->remote)
    {
        return prv_fail(out, CP_CFG_NOT_REMOTE, key);
    }
    for (uint8_t k = 0U; k < out->count; k++)
    {
        if (out->pairs[k].id == def->id)
        {
            return CP_CFG_BAD_JSON; /* duplicate key */
        }
    }
    if (out->count >= CP_CFG_MAX_KEYS)
    {
        return CP_CFG_TOO_MANY_KEYS;
    }
    if (!prv_fits(def, value))
    {
        return prv_fail(out, CP_CFG_INVALID_VALUE, key);
    }

    cp_cfg_pair_t *pair = &out->pairs[out->count];

    pair->key = def->name;
    pair->id = def->id;
    pair->offset = def->offset;
    pair->size = def->size;
    pair->is_signed = def->is_signed;
    pair->value = value;
    out->count++;
    return CP_CFG_OK;
}

cp_cfg_status_t cp_parse_config_command(const uint8_t *payload, uint32_t len, cp_cfg_cmd_t *out)
{
    if (out == NULL)
    {
        return CP_CFG_BAD_JSON;
    }
    (void) memset(out, 0, sizeof(*out));
    if ((payload == NULL) || (len == 0U))
    {
        return CP_CFG_BAD_JSON;
    }

    uint32_t i = 0U;

    prv_skip_space(payload, len, &i);
    if ((i >= len) || (payload[i] != (uint8_t) '{'))
    {
        return CP_CFG_BAD_JSON;
    }
    i++;
    prv_skip_space(payload, len, &i);
    if ((i < len) && (payload[i] == (uint8_t) '}'))
    {
        return CP_CFG_BAD_JSON; /* empty object: nothing to apply */
    }

    for (;;)
    {
        char key[CP_CFG_KEY_LEN];
        int64_t value = 0;

        prv_skip_space(payload, len, &i);
        if (!prv_scan_key(payload, len, &i, key))
        {
            return CP_CFG_BAD_JSON;
        }
        prv_skip_space(payload, len, &i);
        if ((i >= len) || (payload[i] != (uint8_t) ':'))
        {
            return CP_CFG_BAD_JSON;
        }
        i++;
        prv_skip_space(payload, len, &i);
        if (!prv_scan_int(payload, len, &i, &value))
        {
            return CP_CFG_BAD_JSON;
        }

        const cp_cfg_status_t status = prv_add_member(out, key, value);

        if (status != CP_CFG_OK)
        {
            return status;
        }

        prv_skip_space(payload, len, &i);
        if (i >= len)
        {
            return CP_CFG_BAD_JSON;
        }
        if (payload[i] == (uint8_t) '}')
        {
            i++;
            break;
        }
        if (payload[i] != (uint8_t) ',')
        {
            return CP_CFG_BAD_JSON;
        }
        i++;
    }

    prv_skip_space(payload, len, &i);
    if ((i != len) || (out->count == 0U))
    {
        return CP_CFG_BAD_JSON; /* trailing data, or only "req" */
    }
    return CP_CFG_OK;
}

const char *cp_cfg_reason_string(cp_cfg_status_t status)
{
    switch (status)
    {
    case CP_CFG_UNKNOWN_KEY:
        return "unknown_key";
    case CP_CFG_NOT_REMOTE:
        return "not_remote";
    case CP_CFG_TOO_MANY_KEYS:
        return "too_many_keys";
    case CP_CFG_INVALID_VALUE:
        return "invalid_value";
    case CP_CFG_BUSY:
        return "busy";
    default:
        return "bad_json";
    }
}

uint32_t cp_serialise_config_result(char *buf, uint32_t buf_size, bool req_known, uint32_t req,
                                    cp_cfg_status_t status, const char *key,
                                    const time_provider_ts_t *ts, const char *device_serial)
{
    uint32_t len = 0U;

    bool ok = prv_common(buf, buf_size, &len, device_serial, ts);

    ok = ok && prv_append(buf, buf_size, &len, ",\"event\":\"config\"");
    if (req_known)
    {
        ok = ok && prv_append(buf, buf_size, &len, ",\"req\":%lu", (unsigned long) req);
    }
    if (status == CP_CFG_OK)
    {
        ok = ok && prv_append(buf, buf_size, &len, ",\"result\":\"applied\"");
    }
    else
    {
        ok = ok && prv_append(buf, buf_size, &len, ",\"result\":\"rejected\",\"reason\":\"%s\"",
                              cp_cfg_reason_string(status));
        if ((key != NULL) && (key[0] != '\0') &&
            ((status == CP_CFG_UNKNOWN_KEY) || (status == CP_CFG_NOT_REMOTE) ||
             (status == CP_CFG_INVALID_VALUE)))
        {
            ok = ok && prv_append(buf, buf_size, &len, ",\"key\":\"%s\"", key);
        }
    }

    return ok ? prv_close(buf, buf_size, len) : 0U;
}

/* ---------------------------------------------------------------------- */
/* OTA (update-service-lld.md section 6)                                   */
/* ---------------------------------------------------------------------- */

uint32_t cp_serialise_ota_request(char *buf, uint32_t buf_size, const us_chunk_request_t *req,
                                  const time_provider_ts_t *ts, const char *device_serial)
{
    uint32_t len = 0U;

    bool ok = prv_common(buf, buf_size, &len, device_serial, ts);

    ok = ok &&
         prv_append(buf, buf_size, &len, ",\"job\":%lu,\"slot\":\"%c\",\"offset\":%lu,\"len\":%lu",
                    (unsigned long) req->job, (req->slot == FIRMWARE_SLOT_B) ? 'B' : 'A',
                    (unsigned long) req->offset, (unsigned long) req->len);

    return ok ? prv_close(buf, buf_size, len) : 0U;
}

static const char *prv_result_str(us_result_t result)
{
    switch (result)
    {
    case US_RESULT_ACCEPTED:
        return "accepted";
    case US_RESULT_REJECTED:
        return "rejected";
    case US_RESULT_FAILED:
        return "failed";
    case US_RESULT_SUCCESS:
        return "success";
    case US_RESULT_ROLLED_BACK:
        return "rolled_back";
    default:
        return "failed";
    }
}

uint32_t cp_serialise_ota_report(char *buf, uint32_t buf_size, const us_ota_report_t *rep,
                                 const time_provider_ts_t *ts, const char *device_serial)
{
    uint32_t len = 0U;

    bool ok = prv_common(buf, buf_size, &len, device_serial, ts);

    ok = ok && prv_append(buf, buf_size, &len, ",\"event\":\"ota\"");
    if (rep->job_known)
    {
        ok = ok && prv_append(buf, buf_size, &len, ",\"job\":%lu", (unsigned long) rep->job);
    }
    ok = ok && prv_append(buf, buf_size, &len, ",\"result\":\"%s\"", prv_result_str(rep->result));
    if (rep->reason != US_REASON_NONE)
    {
        ok = ok && prv_append(buf, buf_size, &len, ",\"reason\":\"%s\"",
                              update_service_reason_string(rep->reason));
    }
    if (rep->version != 0U)
    {
        ok = ok &&
             prv_append(buf, buf_size, &len, ",\"version\":\"%u.%u.%u\",\"rollbacks\":%lu",
                        (unsigned) (rep->version >> 24), (unsigned) ((rep->version >> 16) & 0xFFU),
                        (unsigned) (rep->version & 0xFFFFU), (unsigned long) rep->rollback_count);
    }

    return ok ? prv_close(buf, buf_size, len) : 0U;
}

/** @brief Read the unsigned decimal after "key": ; false if absent, malformed or above 32 bits. */
static bool prv_find_u32(const char *text, const char *key, uint32_t *out)
{
    char pattern[16];

    (void) snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    const char *p = strstr(text, pattern);
    if (p == NULL)
    {
        return false;
    }
    p += strlen(pattern);
    while (*p == ' ')
    {
        p++;
    }

    if ((*p < '0') || (*p > '9'))
    {
        return false;
    }

    uint64_t value = 0u;
    while ((*p >= '0') && (*p <= '9'))
    {
        value = (value * 10u) + (uint64_t) (*p - '0');
        if (value > 0xFFFFFFFFull)
        {
            return false;
        }
        p++;
    }

    while (*p == ' ')
    {
        p++;
    }
    if ((*p != ',') && (*p != '}'))
    {
        return false;
    }

    *out = (uint32_t) value;
    return true;
}

bool cp_parse_ota_command(const uint8_t *payload, uint32_t len, us_ota_command_t *out,
                          uint32_t *job_out, bool *job_known_out)
{
    char text[CP_OTA_CMD_TEXT_MAX];
    uint32_t job = 0u;

    if (job_known_out != NULL)
    {
        *job_known_out = false;
    }
    if ((payload == NULL) || (out == NULL) || (len == 0u) || (len >= sizeof(text)))
    {
        return false;
    }

    (void) memcpy(text, payload, len);
    text[len] = '\0';

    const bool have_job = prv_find_u32(text, "job", &job);
    if (have_job && (job_known_out != NULL))
    {
        *job_known_out = true;
        if (job_out != NULL)
        {
            *job_out = job;
        }
    }

    uint32_t size_a = 0u;
    uint32_t size_b = 0u;
    if (!have_job || !prv_find_u32(text, "size_a", &size_a) ||
        !prv_find_u32(text, "size_b", &size_b))
    {
        return false;
    }

    out->job = job;
    out->size_a = size_a;
    out->size_b = size_b;
    return true;
}
