/**
 * @file main_host.c
 * @brief Host simulator for the Gateway console TUI.
 *
 * Builds the real console_service.c and core/ against fake services (random-walking sensors, a
 * ConfigService stand-in that really applies thresholds) so the whole interface can be iterated
 * in a terminal, or scripted and captured, without flashing. See build.sh.
 *
 *   ./envmon-console-sim                    interactive: 88x30+ truecolor terminal, Ctrl-C to quit
 *   ./envmon-console-sim 6 '\r' 'help\r'    scripted: each argument is typed, then one 1 Hz tick;
 *                                           the raw escape stream goes to stdout
 *
 * Escapes understood in script arguments: \r \t \e \\ .
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console_service.h"
#include "console_service/port/console_port.h"

/* ---- fake system -------------------------------------------------------- */

static config_params_t s_cfg;
static sensor_snapshot_t s_snap;
static health_snapshot_t s_health;
static uint32_t s_ms;
static uint32_t s_rng = 12345U;
static bool s_scripted;

static int32_t rnd(int32_t span)
{
    s_rng = (s_rng * 1103515245U) + 12345U;
    return (int32_t) ((s_rng >> 16) % (uint32_t) (2 * span + 1)) - span;
}

static void walk(void)
{
    sensor_reading_t *t = &s_snap.readings[SENSOR_ID_TEMPERATURE];
    sensor_reading_t *h = &s_snap.readings[SENSOR_ID_HUMIDITY];
    sensor_reading_t *p = &s_snap.readings[SENSOR_ID_PRESSURE];

    t->value += rnd(25) + 8; /* drifts up, towards the limit */
    t->value += (2300 - t->value) / 40;
    h->value += rnd(60) + (5800 - h->value) / 30;
    p->value += rnd(6) + (10130 - p->value) / 30;
    for (uint32_t i = 0U; i < (uint32_t) SENSOR_ID_COUNT; i++)
    {
        s_snap.readings[i].valid = true;
        s_snap.readings[i].timestamp.epoch += 1U;
    }
    s_health.uptime_s += 1U;
}

static sensor_service_err_t sim_snapshot(sensor_snapshot_t *s)
{
    *s = s_snap;
    return SENSOR_SERVICE_ERR_OK;
}

static bool sim_ready(void)
{
    return true;
}

static const isensor_service_t s_sensors = {.get_snapshot = sim_snapshot, .is_ready = sim_ready};

static alarm_service_err_t sim_alarm_states(alarm_state_t st[SENSOR_ID_COUNT])
{
    memset(st, 0, sizeof(alarm_state_t) * SENSOR_ID_COUNT);
    if (s_snap.readings[SENSOR_ID_TEMPERATURE].value > s_cfg.temp_alarm_high)
    {
        st[SENSOR_ID_TEMPERATURE] = ALARM_STATE_ACTIVE_HIGH;
    }
    return ALARM_SERVICE_ERR_OK;
}

static const ialarm_service_t s_alarms = {.get_all_states = sim_alarm_states};

static const config_params_t *sim_params(void *ctx)
{
    (void) ctx;
    return &s_cfg;
}

static config_service_err_t sim_set(void *ctx, config_param_id_t id, const void *v)
{
    (void) ctx;
    switch (id)
    {
    case CONFIG_PARAM_TEMP_ALARM_HIGH:
        memcpy(&s_cfg.temp_alarm_high, v, sizeof(int16_t));
        return CONFIG_SERVICE_OK;
    case CONFIG_PARAM_TEMP_ALARM_LOW:
        memcpy(&s_cfg.temp_alarm_low, v, sizeof(int16_t));
        return CONFIG_SERVICE_OK;
    case CONFIG_PARAM_HUMIDITY_ALARM_HIGH:
        memcpy(&s_cfg.humidity_alarm_high, v, sizeof(uint16_t));
        return CONFIG_SERVICE_OK;
    case CONFIG_PARAM_HUMIDITY_ALARM_LOW:
        memcpy(&s_cfg.humidity_alarm_low, v, sizeof(uint16_t));
        return CONFIG_SERVICE_OK;
    case CONFIG_PARAM_PRESSURE_ALARM_HIGH:
        memcpy(&s_cfg.pressure_alarm_high, v, sizeof(uint16_t));
        return CONFIG_SERVICE_OK;
    case CONFIG_PARAM_PRESSURE_ALARM_LOW:
        memcpy(&s_cfg.pressure_alarm_low, v, sizeof(uint16_t));
        return CONFIG_SERVICE_OK;
    default:
        return CONFIG_SERVICE_OK;
    }
}

static config_service_err_t sim_validate(void *ctx, config_param_id_t id, const void *v)
{
    (void) ctx;
    (void) id;
    (void) v;
    return CONFIG_SERVICE_OK;
}

static config_service_err_t sim_persist(void *ctx)
{
    (void) ctx;
    return CONFIG_SERVICE_OK;
}

