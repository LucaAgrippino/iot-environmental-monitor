/**
 * @file cloud_publisher_json.c
 * @brief JSON serialisation helpers for CloudPublisher (Gateway).
 *
 * Schemas per docs/lld/application/cloud-publisher-lld.md §7. Each
 * function returns 0 on truncation (buffer too small — CP_ERR_SERIALISE
 * at the call site) or the number of bytes written (excluding the null
 * terminator) on success.
 *
 * @note Timestamp uses xTaskGetTickCount() as a placeholder — the
 *       companion's schemas call for a wall-clock epoch, but no
 *       ITimeService dependency is injected into CloudPublisher's
 *       config (open item; see session report deviations).
 */

#include "cloud_publisher_json.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "update_service/us_reason.h"

#define CP_OTA_CMD_TEXT_MAX 129u /**< 128 payload bytes plus the terminator. */

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

/* Wire model of the onboard readings. The cloud contract predates SensorService's
 * fixed-point units, so the conversion to it lives here (SS-O8). */
typedef struct
{
    int temperature_deci_c;
    unsigned humidity_pct;
    unsigned pressure_hpa;
    int accel_x_mg;
    int accel_y_mg;
    int accel_z_mg;
    int gyro_x_mdps;
    int gyro_y_mdps;
    int gyro_z_mdps;
    int mag_x_mgauss;
    int mag_y_mgauss;
    int mag_z_mgauss;
    bool time_synchronised;
} cp_wire_reading_t;

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

/* An invalid channel reports 0 and is named in invalid_channels (SS-O12). */
static int32_t prv_channel(const sensor_snapshot_t *snap, sensor_id_t id)
{
    return snap->readings[id].valid ? snap->readings[id].value : 0;
}

static cp_wire_reading_t prv_to_wire(const sensor_snapshot_t *snap)
{
    cp_wire_reading_t w;
    w.temperature_deci_c = prv_div_round(prv_channel(snap, SENSOR_ID_TEMPERATURE), 10);
    w.humidity_pct =
        (unsigned) prv_clamp(prv_div_round(prv_channel(snap, SENSOR_ID_HUMIDITY), 100), 0, 100);
    w.pressure_hpa =
        (unsigned) prv_clamp(prv_div_round(prv_channel(snap, SENSOR_ID_PRESSURE), 10), 0, 65535);
    w.accel_x_mg = prv_channel(snap, SENSOR_ID_ACCEL_X);
    w.accel_y_mg = prv_channel(snap, SENSOR_ID_ACCEL_Y);
    w.accel_z_mg = prv_channel(snap, SENSOR_ID_ACCEL_Z);
    w.gyro_x_mdps = prv_channel(snap, SENSOR_ID_GYRO_X);
    w.gyro_y_mdps = prv_channel(snap, SENSOR_ID_GYRO_Y);
    w.gyro_z_mdps = prv_channel(snap, SENSOR_ID_GYRO_Z);
    w.mag_x_mgauss = prv_channel(snap, SENSOR_ID_MAG_X);
    w.mag_y_mgauss = prv_channel(snap, SENSOR_ID_MAG_Y);
    w.mag_z_mgauss = prv_channel(snap, SENSOR_ID_MAG_Z);
    w.time_synchronised =
        (snap->readings[SENSOR_ID_TEMPERATURE].timestamp.sync_state == TIME_SYNC_SYNCHRONISED);
    return w;
}

/* Wire names in sensor_id_t order, used for the invalid_channels list (SS-O12). */
static const char *const s_channel_names[SENSOR_ID_COUNT] = {
    "temperature_deci_c", "humidity_pct", "pressure_hpa", "accel_x_mg",
    "accel_y_mg",         "accel_z_mg",   "gyro_x_mdps",  "gyro_y_mdps",
    "gyro_z_mdps",        "mag_x_mgauss", "mag_y_mgauss", "mag_z_mgauss",
};

