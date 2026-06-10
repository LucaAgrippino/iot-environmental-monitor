/**
 * @file config_service.c
 * @brief ConfigService — validate → apply → persist pipeline.
 *
 * @see docs/lld/application/config-service.md
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#include "FreeRTOS.h"
#include "semphr.h"

#ifdef TEST
#define LOG_ERROR(m, f, ...) ((void) 0)
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#define LOG_DEBUG(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

#include "config_service/config_service.h"

/* ========================================================================= */
/* Build-time size guard (CS-O3)                                             */
/* ========================================================================= */

static_assert(sizeof(config_blob_t) <= CONFIG_STORE_MAX_DATA_BYTES,
              "config_blob_t exceeds CONFIG_STORE_MAX_DATA_BYTES");

/* ========================================================================= */
/* Module tag                                                                */
/* ========================================================================= */

#define MODULE_TAG "CS"

/* ========================================================================= */
/* Compile-time defaults (fixed-point, matching config_params.h scales)     */
/* ========================================================================= */

#define DEFAULT_POLL_INTERVAL_MS 1000U
#define DEFAULT_FILTER_ALPHA 100U /* 0.100 × 1000 */

#define DEFAULT_TEMP_RANGE_MIN (-4000) /* −40.0 °C × 100 */
#define DEFAULT_TEMP_RANGE_MAX 8500    /*  85.0 °C × 100 */
#define DEFAULT_HUMIDITY_RANGE_MIN 0U
#define DEFAULT_HUMIDITY_RANGE_MAX 10000U /* 100.0 %RH × 100 */
#define DEFAULT_PRESSURE_RANGE_MIN 3000U  /*  300.0 hPa × 10 */
#define DEFAULT_PRESSURE_RANGE_MAX 11000U /* 1100.0 hPa × 10 */

#define DEFAULT_TEMP_ALARM_HIGH 4000       /*  40.0 °C × 100 */
#define DEFAULT_TEMP_ALARM_LOW 0           /*   0.0 °C × 100 */
#define DEFAULT_TEMP_HYSTERESIS 100        /*   1.0 °C × 100 */
#define DEFAULT_HUMIDITY_ALARM_HIGH 8000U  /*  80.0 %RH × 100 */
#define DEFAULT_HUMIDITY_ALARM_LOW 2000U   /*  20.0 %RH × 100 */
#define DEFAULT_HUMIDITY_HYSTERESIS 200U   /*   2.0 %RH × 100 */
#define DEFAULT_PRESSURE_ALARM_HIGH 10500U /* 1050.0 hPa × 10 */
#define DEFAULT_PRESSURE_ALARM_LOW 9500U   /*  950.0 hPa × 10 */
#define DEFAULT_PRESSURE_HYSTERESIS 50U    /*    5.0 hPa × 10 */

#define DEFAULT_MODBUS_SLAVE_ADDR 1U
#define DEFAULT_MODBUS_POLL_PERIOD_MS 1000U

/* ========================================================================= */
/* Absolute physical limits used in validation (fixed-point)                */
/* ========================================================================= */

#define POLL_INTERVAL_MIN_MS 100U
#define POLL_INTERVAL_MAX_MS 60000U

/* Temperature: int16_t ×100 centi-°C */
#define TEMP_ABS_MIN (-5500)   /* −55.0 °C */
#define TEMP_ABS_MAX 12500     /* 125.0 °C */
#define TEMP_RANGE_GAP_MIN 100 /* minimum 1.0 °C gap between range_min and range_max */

/* Humidity: uint16_t ×100 centi-%RH */
#define HUMIDITY_ABS_MIN 0U
#define HUMIDITY_ABS_MAX 10000U     /* 100.0 %RH */
#define HUMIDITY_RANGE_GAP_MIN 100U /* minimum 1.0 %RH gap */

/* Pressure: uint16_t ×10 deci-hPa */
#define PRESSURE_ABS_MIN 0U
#define PRESSURE_ABS_MAX 12000U    /* 1200.0 hPa */
#define PRESSURE_RANGE_GAP_MIN 10U /* minimum 1.0 hPa gap */

