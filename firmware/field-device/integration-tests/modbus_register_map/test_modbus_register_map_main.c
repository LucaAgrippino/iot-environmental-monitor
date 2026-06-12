/**
 * @file test_modbus_register_map_main.c
 * @brief Integration test for ModbusRegisterMap on STM32F469I-DISCO hardware.
 *
 * Exercises ModbusRegisterMap in a live FreeRTOS environment using
 * lightweight stub vtables (no real SensorService / AlarmService — all
 * providers are function-pointer stubs defined in this file).
 *
 * Activation in CubeIDE:
 *   1. Exclude Src/main.c from build (Resource Config → Exclude from Build).
 *   2. Add integration-tests/modbus_register_map/ to project source paths.
 *   3. Build, flash, open PuTTY on ST-Link VCP at 115200 / 8N1.
 *
 * Expected output (visual checklist — tick each before declaring good):
 *
 *   Pre-scheduler:
 *   [ ] "[MRM-IT] ===== ModbusRegisterMap integration test ====="
 *   [ ] "[MRM-IT] modbus_register_map_init OK"
 *   [ ] "[MRM-IT] starting scheduler..."
 *
 *   Phase 1 — identity registers:
 *   [ ] "[MRM-IT] Phase 1: MAP_VERSION=1 VENDOR_CODE=0x1A45 [PASS]"
 *
 *   Phase 2 — sensor registers (stub values):
 *   [ ] "[MRM-IT] Phase 2: TEMP=2200 HUM=5000 [PASS]"
 *
 *   Phase 3 — config read/write round-trip:
 *   [ ] "[MRM-IT] Phase 3: sampling_period read=1000 [PASS]"
 *   [ ] "[MRM-IT] Phase 3: sampling_period write OK [PASS]"
 *
 *   Phase 4 — Mediator (slave address):
 *   [ ] "[MRM-IT] Phase 4: slave addr write OK, set_address called with 5 [PASS]"
 *
 *   Phase 5 — poll_stats:
 *   [ ] "[MRM-IT] Phase 5: poll_stats OK, valid_frames=42 [PASS]"
 *
 *   Phase 6 — periodic tick:
 *   [ ] periodic "[MRM-IT] tick N" at 1 Hz
 *   [ ] No UART freeze or watchdog reset
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "stm32f469xx.h"
#include "system_clock.h"

#include "FreeRTOS.h"
#include "task.h"

#include "debug-uart/debug_uart_driver.h"
#include "rtc/rtc_driver.h"
#include "logger/logger.h"

/* MRM is compiled without TEST — use production header directly */
#undef TEST
#include "sensor_service/sensor_service.h"
#include "alarm_service/alarm_service.h"
#include "config_service/config_service.h"
#include "config_service/config_params.h"
#include "health_monitor/health_monitor.h"
#include "time_provider/time_provider.h"
#include "modbus_register_map/modbus_register_map.h"

/* ===================================================================== */
/* Configuration                                                         */
/* ===================================================================== */

#define LOG_MODULE "MRM-IT"
#define TEST_TASK_STACK_WORDS (512U)
#define TEST_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)

/* ===================================================================== */
/* Stub state shared between stubs and the test task                    */
/* ===================================================================== */

static config_params_t s_params;
static sensor_snapshot_t s_sensor_snap;
static alarm_state_t s_alarm_states[SENSOR_ID_COUNT];
static device_health_snapshot_t s_health_snap;
static modbus_slave_stats_t s_mb_stats;

static volatile bool s_set_param_called;
static volatile uint8_t s_slave_addr_set;
static volatile bool s_slave_addr_set_called;
static volatile bool s_lc_cmd_called;
static volatile bool s_health_update_called;

/* ===================================================================== */
/* Stub implementations                                                  */
/* ===================================================================== */

