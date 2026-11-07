/**
 * @file sim_providers.c
 * @brief Desktop-simulator provider implementations for LcdUi.
 *
 * Synthetic services with the same vtable shapes as the firmware, so lcd_ui_init() runs without
 * hardware or an RTOS. A scenario selects what the data looks like (normal, waiting, stale, error,
 * alarm, degraded, gap), which lets one binary show every state of the design pack. The data
 * follows the simulator's virtual clock: sim_set_time_ms().
 *
 * Sensor values use the firmware's fixed-point units: temperature and humidity in 0.01, pressure in
 * 0.1 hPa. No floating point is used, like the firmware.
 */

#include <stdint.h>
#include <string.h>

#include "lvgl.h"
#include "graphics_library/graphics_library.h"

#include "sensor_service/sensor_service.h"
#include "alarm_service/alarm_service.h"
#include "config_service/config_service.h"
#include "health_monitor/health_monitor.h"
#include "sim_providers.h"

#define SIM_BASE_EPOCH_S                                                                           \
    ((12U * 3600U) + (34U * 60U) + 56U) /* the 12:34:56 of the design mockups */
#define SIM_UPTIME_OFFSET_S ((3U * 86400U) + (14U * 3600U) + (22U * 60U)) /* "3d 14:22" */

static uint32_t s_now_ms;
static sim_scenario_t s_scenario = SIM_NORMAL;

void sim_set_time_ms(uint32_t ms)
{
    s_now_ms = ms;
}

void sim_set_scenario(sim_scenario_t s)
{
    s_scenario = s;
}

sim_scenario_t sim_scenario_from_name(const char *name)
{
    static const char *const k_names[] = {"normal", "waiting",  "stale", "error",
                                          "alarm",  "degraded", "gap"};
    for (uint32_t i = 0U; i < (sizeof(k_names) / sizeof(k_names[0])); i++)
    {
        if (strcmp(name, k_names[i]) == 0)
        {
            return (sim_scenario_t) i;
        }
    }
    return SIM_NORMAL;
}

/* ===================================================================== */
/* Graphics library shims (lcd_ui.c calls these)                         */
/* ===================================================================== */

lv_disp_t *graphics_get_display(void)
{
    return lv_disp_get_default();
}

graphics_err_t graphics_process(void)
{
    return GRAPHICS_ERR_OK;
}

lv_indev_t *graphics_get_indev(void)
{
    return lv_indev_get_act();
}

void graphics_tick_increment(uint32_t ms)
{
    lv_tick_inc(ms);
}

/* ===================================================================== */
/* Sensor service                                                        */
/* ===================================================================== */

/** Triangle wave in [-amp, +amp] with the given period in seconds. */
static int32_t tri(uint32_t t_s, uint32_t period_s, int32_t amp)
{
    const uint32_t phase = t_s % period_s;
    const int32_t half = (int32_t) (period_s / 2U);
    const int32_t up = (phase < (uint32_t) half) ? (int32_t) phase : (int32_t) (period_s - phase);
    return ((up * 2 * amp) / half) - amp;
}

