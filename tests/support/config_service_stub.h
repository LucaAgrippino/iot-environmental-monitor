/**
 * @file config_service_stub.h
 * @brief Narrow stub for ConfigService in lcd_ui unit tests.
 *
 * Provides iconfig_provider_t (get_params) and iconfig_manager_t (set_param)
 * — the only ConfigService methods lcd_ui.c calls. Includes config_params.h
 * directly (safe: header-only, no matching .c to auto-link).
 *
 * config_param_id_t duplicates the relevant enum values from config_service.h
 * so the test build does not pull in config_service.c or config_store.c.
 *
 * Basename does NOT match config_service.c or config_store.c.
 */

#ifndef CONFIG_SERVICE_STUB_H
#define CONFIG_SERVICE_STUB_H

#include <stdint.h>
#include "config_service/config_params.h" /* config_params_t — header-only */

typedef enum
{
    CONFIG_SERVICE_OK           = 0,
    CONFIG_SERVICE_ERR_NOT_INIT = 1,
    CONFIG_SERVICE_ERR_NULL_ARG = 2,
    CONFIG_SERVICE_ERR_INVALID  = 3,
    CONFIG_SERVICE_ERR_PERSIST  = 4,
} config_service_err_t;

typedef enum
{
    CONFIG_PARAM_POLL_INTERVAL      = 0,
    CONFIG_PARAM_FILTER_ALPHA       = 1,
    CONFIG_PARAM_TEMP_RANGE_MIN     = 2,
    CONFIG_PARAM_TEMP_RANGE_MAX     = 3,
    CONFIG_PARAM_HUMIDITY_RANGE_MIN = 4,
    CONFIG_PARAM_HUMIDITY_RANGE_MAX = 5,
    CONFIG_PARAM_PRESSURE_RANGE_MIN = 6,
    CONFIG_PARAM_PRESSURE_RANGE_MAX = 7,
    CONFIG_PARAM_TEMP_ALARM_HIGH    = 8,
    CONFIG_PARAM_TEMP_ALARM_LOW     = 9,
    CONFIG_PARAM_TEMP_HYSTERESIS    = 10,
    CONFIG_PARAM_HUMIDITY_ALARM_HIGH = 11,
    CONFIG_PARAM_HUMIDITY_ALARM_LOW  = 12,
    CONFIG_PARAM_HUMIDITY_HYSTERESIS = 13,
    CONFIG_PARAM_PRESSURE_ALARM_HIGH = 14,
    CONFIG_PARAM_PRESSURE_ALARM_LOW  = 15,
    CONFIG_PARAM_PRESSURE_HYSTERESIS = 16,
    CONFIG_PARAM_MODBUS_SLAVE_ADDR   = 17,
    CONFIG_PARAM_MODBUS_POLL_PERIOD  = 18,
    CONFIG_PARAM_COUNT,
} config_param_id_t;

/* --------------------------------------------------------------------- */
/* iconfig_provider_t — read side                                       */
/* --------------------------------------------------------------------- */

typedef struct
{
    const config_params_t *(*get_params)(void);
} iconfig_provider_t;

/* --------------------------------------------------------------------- */
/* iconfig_manager_t — write side (only set_param used by lcd_ui)       */
/* --------------------------------------------------------------------- */

typedef struct
{
    config_service_err_t (*set_param)(config_param_id_t id, const void *value);
} iconfig_manager_t;

#endif /* CONFIG_SERVICE_STUB_H */