/* Filter alpha: uint16_t ×1000 per-mille, range (0, 1000) exclusive */
#define FILTER_ALPHA_MAX 1000U

#define MODBUS_ADDR_MIN 1U
#define MODBUS_ADDR_MAX 247U

#define MODBUS_POLL_MIN_MS 100U
#define MODBUS_POLL_MAX_MS 60000U

/* ========================================================================= */
/* Internal state                                                            */
/* ========================================================================= */

typedef struct
{
    bool initialised;
    config_params_t params;   /* live in-memory config */
    config_params_t snapshot; /* rollback copy (set by snapshot()) */
    bool snapshot_valid;
    const iconfig_store_t *store;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_buf;
    config_change_cb_t callbacks[CONFIG_SERVICE_MAX_CALLBACKS];
    uint8_t callback_count;
} ConfigServiceState;

static ConfigServiceState s_cs;

/* ========================================================================= */
/* Singleton vtables                                                         */
/* ========================================================================= */

static const iconfig_provider_t s_provider_vtable = {
    .get_params = config_service_get_params,
};

static const iconfig_manager_t s_manager_vtable = {
    .init = config_service_init,
    .apply_loaded = config_service_apply_loaded,
    .set_param = config_service_set_param,
    .validate_param = config_service_validate_param,
    .snapshot = config_service_snapshot,
    .restore_snapshot = config_service_restore_snapshot,
    .flush = config_service_flush,
    .register_change_callback = config_service_register_change_callback,
};

const iconfig_provider_t *const config_provider = &s_provider_vtable;
const iconfig_manager_t *const config_manager = &s_manager_vtable;

/* ========================================================================= */
/* Private helpers — forward declarations                                    */
/* ========================================================================= */

static void apply_defaults(config_params_t *p);
static config_service_err_t validate_param_internal(config_param_id_t id, const void *value,
                                                    const config_params_t *cur);
static void apply_param_to_struct(config_params_t *p, config_param_id_t id, const void *value);
static config_service_err_t persist_to_store(void);
static void fire_callbacks(config_param_id_t id);

/* ========================================================================= */
/* Public API — IConfigProvider                                              */
/* ========================================================================= */

const config_params_t *config_service_get_params(void)
{
    if (!s_cs.initialised)
    {
        return NULL;
    }
    /* Mutex-free read intentional: 16/32-bit scalar fields are atomically
     * accessible on Cortex-M4 (natural alignment, 32-bit bus).  String
     * fields must only be read under the mutex (CS-O2). */
    return &s_cs.params;
}

/* ========================================================================= */
/* Public API — IConfigManager                                              */
/* ========================================================================= */

config_service_err_t config_service_init(const iconfig_store_t *store)
{
    if (store == NULL)
    {
        return CONFIG_SERVICE_ERR_NULL_ARG;
    }

    (void) memset(&s_cs, 0, sizeof(s_cs));
    s_cs.store = store;

    s_cs.mutex = xSemaphoreCreateMutexStatic(&s_cs.mutex_buf);

    apply_defaults(&s_cs.params);

    s_cs.initialised = true;

    LOG_INFO(MODULE_TAG, "ConfigService initialised (defaults applied)");
    return CONFIG_SERVICE_OK;
}