/* ISensorService */
static sensor_service_err_t it_sensor_get_snapshot(sensor_snapshot_t *snap)
{
    *snap = s_sensor_snap;
    return SENSOR_SERVICE_ERR_OK;
}
static sensor_service_err_t it_sensor_init(void)
{
    return SENSOR_SERVICE_ERR_OK;
}
static sensor_service_err_t it_sensor_run_cycle(void)
{
    return SENSOR_SERVICE_ERR_OK;
}
static sensor_service_err_t it_sensor_subscribe(void (*cb)(const sensor_snapshot_t *))
{
    (void) cb;
    return SENSOR_SERVICE_ERR_OK;
}
static sensor_service_err_t it_sensor_read_on_demand(void)
{
    return SENSOR_SERVICE_ERR_OK;
}
static bool it_sensor_is_ready(void)
{
    return true;
}

static const isensor_service_t s_sensor_vtable = {
    .init = it_sensor_init,
    .run_cycle = it_sensor_run_cycle,
    .get_snapshot = it_sensor_get_snapshot,
    .subscribe = it_sensor_subscribe,
    .read_on_demand = it_sensor_read_on_demand,
    .is_ready = it_sensor_is_ready,
};

/* IAlarmService */
static alarm_service_err_t it_alarm_init(void)
{
    return ALARM_SERVICE_ERR_OK;
}
static alarm_service_err_t it_alarm_get_state(sensor_id_t s, alarm_state_t *o)
{
    *o = s_alarm_states[s];
    return ALARM_SERVICE_ERR_OK;
}
static alarm_service_err_t it_alarm_get_all_states(alarm_state_t states[SENSOR_ID_COUNT])
{
    memcpy(states, s_alarm_states, SENSOR_ID_COUNT * sizeof(alarm_state_t));
    return ALARM_SERVICE_ERR_OK;
}
static alarm_service_err_t it_alarm_subscribe(void (*cb)(sensor_id_t, alarm_event_t,
                                                         const sensor_reading_t *))
{
    (void) cb;
    return ALARM_SERVICE_ERR_OK;
}
static alarm_service_err_t it_alarm_ack_all(void)
{
    return ALARM_SERVICE_ERR_OK;
}

static const ialarm_service_t s_alarm_vtable = {
    .init = it_alarm_init,
    .get_state = it_alarm_get_state,
    .get_all_states = it_alarm_get_all_states,
    .subscribe = it_alarm_subscribe,
    .ack_all = it_alarm_ack_all,
};

/* IConfigProvider */
static const config_params_t *it_cfg_get_params(void)
{
    return &s_params;
}

static const iconfig_provider_t s_cfg_read_vtable = {
    .get_params = it_cfg_get_params,
};

/* IConfigManager */
static config_service_err_t it_cfg_set_param(config_param_id_t id, const void *value)
{
    (void) id;
    (void) value;
    s_set_param_called = true;
    /* Update s_params so read-back is consistent */
    if (id == CONFIG_PARAM_POLL_INTERVAL)
    {
        s_params.polling_interval_ms = *(const uint32_t *) value;
    }
    return CONFIG_SERVICE_OK;
}
static config_service_err_t it_cfg_init(const iconfig_store_t *store)
{
    (void) store;
    return CONFIG_SERVICE_OK;
}
static config_service_err_t it_cfg_apply_loaded(const void *blob, uint32_t len)
{
    (void) blob;
    (void) len;
    return CONFIG_SERVICE_OK;
}
static config_service_err_t it_cfg_validate_param(config_param_id_t id, const void *value)
{
    (void) id;
    (void) value;
    return CONFIG_SERVICE_OK;
}
static config_service_err_t it_cfg_snapshot(void)
{
    return CONFIG_SERVICE_OK;
}
static config_service_err_t it_cfg_restore_snapshot(void)
{
    return CONFIG_SERVICE_OK;
}
static config_service_err_t it_cfg_flush(void)
{
    return CONFIG_SERVICE_OK;
}
static config_service_err_t it_cfg_register_change_cb(config_change_cb_t cb)
{
    (void) cb;
    return CONFIG_SERVICE_OK;
}