/* Comma-separated quoted names of the channels whose reading is not valid. */
static void prv_invalid_list(const sensor_snapshot_t *snap, char *out, size_t out_size)
{
    size_t len = 0U;
    out[0] = '\0';
    for (uint32_t i = 0U; i < (uint32_t) SENSOR_ID_COUNT; i++)
    {
        if (snap->readings[i].valid)
        {
            continue;
        }
        const int n = snprintf(&out[len], out_size - len, "%s\"%s\"", (len > 0U) ? "," : "",
                               s_channel_names[i]);
        if ((n < 0) || ((size_t) n >= (out_size - len)))
        {
            out[len] = '\0'; /* cannot happen: buffer sized for every channel */
            return;
        }
        len += (size_t) n;
    }
}

uint32_t cp_serialise_telemetry(char *buf, uint32_t buf_size, const sensor_snapshot_t *snap,
                                const modbus_poller_fd_reading_t *fd, const char *device_serial)
{
    const cp_wire_reading_t w = prv_to_wire(snap);
    char invalid[256];
    int n;

    prv_invalid_list(snap, invalid, sizeof(invalid));

    if (fd->valid)
    {
        n = snprintf(
            buf, buf_size,
            "{\"schema_version\":\"1.0\",\"device_id\":\"%s\",\"timestamp\":%lu,"
            "\"sync_state\":\"%s\","
            "\"temperature_deci_c\":%d,\"humidity_pct\":%u,\"pressure_hpa\":%u,"
            "\"accel_x_mg\":%d,\"accel_y_mg\":%d,\"accel_z_mg\":%d,"
            "\"gyro_x_mdps\":%d,\"gyro_y_mdps\":%d,\"gyro_z_mdps\":%d,"
            "\"mag_x_mgauss\":%d,\"mag_y_mgauss\":%d,\"mag_z_mgauss\":%d,\"invalid_channels\":[%s],"
            "\"field_device_temp_deci_c\":%d,\"field_device_humidity_pct\":%u,"
            "\"field_device_pressure_hpa\":%u,\"field_device_valid\":true}",
            device_serial, (unsigned long) xTaskGetTickCount(),
            w.time_synchronised ? "synchronised" : "unsynchronised", w.temperature_deci_c,
            w.humidity_pct, w.pressure_hpa, w.accel_x_mg, w.accel_y_mg, w.accel_z_mg, w.gyro_x_mdps,
            w.gyro_y_mdps, w.gyro_z_mdps, w.mag_x_mgauss, w.mag_y_mgauss, w.mag_z_mgauss, invalid,
            fd->temp_deci_c, fd->humidity_pct, fd->pressure_hpa);
    }
    else
    {
        /* REQ-SA-160: field_device_* fields omitted when the FD is offline. */
        n = snprintf(
            buf, buf_size,
            "{\"schema_version\":\"1.0\",\"device_id\":\"%s\",\"timestamp\":%lu,"
            "\"sync_state\":\"%s\","
            "\"temperature_deci_c\":%d,\"humidity_pct\":%u,\"pressure_hpa\":%u,"
            "\"accel_x_mg\":%d,\"accel_y_mg\":%d,\"accel_z_mg\":%d,"
            "\"gyro_x_mdps\":%d,\"gyro_y_mdps\":%d,\"gyro_z_mdps\":%d,"
            "\"mag_x_mgauss\":%d,\"mag_y_mgauss\":%d,\"mag_z_mgauss\":%d,\"invalid_channels\":[%s],"
            "\"field_device_valid\":false}",
            device_serial, (unsigned long) xTaskGetTickCount(),
            w.time_synchronised ? "synchronised" : "unsynchronised", w.temperature_deci_c,
            w.humidity_pct, w.pressure_hpa, w.accel_x_mg, w.accel_y_mg, w.accel_z_mg, w.gyro_x_mdps,
            w.gyro_y_mdps, w.gyro_z_mdps, w.mag_x_mgauss, w.mag_y_mgauss, w.mag_z_mgauss, invalid);
    }

    if ((n < 0) || ((uint32_t) n >= buf_size))
    {
        return 0u;
    }
    return (uint32_t) n;
}