static const iconfig_provider_t s_cfg_read = {.ctx = NULL, .get_params = sim_params};
static const iconfig_manager_t s_cfg_write = {.ctx = NULL,
                                              .set_param = sim_set,
                                              .validate_param = sim_validate,
                                              .set_param_deferred = sim_set,
                                              .persist_pending = sim_persist};

static device_profile_t s_profiles[2] = {{1U, 0U, 10U}, {2U, 100U, 4U}};

static bool sim_next(const device_profile_t **p, uint8_t *i)
{
    if (*i >= 2U)
    {
        return false;
    }
    *p = &s_profiles[*i];
    (*i)++;
    return true;
}

static const idevice_profile_provider_t s_prof = {.get_next = sim_next};

health_monitor_err_t health_monitor_get_snapshot(health_monitor_handle_t h, health_snapshot_t *out)
{
    (void) h;
    *out = s_health;
    return HEALTH_MONITOR_ERR_OK;
}

void console_port_uid(uint32_t uid[3])
{
    uid[0] = 0x002D004FU;
    uid[1] = 0x34425010U;
    uid[2] = 0x20323848U;
}

void console_port_reset(void)
{
    exit(0);
}

uint32_t console_port_millis(void)
{
    return s_ms;
}

/* ---- terminal ---------------------------------------------------------- */

extern void tty_raw_on(void);
extern void tty_raw_off(void);
extern long tty_read(char *buf, unsigned long n);

static debug_uart_byte_callback_t s_rx;

static debug_uart_err_t sim_send(const uint8_t *d, size_t n, uint32_t timeout_ms)
{
    (void) timeout_ms;
    (void) fwrite(d, 1U, n, stdout);
    (void) fflush(stdout);
    return DEBUG_UART_OK;
}

static debug_uart_err_t sim_attach(debug_uart_byte_callback_t cb, void *ctx)
{
    (void) ctx;
    s_rx = cb;
    return DEBUG_UART_OK;
}

static const idebug_uart_t s_uart = {.send = sim_send, .attach_rx_byte = sim_attach};

static void feed(const char *keys)
{
    for (const char *p = keys; *p != '\0'; p++)
    {
        s_rx((uint8_t) *p, NULL);
    }
}

static void unescape(const char *in, char *out)
{
    while (*in != '\0')
    {
        if (*in == '\\')
        {
            in++;
            *out++ = (*in == 'r') ? '\r' : (*in == 't') ? '\t' : (*in == 'e') ? '\x1b' : *in;
            if (*in != '\0')
            {
                in++;
            }
        }
        else
        {
            *out++ = *in++;
        }
    }
    *out = '\0';
}

int main(int argc, char **argv)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.temp_alarm_high = 2600;
    s_cfg.temp_alarm_low = 0;
    s_cfg.humidity_alarm_high = 9000U;
    s_cfg.humidity_alarm_low = 2000U;
    s_cfg.pressure_alarm_high = 11000U;
    s_cfg.pressure_alarm_low = 9900U;
    s_cfg.mqtt_port = 8883U;
    snprintf(s_cfg.mqtt_broker, sizeof(s_cfg.mqtt_broker), "iot.example.com");
    s_snap.readings[SENSOR_ID_TEMPERATURE].value = 2250;
    s_snap.readings[SENSOR_ID_HUMIDITY].value = 5800;
    s_snap.readings[SENSOR_ID_PRESSURE].value = 10130;
    s_health.wifi_rssi_dbm = -62;
    s_health.cloud_connected = true;
    s_health.modbus_link_online = true;
    s_health.uptime_s = 3U * 86400U + 14U * 3600U + 22U * 60U;

    const console_deps_t deps = {&s_uart,     &s_sensors,   &s_alarms,
                                 &s_cfg_read, &s_cfg_write, (health_monitor_handle_t) 1,
                                 &s_prof};

    if (console_service_init(&deps) != CONSOLE_SERVICE_ERR_OK)
    {
        return 1;
    }
    s_scripted = (argc > 1);
    for (int i = 0; i < 92; i++) /* fill the chart history with data ticks */
    {
        walk();
        (void) console_service_cycle_for_test(1UL << 1);
    }

    (void) console_service_cycle_for_test(1UL << 1);
    if (s_scripted)
    {
        for (int i = 1; i < argc; i++)
        {
            char keys[256];

            unescape(argv[i], keys);
            feed(keys);
            s_ms += 50U;
            (void) console_service_cycle_for_test(1UL << 0);
            walk();
            (void) console_service_cycle_for_test(1UL << 1);
        }
        return 0;
    }

    tty_raw_on();
    for (uint32_t tick = 0U;; tick++)
    {
        char buf[16];
        const long n = tty_read(buf, sizeof(buf));

        for (long i = 0; i < n; i++)
        {
            s_rx((uint8_t) buf[i], NULL);
        }
        s_ms += 100U;
        uint32_t bits = (n > 0) ? (1UL << 0) : 0U;

        if ((tick % 10U) == 9U)
        {
            walk();
            bits |= 1UL << 1;
        }
        if ((tick % 5U) == 4U)
        {
            bits |= 1UL << 2;
        }
        (void) console_service_cycle_for_test(bits);
    }
}
