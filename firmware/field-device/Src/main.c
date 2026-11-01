/**
 * @file main.c
 * @brief Field Device application entry point (STM32F469I-DISCO).
 *
 * Wires the real drivers, middleware and application modules, creates the four application
 * tasks of the HLD task table (docs/hld/task-breakdown.md section 4.2; LcdUiTask is not built
 * yet) and starts the scheduler. LifecycleController runs the boot sequence and releases the
 * start gate; the Modbus slave answers the Gateway on USART6.
 *
 * Not part of the CubeIDE build (Src/main.c is excluded there): build and flash with
 * scripts/build-fd-app.sh. The older bring-up harness in
 * integration-tests/lifecycle_controller is what CubeIDE still links.
 *
 * Creation order (all static, no heap):
 *   1. clock, GPIO, LEDs, RTC, debug UART, I2C, QSPI, Modbus UART
 *   2. HealthMonitor, Logger, TimeProvider, ConfigStore, ConfigService (+ stored values)
 *   3. ConsoleService, ModbusRegisterMap, LifecycleController
 *   4. tasks: Sensor, Modbus, Console, Lifecycle (last), then the scheduler
 *
 * SensorService binds its poll timer to the task that calls sensor_service_init(), so the init
 * runs in SensorTask. LifecycleController's BringingUpSensors step is given adapters that wait
 * for that task instead of initialising a second time.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "alarm_service/alarm_service.h"
#include "config_service/config_service.h"
#include "config_store/config_store.h"
#include "console_service/console_service.h"
#include "debug-uart/debug_uart_driver.h"
#include "gpio/gpio_driver.h"
#include "health_monitor/health_monitor.h"
#include "i2c/i2c_driver.h"
#include "led/led_driver.h"
#include "lifecycle_controller/lifecycle_controller.h"
#include "logger/logger.h"
#include "modbus_register_map/modbus_register_map.h"
#include "modbus_slave/modbus_slave.h"
#include "modbus_uart_driver/modbus_uart_driver.h"
#include "qspi_flash_driver/qspi_flash_driver.h"
#include "rtc/rtc_driver.h"
#include "sensor_service/sensor_service.h"
#include "stm32f469xx.h"
#include "firmware_version.h"
#include "system_clock.h"
#include "time_provider/time_provider.h"

#define MODULE_TAG "Main"

/* ======================================================================= */
/* Tasks: priorities and stacks from docs/hld/task-breakdown.md 4.2         */
/* ======================================================================= */

#define SENSOR_TASK_PRIORITY (5U)
#define SENSOR_TASK_STACK_WORDS (768U)
#define MODBUS_TASK_PRIORITY (4U)
#define MODBUS_TASK_STACK_WORDS (512U)
#define CONSOLE_TASK_PRIORITY (1U)
#define CONSOLE_TASK_STACK_WORDS (768U)
#define LC_TASK_PRIORITY (1U)
#define LC_TASK_STACK_WORDS (2048U)
#define REPORT_TASK_PRIORITY (1U)
#define REPORT_TASK_STACK_WORDS (384U)

/** Time the Lifecycle step waits for SensorTask to finish its init. */
#define SENSOR_READY_WAIT_MS (2000U)
/** Delay before the one-shot stack report, long enough for boot and a few Modbus requests. */
#define STACK_REPORT_DELAY_MS (30000U)

static StackType_t s_sensor_stack[SENSOR_TASK_STACK_WORDS] __attribute__((aligned(8)));
static StaticTask_t s_sensor_tcb;
static StackType_t s_modbus_stack[MODBUS_TASK_STACK_WORDS] __attribute__((aligned(8)));
static StaticTask_t s_modbus_tcb;
static StackType_t s_console_stack[CONSOLE_TASK_STACK_WORDS] __attribute__((aligned(8)));
static StaticTask_t s_console_tcb;
static StackType_t s_lc_stack[LC_TASK_STACK_WORDS] __attribute__((aligned(8)));
static StaticTask_t s_lc_tcb;
static StackType_t s_report_stack[REPORT_TASK_STACK_WORDS] __attribute__((aligned(8)));
static StaticTask_t s_report_tcb;