static sensor_service_err_t sim_get_snapshot(sensor_snapshot_t *snap)
{
    const uint32_t t = s_now_ms / 1000U;
    const uint32_t epoch = SIM_BASE_EPOCH_S + t;
    (void) memset(snap, 0, sizeof(*snap));

    if (s_scenario == SIM_WAITING)
    {
        return SENSOR_SERVICE_ERR_OK; /* cycle_count 0, nothing valid */
    }
    snap->cycle_count = (t / 2U) + 1U;

    const int32_t temp = 2340 + tri(t, 90U, 60) + ((s_scenario == SIM_ALARM) ? 1930 : 0);
    const int32_t hum = 5820 + tri(t, 70U, 90);
    const int32_t pres = ((s_scenario == SIM_ALARM) ? 9820 : 10132) + tri(t, 110U, 8);

    snap->readings[SENSOR_ID_TEMPERATURE] =
        (sensor_reading_t){.value = temp,
                           .valid = true,
                           .timestamp = {.epoch = epoch, .sync_state = TIME_SYNC_SYNCHRONISED}};
    snap->readings[SENSOR_ID_HUMIDITY] =
        (sensor_reading_t){.value = hum,
                           .valid = true,
                           .timestamp = {.epoch = epoch, .sync_state = TIME_SYNC_SYNCHRONISED}};
    snap->readings[SENSOR_ID_PRESSURE] =
        (sensor_reading_t){.value = pres,
                           .valid = true,
                           .timestamp = {.epoch = epoch, .sync_state = TIME_SYNC_SYNCHRONISED}};

    if ((s_scenario == SIM_STALE) && (t > 20U))
    {
        snap->readings[SENSOR_ID_PRESSURE].timestamp.epoch = epoch - 100U; /* overdue by 100 s */
    }
    if ((s_scenario == SIM_ERROR) && (t > 8U))
    {
        snap->readings[SENSOR_ID_TEMPERATURE].valid = false;
    }
    if ((s_scenario == SIM_GAP) && (t >= 120U) && (t < 150U))
    {
        snap->readings[SENSOR_ID_HUMIDITY].valid = false;
    }
    return SENSOR_SERVICE_ERR_OK;
}

const isensor_service_t sim_sensor_svc = {
    .get_snapshot = sim_get_snapshot,
};

sensor_service_err_t sensor_service_subscribe(void (*cb)(const sensor_snapshot_t *s))
{
    (void) cb;
    return SENSOR_SERVICE_ERR_OK;
}

/* ===================================================================== */
/* Alarm service                                                         */
/* ===================================================================== */

static alarm_service_err_t sim_get_all_states(alarm_state_t states[SENSOR_ID_COUNT])
{
    for (uint32_t i = 0U; i < (uint32_t) SENSOR_ID_COUNT; i++)
    {
        states[i] = ALARM_STATE_CLEAR;
    }
    if (s_scenario == SIM_ALARM)
    {
        states[SENSOR_ID_TEMPERATURE] = ALARM_STATE_ACTIVE_HIGH;
        states[SENSOR_ID_PRESSURE] = ALARM_STATE_ACTIVE_LOW;
    }
    return ALARM_SERVICE_ERR_OK;
}

const ialarm_service_t sim_alarm_svc = {
    .get_all_states = sim_get_all_states,
};

/* ===================================================================== */
/* Config service                                                        */
/* ===================================================================== */

static config_params_t s_sim_params = {
    .polling_interval_ms = 5000U,
    .temp_alarm_high = 4000,       /* 40.00 degC x100 */
    .temp_alarm_low = 1000,        /* 10.00 degC x100 */
    .humidity_alarm_high = 8000U,  /* 80.00 %RH x100  */
    .humidity_alarm_low = 2000U,   /* 20.00 %RH x100  */
    .pressure_alarm_high = 10500U, /* 1050.0 hPa x10  */
    .pressure_alarm_low = 9500U,   /*  950.0 hPa x10  */
    .filter_alpha = 800U,
};

static const config_params_t *sim_get_params(void)
{
    return &s_sim_params;
}

static config_service_err_t sim_set_param(config_param_id_t id, const void *value)
{
    switch (id)
    {
    case CONFIG_PARAM_POLL_INTERVAL:
        s_sim_params.polling_interval_ms = *(const uint32_t *) value;
        break;
    case CONFIG_PARAM_TEMP_ALARM_HIGH:
        s_sim_params.temp_alarm_high = *(const int16_t *) value;
        break;
    case CONFIG_PARAM_TEMP_ALARM_LOW:
        s_sim_params.temp_alarm_low = *(const int16_t *) value;
        break;
    case CONFIG_PARAM_HUMIDITY_ALARM_HIGH:
        s_sim_params.humidity_alarm_high = *(const uint16_t *) value;
        break;
    case CONFIG_PARAM_HUMIDITY_ALARM_LOW:
        s_sim_params.humidity_alarm_low = *(const uint16_t *) value;
        break;
    case CONFIG_PARAM_PRESSURE_ALARM_HIGH:
        s_sim_params.pressure_alarm_high = *(const uint16_t *) value;
        break;
    case CONFIG_PARAM_PRESSURE_ALARM_LOW:
        s_sim_params.pressure_alarm_low = *(const uint16_t *) value;
        break;
    default:
        break;
    }
    return CONFIG_SERVICE_OK;
}