config_service_err_t config_service_apply_loaded(const void *blob, uint32_t len)
{
    if (!s_cs.initialised)
    {
        return CONFIG_SERVICE_ERR_NOT_INIT;
    }
    if (blob == NULL)
    {
        return CONFIG_SERVICE_ERR_NULL_ARG;
    }
    if (len < (uint32_t) sizeof(config_blob_t))
    {
        return CONFIG_SERVICE_ERR_INVALID;
    }

    const config_blob_t *b = (const config_blob_t *) blob;

    (void) xSemaphoreTake(s_cs.mutex, portMAX_DELAY);

    if (b->schema_version != CONFIG_SCHEMA_VERSION)
    {
        /* Schema mismatch — discard stored blob, keep defaults (CS-O1). */
        LOG_WARN(MODULE_TAG, "schema ver mismatch (stored %lu vs %u), using defaults",
                 (unsigned long) b->schema_version, (unsigned) CONFIG_SCHEMA_VERSION);
        apply_defaults(&s_cs.params);
        (void) xSemaphoreGive(s_cs.mutex);
        return CONFIG_SERVICE_OK;
    }

    /* Validate each field individually (bounds-only); apply default for any
     * field that is out of range due to flash corruption or partial write. */
    config_params_t candidate;
    (void) memcpy(&candidate, &b->params, sizeof(config_params_t));

    /* Establish defaults as the authoritative baseline. */
    apply_defaults(&s_cs.params);

    if ((candidate.polling_interval_ms < POLL_INTERVAL_MIN_MS) ||
        (candidate.polling_interval_ms > POLL_INTERVAL_MAX_MS))
    {
        candidate.polling_interval_ms = DEFAULT_POLL_INTERVAL_MS;
    }

    if ((candidate.filter_alpha == 0U) || (candidate.filter_alpha >= FILTER_ALPHA_MAX))
    {
        candidate.filter_alpha = DEFAULT_FILTER_ALPHA;
    }

    if ((candidate.temp_range_min < (int16_t) TEMP_ABS_MIN) ||
        ((int32_t) candidate.temp_range_min >=
         (int32_t) candidate.temp_range_max - (int32_t) TEMP_RANGE_GAP_MIN))
    {
        candidate.temp_range_min = (int16_t) DEFAULT_TEMP_RANGE_MIN;
    }
    if ((candidate.temp_range_max > (int16_t) TEMP_ABS_MAX) ||
        ((int32_t) candidate.temp_range_max <=
         (int32_t) candidate.temp_range_min + (int32_t) TEMP_RANGE_GAP_MIN))
    {
        candidate.temp_range_max = (int16_t) DEFAULT_TEMP_RANGE_MAX;
    }

    /* uint16_t humidity_range_min is always >= HUMIDITY_ABS_MIN (0). */
    if ((uint32_t) candidate.humidity_range_min + HUMIDITY_RANGE_GAP_MIN >
        (uint32_t) candidate.humidity_range_max)
    {
        candidate.humidity_range_min = DEFAULT_HUMIDITY_RANGE_MIN;
    }
    if ((candidate.humidity_range_max > HUMIDITY_ABS_MAX) ||
        ((uint32_t) candidate.humidity_range_max <
         (uint32_t) candidate.humidity_range_min + HUMIDITY_RANGE_GAP_MIN))
    {
        candidate.humidity_range_max = DEFAULT_HUMIDITY_RANGE_MAX;
    }

    /* uint16_t pressure_range_min is always >= PRESSURE_ABS_MIN (0). */
    if ((uint32_t) candidate.pressure_range_min + PRESSURE_RANGE_GAP_MIN >
        (uint32_t) candidate.pressure_range_max)
    {
        candidate.pressure_range_min = DEFAULT_PRESSURE_RANGE_MIN;
    }
    if ((candidate.pressure_range_max > PRESSURE_ABS_MAX) ||
        ((uint32_t) candidate.pressure_range_max <
         (uint32_t) candidate.pressure_range_min + PRESSURE_RANGE_GAP_MIN))
    {
        candidate.pressure_range_max = DEFAULT_PRESSURE_RANGE_MAX;
    }

    /* Alarm bounds (use defaults if out of physical range). */
    if (((int32_t) candidate.temp_alarm_high < (int32_t) candidate.temp_range_min) ||
        ((int32_t) candidate.temp_alarm_high > (int32_t) candidate.temp_range_max))
    {
        candidate.temp_alarm_high = (int16_t) DEFAULT_TEMP_ALARM_HIGH;
    }
    if (((int32_t) candidate.temp_alarm_low < (int32_t) candidate.temp_range_min) ||
        ((int32_t) candidate.temp_alarm_low >= (int32_t) candidate.temp_alarm_high))
    {
        candidate.temp_alarm_low = (int16_t) DEFAULT_TEMP_ALARM_LOW;
    }
    if ((candidate.temp_hysteresis <= 0) ||
        (2 * (int32_t) candidate.temp_hysteresis >=
         (int32_t) candidate.temp_alarm_high - (int32_t) candidate.temp_alarm_low))
    {
        candidate.temp_hysteresis = (int16_t) DEFAULT_TEMP_HYSTERESIS;
    }

    if (((uint32_t) candidate.humidity_alarm_high < (uint32_t) candidate.humidity_range_min) ||
        ((uint32_t) candidate.humidity_alarm_high > (uint32_t) candidate.humidity_range_max))
    {
        candidate.humidity_alarm_high = DEFAULT_HUMIDITY_ALARM_HIGH;
    }
    if (((uint32_t) candidate.humidity_alarm_low < (uint32_t) candidate.humidity_range_min) ||
        ((uint32_t) candidate.humidity_alarm_low >= (uint32_t) candidate.humidity_alarm_high))
    {
        candidate.humidity_alarm_low = DEFAULT_HUMIDITY_ALARM_LOW;
    }
    if ((candidate.humidity_hysteresis == 0U) ||
        (2U * (uint32_t) candidate.humidity_hysteresis >=
         (uint32_t) candidate.humidity_alarm_high - (uint32_t) candidate.humidity_alarm_low))
    {
        candidate.humidity_hysteresis = DEFAULT_HUMIDITY_HYSTERESIS;
    }

    if (((uint32_t) candidate.pressure_alarm_high < (uint32_t) candidate.pressure_range_min) ||
        ((uint32_t) candidate.pressure_alarm_high > (uint32_t) candidate.pressure_range_max))
    {
        candidate.pressure_alarm_high = DEFAULT_PRESSURE_ALARM_HIGH;
    }
    if (((uint32_t) candidate.pressure_alarm_low < (uint32_t) candidate.pressure_range_min) ||
        ((uint32_t) candidate.pressure_alarm_low >= (uint32_t) candidate.pressure_alarm_high))
    {
        candidate.pressure_alarm_low = DEFAULT_PRESSURE_ALARM_LOW;
    }
    if ((candidate.pressure_hysteresis == 0U) ||
        (2U * (uint32_t) candidate.pressure_hysteresis >=
         (uint32_t) candidate.pressure_alarm_high - (uint32_t) candidate.pressure_alarm_low))
    {
        candidate.pressure_hysteresis = DEFAULT_PRESSURE_HYSTERESIS;
    }

    if ((candidate.modbus_slave_addr < MODBUS_ADDR_MIN) ||
        (candidate.modbus_slave_addr > MODBUS_ADDR_MAX))
    {
        candidate.modbus_slave_addr = DEFAULT_MODBUS_SLAVE_ADDR;
    }
    if ((candidate.modbus_poll_period_ms < MODBUS_POLL_MIN_MS) ||
        (candidate.modbus_poll_period_ms > MODBUS_POLL_MAX_MS))
    {
        candidate.modbus_poll_period_ms = DEFAULT_MODBUS_POLL_PERIOD_MS;
    }

    (void) memcpy(&s_cs.params, &candidate, sizeof(config_params_t));

    (void) xSemaphoreGive(s_cs.mutex);

    LOG_INFO(MODULE_TAG, "Config loaded from store");
    return CONFIG_SERVICE_OK;
}