static TaskHandle_t s_sensor_task;
static TaskHandle_t s_modbus_task;
static TaskHandle_t s_console_task;
static TaskHandle_t s_lc_task;

/* STM32F469I-DISCO user LEDs: LD1 green (PG6), LD3 red (PD5), both active low. */
static const led_pin_t k_fd_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_G, .pin = 6U, .active_high = false, .fitted = true},
    [LED_RED] = {.port = GPIO_PORT_D, .pin = 5U, .active_high = false, .fitted = true},
};

/* ======================================================================= */
/* SensorTask and its Lifecycle adapters                                    */
/* ======================================================================= */

static volatile bool s_sensors_ready;

/* The alarm log: the Field Device otherwise says nothing when a limit is crossed, which makes
 * the alarm-propagation test (2B-007) impossible to time from the log. */
static void on_alarm_event(sensor_id_t sensor, alarm_event_t event, const sensor_reading_t *reading)
{
    static const char *const names[] = {"TEMPERATURE", "HUMIDITY", "PRESSURE"};
    const char *name = ((unsigned) sensor < 3U) ? names[sensor] : "OTHER";

    if (event == ALARM_EVENT_CLEARED)
    {
        LOG_INFO(MODULE_TAG, "alarm cleared: %s (%ld)", name, (long) reading->value);
    }
    else
    {
        LOG_WARN(MODULE_TAG, "alarm raised %s: %s (%ld)",
                 (event == ALARM_EVENT_RAISED_HIGH) ? "HIGH" : "LOW", name, (long) reading->value);
    }
}

