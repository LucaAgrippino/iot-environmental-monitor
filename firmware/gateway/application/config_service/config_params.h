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
 * The field names, widths and order match the Field Device schema. The struct is packed
 * so the stored blob has no compiler-dependent padding, which means some wider fields sit
 * at odd offsets and are not single-copy atomic (CFG-O5).
 *
 * @note Companion: docs/lld/application/config-service-lld.md §2
 */

#ifndef CONFIG_PARAMS_H
#define CONFIG_PARAMS_H

#include <stdint.h>

#define CONFIG_MAX_NTP_SERVERS (4U)   /**< Number of NTP server slots.         */
#define CONFIG_MAX_BROKER_LEN (128U)  /**< MQTT broker string size, with NUL.  */
#define CONFIG_MAX_NTP_HOST_LEN (64U) /**< One NTP host string size, with NUL. */

/** @brief Live configuration. All fields are in the units listed above. */
typedef struct __attribute__((packed))
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
    uint32_t modbus_poll_period_ms; /**< Poll period; default 1000; range [100, 60000]          */

    /* -- Cloud connectivity -- */
    char mqtt_broker[CONFIG_MAX_BROKER_LEN]; /**< Host, NUL-terminated; default empty        */
    uint16_t mqtt_port;                      /**< default 8883                                */
    uint32_t telemetry_interval_ms;          /**< default 60000; range [1000, 3600000]        */
    uint32_t health_interval_ms;             /**< default 600000; range [10000, 86400000]     */

    /* -- Time sync -- */
    char ntp_servers[CONFIG_MAX_NTP_SERVERS][CONFIG_MAX_NTP_HOST_LEN]; /**< IPv4 literals  */
    uint8_t ntp_server_count;                                          /**< default 0       */
} config_params_t;

#endif /* CONFIG_PARAMS_H */
