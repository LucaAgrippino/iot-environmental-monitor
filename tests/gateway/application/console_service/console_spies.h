/**
 * @file console_spies.h
 * @brief Fake services and platform hooks shared by the ConsoleService host tests.
 *
 * Defines the external symbols the console links against (health_monitor_get_snapshot,
 * console_port_*) and a full set of injected service interfaces whose behaviour each test sets
 * through the g_spy_* variables. This header has no matching .c file, so including it never makes
 * Ceedling link a real module.
 */

#ifndef CONSOLE_SPIES_H
#define CONSOLE_SPIES_H

#include <string.h>

#include "console_service/console_types.h"
#include "console_service/port/console_port.h"

/* ---- platform hooks ---------------------------------------------------- */

static uint32_t g_spy_uid[3];
static uint32_t g_spy_reset_calls;
static uint32_t g_spy_millis;

void console_port_uid(uint32_t uid[3])
{
    (void) memcpy(uid, g_spy_uid, sizeof(g_spy_uid));
}

void console_port_reset(void)
{
    g_spy_reset_calls++;
}

uint32_t console_port_millis(void)
{
    return g_spy_millis;
}

/* ---- health ------------------------------------------------------------ */

static health_snapshot_t g_spy_health;
static health_monitor_err_t g_spy_health_rc;

health_monitor_err_t health_monitor_get_snapshot(health_monitor_handle_t handle,
                                                 health_snapshot_t *snap_out)
{
    (void) handle;
    *snap_out = g_spy_health;
    return g_spy_health_rc;
}

/* ---- sensors ----------------------------------------------------------- */

static sensor_snapshot_t g_spy_snapshot;
static sensor_service_err_t g_spy_snapshot_rc;
static bool g_spy_ready;

static sensor_service_err_t spy_get_snapshot(sensor_snapshot_t *snap)
{
    *snap = g_spy_snapshot;
    return g_spy_snapshot_rc;
}

static bool spy_is_ready(void)
{
    return g_spy_ready;
}

static const isensor_service_t g_spy_sensors = {.get_snapshot = spy_get_snapshot,
                                                .is_ready = spy_is_ready};

/* ---- alarms ------------------------------------------------------------ */

static alarm_state_t g_spy_alarm_states[SENSOR_ID_COUNT];
static alarm_service_err_t g_spy_alarm_rc;

static alarm_service_err_t spy_get_all_states(alarm_state_t states[SENSOR_ID_COUNT])
{
    (void) memcpy(states, g_spy_alarm_states, sizeof(g_spy_alarm_states));
    return g_spy_alarm_rc;
}

static const ialarm_service_t g_spy_alarms = {.get_all_states = spy_get_all_states};

/* ---- configuration ----------------------------------------------------- */

static config_params_t g_spy_cfg;

static bool g_spy_cfg_read_null;

static const config_params_t *spy_get_params(void *ctx)
{
    (void) ctx;
    return g_spy_cfg_read_null ? NULL : &g_spy_cfg;
}

static const iconfig_provider_t g_spy_cfg_read = {.ctx = NULL, .get_params = spy_get_params};

static config_service_err_t g_spy_validate_rc;
static config_service_err_t g_spy_set_rc;
static uint32_t g_spy_validate_calls;
static uint32_t g_spy_set_calls;
static uint32_t g_spy_persist_calls;
static config_service_err_t g_spy_persist_rc;
static config_param_id_t g_spy_set_ids[16];
static uint8_t g_spy_set_values[16][136]; /* raw copy of each value passed to set_param_deferred */

static config_service_err_t spy_validate(void *ctx, config_param_id_t id, const void *value)
{
    (void) ctx;
    (void) id;
    (void) value;
    g_spy_validate_calls++;
    return g_spy_validate_rc;
}

static config_service_err_t spy_set_deferred(void *ctx, config_param_id_t id, const void *value)
{
    (void) ctx;
    if (g_spy_set_calls < 16U)
    {
        g_spy_set_ids[g_spy_set_calls] = id;
        (void) memcpy(g_spy_set_values[g_spy_set_calls], value, 4U);
        if (id == CONFIG_PARAM_MQTT_BROKER)
        {
            (void) strncpy((char *) g_spy_set_values[g_spy_set_calls], (const char *) value, 135U);
        }
    }
    g_spy_set_calls++;
    return g_spy_set_rc;
}

static config_service_err_t spy_persist(void *ctx)
{
    (void) ctx;
    g_spy_persist_calls++;
    return g_spy_persist_rc;
}

static const iconfig_manager_t g_spy_cfg_write = {.ctx = NULL,
                                                  .set_param = spy_set_deferred,
                                                  .validate_param = spy_validate,
                                                  .set_param_deferred = spy_set_deferred,
                                                  .persist_pending = spy_persist};

/* ---- device profiles --------------------------------------------------- */

static device_profile_t g_spy_profiles[4];
static uint8_t g_spy_profile_count;

static bool spy_profile_next(device_profile_t *p, uint8_t *index)
{
    if (*index >= g_spy_profile_count)
    {
        return false;
    }
    *p = g_spy_profiles[*index];
    (*index)++;
    return true;
}

