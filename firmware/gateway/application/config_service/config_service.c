/**
 * @file config_service.c
 * @brief ConfigService implementation (Gateway) — validate, apply, persist, notify.
 *
 * @see docs/lld/application/config-service-lld.md
 */

#include "config_service.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

#ifdef TEST
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

#define MODULE_TAG "CFG"

/* ======================================================================= */
/* Defaults and limits                                                      */
/* ======================================================================= */

#define DEFAULT_POLLING_INTERVAL_MS (1000U)
#define DEFAULT_FILTER_ALPHA (100U)
#define DEFAULT_MODBUS_SLAVE_ADDR (1U)
#define DEFAULT_MODBUS_POLL_PERIOD_MS (1000U)
#define DEFAULT_MQTT_PORT (8883U)
#define DEFAULT_TELEMETRY_INTERVAL_MS (60000U)
#define DEFAULT_HEALTH_INTERVAL_MS (600000U)

#define POLL_MS_MIN (100)
#define POLL_MS_MAX (60000)
#define ALPHA_MIN (1)
#define ALPHA_MAX (999)
#define SLAVE_ADDR_MIN (1)
#define SLAVE_ADDR_MAX (247)
#define MQTT_PORT_MIN (1)
#define MQTT_PORT_MAX (65535)
#define TELEMETRY_MS_MIN (1000)
#define TELEMETRY_MS_MAX (3600000)
#define HEALTH_MS_MIN (10000)
#define HEALTH_MS_MAX (86400000)
#define NTP_COUNT_MIN (1)

/* Alarm band rule: high - low must exceed twice the hysteresis (CFG-D7). */
#define HYSTERESIS_BAND_FACTOR (2)

/* ======================================================================= */
/* Field table                                                              */
/* ======================================================================= */

typedef enum
{
    KIND_U8,
    KIND_U16,
    KIND_I16,
    KIND_U32,
    KIND_STRING,
    KIND_NTP_BLOCK,
} field_kind_t;

typedef struct
{
    uint16_t offset;
    uint16_t size;
    field_kind_t kind;
} field_desc_t;

#define FIELD(member, kind)                                                                        \
    {                                                                                              \
        (uint16_t) offsetof(config_params_t, member),                                              \
            (uint16_t) sizeof(((config_params_t *) 0)->member), (kind)                             \
    }

static const field_desc_t s_fields[CONFIG_PARAM_COUNT] = {
    [CONFIG_PARAM_POLL_INTERVAL] = FIELD(polling_interval_ms, KIND_U32),
    [CONFIG_PARAM_FILTER_ALPHA] = FIELD(filter_alpha, KIND_U16),
    [CONFIG_PARAM_TEMP_RANGE_MIN] = FIELD(temp_range_min, KIND_I16),
    [CONFIG_PARAM_TEMP_RANGE_MAX] = FIELD(temp_range_max, KIND_I16),
    [CONFIG_PARAM_HUMIDITY_RANGE_MIN] = FIELD(humidity_range_min, KIND_U16),
    [CONFIG_PARAM_HUMIDITY_RANGE_MAX] = FIELD(humidity_range_max, KIND_U16),
    [CONFIG_PARAM_PRESSURE_RANGE_MIN] = FIELD(pressure_range_min, KIND_U16),
    [CONFIG_PARAM_PRESSURE_RANGE_MAX] = FIELD(pressure_range_max, KIND_U16),
    [CONFIG_PARAM_TEMP_ALARM_HIGH] = FIELD(temp_alarm_high, KIND_I16),
    [CONFIG_PARAM_TEMP_ALARM_LOW] = FIELD(temp_alarm_low, KIND_I16),
    [CONFIG_PARAM_TEMP_HYSTERESIS] = FIELD(temp_hysteresis, KIND_I16),
    [CONFIG_PARAM_HUMIDITY_ALARM_HIGH] = FIELD(humidity_alarm_high, KIND_U16),
    [CONFIG_PARAM_HUMIDITY_ALARM_LOW] = FIELD(humidity_alarm_low, KIND_U16),
    [CONFIG_PARAM_HUMIDITY_HYSTERESIS] = FIELD(humidity_hysteresis, KIND_U16),
    [CONFIG_PARAM_PRESSURE_ALARM_HIGH] = FIELD(pressure_alarm_high, KIND_U16),
    [CONFIG_PARAM_PRESSURE_ALARM_LOW] = FIELD(pressure_alarm_low, KIND_U16),
    [CONFIG_PARAM_PRESSURE_HYSTERESIS] = FIELD(pressure_hysteresis, KIND_U16),
    [CONFIG_PARAM_MODBUS_SLAVE_ADDR] = FIELD(modbus_slave_addr, KIND_U8),
    [CONFIG_PARAM_MODBUS_POLL_PERIOD] = FIELD(modbus_poll_period_ms, KIND_U32),
    [CONFIG_PARAM_MQTT_BROKER] = FIELD(mqtt_broker, KIND_STRING),
    [CONFIG_PARAM_MQTT_PORT] = FIELD(mqtt_port, KIND_U16),
    [CONFIG_PARAM_TELEMETRY_INTERVAL] = FIELD(telemetry_interval_ms, KIND_U32),
    [CONFIG_PARAM_HEALTH_INTERVAL] = FIELD(health_interval_ms, KIND_U32),
    [CONFIG_PARAM_NTP_SERVERS] = FIELD(ntp_servers, KIND_NTP_BLOCK),
    [CONFIG_PARAM_NTP_SERVER_COUNT] = FIELD(ntp_server_count, KIND_U8),
};

