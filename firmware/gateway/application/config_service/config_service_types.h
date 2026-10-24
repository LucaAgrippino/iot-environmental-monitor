/**
 * @file config_service_types.h
 * @brief Types shared by the ConfigService API and its two provided interfaces.
 */

#ifndef CONFIG_SERVICE_TYPES_H
#define CONFIG_SERVICE_TYPES_H

/** @brief Error codes returned by ConfigService operations. */
typedef enum
{
    CONFIG_SERVICE_OK = 0,
    CONFIG_SERVICE_ERR_NOT_INIT = 1, /**< API called before config_service_init().    */
    CONFIG_SERVICE_ERR_NULL_ARG = 2, /**< A required pointer is NULL.                 */
    CONFIG_SERVICE_ERR_INVALID = 3,  /**< Validation failed (REQ-DM-001).             */
    CONFIG_SERVICE_ERR_PERSIST = 4,  /**< config_store_save() failed.                 */
} config_service_err_t;

/**
 * @brief Parameter identifiers; the order is the order loaded values are validated in.
 *
 * Value type behind the @c value pointer of set_param / validate_param:
 * POLL_INTERVAL, MODBUS_POLL_PERIOD, TELEMETRY_INTERVAL, HEALTH_INTERVAL: uint32_t;
 * FILTER_ALPHA, humidity and pressure parameters, MQTT_PORT: uint16_t;
 * temperature parameters: int16_t; MODBUS_SLAVE_ADDR, NTP_SERVER_COUNT: uint8_t;
 * MQTT_BROKER: NUL-terminated string; NTP_SERVERS: a
 * char[CONFIG_MAX_NTP_SERVERS][CONFIG_MAX_NTP_HOST_LEN] block (CFG-D11).
 */
typedef enum
{
    CONFIG_PARAM_POLL_INTERVAL = 0,
    CONFIG_PARAM_FILTER_ALPHA,
    CONFIG_PARAM_TEMP_RANGE_MIN,
    CONFIG_PARAM_TEMP_RANGE_MAX,
    CONFIG_PARAM_HUMIDITY_RANGE_MIN,
    CONFIG_PARAM_HUMIDITY_RANGE_MAX,
    CONFIG_PARAM_PRESSURE_RANGE_MIN,
    CONFIG_PARAM_PRESSURE_RANGE_MAX,
    CONFIG_PARAM_TEMP_ALARM_HIGH,
    CONFIG_PARAM_TEMP_ALARM_LOW,
    CONFIG_PARAM_TEMP_HYSTERESIS,
    CONFIG_PARAM_HUMIDITY_ALARM_HIGH,
    CONFIG_PARAM_HUMIDITY_ALARM_LOW,
    CONFIG_PARAM_HUMIDITY_HYSTERESIS,
    CONFIG_PARAM_PRESSURE_ALARM_HIGH,
    CONFIG_PARAM_PRESSURE_ALARM_LOW,
    CONFIG_PARAM_PRESSURE_HYSTERESIS,
    CONFIG_PARAM_MODBUS_SLAVE_ADDR,
    CONFIG_PARAM_MODBUS_POLL_PERIOD,
    CONFIG_PARAM_MQTT_BROKER,
    CONFIG_PARAM_MQTT_PORT,
    CONFIG_PARAM_TELEMETRY_INTERVAL,
    CONFIG_PARAM_HEALTH_INTERVAL,
    CONFIG_PARAM_NTP_SERVERS,
    CONFIG_PARAM_NTP_SERVER_COUNT,
    CONFIG_PARAM_COUNT,
} config_param_id_t;

/** @brief Callback fired after a parameter changed; runs in the caller's task, mutex released. */
typedef void (*config_change_cb_t)(config_param_id_t param_id);

/** Maximum number of change callbacks (compile-time). */
#define CONFIG_MAX_CHANGE_CALLBACKS (4U)

#endif /* CONFIG_SERVICE_TYPES_H */