static void sensor_task(void *arg)
{
    (void) arg;

    if ((sensor_service_init() != SENSOR_SERVICE_ERR_OK) ||
        (alarm_service_init() != ALARM_SERVICE_ERR_OK))
    {
        LOG_ERROR(MODULE_TAG, "SensorTask: sensor or alarm init failed");
        vTaskDelete(NULL);
        return;
    }
    (void) alarm_service_subscribe(on_alarm_event);
    s_sensors_ready = true;

    for (;;)
    {
        (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* 100 ms poll timer */
        (void) sensor_service_run_cycle();
    }
}

static bool wait_sensors_ready(void)
{
    for (uint32_t waited = 0U; waited < SENSOR_READY_WAIT_MS; waited += 10U)
    {
        if (s_sensors_ready)
        {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    return s_sensors_ready;
}

static sensor_service_err_t lc_sensors_init(void)
{
    return wait_sensors_ready() ? SENSOR_SERVICE_ERR_OK : SENSOR_SERVICE_ERR_NOT_INIT;
}

static alarm_service_err_t lc_alarms_init(void)
{
    return wait_sensors_ready() ? ALARM_SERVICE_ERR_OK : ALARM_SERVICE_ERR_NOT_INIT;
}

static isensor_service_t s_lc_sensors;
static ialarm_service_t s_lc_alarms;

/* ======================================================================= */
/* IModbusSlave stats and ILifecycleController views for the register map   */
/* ======================================================================= */

static void mb_stats_snapshot(modbus_slave_stats_t *out)
{
    (void) modbus_slave_get_stats(out);
}

static const imodbus_slave_stats_t s_mb_stats_vtable = {
    .snapshot = mb_stats_snapshot,
};

static lifecycle_err_t lc_handle_remote_command(lifecycle_remote_cmd_t cmd)
{
    (void) lifecycle_controller->handle_remote_command(cmd);
    return LIFECYCLE_ERR_OK;
}

static ilifecycle_t s_lc_ctrl_vtable = {
    .handle_remote_command = lc_handle_remote_command,
};

/* ======================================================================= */
/* Legacy IModbusRegisterMap bridge (MRM-DEVIATION-001, still in force)                    */
/*                                                                         */
/* modbus_slave.c uses the old per-register IModbusRegisterMap API.        */
/* ModbusRegisterMap provides the new bulk imodbus_register_map_t API.     */
/* This bridge adapts between the two until the refactor is complete.      */
/* ======================================================================= */

static imodbus_register_map_t s_mrm_iface;

static modbus_slave_err_t bridge_read_input(uint16_t addr, uint16_t *val)
{
    modbus_exception_t exc = s_mrm_iface.read_input_regs(s_mrm_iface.ctx, addr, 1U, val);
    return (exc == MB_EXC_NONE) ? MODBUS_SLAVE_ERR_OK : MODBUS_SLAVE_ERR_INVALID_ADDR;
}

static modbus_slave_err_t bridge_read_holding(uint16_t addr, uint16_t *val)
{
    modbus_exception_t exc = s_mrm_iface.read_holding_regs(s_mrm_iface.ctx, addr, 1U, val);
    return (exc == MB_EXC_NONE) ? MODBUS_SLAVE_ERR_OK : MODBUS_SLAVE_ERR_INVALID_ADDR;
}

static modbus_slave_err_t bridge_exc_to_err(modbus_exception_t exc)
{
    modbus_slave_err_t err = MODBUS_SLAVE_ERR_OK;

    if (exc == MB_EXC_ILLEGAL_DATA_VALUE)
    {
        err = MODBUS_SLAVE_ERR_INVALID_VALUE;
    }
    else if (exc != MB_EXC_NONE)
    {
        err = MODBUS_SLAVE_ERR_INVALID_ADDR;
    }
    return err;
}

static modbus_slave_err_t bridge_write_holding(uint16_t addr, uint16_t val)
{
    return bridge_exc_to_err(s_mrm_iface.write_single_reg(s_mrm_iface.ctx, addr, val));
}

/* FC16 reaches the map as one block, so the CMD_SET_TIME pair is seen whole (B2). */
static modbus_slave_err_t bridge_write_block(uint16_t addr, const uint16_t *vals, uint16_t count)
{
    return bridge_exc_to_err(s_mrm_iface.write_multiple_regs(s_mrm_iface.ctx, addr, count, vals));
}

static const IModbusRegisterMap s_legacy_map = {
    .read_input = bridge_read_input,
    .read_holding = bridge_read_holding,
    .write_holding = bridge_write_holding,
    .write_block = bridge_write_block,
};

/* ======================================================================= */
/* ModbusTask: two-phase Modbus slave init                                  */
/* ======================================================================= */

static void modbus_task(void *arg)
{
    (void) arg;

    const config_params_t *cfg = config_provider->get_params();
    const uint8_t slave_addr = (cfg != NULL) ? (uint8_t) cfg->modbus_slave_addr : 1U;

    if (modbus_slave_init(&s_legacy_map, slave_addr, xTaskGetCurrentTaskHandle()) !=
        MODBUS_SLAVE_ERR_OK)
    {
        LOG_ERROR(MODULE_TAG, "modbus_slave_init failed");
        vTaskDelete(NULL);
        return;
    }
    LOG_INFO(MODULE_TAG, "Modbus slave %u ready", (unsigned) slave_addr);

    for (;;)
    {
        (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        (void) modbus_slave_process();
    }
}

/* ======================================================================= */
/* One-shot stack report (measured marks for the task table, LLD O2)        */
/* ======================================================================= */

static void report_stack(const char *name, TaskHandle_t t, uint32_t words)
{
    LOG_INFO(MODULE_TAG, "stack %-10s free %4lu of %4lu words", name,
             (unsigned long) uxTaskGetStackHighWaterMark(t), (unsigned long) words);
}

static void report_task(void *arg)
{
    (void) arg;
    vTaskDelay(pdMS_TO_TICKS(STACK_REPORT_DELAY_MS));
    report_stack("sensor", s_sensor_task, SENSOR_TASK_STACK_WORDS);
    report_stack("modbus", s_modbus_task, MODBUS_TASK_STACK_WORDS);
    report_stack("console", s_console_task, CONSOLE_TASK_STACK_WORDS);
    report_stack("lifecycle", s_lc_task, LC_TASK_STACK_WORDS);
    vTaskDelete(NULL);
}

/* ======================================================================= */
/* main                                                                     */
/* ======================================================================= */

int main(void)
{
    /* Reset cause first: the flags are cleared by reading them. */
    const lifecycle_reset_cause_t reset_cause = lifecycle_detect_reset_cause();

    /* 1. Clock tree (180 MHz) and drivers with no dependencies on each other. */
    system_clock_init();
    (void) gpio_init();
    (void) led_init(k_fd_led_pins, LED_COUNT);
    (void) rtc_init();
    (void) debug_uart_init();
    (void) i2c_init();
    (void) qspi_flash_init();
    (void) modbus_uart_init();

    /* 2. Middleware. HealthMonitor first (ConfigStore and TimeProvider report to it), then the
     *    Logger (its pre-scheduler calls write synchronously). */
    (void) health_monitor_init();
    (void) logger_init(LOG_LEVEL_INFO);
    (void) time_provider_init(health_report);
    (void) config_store_init(health_report);

    /* 3. Configuration: defaults, then the stored values, so the Modbus task sees the stored
     *    slave address when it starts. LifecycleController loads and applies them again. */
    (void) config_service_init(config_store);
    {
        config_blob_t blob;
        uint32_t len = 0U;

        if (config_store->load(&blob, &len, sizeof(blob)) == CONFIG_STORE_OK)
        {
            (void) config_service_apply_loaded(&blob, len);
        }
    }

    /* 4. Application services that need no task. */
    (void) console_service_init(debug_uart, sensor_service, config_provider, config_manager,
                                health_snapshot);
    modbus_register_map_t *mrm = modbus_register_map_instance();

    (void) modbus_register_map_init(
        mrm, sensor_service, alarm_service, config_provider, (iconfig_manager_t *) config_manager,
        health_snapshot, (ihealth_report_t *) health_report, time_provider, &s_mb_stats_vtable,
        (imodbus_slave_t *) modbus_slave, &s_lc_ctrl_vtable);
    modbus_register_map_get_iface(mrm, &s_mrm_iface);

    /* 5. LifecycleController, given adapters for the sensor and alarm bring-up step. */
    s_lc_sensors = *sensor_service;
    s_lc_sensors.init = lc_sensors_init;
    s_lc_alarms = *alarm_service;
    s_lc_alarms.init = lc_alarms_init;
    (void) lifecycle_controller_init(reset_cause, config_store, config_provider, config_manager,
                                     &s_lc_sensors, &s_lc_alarms, console_service, health_report,
                                     modbus_slave);

    LOG_INFO(MODULE_TAG, "Field Device %s, starting the scheduler", FW_VERSION_STRING);

    /* 6. Tasks. LifecycleTask is created last. */
    s_sensor_task = xTaskCreateStatic(sensor_task, "SensorTask", SENSOR_TASK_STACK_WORDS, NULL,
                                      SENSOR_TASK_PRIORITY, s_sensor_stack, &s_sensor_tcb);
    s_modbus_task = xTaskCreateStatic(modbus_task, "ModbusTask", MODBUS_TASK_STACK_WORDS, NULL,
                                      MODBUS_TASK_PRIORITY, s_modbus_stack, &s_modbus_tcb);
    s_console_task = xTaskCreateStatic(console_task_body, "Console", CONSOLE_TASK_STACK_WORDS, NULL,
                                       CONSOLE_TASK_PRIORITY, s_console_stack, &s_console_tcb);
    (void) xTaskCreateStatic(report_task, "StackReport", REPORT_TASK_STACK_WORDS, NULL,
                             REPORT_TASK_PRIORITY, s_report_stack, &s_report_tcb);
    s_lc_task = xTaskCreateStatic(lifecycle_task_body, "LifecycleTask", LC_TASK_STACK_WORDS, NULL,
                                  LC_TASK_PRIORITY, s_lc_stack, &s_lc_tcb);

    vTaskStartScheduler();

    for (;;)
    {
    }
}