/* Sensor channels share one rule set: range, alarm band and hysteresis (CFG-D7, CFG-D10). */
typedef struct
{
    config_param_id_t range_min;
    config_param_id_t range_max;
    config_param_id_t high;
    config_param_id_t low;
    config_param_id_t hysteresis;
    int32_t abs_min; /* hard limit of the sensor, same unit as the parameters */
    int32_t abs_max;
    int32_t gap; /* smallest allowed range_max - range_min */
} channel_rule_t;

static const channel_rule_t s_channels[] = {
    {CONFIG_PARAM_TEMP_RANGE_MIN, CONFIG_PARAM_TEMP_RANGE_MAX, CONFIG_PARAM_TEMP_ALARM_HIGH,
     CONFIG_PARAM_TEMP_ALARM_LOW, CONFIG_PARAM_TEMP_HYSTERESIS, -5500, 12500, 100},
    {CONFIG_PARAM_HUMIDITY_RANGE_MIN, CONFIG_PARAM_HUMIDITY_RANGE_MAX,
     CONFIG_PARAM_HUMIDITY_ALARM_HIGH, CONFIG_PARAM_HUMIDITY_ALARM_LOW,
     CONFIG_PARAM_HUMIDITY_HYSTERESIS, 0, 10000, 100},
    {CONFIG_PARAM_PRESSURE_RANGE_MIN, CONFIG_PARAM_PRESSURE_RANGE_MAX,
     CONFIG_PARAM_PRESSURE_ALARM_HIGH, CONFIG_PARAM_PRESSURE_ALARM_LOW,
     CONFIG_PARAM_PRESSURE_HYSTERESIS, 2600, 12600, 10},
};
#define CHANNEL_COUNT ((uint32_t) (sizeof(s_channels) / sizeof(s_channels[0])))

/* ======================================================================= */
/* State                                                                    */
/* ======================================================================= */

static struct
{
    bool initialised;
    config_store_handle_t store;
    config_params_t params;
    config_params_t snapshot;
    bool snapshot_valid;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_buf;
    config_change_cb_t change_cbs[CONFIG_MAX_CHANGE_CALLBACKS];
    uint8_t change_cb_count;
} s_cs;

static config_service_err_t prv_set_param(void *ctx, config_param_id_t id, const void *value);
static config_service_err_t prv_validate_param(void *ctx, config_param_id_t id, const void *value);
static const config_params_t *prv_get_params(void *ctx);

static const iconfig_provider_t s_provider = {.ctx = NULL, .get_params = prv_get_params};
static const iconfig_manager_t s_manager = {
    .ctx = NULL, .set_param = prv_set_param, .validate_param = prv_validate_param};

/* ======================================================================= */
/* Field access                                                             */
/* ======================================================================= */

