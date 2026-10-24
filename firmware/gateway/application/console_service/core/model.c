/**
 * @file model.c
 * @brief Pulls the console's view of the system out of the injected services.
 */

#include "model.h"

#include <stdio.h>
#include <string.h>

#define SECONDS_PER_MINUTE (60U)
#define SECONDS_PER_HOUR (3600U)
#define SECONDS_PER_DAY (86400U)

static const channel_info_t s_channels[CHART_CHANNELS] = {
    {"TEMPERATURE", "temp", "°C", SENSOR_ID_TEMPERATURE, 100, 200, 1U},
    {"HUMIDITY", "humid", "%RH", SENSOR_ID_HUMIDITY, 100, 1200, 1U},
    {"PRESSURE", "press", "hPa", SENSOR_ID_PRESSURE, 10, 120, 0U},
};

const channel_info_t *model_channel(uint8_t ch)
{
    return &s_channels[(ch < CHART_CHANNELS) ? ch : 0U];
}

void model_refresh(const console_deps_t *deps, console_model_t *m)
{
    m->sensors_ok = (deps->sensors->get_snapshot(&m->snap) == SENSOR_SERVICE_ERR_OK);
    if (!m->sensors_ok)
    {
        (void) memset(&m->snap, 0, sizeof(m->snap));
    }

    m->health_ok = (health_monitor_get_snapshot(deps->health, &m->health) == HEALTH_MONITOR_ERR_OK);
    if (!m->health_ok)
    {
        (void) memset(&m->health, 0, sizeof(m->health));
    }

    m->alarms_ok = (deps->alarms->get_all_states(m->alarm_state) == ALARM_SERVICE_ERR_OK);
    if (!m->alarms_ok)
    {
        (void) memset(m->alarm_state, 0, sizeof(m->alarm_state));
    }

    m->cfg = deps->cfg_read->get_params(deps->cfg_read->ctx);
}

bool model_value(const console_model_t *m, uint8_t ch, int32_t *value_out)
{
    const sensor_reading_t *r = &m->snap.readings[model_channel(ch)->id];

    if (!m->sensors_ok || !r->valid)
    {
        return false;
    }
    *value_out = r->value;
    return true;
}

chart_limits_t model_limits(const console_model_t *m, uint8_t ch)
{
    const channel_info_t *info = model_channel(ch);
    chart_limits_t l;

    l.band = info->band;
    l.scale = info->scale;
    l.has_hi = false;
    l.has_lo = false;
    l.hi = 0;
    l.lo = 0;
    if (m->cfg == NULL)
    {
        return l;
    }
    l.has_hi = true;
    l.has_lo = true;
    switch (ch)
    {
    case 1U:
        l.hi = m->cfg->humidity_alarm_high;
        l.lo = m->cfg->humidity_alarm_low;
        break;
    case 2U:
        l.hi = m->cfg->pressure_alarm_high;
        l.lo = m->cfg->pressure_alarm_low;
        break;
    default:
        l.hi = m->cfg->temp_alarm_high;
        l.lo = m->cfg->temp_alarm_low;
        break;
    }
    return l;
}

uint8_t model_alarm_count(const console_model_t *m)
{
    uint8_t n = 0U;

    for (uint8_t ch = 0U; ch < CHART_CHANNELS; ch++)
    {
        if (m->alarm_state[model_channel(ch)->id] != ALARM_STATE_CLEAR)
        {
            n++;
        }
    }
    return n;
}

uint8_t model_sensors_online(const console_model_t *m)
{
    static const sensor_id_t groups[4][3] = {
        {SENSOR_ID_TEMPERATURE, SENSOR_ID_HUMIDITY, SENSOR_ID_COUNT},
        {SENSOR_ID_PRESSURE, SENSOR_ID_COUNT, SENSOR_ID_COUNT},
        {SENSOR_ID_ACCEL_X, SENSOR_ID_GYRO_X, SENSOR_ID_COUNT},
        {SENSOR_ID_MAG_X, SENSOR_ID_COUNT, SENSOR_ID_COUNT},
    };
    uint8_t online = 0U;

    for (uint32_t g = 0U; g < 4U; g++)
    {
        for (uint32_t i = 0U; (i < 3U) && (groups[g][i] != SENSOR_ID_COUNT); i++)
        {
            if (m->sensors_ok && m->snap.readings[groups[g][i]].valid)
            {
                online++;
                break;
            }
        }
    }
    return online;
}

void model_uptime_short(const console_model_t *m, char *buf, size_t size)
{
    const uint32_t s = m->health.uptime_s;
    const uint32_t d = s / SECONDS_PER_DAY;
    const uint32_t h = (s % SECONDS_PER_DAY) / SECONDS_PER_HOUR;
    const uint32_t min = (s % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE;

    if (d > 0U)
    {
        (void) snprintf(buf, size, "%lud %02lu:%02lu", (unsigned long) d, (unsigned long) h,
                        (unsigned long) min);
    }
    else
    {
        (void) snprintf(buf, size, "%02lu:%02lu", (unsigned long) h, (unsigned long) min);
    }
}

void model_uptime_long(const console_model_t *m, char *buf, size_t size)
{
    const uint32_t s = m->health.uptime_s;
    const uint32_t d = s / SECONDS_PER_DAY;
    const uint32_t h = (s % SECONDS_PER_DAY) / SECONDS_PER_HOUR;
    const uint32_t min = (s % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE;
    const uint32_t sec = s % SECONDS_PER_MINUTE;

    (void) snprintf(buf, size, "%lud %02lu:%02lu:%02lu", (unsigned long) d, (unsigned long) h,
                    (unsigned long) min, (unsigned long) sec);
}

void model_time_of(const sensor_reading_t *r, char *buf, size_t size)
{
    const uint32_t t = r->timestamp.epoch % SECONDS_PER_DAY;

    (void) snprintf(buf, size, "%02lu:%02lu:%02lu", (unsigned long) (t / SECONDS_PER_HOUR),
                    (unsigned long) ((t % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE),
                    (unsigned long) (t % SECONDS_PER_MINUTE));
}