static const idevice_profile_provider_t g_spy_prof_read = {.get_next = spy_profile_next};

/* ---- UART -------------------------------------------------------------- */

static uint8_t g_spy_tx[65536];
static size_t g_spy_tx_len;
static debug_uart_err_t g_spy_send_rc;
static debug_uart_err_t g_spy_attach_rc;
static void (*g_spy_send_hook)(
    void); /* called inside every send: lets a test inject a key mid-frame */
static debug_uart_byte_callback_t g_spy_rx_cb;

static debug_uart_err_t spy_send(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    (void) timeout_ms;
    if ((g_spy_tx_len + len) <= sizeof(g_spy_tx))
    {
        (void) memcpy(&g_spy_tx[g_spy_tx_len], data, len);
        g_spy_tx_len += len;
    }
    if (g_spy_send_hook != NULL)
    {
        g_spy_send_hook();
    }
    return g_spy_send_rc;
}

static debug_uart_err_t spy_attach_byte(debug_uart_byte_callback_t cb, void *ctx)
{
    (void) ctx;
    g_spy_rx_cb = cb;
    return g_spy_attach_rc;
}

static const idebug_uart_t g_spy_uart = {.send = spy_send, .attach_rx_byte = spy_attach_byte};

/* ---- wiring ------------------------------------------------------------ */

static console_deps_t spy_deps(void)
{
    console_deps_t d;

    d.uart = &g_spy_uart;
    d.sensors = &g_spy_sensors;
    d.alarms = &g_spy_alarms;
    d.cfg_read = &g_spy_cfg_read;
    d.cfg_write = &g_spy_cfg_write;
    d.health = (health_monitor_handle_t) 0x1234;
    d.prof_read = &g_spy_prof_read;
    return d;
}

/* ConfigService defaults, in fixed point. */
static void spy_default_cfg(config_params_t *p)
{
    (void) memset(p, 0, sizeof(*p));
    p->polling_interval_ms = 1000U;
    p->filter_alpha = 100U;
    p->temp_range_min = -4000;
    p->temp_range_max = 8500;
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
    p->modbus_slave_addr = 1U;
    p->modbus_poll_period_ms = 1000U;
    p->mqtt_port = 8883U;
    p->telemetry_interval_ms = 60000U;
    p->health_interval_ms = 600000U;
}

static void spy_set_reading(sensor_id_t id, int32_t value, bool valid)
{
    g_spy_snapshot.readings[id].value = value;
    g_spy_snapshot.readings[id].valid = valid;
}

/* Reset every spy to a healthy, connected system: 22.00 C, 50.00 %RH, 1013.0 hPa. */
static void spy_reset(void)
{
    (void) memset(g_spy_uid, 0, sizeof(g_spy_uid));
    g_spy_reset_calls = 0U;
    g_spy_millis = 0U;
    (void) memset(&g_spy_health, 0, sizeof(g_spy_health));
    g_spy_health.wifi_rssi_dbm = -42;
    g_spy_health.cloud_connected = true;
    g_spy_health.modbus_link_online = true;
    g_spy_health.uptime_s = 3U * 86400U + 14U * 3600U + 22U * 60U + 5U;
    g_spy_health_rc = HEALTH_MONITOR_ERR_OK;
    (void) memset(&g_spy_snapshot, 0, sizeof(g_spy_snapshot));
    for (uint32_t i = 0U; i < (uint32_t) SENSOR_ID_COUNT; i++)
    {
        spy_set_reading((sensor_id_t) i, 0, true);
    }
    spy_set_reading(SENSOR_ID_TEMPERATURE, 2200, true);
    spy_set_reading(SENSOR_ID_HUMIDITY, 5000, true);
    spy_set_reading(SENSOR_ID_PRESSURE, 10130, true);
    g_spy_snapshot_rc = SENSOR_SERVICE_ERR_OK;
    g_spy_ready = true;
    (void) memset(g_spy_alarm_states, 0, sizeof(g_spy_alarm_states));
    g_spy_alarm_rc = ALARM_SERVICE_ERR_OK;
    spy_default_cfg(&g_spy_cfg);
    g_spy_cfg_read_null = false;
    g_spy_validate_rc = CONFIG_SERVICE_OK;
    g_spy_set_rc = CONFIG_SERVICE_OK;
    g_spy_persist_rc = CONFIG_SERVICE_OK;
    g_spy_validate_calls = 0U;
    g_spy_set_calls = 0U;
    g_spy_persist_calls = 0U;
    (void) memset(g_spy_set_ids, 0, sizeof(g_spy_set_ids));
    (void) memset(g_spy_set_values, 0, sizeof(g_spy_set_values));
    (void) memset(g_spy_profiles, 0, sizeof(g_spy_profiles));
    g_spy_profile_count = 0U;
    g_spy_tx_len = 0U;
    g_spy_send_rc = DEBUG_UART_OK;
    g_spy_attach_rc = DEBUG_UART_OK;
    g_spy_rx_cb = NULL;
    g_spy_send_hook = NULL;
}

#endif /* CONSOLE_SPIES_H */