static int64_t prv_read_numeric(field_kind_t kind, const void *src)
{
    switch (kind)
    {
    case KIND_U8:
    {
        uint8_t v = 0U;
        (void) memcpy(&v, src, sizeof(v));
        return (int64_t) v;
    }
    case KIND_U16:
    {
        uint16_t v = 0U;
        (void) memcpy(&v, src, sizeof(v));
        return (int64_t) v;
    }
    case KIND_I16:
    {
        int16_t v = 0;
        (void) memcpy(&v, src, sizeof(v));
        return (int64_t) v;
    }
    case KIND_U32:
    {
        uint32_t v = 0U;
        (void) memcpy(&v, src, sizeof(v));
        return (int64_t) v;
    }
    default:
        return 0;
    }
}

static const void *prv_field_ptr(const config_params_t *p, config_param_id_t id)
{
    return (const uint8_t *) p + s_fields[id].offset;
}

static int64_t prv_get_numeric(const config_params_t *p, config_param_id_t id)
{
    return prv_read_numeric(s_fields[id].kind, prv_field_ptr(p, id));
}

static void prv_write_field(config_params_t *p, config_param_id_t id, const void *value)
{
    uint8_t *dst = (uint8_t *) p + s_fields[id].offset;
    const field_desc_t *f = &s_fields[id];

    if (f->kind == KIND_STRING)
    {
        (void) memset(dst, 0, f->size);
        (void) memcpy(dst, value, strnlen((const char *) value, (size_t) f->size - 1U));
    }
    else
    {
        (void) memcpy(dst, value, f->size);
    }
}

/* ======================================================================= */
/* Validation                                                               */
/* ======================================================================= */

static bool prv_in_range(int64_t v, int64_t lo, int64_t hi)
{
    return (v >= lo) && (v <= hi);
}

/* Validate one channel parameter against the other values in @p ctx (CFG-D7). */
static bool prv_validate_channel(const channel_rule_t *ch, config_param_id_t id, int64_t v,
                                 const config_params_t *ctx)
{
    const int64_t rmin = prv_get_numeric(ctx, ch->range_min);
    const int64_t rmax = prv_get_numeric(ctx, ch->range_max);
    const int64_t high = prv_get_numeric(ctx, ch->high);
    const int64_t low = prv_get_numeric(ctx, ch->low);
    const int64_t hyst = prv_get_numeric(ctx, ch->hysteresis);

    if (id == ch->range_min)
    {
        return prv_in_range(v, ch->abs_min, rmax - ch->gap);
    }
    if (id == ch->range_max)
    {
        return prv_in_range(v, rmin + ch->gap, ch->abs_max);
    }
    if (id == ch->high)
    {
        return prv_in_range(v, rmin, rmax) && ((v - low) > (HYSTERESIS_BAND_FACTOR * hyst));
    }
    if (id == ch->low)
    {
        return prv_in_range(v, rmin, rmax) && ((high - v) > (HYSTERESIS_BAND_FACTOR * hyst));
    }
    /* hysteresis */
    return (v > 0) && ((high - low) > (HYSTERESIS_BAND_FACTOR * v));
}

static bool prv_strings_terminated(const char *base, uint32_t count, uint32_t stride)
{
    for (uint32_t i = 0U; i < count; i++)
    {
        if (strnlen(&base[i * stride], stride) >= stride)
        {
            return false;
        }
    }
    return true;
}

static bool prv_validate_ntp_count(int64_t count, const config_params_t *ctx)
{
    if (!prv_in_range(count, NTP_COUNT_MIN, CONFIG_MAX_NTP_SERVERS))
    {
        return false;
    }
    for (int64_t i = 0; i < count; i++)
    {
        if (ctx->ntp_servers[i][0] == '\0')
        {
            return false;
        }
    }
    return true;
}