static iconfig_manager_t s_cfg_write_vtable = {
    .init = it_cfg_init,
    .apply_loaded = it_cfg_apply_loaded,
    .set_param = it_cfg_set_param,
    .validate_param = it_cfg_validate_param,
    .snapshot = it_cfg_snapshot,
    .restore_snapshot = it_cfg_restore_snapshot,
    .flush = it_cfg_flush,
    .register_change_callback = it_cfg_register_change_cb,
};

/* IHealthSnapshot */
static health_monitor_err_t it_health_get_snapshot(device_health_snapshot_t *snap)
{
    *snap = s_health_snap;
    return HEALTH_MONITOR_ERR_OK;
}

static const ihealth_snapshot_t s_health_read_vtable = {
    .get_snapshot = it_health_get_snapshot,
};

/* IHealthReport */
static health_monitor_err_t it_health_init(void)
{
    return HEALTH_MONITOR_ERR_OK;
}
static health_monitor_err_t it_health_push_event(health_event_t e, uint32_t p)
{
    (void) e;
    (void) p;
    return HEALTH_MONITOR_ERR_OK;
}
static health_monitor_err_t it_health_update_modbus_stats(const modbus_slave_stats_t *stats)
{
    s_mb_stats = *stats;
    s_health_update_called = true;
    return HEALTH_MONITOR_ERR_OK;
}
static health_monitor_err_t it_health_update_stack_wm(const uint16_t wm[HEALTH_TASK_COUNT])
{
    (void) wm;
    return HEALTH_MONITOR_ERR_OK;
}
static void it_health_set_led_fault(void)
{
}

static ihealth_report_t s_health_report_vtable = {
    .init = it_health_init,
    .push_event = it_health_push_event,
    .update_modbus_slave_stats = it_health_update_modbus_stats,
    .update_stack_watermarks = it_health_update_stack_wm,
    .set_led_fault = it_health_set_led_fault,
};

/* ITimeProvider */
static time_provider_err_t it_time_get(time_provider_ts_t *ts)
{
    ts->epoch = 0;
    ts->sync_state = TIME_SYNC_UNSYNCHRONISED;
    return TIME_PROVIDER_ERR_OK;
}
static time_provider_err_t it_time_set(uint32_t e)
{
    (void) e;
    return TIME_PROVIDER_ERR_OK;
}
static time_provider_err_t it_time_mark_unsync(void)
{
    return TIME_PROVIDER_ERR_OK;
}
static time_sync_state_t it_time_get_sync(void)
{
    return TIME_SYNC_UNSYNCHRONISED;
}

static const itime_provider_t s_time_vtable = {
    .get = it_time_get,
    .set_time = it_time_set,
    .mark_unsynchronised = it_time_mark_unsync,
    .get_sync_state = it_time_get_sync,
};

/* IModbusSlaveStats */
static modbus_slave_stats_t s_mb_stats_live;
static void it_mb_stats_snapshot(modbus_slave_stats_t *out)
{
    *out = s_mb_stats_live;
}

static const imodbus_slave_stats_t s_mb_stats_vtable = {
    .snapshot = it_mb_stats_snapshot,
};

/* IModbusSlave */
static void it_set_slave_address(uint8_t new_addr)
{
    s_slave_addr_set = new_addr;
    s_slave_addr_set_called = true;
}

static imodbus_slave_t s_mb_slave_vtable = {
    .set_slave_address = it_set_slave_address,
};

/* ILifecycleController */
static void it_handle_remote_command(lc_remote_cmd_t cmd)
{
    (void) cmd;
    s_lc_cmd_called = true;
}

static ilifecycle_controller_t s_lifecycle_vtable = {
    .handle_remote_command = it_handle_remote_command,
};

/* ===================================================================== */
/* Test task                                                             */
/* ===================================================================== */

static StaticTask_t s_test_tcb;
static StackType_t s_test_stack[TEST_TASK_STACK_WORDS] __attribute__((aligned(8)));

static modbus_register_map_t *s_mrm;
static imodbus_register_map_t s_iface;