config_service_err_t config_service_set_param(config_param_id_t id, const void *value)
{
    if (!s_cs.initialised)
    {
        return CONFIG_SERVICE_ERR_NOT_INIT;
    }
    if (value == NULL)
    {
        return CONFIG_SERVICE_ERR_NULL_ARG;
    }

    (void) xSemaphoreTake(s_cs.mutex, portMAX_DELAY);

    config_service_err_t v_err = validate_param_internal(id, value, &s_cs.params);
    if (v_err != CONFIG_SERVICE_OK)
    {
        (void) xSemaphoreGive(s_cs.mutex);

        return v_err;
    }

    apply_param_to_struct(&s_cs.params, id, value);

    config_service_err_t p_err = persist_to_store();
    if (p_err != CONFIG_SERVICE_OK)
    {
        LOG_WARN(MODULE_TAG, "ConfigStore write failed (%d)", (int) p_err);
    }

    (void) xSemaphoreGive(s_cs.mutex);

    if (p_err == CONFIG_SERVICE_OK)
    {
        fire_callbacks(id);
    }

    return p_err;
}

config_service_err_t config_service_validate_param(config_param_id_t id, const void *value)
{
    if (!s_cs.initialised)
    {
        return CONFIG_SERVICE_ERR_NOT_INIT;
    }
    if (value == NULL)
    {
        return CONFIG_SERVICE_ERR_NULL_ARG;
    }
    /* Mutex-free: reads 16/32-bit scalar fields atomically on Cortex-M4 (CS-O2). */
    return validate_param_internal(id, value, &s_cs.params);
}