static bool prv_validate_scalar(config_param_id_t id, int64_t v, const config_params_t *ctx)
{
    switch (id)
    {
    case CONFIG_PARAM_POLL_INTERVAL:
    case CONFIG_PARAM_MODBUS_POLL_PERIOD:
        return prv_in_range(v, POLL_MS_MIN, POLL_MS_MAX);
    case CONFIG_PARAM_FILTER_ALPHA:
        return prv_in_range(v, ALPHA_MIN, ALPHA_MAX);
    case CONFIG_PARAM_MODBUS_SLAVE_ADDR:
        return prv_in_range(v, SLAVE_ADDR_MIN, SLAVE_ADDR_MAX);
    case CONFIG_PARAM_MQTT_PORT:
        return prv_in_range(v, MQTT_PORT_MIN, MQTT_PORT_MAX);
    case CONFIG_PARAM_TELEMETRY_INTERVAL:
        return prv_in_range(v, TELEMETRY_MS_MIN, TELEMETRY_MS_MAX);
    case CONFIG_PARAM_HEALTH_INTERVAL:
        return prv_in_range(v, HEALTH_MS_MIN, HEALTH_MS_MAX);
    case CONFIG_PARAM_NTP_SERVER_COUNT:
        return prv_validate_ntp_count(v, ctx);
    default:
        return false;
    }
}

/* Check @p value for @p id against the values in @p ctx. */
static bool prv_validate(config_param_id_t id, const void *value, const config_params_t *ctx)
{
    const field_desc_t *f = &s_fields[id];

    if (f->kind == KIND_STRING)
    {
        const size_t len = strnlen((const char *) value, f->size);
        return (len > 0U) && (len < (size_t) f->size);
    }
    if (f->kind == KIND_NTP_BLOCK)
    {
        return prv_strings_terminated((const char *) value, CONFIG_MAX_NTP_SERVERS,
                                      CONFIG_MAX_NTP_HOST_LEN);
    }

    const int64_t v = prv_read_numeric(f->kind, value);
    for (uint32_t i = 0U; i < CHANNEL_COUNT; i++)
    {
        const channel_rule_t *ch = &s_channels[i];
        if ((id == ch->range_min) || (id == ch->range_max) || (id == ch->high) || (id == ch->low) ||
            (id == ch->hysteresis))
        {
            return prv_validate_channel(ch, id, v, ctx);
        }
    }
    return prv_validate_scalar(id, v, ctx);
}

/* ======================================================================= */
/* Defaults, persistence                                                    */
/* ======================================================================= */

static void prv_apply_defaults(config_params_t *p)
{
    (void) memset(p, 0, sizeof(*p));
    p->polling_interval_ms = DEFAULT_POLLING_INTERVAL_MS;
    p->filter_alpha = DEFAULT_FILTER_ALPHA;
    p->temp_range_min = -4000;
    p->temp_range_max = 8500;
    p->humidity_range_min = 0U;
    p->humidity_range_max = 10000U;
    p->pressure_range_min = 3000U;
    p->pressure_range_max = 11000U;
    p->temp_alarm_high = 4000;
    p->temp_alarm_low = 0;
    p->temp_hysteresis = 100;
    p->humidity_alarm_high = 8000U;
    p->humidity_alarm_low = 2000U;
    p->humidity_hysteresis = 200U;
    p->pressure_alarm_high = 10500U;
    p->pressure_alarm_low = 9500U;
    p->pressure_hysteresis = 50U;
    p->modbus_slave_addr = DEFAULT_MODBUS_SLAVE_ADDR;
    p->modbus_poll_period_ms = DEFAULT_MODBUS_POLL_PERIOD_MS;
    p->mqtt_port = DEFAULT_MQTT_PORT;
    p->telemetry_interval_ms = DEFAULT_TELEMETRY_INTERVAL_MS;
    p->health_interval_ms = DEFAULT_HEALTH_INTERVAL_MS;
}

/* Store the parameters; call with the mutex held. */
static config_service_err_t prv_persist(const config_params_t *p)
{
    config_blob_t blob;
    (void) memset(&blob, 0, sizeof(blob));
    blob.schema_version = CONFIG_SCHEMA_VERSION;
    (void) memcpy(&blob.params, p, sizeof(blob.params));

    if (config_store_save(s_cs.store, &blob, (uint32_t) sizeof(blob)) != CONFIG_STORE_ERR_OK)
    {
        LOG_WARN(MODULE_TAG, "persist failed");
        return CONFIG_SERVICE_ERR_PERSIST;
    }
    return CONFIG_SERVICE_OK;
}