uint32_t cp_serialise_health(char *buf, uint32_t buf_size, const cp_health_snapshot_t *snap,
                             const char *device_serial)
{
    int n = snprintf(buf, buf_size,
                     "{\"schema_version\":\"1.0\",\"device_id\":\"%s\",\"timestamp\":%lu,"
                     "\"uptime_s\":%lu,\"cloud_connected\":%s,"
                     "\"mqtt_reconnect_count\":%lu,\"buffer_entry_count\":%lu}",
                     device_serial, (unsigned long) xTaskGetTickCount(),
                     (unsigned long) snap->uptime_s, snap->cloud_connected ? "true" : "false",
                     (unsigned long) snap->mqtt_reconnect_count,
                     (unsigned long) snap->buffer_entry_count);

    if ((n < 0) || ((uint32_t) n >= buf_size))
    {
        return 0u;
    }
    return (uint32_t) n;
}

uint32_t cp_serialise_alarm(char *buf, uint32_t buf_size, const cp_alarm_event_t *event,
                            const char *device_serial)
{
    int n = snprintf(buf, buf_size,
                     "{\"schema_version\":\"1.0\",\"device_id\":\"%s\",\"timestamp\":%lu,"
                     "\"sensor_id\":\"%s\",\"alarm_type\":\"%s\","
                     "\"measured_value_raw\":%ld,\"threshold_value_raw\":%ld,\"source\":\"%s\"}",
                     device_serial, (unsigned long) xTaskGetTickCount(), event->sensor_name,
                     prv_alarm_type_str(event->alarm_type), (long) event->measured_value_raw,
                     (long) event->threshold_value_raw, prv_alarm_source_str(event->source));

    if ((n < 0) || ((uint32_t) n >= buf_size))
    {
        return 0u;
    }
    return (uint32_t) n;
}

/* ---------------------------------------------------------------------- */
/* OTA (update-service-lld.md section 6)                                   */
/* ---------------------------------------------------------------------- */

uint32_t cp_serialise_ota_request(char *buf, uint32_t buf_size, const us_chunk_request_t *req)
{
    const int n =
        snprintf(buf, buf_size, "{\"job\":%lu,\"slot\":\"%c\",\"offset\":%lu,\"len\":%lu}",
                 (unsigned long) req->job, (req->slot == FIRMWARE_SLOT_B) ? 'B' : 'A',
                 (unsigned long) req->offset, (unsigned long) req->len);

    if ((n < 0) || ((uint32_t) n >= buf_size))
    {
        return 0u;
    }
    return (uint32_t) n;
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

uint32_t cp_serialise_ota_report(char *buf, uint32_t buf_size, const us_ota_report_t *rep)
{
    uint32_t len = 0u;
    int n = snprintf(buf, buf_size, "{\"event\":\"ota\"");

    if ((n < 0) || ((uint32_t) n >= buf_size))
    {
        return 0u;
    }
    len = (uint32_t) n;

    if (rep->job_known)
    {
        n = snprintf(&buf[len], buf_size - len, ",\"job\":%lu", (unsigned long) rep->job);
        if ((n < 0) || ((uint32_t) n >= (buf_size - len)))
        {
            return 0u;
        }
        len += (uint32_t) n;
    }

    n = snprintf(&buf[len], buf_size - len, ",\"result\":\"%s\"", prv_result_str(rep->result));
    if ((n < 0) || ((uint32_t) n >= (buf_size - len)))
    {
        return 0u;
    }
    len += (uint32_t) n;

    if (rep->reason != US_REASON_NONE)
    {
        n = snprintf(&buf[len], buf_size - len, ",\"reason\":\"%s\"",
                     update_service_reason_string(rep->reason));
        if ((n < 0) || ((uint32_t) n >= (buf_size - len)))
        {
            return 0u;
        }
        len += (uint32_t) n;
    }

    if (rep->version != 0u)
    {
        n = snprintf(&buf[len], buf_size - len, ",\"version\":\"%u.%u.%u\",\"rollbacks\":%lu",
                     (unsigned) (rep->version >> 24), (unsigned) ((rep->version >> 16) & 0xFFu),
                     (unsigned) (rep->version & 0xFFFFu), (unsigned long) rep->rollback_count);
        if ((n < 0) || ((uint32_t) n >= (buf_size - len)))
        {
            return 0u;
        }
        len += (uint32_t) n;
    }

    if ((len + 2u) > buf_size)
    {
        return 0u;
    }
    buf[len] = '}';
    buf[len + 1u] = '\0';

    return len + 1u;
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
