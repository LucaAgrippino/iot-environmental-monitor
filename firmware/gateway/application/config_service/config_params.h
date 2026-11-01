/**
 * @file config_params.h
 * @brief ConfigService parameter schema (Gateway).
 *
 * Sensor fields are fixed-point integers (no floating point), in the same scales as
 * SensorService and AlarmService, so thresholds need no conversion:
 *   - Temperature : int16_t  x100 (centi-degC), e.g. -4000 = -40.00 degC
 *   - Humidity    : uint16_t x100 (centi-%RH)
 *   - Pressure    : uint16_t x10  (deci-hPa)
 *   - filter_alpha: uint16_t x1000 (per-mille), 100 = 0.100
 *
 * The field names, widths and order match the Field Device schema. Unlike the Field Device
 * struct this one is not packed: every field sits at its natural alignment, with explicit
 * reserved bytes where the Field Device layout would have left a field misaligned. A read of
 * any integer field is therefore a single access and cannot tear against a write (CFG-D3);
 * only the string fields need config_service_lock() (CFG-O5, closed).
 *
 * @note Companion: docs/lld/application/config-service-lld.md §2
 */

#ifndef CONFIG_PARAMS_H
#define CONFIG_PARAMS_H

#include <stddef.h>
#include <stdint.h>

#define CONFIG_MAX_NTP_SERVERS (4U)   /**< Number of NTP server slots.         */
#define CONFIG_MAX_BROKER_LEN (128U)  /**< MQTT broker string size, with NUL.  */
#define CONFIG_MAX_NTP_HOST_LEN (64U) /**< One NTP host string size, with NUL. */

/** @brief Live configuration. All fields are in the units listed above. */
typedef struct
{
    /* -- Sensor acquisition (REQ-SA-070, SA-140) -- */
    uint32_t polling_interval_ms; /**< ms; default 1000; range [100, 60000]               */
    uint16_t filter_alpha;        /**< per-mille; default 100 (0.100); range [1, 999]     */

    /* -- Sensor physical ranges (REQ-SA-020, SA-120) -- */
    int16_t temp_range_min;      /**< centi-degC; default -4000                           */
    int16_t temp_range_max;      /**< centi-degC; default  8500                           */
    uint16_t humidity_range_min; /**< centi-%RH;  default     0                           */
    uint16_t humidity_range_max; /**< centi-%RH;  default 10000                           */
    uint16_t pressure_range_min; /**< deci-hPa;   default  3000                           */
    uint16_t pressure_range_max; /**< deci-hPa;   default 11000                           */

    /* -- Alarm thresholds (REQ-AM-000, AM-011) -- */
    int16_t temp_alarm_high;      /**< centi-degC; default  4000                          */
    int16_t temp_alarm_low;       /**< centi-degC; default     0                          */
    int16_t temp_hysteresis;      /**< centi-degC; default   100                          */
    uint16_t humidity_alarm_high; /**< centi-%RH;  default  8000                          */
    uint16_t humidity_alarm_low;  /**< centi-%RH;  default  2000                          */
    uint16_t humidity_hysteresis; /**< centi-%RH;  default   200                          */
    uint16_t pressure_alarm_high; /**< deci-hPa;   default 10500                          */
    uint16_t pressure_alarm_low;  /**< deci-hPa;   default  9500                          */
    uint16_t pressure_hysteresis; /**< deci-hPa;   default    50                          */

    /* -- Modbus -- */
    uint8_t modbus_slave_addr;      /**< Field Device address; unused on the Gateway; default 1 */
    uint8_t reserved_a[3];          /**< Padding so the next field is 4-byte aligned; zero      */
    uint32_t modbus_poll_period_ms; /**< Poll period; default 1000; range [100, 60000]          */

    /* -- Cloud connectivity -- */
    char mqtt_broker[CONFIG_MAX_BROKER_LEN]; /**< Host, NUL-terminated; default empty        */
    uint16_t mqtt_port;                      /**< default 8883                                */
    uint8_t reserved_b[2];          /**< Padding so the next field is 4-byte aligned; zero */
    uint32_t telemetry_interval_ms; /**< default 60000; range [1000, 3600000]        */
    uint32_t health_interval_ms;    /**< default 600000; range [10000, 86400000]     */

    /* -- Time sync -- */
    char ntp_servers[CONFIG_MAX_NTP_SERVERS][CONFIG_MAX_NTP_HOST_LEN]; /**< IPv4 or host name */
    uint8_t ntp_server_count;                                          /**< default 0       */
    uint8_t reserved_c;                                                /**< Padding; zero   */
    uint16_t ntp_interval_min; /**< Sync interval, minutes; default 60; range [1, 1440]. Carved out
                                    of the former tail padding, so the blob size and schema stay;
                                    a stored 0 is invalid and repaired to the default (D-8). */
} config_params_t;

/* The layout is part of the stored blob: any change must bump CONFIG_SCHEMA_VERSION. */
_Static_assert((offsetof(config_params_t, polling_interval_ms) % 4U) == 0U, "polling_interval_ms");
_Static_assert((offsetof(config_params_t, modbus_poll_period_ms) % 4U) == 0U,
               "modbus_poll_period_ms");
_Static_assert((offsetof(config_params_t, mqtt_port) % 2U) == 0U, "mqtt_port");
_Static_assert((offsetof(config_params_t, telemetry_interval_ms) % 4U) == 0U,
               "telemetry_interval_ms");
_Static_assert((offsetof(config_params_t, health_interval_ms) % 4U) == 0U, "health_interval_ms");
_Static_assert((offsetof(config_params_t, ntp_interval_min) % 2U) == 0U, "ntp_interval_min");
_Static_assert((sizeof(config_params_t) % 4U) == 0U, "config_params_t size");

#endif /* CONFIG_PARAMS_H */
