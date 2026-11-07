/**
 * @file sim_providers.c
 * @brief Desktop-simulator provider implementations for LcdUi.
 *
 * Supplies synthetic sensor readings and no-op config/alarm/health vtables
 * so that lcd_ui_init() can be called without any real hardware or RTOS.
 *
 * Sensor values use the same fixed-point units as firmware:
 *   TEMPERATURE : int32_t ×100 (0.01 °C)
 *   HUMIDITY    : int32_t ×100 (0.01 %RH)
 *   PRESSURE    : int32_t ×10  (0.1 hPa)
 *
 * Also provides the graphics_library shim symbols that lcd_ui.c calls;
 * the real graphics_library.c is not compiled in the simulator.
 */

#include "lvgl.h"
#include "graphics_library/graphics_library.h"

#include "sensor_service/sensor_service.h"
#include "alarm_service/alarm_service.h"
#include "config_service/config_service.h"
#include "health_monitor/health_monitor.h"

/* ===================================================================== */
/* Graphics library shims (lcd_ui.c calls these)                        */
/* ===================================================================== */

lv_disp_t *graphics_get_display(void)
{
    return lv_disp_get_default();
}

graphics_err_t graphics_process(void)
{
    /* Timer-handler is called from main.c loop; this is a no-op shim. */
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

/* Synthetic readings visible in sim — update at runtime if needed. */
static sensor_snapshot_t s_sim_snap =
{
    .cycle_count = 1U,
    .readings =
    {
        [SENSOR_ID_TEMPERATURE] = { .value = 2340,  .valid = true },  /* 23.40 °C   */
        [SENSOR_ID_HUMIDITY]    = { .value = 5820,  .valid = true },  /* 58.20 %RH  */
        [SENSOR_ID_PRESSURE]    = { .value = 10132, .valid = true },  /* 1013.2 hPa */
    },
};

static sensor_service_err_t sim_get_snapshot(sensor_snapshot_t *snap)
{
    *snap = s_sim_snap;
    return SENSOR_SERVICE_ERR_OK;
}

const isensor_service_t sim_sensor_svc =
{
    .get_snapshot = sim_get_snapshot,
};

/* Satisfy link-time reference from sensor_service_stub.h (alarm_service pulls it) */
sensor_service_err_t sensor_service_subscribe(void (*cb)(const sensor_snapshot_t *s))
{
    (void)cb;
    return SENSOR_SERVICE_ERR_OK;
}

/* ===================================================================== */
/* Alarm service                                                         */
/* ===================================================================== */

static alarm_state_t s_sim_alarm_states[SENSOR_ID_COUNT]; /* all CLEAR */

static alarm_service_err_t sim_get_all_states(alarm_state_t states[SENSOR_ID_COUNT])
{
    for (uint32_t i = 0U; i < (uint32_t)SENSOR_ID_COUNT; i++)
    {
        states[i] = s_sim_alarm_states[i];
    }
    return ALARM_SERVICE_ERR_OK;
}

const ialarm_service_t sim_alarm_svc =
{
    .get_all_states = sim_get_all_states,
};

/* ===================================================================== */
/* Config service                                                        */
/* ===================================================================== */

static config_params_t s_sim_params =
{
    .polling_interval_ms   = 2000U,
    .temp_alarm_high       = 4000,   /* 40.00 °C  ×100 */
    .temp_alarm_low        = 0,      /*  0.00 °C  ×100 */
    .humidity_alarm_high   = 8000U,  /* 80.00 %RH ×100 */
    .humidity_alarm_low    = 2000U,  /* 20.00 %RH ×100 */
    .pressure_alarm_high   = 10500U, /* 1050.0 hPa ×10 */
    .pressure_alarm_low    = 9500U,  /*  950.0 hPa ×10 */
    .filter_alpha          = 800U,   /* 0.800 ×1000    */
};

static const config_params_t *sim_get_params(void)
{
    return &s_sim_params;
}

static config_service_err_t sim_set_param(config_param_id_t id, const void *value)
{
    (void)id;
    (void)value;
    return CONFIG_SERVICE_OK;
}

const iconfig_provider_t sim_cfg_read  = { .get_params = sim_get_params };
const iconfig_manager_t  sim_cfg_write = { .set_param  = sim_set_param  };

/* ===================================================================== */
/* Health monitor                                                        */
/* ===================================================================== */

static device_health_snapshot_t s_sim_health; /* all-zero uptime etc. */

static health_monitor_err_t sim_get_health_snapshot(device_health_snapshot_t *snap)
{
    *snap = s_sim_health;
    return HEALTH_MONITOR_ERR_OK;
}

static health_monitor_err_t sim_push_event(health_event_t ev, uint32_t param)
{
    (void)ev;
    (void)param;
    return HEALTH_MONITOR_ERR_OK;
}

static health_monitor_err_t sim_health_init(void)
{
    return HEALTH_MONITOR_ERR_OK;
}

const ihealth_snapshot_t sim_health_snap   = { .get_snapshot = sim_get_health_snapshot };
const ihealth_report_t   sim_health_report =
{
    .init       = sim_health_init,
    .push_event = sim_push_event,
};

/* Required by health_monitor.h external declaration */
const ihealth_report_t *const health_report = &sim_health_report;