static void prv_fire_callbacks(config_param_id_t id)
{
    for (uint8_t i = 0U; i < s_cs.change_cb_count; i++)
    {
        s_cs.change_cbs[i](id);
    }
}

/* Check the sensor range / alarm / hysteresis chain of @p p as a whole. The cloud and NTP
 * fields are left out: their defaults (no broker, no servers) are deliberately not valid
 * until the device is provisioned, and that must not discard the rest of a stored blob. */
static bool prv_channels_valid(const config_params_t *p)
{
    for (uint32_t id = 0U; id <= (uint32_t) CONFIG_PARAM_PRESSURE_HYSTERESIS; id++)
    {
        if (!prv_validate((config_param_id_t) id, prv_field_ptr(p, (config_param_id_t) id), p))
        {
            return false;
        }
    }
    return true;
}

/* Replace each field of @p loaded that fails validation by its default. */
static void prv_repair(config_params_t *loaded, const config_params_t *defaults)
{
    bool failed[CONFIG_PARAM_COUNT];

    for (uint32_t id = 0U; id < (uint32_t) CONFIG_PARAM_COUNT; id++)
    {
        failed[id] = !prv_validate((config_param_id_t) id,
                                   prv_field_ptr(loaded, (config_param_id_t) id), loaded);
    }
    for (uint32_t id = 0U; id < (uint32_t) CONFIG_PARAM_COUNT; id++)
    {
        if (failed[id])
        {
            uint8_t *dst = (uint8_t *) loaded + s_fields[id].offset;
            const void *src = prv_field_ptr(defaults, (config_param_id_t) id);
            if (memcmp(dst, src, s_fields[id].size) != 0)
            {
                LOG_WARN(MODULE_TAG, "stored parameter %u invalid, default used", (unsigned) id);
                (void) memcpy(dst, src, s_fields[id].size);
            }
        }
    }
}

/* ======================================================================= */
/* Public API                                                               */
/* ======================================================================= */

config_service_err_t config_service_init(config_store_handle_t store)
{
    if (s_cs.initialised)
    {
        return CONFIG_SERVICE_OK; /* idempotent */
    }
    if (store == NULL)
    {
        return CONFIG_SERVICE_ERR_NULL_ARG;
    }

    s_cs.store = store;
    s_cs.mutex = xSemaphoreCreateMutexStatic(&s_cs.mutex_buf);
    prv_apply_defaults(&s_cs.params);
    s_cs.snapshot_valid = false;
    s_cs.change_cb_count = 0U;
    s_cs.initialised = true;
    return CONFIG_SERVICE_OK;
}

const iconfig_provider_t *config_service_get_provider(void)
{
    return s_cs.initialised ? &s_provider : NULL;
}

const iconfig_manager_t *config_service_get_manager(void)
{
    return s_cs.initialised ? &s_manager : NULL;
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

    config_params_t defaults;
    prv_apply_defaults(&defaults);

    config_blob_t loaded;
    bool usable = (len == (uint32_t) sizeof(loaded));
    if (usable)
    {
        (void) memcpy(&loaded, blob, sizeof(loaded));
        usable = (loaded.schema_version == CONFIG_SCHEMA_VERSION);
    }

    (void) xSemaphoreTake(s_cs.mutex, portMAX_DELAY);
    if (!usable)
    {
        s_cs.params = defaults;
        (void) xSemaphoreGive(s_cs.mutex);
        LOG_WARN(MODULE_TAG, "stored configuration discarded, defaults applied");
        return CONFIG_SERVICE_ERR_INVALID;
    }

    prv_repair(&loaded.params, &defaults);
    s_cs.params = prv_channels_valid(&loaded.params) ? loaded.params : defaults;
    (void) xSemaphoreGive(s_cs.mutex);
    LOG_INFO(MODULE_TAG, "stored configuration applied");
    return CONFIG_SERVICE_OK;
}