static void test_task(void *arg)
{
    (void) arg;
    uint32_t tick = 0U;

    /* ------------------------------------------------------------------ */
    /* Phase 1 — identity registers                                        */
    /* ------------------------------------------------------------------ */
    {
        uint16_t buf[8];
        modbus_exception_t exc = s_iface.read_input_regs(s_iface.ctx, 0x0000u, 8u, buf);
        if (exc == MB_EXC_NONE && buf[0] == 1u && buf[7] == 0x1A45u)
        {
            LOG_INFO(LOG_MODULE, "Phase 1: MAP_VERSION=%u VENDOR_CODE=0x%04X [PASS]",
                     (unsigned) buf[0], (unsigned) buf[7]);
        }
        else
        {
            LOG_ERROR(LOG_MODULE, "Phase 1: identity read FAILED exc=%02X ver=%u code=0x%04X",
                      (unsigned) exc, (unsigned) buf[0], (unsigned) buf[7]);
        }
    }

    /* ------------------------------------------------------------------ */
    /* Phase 2 — sensor registers (stub values)                            */
    /* ------------------------------------------------------------------ */
    {
        uint16_t temp_buf = 0, hum_buf = 0;
        modbus_exception_t e1 = s_iface.read_input_regs(s_iface.ctx, 0x0010u, 1u, &temp_buf);
        modbus_exception_t e2 = s_iface.read_input_regs(s_iface.ctx, 0x0011u, 1u, &hum_buf);
        if (e1 == MB_EXC_NONE && e2 == MB_EXC_NONE && temp_buf == 2200u && hum_buf == 5000u)
        {
            LOG_INFO(LOG_MODULE, "Phase 2: TEMP=%u HUM=%u [PASS]", (unsigned) temp_buf,
                     (unsigned) hum_buf);
        }
        else
        {
            LOG_ERROR(LOG_MODULE, "Phase 2: sensor read FAILED temp=%u hum=%u", (unsigned) temp_buf,
                      (unsigned) hum_buf);
        }
    }

    /* ------------------------------------------------------------------ */
    /* Phase 3 — config read / write round-trip (sampling_period)          */
    /* ------------------------------------------------------------------ */
    {
        uint16_t period_buf = 0;
        modbus_exception_t exc = s_iface.read_holding_regs(s_iface.ctx, 0x0130u, 1u, &period_buf);
        if (exc == MB_EXC_NONE && period_buf == 1000u)
        {
            LOG_INFO(LOG_MODULE, "Phase 3: sampling_period read=%u [PASS]", (unsigned) period_buf);
        }
        else
        {
            LOG_ERROR(LOG_MODULE, "Phase 3: sampling_period read FAILED exc=%02X val=%u",
                      (unsigned) exc, (unsigned) period_buf);
        }

        s_set_param_called = false;
        exc = s_iface.write_single_reg(s_iface.ctx, 0x0130u, 2000u);
        if (exc == MB_EXC_NONE && s_set_param_called)
        {
            LOG_INFO(LOG_MODULE, "Phase 3: sampling_period write OK [PASS]");
        }
        else
        {
            LOG_ERROR(LOG_MODULE, "Phase 3: sampling_period write FAILED exc=%02X called=%d",
                      (unsigned) exc, (int) s_set_param_called);
        }
    }

    /* ------------------------------------------------------------------ */
    /* Phase 4 — Mediator: write slave address                             */
    /* ------------------------------------------------------------------ */
    {
        s_slave_addr_set_called = false;
        s_slave_addr_set = 0;
        modbus_exception_t exc = s_iface.write_single_reg(s_iface.ctx, 0x0150u, 5u);
        if (exc == MB_EXC_NONE && s_slave_addr_set_called && s_slave_addr_set == 5u)
        {
            LOG_INFO(LOG_MODULE, "Phase 4: slave addr write OK");
            LOG_INFO(LOG_MODULE, "set_address called with %u [PASS]",
                     (unsigned) s_slave_addr_set);
        }
        else
        {
            LOG_ERROR(LOG_MODULE, "Phase 4: slave addr FAILED exc=%02X called=%d addr=%u",
                      (unsigned) exc, (int) s_slave_addr_set_called, (unsigned) s_slave_addr_set);
        }
    }

    /* ------------------------------------------------------------------ */
    /* Phase 5 — poll_stats                                                */
    /* ------------------------------------------------------------------ */
    {
        s_mb_stats_live.valid_frames = 42u;
        s_health_update_called = false;

        modbus_register_map_err_t rc = modbus_register_map_poll_stats(s_mrm);
        if (rc == MRM_ERR_OK && s_health_update_called && s_mb_stats.valid_frames == 42u)
        {
            LOG_INFO(LOG_MODULE, "Phase 5: poll_stats OK, valid_frames=%lu [PASS]",
                     (unsigned long) s_mb_stats.valid_frames);
        }
        else
        {
            LOG_ERROR(LOG_MODULE, "Phase 5: poll_stats FAILED rc=%d updated=%d frames=%lu",
                      (int) rc, (int) s_health_update_called,
                      (unsigned long) s_mb_stats.valid_frames);
        }
    }

    /* ------------------------------------------------------------------ */
    /* Phase 6 — periodic tick                                             */
    /* ------------------------------------------------------------------ */
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(10000U));
        tick++;
        LOG_INFO(LOG_MODULE, "tick %lu", (unsigned long) tick);
    }
}