config_service_err_t config_service_snapshot(void)
{
    if (!s_cs.initialised)
    {
        return CONFIG_SERVICE_ERR_NOT_INIT;
    }

    (void) xSemaphoreTake(s_cs.mutex, portMAX_DELAY);
    (void) memcpy(&s_cs.snapshot, &s_cs.params, sizeof(config_params_t));
    s_cs.snapshot_valid = true;
    (void) xSemaphoreGive(s_cs.mutex);

    LOG_INFO(MODULE_TAG, "Config snapshot saved");
    return CONFIG_SERVICE_OK;
}

config_service_err_t config_service_restore_snapshot(void)
{
    if (!s_cs.initialised)
    {
        return CONFIG_SERVICE_ERR_NOT_INIT;
    }
    if (!s_cs.snapshot_valid)
    {
        return CONFIG_SERVICE_ERR_INVALID;
    }

    (void) xSemaphoreTake(s_cs.mutex, portMAX_DELAY);
    (void) memcpy(&s_cs.params, &s_cs.snapshot, sizeof(config_params_t));
    config_service_err_t err = persist_to_store();
    (void) xSemaphoreGive(s_cs.mutex);

    if (err == CONFIG_SERVICE_OK)
    {
        LOG_INFO(MODULE_TAG, "Config restored from snapshot");
    }
    return err;
}

config_service_err_t config_service_flush(void)
{
    if (!s_cs.initialised)
    {
        return CONFIG_SERVICE_ERR_NOT_INIT;
    }

    (void) xSemaphoreTake(s_cs.mutex, portMAX_DELAY);
    config_service_err_t err = persist_to_store();
    (void) xSemaphoreGive(s_cs.mutex);

    return err;
}

config_service_err_t config_service_register_change_callback(config_change_cb_t cb)
{
    if (cb == NULL)
    {
        return CONFIG_SERVICE_ERR_NULL_ARG;
    }
    if (s_cs.callback_count >= CONFIG_SERVICE_MAX_CALLBACKS)
    {
        return CONFIG_SERVICE_ERR_INVALID;
    }

    s_cs.callbacks[s_cs.callback_count] = cb;
    s_cs.callback_count++;
    return CONFIG_SERVICE_OK;
}

/* ========================================================================= */
/* Private helpers                                                           */
/* ========================================================================= */

static void apply_defaults(config_params_t *p)
{
    p->polling_interval_ms = DEFAULT_POLL_INTERVAL_MS;
    p->filter_alpha = DEFAULT_FILTER_ALPHA;
    p->temp_range_min = (int16_t) DEFAULT_TEMP_RANGE_MIN;
    p->temp_range_max = (int16_t) DEFAULT_TEMP_RANGE_MAX;
    p->humidity_range_min = DEFAULT_HUMIDITY_RANGE_MIN;
    p->humidity_range_max = DEFAULT_HUMIDITY_RANGE_MAX;
    p->pressure_range_min = DEFAULT_PRESSURE_RANGE_MIN;
    p->pressure_range_max = DEFAULT_PRESSURE_RANGE_MAX;
    p->temp_alarm_high = (int16_t) DEFAULT_TEMP_ALARM_HIGH;
    p->temp_alarm_low = (int16_t) DEFAULT_TEMP_ALARM_LOW;
    p->temp_hysteresis = (int16_t) DEFAULT_TEMP_HYSTERESIS;
    p->humidity_alarm_high = DEFAULT_HUMIDITY_ALARM_HIGH;
    p->humidity_alarm_low = DEFAULT_HUMIDITY_ALARM_LOW;
    p->humidity_hysteresis = DEFAULT_HUMIDITY_HYSTERESIS;
    p->pressure_alarm_high = DEFAULT_PRESSURE_ALARM_HIGH;
    p->pressure_alarm_low = DEFAULT_PRESSURE_ALARM_LOW;
    p->pressure_hysteresis = DEFAULT_PRESSURE_HYSTERESIS;
    p->modbus_slave_addr = DEFAULT_MODBUS_SLAVE_ADDR;
    p->modbus_poll_period_ms = DEFAULT_MODBUS_POLL_PERIOD_MS;

#if defined(BOARD_GATEWAY)
    (void) memset(p->mqtt_broker, 0, sizeof(p->mqtt_broker));
    p->mqtt_port = 8883U;
    p->telemetry_interval_ms = 60000U;
    p->health_interval_ms = 600000U;
    (void) memset(p->ntp_servers, 0, sizeof(p->ntp_servers));
    p->ntp_server_count = 0U;
#endif /* BOARD_GATEWAY */
}