const config_params_t *config_service_get_params(void)
{
    return s_cs.initialised ? &s_cs.params : NULL;
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
    if ((unsigned) id >= (unsigned) CONFIG_PARAM_COUNT)
    {
        return CONFIG_SERVICE_ERR_INVALID;
    }
    return prv_validate(id, value, &s_cs.params) ? CONFIG_SERVICE_OK : CONFIG_SERVICE_ERR_INVALID;
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
    if ((unsigned) id >= (unsigned) CONFIG_PARAM_COUNT)
    {
        return CONFIG_SERVICE_ERR_INVALID;
    }

    (void) xSemaphoreTake(s_cs.mutex, portMAX_DELAY);
    if (!prv_validate(id, value, &s_cs.params))
    {
        (void) xSemaphoreGive(s_cs.mutex);
        return CONFIG_SERVICE_ERR_INVALID;
    }
    prv_write_field(&s_cs.params, id, value);
    const config_service_err_t rc = prv_persist(&s_cs.params);
    (void) xSemaphoreGive(s_cs.mutex);

    /* The value changed in memory even if persisting failed, so consumers are told. */
    prv_fire_callbacks(id);
    return rc;
}

config_service_err_t config_service_snapshot(void)
{
    if (!s_cs.initialised)
    {
        return CONFIG_SERVICE_ERR_NOT_INIT;
    }
    (void) xSemaphoreTake(s_cs.mutex, portMAX_DELAY);
    s_cs.snapshot = s_cs.params;
    s_cs.snapshot_valid = true;
    (void) xSemaphoreGive(s_cs.mutex);
    return CONFIG_SERVICE_OK;
}

config_service_err_t config_service_restore_snapshot(void)
{
    if (!s_cs.initialised)
    {
        return CONFIG_SERVICE_ERR_NOT_INIT;
    }

    (void) xSemaphoreTake(s_cs.mutex, portMAX_DELAY);
    if (!s_cs.snapshot_valid)
    {
        (void) xSemaphoreGive(s_cs.mutex);
        return CONFIG_SERVICE_ERR_INVALID;
    }
    s_cs.params = s_cs.snapshot;
    const config_service_err_t rc = prv_persist(&s_cs.params);
    (void) xSemaphoreGive(s_cs.mutex);
    return rc;
}

config_service_err_t config_service_flush(void)
{
    if (!s_cs.initialised)
    {
        return CONFIG_SERVICE_ERR_NOT_INIT;
    }
    (void) xSemaphoreTake(s_cs.mutex, portMAX_DELAY);
    const config_service_err_t rc = prv_persist(&s_cs.params);
    (void) xSemaphoreGive(s_cs.mutex);
    return rc;
}

config_service_err_t config_service_register_change_cb(config_change_cb_t cb)
{
    if (!s_cs.initialised)
    {
        return CONFIG_SERVICE_ERR_NOT_INIT;
    }
    if (cb == NULL)
    {
        return CONFIG_SERVICE_ERR_NULL_ARG;
    }
    if (s_cs.change_cb_count >= CONFIG_MAX_CHANGE_CALLBACKS)
    {
        return CONFIG_SERVICE_ERR_INVALID;
    }
    s_cs.change_cbs[s_cs.change_cb_count] = cb;
    s_cs.change_cb_count++;
    return CONFIG_SERVICE_OK;
}

void config_service_lock(void)
{
    if (s_cs.initialised)
    {
        (void) xSemaphoreTake(s_cs.mutex, portMAX_DELAY);
    }
}

void config_service_unlock(void)
{
    if (s_cs.initialised)
    {
        (void) xSemaphoreGive(s_cs.mutex);
    }
}

/* ======================================================================= */
/* Interface thunks                                                         */
/* ======================================================================= */

static const config_params_t *prv_get_params(void *ctx)
{
    (void) ctx;
    return config_service_get_params();
}

static config_service_err_t prv_set_param(void *ctx, config_param_id_t id, const void *value)
{
    (void) ctx;
    return config_service_set_param(id, value);
}

static config_service_err_t prv_validate_param(void *ctx, config_param_id_t id, const void *value)
{
    (void) ctx;
    return config_service_validate_param(id, value);
}

#ifdef TEST
void config_service_reset_for_test(void)
{
    (void) memset(&s_cs, 0, sizeof(s_cs));
}
#endif /* TEST */