/* ===================================================================== */
/* main                                                                  */
/* ===================================================================== */

int main(void)
{
    system_clock_init();
    debug_uart_init();
    rtc_init();
    logger_init(LOG_LEVEL_DEBUG);

    LOG_INFO(LOG_MODULE, "===== ModbusRegisterMap integration test =====");

    /* Default stub params */
    memset(&s_params, 0, sizeof s_params);
    s_params.temp_alarm_high = 4000;
    s_params.temp_alarm_low = 0;
    s_params.humidity_alarm_high = 8000;
    s_params.humidity_alarm_low = 2000;
    s_params.pressure_alarm_high = 10500;
    s_params.pressure_alarm_low = 9500;
    s_params.polling_interval_ms = 1000;
    s_params.modbus_slave_addr = 1;

    /* Default sensor readings */
    memset(&s_sensor_snap, 0, sizeof s_sensor_snap);
    s_sensor_snap.readings[SENSOR_ID_TEMPERATURE].value = 2200;
    s_sensor_snap.readings[SENSOR_ID_TEMPERATURE].valid = true;
    s_sensor_snap.readings[SENSOR_ID_HUMIDITY].value = 5000;
    s_sensor_snap.readings[SENSOR_ID_HUMIDITY].valid = true;
    s_sensor_snap.readings[SENSOR_ID_PRESSURE].value = 10132;
    s_sensor_snap.readings[SENSOR_ID_PRESSURE].valid = true;

    memset(s_alarm_states, 0, sizeof s_alarm_states);
    memset(&s_health_snap, 0, sizeof s_health_snap);
    s_health_snap.lifecycle_state = LIFECYCLE_STATE_OPERATIONAL;
    memset(&s_mb_stats_live, 0, sizeof s_mb_stats_live);

    s_mrm = modbus_register_map_instance();

    modbus_register_map_err_t rc = modbus_register_map_init(
        s_mrm, &s_sensor_vtable, &s_alarm_vtable, &s_cfg_read_vtable, &s_cfg_write_vtable,
        &s_health_read_vtable, &s_health_report_vtable, &s_time_vtable, &s_mb_stats_vtable,
        &s_mb_slave_vtable, &s_lifecycle_vtable);
    if (rc != MRM_ERR_OK)
    {
        LOG_ERROR(LOG_MODULE, "modbus_register_map_init FAILED rc=%d", (int) rc);
        for (;;)
        {
        }
    }
    LOG_INFO(LOG_MODULE, "modbus_register_map_init OK");

    modbus_register_map_get_iface(s_mrm, &s_iface);

    LOG_INFO(LOG_MODULE, "starting scheduler...");

    (void) xTaskCreateStatic(test_task, "mrm_it", TEST_TASK_STACK_WORDS, NULL, TEST_TASK_PRIORITY,
                             s_test_stack, &s_test_tcb);

    vTaskStartScheduler();

    for (;;)
    {
    } /* should never reach here */
}
