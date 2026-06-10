/* cppcheck-suppress-file unusedStructMember -- fields consumed by SensorService,
 * AlarmService, ConsoleService, ModbusRegisterMap, and CloudPublisher, none of
 * which are in scope when this header is analysed in isolation. */

/**
 * @file config_params.h
 * @brief ConfigService parameter schema — shared between Field Device and Gateway.
 *
 * All sensor-related fields use fixed-point integers (P9 — no floating-point):
 *   - Temperature : int16_t ×100 (centi-°C).  Signed because temperature can be
 *                   negative (e.g. temp_range_min default −40 °C = −4000).
 *   - Humidity    : uint16_t ×100 (centi-%RH).
 *   - Pressure    : uint16_t ×10  (deci-hPa).
 *   - filter_alpha: uint16_t ×1000 (per-mille); 100 = 0.100.
 *
 * Board-specific cloud / time-sync fields are conditionally compiled with
 * BOARD_GATEWAY.  The struct is packed to eliminate compiler-version padding
 * differences (§6 serialisation).
 *
 * @see docs/lld/application/config-service.md §2
 */

#ifndef CONFIG_PARAMS_H
#define CONFIG_PARAMS_H

#include <stdint.h>

/* ========================================================================= */
/* String-length constants                                                   */
/* ========================================================================= */

#define CONFIG_MAX_NTP_SERVERS 4U   /**< Max NTP server entries (GW only).    */
#define CONFIG_MAX_BROKER_LEN 128U  /**< MQTT broker string length (GW only). */
#define CONFIG_MAX_NTP_HOST_LEN 64U /**< NTP host string length (GW only).    */

/* ========================================================================= */
/* config_params_t                                                           */
/* ========================================================================= */

typedef struct __attribute__((packed))
{
    /* ── Sensor acquisition (REQ-SA-070, SA-140) ── */
    uint32_t polling_interval_ms; /**< ms; default 1000; range [100, 60000]           */
    uint16_t filter_alpha;        /**< ×1000 per-mille; default 100 (=0.100); (0,1000) excl. */

    /* ── Sensor physical ranges (REQ-SA-020, SA-120) ── */
    int16_t temp_range_min;      /**< centi-°C (×100); default −4000 (=−40.0 °C)    */
    int16_t temp_range_max;      /**< centi-°C (×100); default  8500 (= 85.0 °C)    */
    uint16_t humidity_range_min; /**< centi-%RH (×100); default    0 (=  0.0 %RH)   */
    uint16_t humidity_range_max; /**< centi-%RH (×100); default 10000 (=100.0 %RH)  */
    uint16_t pressure_range_min; /**< deci-hPa (×10);   default  3000 (= 300.0 hPa) */
    uint16_t pressure_range_max; /**< deci-hPa (×10);   default 11000 (=1100.0 hPa) */

    /* ── Alarm thresholds (REQ-AM-000, AM-011) ── */
    int16_t temp_alarm_high;      /**< centi-°C (×100); default  4000 (= 40.0 °C) */
    int16_t temp_alarm_low;       /**< centi-°C (×100); default     0 (=  0.0 °C) */
    int16_t temp_hysteresis;      /**< centi-°C (×100); default   100 (=  1.0 °C) */
    uint16_t humidity_alarm_high; /**< centi-%RH (×100); default  8000 (= 80.0 %RH) */
    uint16_t humidity_alarm_low;  /**< centi-%RH (×100); default  2000 (= 20.0 %RH) */
    uint16_t humidity_hysteresis; /**< centi-%RH (×100); default   200 (=  2.0 %RH) */
    uint16_t pressure_alarm_high; /**< deci-hPa (×10);   default 10500 (=1050.0 hPa) */
    uint16_t pressure_alarm_low;  /**< deci-hPa (×10);   default  9500 (= 950.0 hPa) */
    uint16_t pressure_hysteresis; /**< deci-hPa (×10);   default    50 (=   5.0 hPa) */

    /* ── Modbus ── */
    uint8_t modbus_slave_addr;      /**< FD own address [1..247]; default 1             */
    uint32_t modbus_poll_period_ms; /**< GW poll scheduler period; default 1000         */

#if defined(BOARD_GATEWAY)
    /* ── Cloud connectivity (GW only) ── */
    char mqtt_broker[CONFIG_MAX_BROKER_LEN];
    uint16_t mqtt_port;             /**< default 8883   */
    uint32_t telemetry_interval_ms; /**< default 60000  */
    uint32_t health_interval_ms;    /**< default 600000 */

    /* ── Time sync (GW only) ── */
    char ntp_servers[CONFIG_MAX_NTP_SERVERS][CONFIG_MAX_NTP_HOST_LEN];
    uint8_t ntp_server_count;
#endif /* BOARD_GATEWAY */
} config_params_t;

#endif /* CONFIG_PARAMS_H */
