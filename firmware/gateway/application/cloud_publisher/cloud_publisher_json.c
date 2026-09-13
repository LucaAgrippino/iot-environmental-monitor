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

#include "FreeRTOS.h"
#include "task.h"

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

uint32_t cp_serialise_telemetry(char *buf, uint32_t buf_size, const sensor_reading_t *reading,
                                const modbus_poller_fd_reading_t *fd, const char *device_serial)
{
    int n;

    if (fd->valid)
    {
        n = snprintf(buf, buf_size,
                     "{\"schema_version\":\"1.0\",\"device_id\":\"%s\",\"timestamp\":%lu,"
                     "\"sync_state\":\"%s\","
                     "\"temperature_deci_c\":%d,\"humidity_pct\":%u,\"pressure_hpa\":%u,"
                     "\"accel_x_mg\":%d,\"accel_y_mg\":%d,\"accel_z_mg\":%d,"
                     "\"gyro_x_mdps\":%d,\"gyro_y_mdps\":%d,\"gyro_z_mdps\":%d,"
                     "\"mag_x_mgauss\":%d,\"mag_y_mgauss\":%d,\"mag_z_mgauss\":%d,"
                     "\"field_device_temp_deci_c\":%d,\"field_device_humidity_pct\":%u,"
                     "\"field_device_pressure_hpa\":%u,\"field_device_valid\":true}",
                     device_serial, (unsigned long) xTaskGetTickCount(),
                     reading->time_synchronised ? "synchronised" : "unsynchronised",
                     reading->temperature_deci_c, reading->humidity_pct, reading->pressure_hpa,
                     reading->accel_x_mg, reading->accel_y_mg, reading->accel_z_mg,
                     reading->gyro_x_mdps, reading->gyro_y_mdps, reading->gyro_z_mdps,
                     reading->mag_x_mgauss, reading->mag_y_mgauss, reading->mag_z_mgauss,
                     fd->temp_deci_c, fd->humidity_pct, fd->pressure_hpa);
    }
    else
    {
        /* REQ-SA-160: field_device_* fields omitted when the FD is offline. */
        n = snprintf(buf, buf_size,
                     "{\"schema_version\":\"1.0\",\"device_id\":\"%s\",\"timestamp\":%lu,"
                     "\"sync_state\":\"%s\","
                     "\"temperature_deci_c\":%d,\"humidity_pct\":%u,\"pressure_hpa\":%u,"
                     "\"accel_x_mg\":%d,\"accel_y_mg\":%d,\"accel_z_mg\":%d,"
                     "\"gyro_x_mdps\":%d,\"gyro_y_mdps\":%d,\"gyro_z_mdps\":%d,"
                     "\"mag_x_mgauss\":%d,\"mag_y_mgauss\":%d,\"mag_z_mgauss\":%d,"
                     "\"field_device_valid\":false}",
                     device_serial, (unsigned long) xTaskGetTickCount(),
                     reading->time_synchronised ? "synchronised" : "unsynchronised",
                     reading->temperature_deci_c, reading->humidity_pct, reading->pressure_hpa,
                     reading->accel_x_mg, reading->accel_y_mg, reading->accel_z_mg,
                     reading->gyro_x_mdps, reading->gyro_y_mdps, reading->gyro_z_mdps,
                     reading->mag_x_mgauss, reading->mag_y_mgauss, reading->mag_z_mgauss);
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

uint32_t cp_serialise_alarm(char *buf, uint32_t buf_size, const alarm_event_t *event,
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
