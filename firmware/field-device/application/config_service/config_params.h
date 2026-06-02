/* cppcheck-suppress-file unusedStructMember -- fields consumed by SensorService,
 * AlarmService, ConsoleService, ModbusRegisterMap, and CloudPublisher, none of
 * which are in scope when this header is analysed in isolation. */

/**
 * @file config_params.h
 * @brief ConfigService parameter schema — shared between Field Device and Gateway.
 *
 * Defines config_params_t and the CONFIG_MAX_* string-length constants.
 * Board-specific cloud / time-sync fields are conditionally compiled
 * with BOARD_GATEWAY.  The struct is packed to eliminate compiler-version
 * padding differences (§6 serialisation).
 *
 * @see docs/lld/application/config-service.md §2
 */

#ifndef CONFIG_PARAMS_H
#define CONFIG_PARAMS_H

#include <stdint.h>

/* ========================================================================= */
/* String-length constants                                                   */
/* ========================================================================= */

#define CONFIG_MAX_NTP_SERVERS    4U    /**< Max NTP server entries (GW only).  */
#define CONFIG_MAX_BROKER_LEN   128U   /**< MQTT broker string length (GW only). */
#define CONFIG_MAX_NTP_HOST_LEN  64U   /**< NTP host string length (GW only).    */

/* ========================================================================= */
/* config_params_t                                                           */
/* ========================================================================= */

typedef struct __attribute__((packed))
{
    /* ── Sensor acquisition (REQ-SA-070, SA-140) ── */
    uint32_t polling_interval_ms;     /**< default 1000; range [100, 60000]  */
    float    filter_alpha;            /**< default 0.1f; range (0.0, 1.0) exclusive */

    /* ── Sensor physical ranges (REQ-SA-020, SA-120) ── */
    float    temp_range_min;          /**< °C;  default −40.0  */
    float    temp_range_max;          /**< °C;  default  85.0  */
    float    humidity_range_min;      /**< %RH; default   0.0  */
    float    humidity_range_max;      /**< %RH; default 100.0  */
    float    pressure_range_min;      /**< hPa; default 300.0  */
    float    pressure_range_max;      /**< hPa; default 1100.0 */

    /* ── Alarm thresholds (REQ-AM-000, AM-011) ── */
    float    temp_alarm_high;         /**< default  40.0  */
    float    temp_alarm_low;          /**< default   0.0  */
    float    temp_hysteresis;         /**< default   1.0  */
    float    humidity_alarm_high;     /**< default  80.0  */
    float    humidity_alarm_low;      /**< default  20.0  */
    float    humidity_hysteresis;     /**< default   2.0  */
    float    pressure_alarm_high;     /**< default 1050.0 */
    float    pressure_alarm_low;      /**< default  950.0 */
    float    pressure_hysteresis;     /**< default    5.0 */

    /* ── Modbus ── */
    uint8_t  modbus_slave_addr;       /**< FD own address [1..247]; default 1    */
    uint32_t modbus_poll_period_ms;   /**< GW poll scheduler period; default 1000 */

#if defined(BOARD_GATEWAY)
    /* ── Cloud connectivity (GW only) ── */
    char     mqtt_broker[CONFIG_MAX_BROKER_LEN];
    uint16_t mqtt_port;               /**< default 8883   */
    uint32_t telemetry_interval_ms;   /**< default 60000  */
    uint32_t health_interval_ms;      /**< default 600000 */

    /* ── Time sync (GW only) ── */
    char     ntp_servers[CONFIG_MAX_NTP_SERVERS][CONFIG_MAX_NTP_HOST_LEN];
    uint8_t  ntp_server_count;
#endif /* BOARD_GATEWAY */
} config_params_t;

#endif /* CONFIG_PARAMS_H */