static config_service_err_t validate_param_internal(config_param_id_t id, const void *value,
                                                    const config_params_t *cur)
{
    switch (id)
    {
    case CONFIG_PARAM_POLL_INTERVAL:
    {
        uint32_t v = *(const uint32_t *) value;
        return ((v >= POLL_INTERVAL_MIN_MS) && (v <= POLL_INTERVAL_MAX_MS))
                   ? CONFIG_SERVICE_OK
                   : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_FILTER_ALPHA:
    {
        uint16_t v = *(const uint16_t *) value;
        /* (0, 1000) exclusive in per-mille units. */
        return ((v > 0U) && (v < FILTER_ALPHA_MAX)) ? CONFIG_SERVICE_OK
                                                    : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_TEMP_RANGE_MIN:
    {
        int16_t v = *(const int16_t *) value;
        return (((int32_t) v >= (int32_t) TEMP_ABS_MIN) &&
                ((int32_t) v <= (int32_t) cur->temp_range_max - (int32_t) TEMP_RANGE_GAP_MIN))
                   ? CONFIG_SERVICE_OK
                   : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_TEMP_RANGE_MAX:
    {
        int16_t v = *(const int16_t *) value;
        return (((int32_t) v >= (int32_t) cur->temp_range_min + (int32_t) TEMP_RANGE_GAP_MIN) &&
                ((int32_t) v <= (int32_t) TEMP_ABS_MAX))
                   ? CONFIG_SERVICE_OK
                   : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_HUMIDITY_RANGE_MIN:
    {
        uint16_t v = *(const uint16_t *) value;
        /* uint16_t is always >= HUMIDITY_ABS_MIN (0); no lower-bound check needed. */
        return ((uint32_t) v + HUMIDITY_RANGE_GAP_MIN <= (uint32_t) cur->humidity_range_max)
                   ? CONFIG_SERVICE_OK
                   : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_HUMIDITY_RANGE_MAX:
    {
        uint16_t v = *(const uint16_t *) value;
        return (((uint32_t) v <= HUMIDITY_ABS_MAX) &&
                ((uint32_t) v >= (uint32_t) cur->humidity_range_min + HUMIDITY_RANGE_GAP_MIN))
                   ? CONFIG_SERVICE_OK
                   : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_PRESSURE_RANGE_MIN:
    {
        uint16_t v = *(const uint16_t *) value;
        /* uint16_t is always >= PRESSURE_ABS_MIN (0); no lower-bound check needed. */
        return ((uint32_t) v + PRESSURE_RANGE_GAP_MIN <= (uint32_t) cur->pressure_range_max)
                   ? CONFIG_SERVICE_OK
                   : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_PRESSURE_RANGE_MAX:
    {
        uint16_t v = *(const uint16_t *) value;
        return (((uint32_t) v <= PRESSURE_ABS_MAX) &&
                ((uint32_t) v >= (uint32_t) cur->pressure_range_min + PRESSURE_RANGE_GAP_MIN))
                   ? CONFIG_SERVICE_OK
                   : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_TEMP_ALARM_HIGH:
    {
        int16_t v = *(const int16_t *) value;
        /* Must be within physical range and strictly above alarm_low + hysteresis. */
        return (((int32_t) v >= (int32_t) cur->temp_range_min) &&
                ((int32_t) v <= (int32_t) cur->temp_range_max) &&
                ((int32_t) v > (int32_t) cur->temp_alarm_low + (int32_t) cur->temp_hysteresis))
                   ? CONFIG_SERVICE_OK
                   : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_TEMP_ALARM_LOW:
    {
        int16_t v = *(const int16_t *) value;
        /* Must be within physical range and low enough to preserve hysteresis gap. */
        return (((int32_t) v >= (int32_t) cur->temp_range_min) &&
                ((int32_t) v + (int32_t) cur->temp_hysteresis < (int32_t) cur->temp_alarm_high))
                   ? CONFIG_SERVICE_OK
                   : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_TEMP_HYSTERESIS:
    {
        int16_t v = *(const int16_t *) value;
        /* Must be positive and less than half the alarm span. */
        int32_t span = (int32_t) cur->temp_alarm_high - (int32_t) cur->temp_alarm_low;
        return (((int32_t) v > 0) && (2 * (int32_t) v < span)) ? CONFIG_SERVICE_OK
                                                               : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_HUMIDITY_ALARM_HIGH:
    {
        uint16_t v = *(const uint16_t *) value;
        return (((uint32_t) v >= (uint32_t) cur->humidity_range_min) &&
                ((uint32_t) v <= (uint32_t) cur->humidity_range_max) &&
                ((uint32_t) v >
                 (uint32_t) cur->humidity_alarm_low + (uint32_t) cur->humidity_hysteresis))
                   ? CONFIG_SERVICE_OK
                   : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_HUMIDITY_ALARM_LOW:
    {
        uint16_t v = *(const uint16_t *) value;
        return (((uint32_t) v >= (uint32_t) cur->humidity_range_min) &&
                ((uint32_t) v + (uint32_t) cur->humidity_hysteresis <
                 (uint32_t) cur->humidity_alarm_high))
                   ? CONFIG_SERVICE_OK
                   : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_HUMIDITY_HYSTERESIS:
    {
        uint16_t v = *(const uint16_t *) value;
        uint32_t span = (uint32_t) cur->humidity_alarm_high - (uint32_t) cur->humidity_alarm_low;
        return (((uint32_t) v > 0U) && (2U * (uint32_t) v < span)) ? CONFIG_SERVICE_OK
                                                                   : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_PRESSURE_ALARM_HIGH:
    {
        uint16_t v = *(const uint16_t *) value;
        return (((uint32_t) v >= (uint32_t) cur->pressure_range_min) &&
                ((uint32_t) v <= (uint32_t) cur->pressure_range_max) &&
                ((uint32_t) v >
                 (uint32_t) cur->pressure_alarm_low + (uint32_t) cur->pressure_hysteresis))
                   ? CONFIG_SERVICE_OK
                   : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_PRESSURE_ALARM_LOW:
    {
        uint16_t v = *(const uint16_t *) value;
        return (((uint32_t) v >= (uint32_t) cur->pressure_range_min) &&
                ((uint32_t) v + (uint32_t) cur->pressure_hysteresis <
                 (uint32_t) cur->pressure_alarm_high))
                   ? CONFIG_SERVICE_OK
                   : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_PRESSURE_HYSTERESIS:
    {
        uint16_t v = *(const uint16_t *) value;
        uint32_t span = (uint32_t) cur->pressure_alarm_high - (uint32_t) cur->pressure_alarm_low;
        return (((uint32_t) v > 0U) && (2U * (uint32_t) v < span)) ? CONFIG_SERVICE_OK
                                                                   : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_MODBUS_SLAVE_ADDR:
    {
        uint8_t v = *(const uint8_t *) value;
        return ((v >= MODBUS_ADDR_MIN) && (v <= MODBUS_ADDR_MAX)) ? CONFIG_SERVICE_OK
                                                                  : CONFIG_SERVICE_ERR_INVALID;
    }

    case CONFIG_PARAM_MODBUS_POLL_PERIOD:
    {
        uint32_t v = *(const uint32_t *) value;
        return ((v >= MODBUS_POLL_MIN_MS) && (v <= MODBUS_POLL_MAX_MS))
                   ? CONFIG_SERVICE_OK
                   : CONFIG_SERVICE_ERR_INVALID;
    }

    default:
        return CONFIG_SERVICE_ERR_INVALID;
    }
}

static void apply_param_to_struct(config_params_t *p, config_param_id_t id, const void *value)
{
    switch (id)
    {
    case CONFIG_PARAM_POLL_INTERVAL:
        p->polling_interval_ms = *(const uint32_t *) value;
        break;
    case CONFIG_PARAM_FILTER_ALPHA:
        p->filter_alpha = *(const uint16_t *) value;
        break;
    case CONFIG_PARAM_TEMP_RANGE_MIN:
        p->temp_range_min = *(const int16_t *) value;
        break;
    case CONFIG_PARAM_TEMP_RANGE_MAX:
        p->temp_range_max = *(const int16_t *) value;
        break;
    case CONFIG_PARAM_HUMIDITY_RANGE_MIN:
        p->humidity_range_min = *(const uint16_t *) value;
        break;
    case CONFIG_PARAM_HUMIDITY_RANGE_MAX:
        p->humidity_range_max = *(const uint16_t *) value;
        break;
    case CONFIG_PARAM_PRESSURE_RANGE_MIN:
        p->pressure_range_min = *(const uint16_t *) value;
        break;
    case CONFIG_PARAM_PRESSURE_RANGE_MAX:
        p->pressure_range_max = *(const uint16_t *) value;
        break;
    case CONFIG_PARAM_TEMP_ALARM_HIGH:
        p->temp_alarm_high = *(const int16_t *) value;
        break;
    case CONFIG_PARAM_TEMP_ALARM_LOW:
        p->temp_alarm_low = *(const int16_t *) value;
        break;
    case CONFIG_PARAM_TEMP_HYSTERESIS:
        p->temp_hysteresis = *(const int16_t *) value;
        break;
    case CONFIG_PARAM_HUMIDITY_ALARM_HIGH:
        p->humidity_alarm_high = *(const uint16_t *) value;
        break;
    case CONFIG_PARAM_HUMIDITY_ALARM_LOW:
        p->humidity_alarm_low = *(const uint16_t *) value;
        break;
    case CONFIG_PARAM_HUMIDITY_HYSTERESIS:
        p->humidity_hysteresis = *(const uint16_t *) value;
        break;
    case CONFIG_PARAM_PRESSURE_ALARM_HIGH:
        p->pressure_alarm_high = *(const uint16_t *) value;
        break;
    case CONFIG_PARAM_PRESSURE_ALARM_LOW:
        p->pressure_alarm_low = *(const uint16_t *) value;
        break;
    case CONFIG_PARAM_PRESSURE_HYSTERESIS:
        p->pressure_hysteresis = *(const uint16_t *) value;
        break;
    case CONFIG_PARAM_MODBUS_SLAVE_ADDR:
        p->modbus_slave_addr = *(const uint8_t *) value;
        break;
    case CONFIG_PARAM_MODBUS_POLL_PERIOD:
        p->modbus_poll_period_ms = *(const uint32_t *) value;
        break;
    default:
        break;
    }
}

static config_service_err_t persist_to_store(void)
{
    config_blob_t blob;
    blob.schema_version = CONFIG_SCHEMA_VERSION;
    (void) memcpy(&blob.params, &s_cs.params, sizeof(config_params_t));

    config_store_err_t cs_err = s_cs.store->save(&blob, sizeof(config_blob_t));
    if (cs_err != CONFIG_STORE_OK)
    {
        return CONFIG_SERVICE_ERR_PERSIST;
    }
    return CONFIG_SERVICE_OK;
}

static void fire_callbacks(config_param_id_t id)
{
    for (uint8_t i = 0U; i < s_cs.callback_count; i++)
    {
        if (s_cs.callbacks[i] != NULL)
        {
            s_cs.callbacks[i](id);
        }
    }
}

/* ========================================================================= */
/* Test-only hooks                                                           */
/* ========================================================================= */

#ifdef TEST
void config_service_reset_for_test(void)
{
    (void) memset(&s_cs, 0, sizeof(s_cs));
}
#endif /* TEST */