const iconfig_provider_t sim_cfg_read = {.get_params = sim_get_params};
const iconfig_manager_t sim_cfg_write = {.set_param = sim_set_param};

/* ===================================================================== */
/* Health monitor                                                        */
/* ===================================================================== */

static health_monitor_err_t sim_get_health_snapshot(device_health_snapshot_t *snap)
{
    (void) memset(snap, 0, sizeof(*snap));
    snap->uptime_s = SIM_UPTIME_OFFSET_S + (s_now_ms / 1000U);
    snap->lifecycle_state = LIFECYCLE_STATE_OPERATIONAL;
    snap->modbus_slave_ok = (s_scenario != SIM_DEGRADED) || (s_now_ms < 10000U); /* up, then lost */
    snap->modbus_valid_frames = 14829U + (s_now_ms / 1000U);
    snap->modbus_crc_errors = (s_scenario == SIM_DEGRADED) ? 12U : 0U;
    snap->modbus_exception_responses = (s_scenario == SIM_DEGRADED) ? 4U : 0U;
    snap->sensor_valid[SENSOR_ID_TEMPERATURE] = (s_scenario != SIM_ERROR) || (s_now_ms < 8000U);
    snap->sensor_valid[SENSOR_ID_HUMIDITY] = true;
    snap->sensor_valid[SENSOR_ID_PRESSURE] = true;
    snap->sensor_fail_count = (s_scenario == SIM_ERROR) ? 3U : 0U;
    snap->alarm_raise_count = (s_scenario == SIM_ALARM) ? 2U : 0U;
    if (s_scenario == SIM_ALARM)
    {
        snap->alarm_state[SENSOR_ID_TEMPERATURE] = ALARM_STATE_ACTIVE_HIGH;
        snap->alarm_state[SENSOR_ID_PRESSURE] = ALARM_STATE_ACTIVE_LOW;
    }
    for (uint32_t i = 0U; i < HEALTH_TASK_COUNT; i++)
    {
        snap->stack_watermark_words[i] = (uint16_t) (78U + (i * 40U)); /* the smallest is 312 B */
    }
    /* Resources: healthy values drift a little; the degraded scenario runs hot (warning/error) */
    const bool hot = (s_scenario == SIM_DEGRADED) && (s_now_ms >= 10000U);
    snap->cpu_load_pct = hot ? 92U : (uint8_t) (18U + ((s_now_ms / 1000U) % 7U));
    snap->free_ram_bytes = hot ? (3U * 1024U) : (94U * 1024U);
    snap->mcu_temp_valid = true;
    snap->mcu_temp_deci_c =
        hot ? (int16_t) 872 : (int16_t) (412 + (int16_t) ((s_now_ms / 1000U) % 5U));
    return HEALTH_MONITOR_ERR_OK;
}

static health_monitor_err_t sim_push_event(health_event_t ev, uint32_t param)
{
    (void) ev;
    (void) param;
    return HEALTH_MONITOR_ERR_OK;
}

static health_monitor_err_t sim_health_init(void)
{
    return HEALTH_MONITOR_ERR_OK;
}

const ihealth_snapshot_t sim_health_snap = {.get_snapshot = sim_get_health_snapshot};
const ihealth_report_t sim_health_report = {
    .init = sim_health_init,
    .push_event = sim_push_event,
};

/* Required by health_monitor.h external declaration */
const ihealth_report_t *const health_report = &sim_health_report;
