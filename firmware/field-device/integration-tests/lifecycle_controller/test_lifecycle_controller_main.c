/**
 * @file test_lifecycle_controller_main.c
 * @brief On-target integration test scaffold — IT-LC-001..014.
 *
 * Executed manually on STM32F469I-DISCO (FD) with a debug-UART terminal.
 * Each test case corresponds to a physical setup step; the pass criterion
 * is checked via UART log messages and observable hardware state
 * (LED fault pattern, splash/dismiss on LCD).
 *
 * This file drives the lifecycle_controller module in a real FreeRTOS
 * environment and logs each state transition.  No automated assertion
 * framework is used; the technician reads the UART log.
 *
 * Build: include in the field-device firmware project, compile with
 *   -DBOARD_FIELD_DEVICE -DINTEGRATION_TEST
 *
 * @see docs/lld/application/lifecycle-controller.md §22
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "stm32f469xx.h"
#include "system_clock.h"
#include "gpio/gpio_driver.h"
#include "debug-uart/debug_uart_driver.h"
#include "rtc/rtc_driver.h"
#include "qspi_flash_driver/qspi_flash_driver.h"
#include "modbus_uart_driver/modbus_uart_driver.h"
#include "i2c/i2c_driver.h"
#include "logger/logger.h"
#include "health_monitor/health_monitor.h"
#include "config_store/config_store.h"
#include "config_service/config_service.h"
#include "sensor_service/sensor_service.h"
#include "alarm_service/alarm_service.h"
#include "console_service/console_service.h"
#include "modbus_register_map/modbus_register_map.h"
#include "modbus_slave/modbus_slave.h"
#include "lifecycle_controller/lifecycle_controller.h"

/* ======================================================================= */
/* Integration-test configuration                                          */
/* ======================================================================= */

#define IT_TASK_STACK_WORDS (2048U)
#define IT_MONITOR_PERIOD_MS (500U)

/* ======================================================================= */
/* Monitor task — logs lifecycle state transitions on UART                 */
/* ======================================================================= */

static StackType_t  s_monitor_stack[IT_TASK_STACK_WORDS];
static StaticTask_t s_monitor_tcb;

static void monitor_task(void *arg)
{
    (void)arg;
    lifecycle_state_t prev_state = LIFECYCLE_STATE_INIT;

    /* Wait for the start-gate before entering the polling loop */
    EventGroupHandle_t gate = lifecycle_get_start_gate();
    (void)xEventGroupWaitBits(gate, LIFECYCLE_START_GATE_BIT,
                              pdFALSE, pdTRUE, portMAX_DELAY);

    LOG_INFO("[IT-LC]","Start-gate released — Operational entered");

    for (;;)
    {
        lifecycle_state_t cur = lifecycle_controller->get_state();
        if (cur != prev_state)
        {
            static const char *const state_names[] = {
                "INIT", "OPERATIONAL", "EDITING_CONFIG",
                "RESTARTING", "UPDATING_FW", "FAULTED"
            };
            const char *name = (cur < 6U) ? state_names[cur] : "???";
            LOG_INFO("[IT-LC] State → %s  (reset_cause=%u)",
                     name,
                     (unsigned)lifecycle_controller->get_reset_cause());
            prev_state = cur;
        }
        vTaskDelay(pdMS_TO_TICKS(IT_MONITOR_PERIOD_MS));
    }
}

/* ======================================================================= */
/* it_lc_start — call from application main() after all service init       */
/* ======================================================================= */

/**
 * @brief Launch the lifecycle controller integration-test monitor task.
 *
 * Call this after lifecycle_controller_init() and before vTaskStartScheduler().
 * The lifecycle_task_body() must also be created by the application main().
 */
void it_lc_start(void)
{
    (void)xTaskCreateStatic(monitor_task,
                            "IT_LC_Monitor",
                            IT_TASK_STACK_WORDS,
                            NULL,
                            tskIDLE_PRIORITY + 1U,
                            s_monitor_stack,
                            &s_monitor_tcb);

    LOG_INFO("[IT-LC]", "Integration test monitor started");
    LOG_INFO("[IT-LC]", "IT-LC-001: Cold boot FD -> 5 sub-states then Operational");
    LOG_INFO("[IT-LC]", "IT-LC-003: Corrupt config — expect Faulted + LED fault");
    LOG_INFO("[IT-LC]", "IT-LC-005: 'config commit' -> EditingConfig -> Operational");
    LOG_INFO("[IT-LC]", "IT-LC-007: 5min idle -> Operational (auto-cancel)");
}

/* ======================================================================= */
/* ConsoleTask static storage (§1: 512 words = 2 KiB)                     */
/* ======================================================================= */

#define CONSOLE_TASK_STACK_WORDS (512U)
#define CONSOLE_TASK_PRIORITY    (tskIDLE_PRIORITY + 1U)

static StackType_t  s_console_stack[CONSOLE_TASK_STACK_WORDS] __attribute__((aligned(8)));
static StaticTask_t s_console_tcb;

/* ======================================================================= */
/* ModbusTask static storage (two-phase init — §17)                        */
/* ======================================================================= */

#define MODBUS_TASK_STACK_WORDS (512U)
#define MODBUS_TASK_PRIORITY    (tskIDLE_PRIORITY + 1U)

static StackType_t  s_modbus_stack[MODBUS_TASK_STACK_WORDS] __attribute__((aligned(8)));
static StaticTask_t s_modbus_tcb;

/* LifecycleTask static storage (last task created per §17) */
#define LC_TASK_STACK_WORDS  (2048U)
#define LC_TASK_PRIORITY     (tskIDLE_PRIORITY + 2U)

static StackType_t  s_lc_stack[LC_TASK_STACK_WORDS] __attribute__((aligned(8)));
static StaticTask_t s_lc_tcb;

/* ======================================================================= */
/* IModbusSlave stats vtable (routes to modbus_slave_get_stats)            */
/* ======================================================================= */

static void it_mb_stats_snapshot(modbus_slave_stats_t *out)
{
    (void) modbus_slave_get_stats(out);
}

static const imodbus_slave_stats_t s_mb_stats_vtable = {
    .snapshot = it_mb_stats_snapshot,
};

/* ======================================================================= */
/* ILifecycleController vtable (routes to lifecycle_controller singleton)  */
/* ======================================================================= */

static lifecycle_err_t it_handle_remote_command(lifecycle_remote_cmd_t cmd)
{
    (void) lifecycle_controller->handle_remote_command((lifecycle_remote_cmd_t) cmd);
    return LIFECYCLE_ERR_OK;
}

static ilifecycle_t s_lc_ctrl_vtable = {
    .handle_remote_command = it_handle_remote_command,
};

/* ======================================================================= */
/* Legacy IModbusRegisterMap bridge (MRM-DEVIATION-001)                    */
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

static modbus_slave_err_t bridge_write_holding(uint16_t addr, uint16_t val)
{
    modbus_exception_t exc = s_mrm_iface.write_single_reg(s_mrm_iface.ctx, addr, val);
    return (exc == MB_EXC_NONE) ? MODBUS_SLAVE_ERR_OK : MODBUS_SLAVE_ERR_INVALID_ADDR;
}

static const IModbusRegisterMap s_legacy_map = {
    .read_input   = bridge_read_input,
    .read_holding = bridge_read_holding,
    .write_holding = bridge_write_holding,
};

/* ======================================================================= */
/* ModbusTask — two-phase Modbus slave init (§17)                          */
/* ======================================================================= */

static void modbus_task(void *arg)
{
    (void) arg;

    const config_params_t *cfg = config_provider->get_params();
    uint8_t slave_addr = (cfg != NULL) ? (uint8_t) cfg->modbus_slave_addr : 1U;

    modbus_slave_err_t err = modbus_slave_init(&s_legacy_map, slave_addr,
                                               xTaskGetCurrentTaskHandle());
    if (err != MODBUS_SLAVE_ERR_OK)
    {
        LOG_ERROR("[IT-LC]", "modbus_slave_init FAILED err=%u", (unsigned) err);
        vTaskDelete(NULL);
        return;
    }

    for (;;)
    {
        (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        (void) modbus_slave_process();
    }
}

/* ======================================================================= */
/* main                                                                    */
/* ======================================================================= */

int main(void)
{
    /* 1. Clock tree — 180 MHz. */
    system_clock_init();

    /* 2. GPIO peripheral clocks. */
    (void) gpio_init();

    /* 3. Drivers with no inter-driver dependencies. */
    (void) debug_uart_init();
    (void) rtc_init();
    (void) i2c_init();
    (void) qspi_flash_init();
    (void) modbus_uart_init();

    /* 4. HealthMonitor must precede ConfigStore (writes fault events). */
    (void) health_monitor_init();

    /* 5. Logger — pre-scheduler calls use the synchronous path. */
    (void) logger_init(LOG_LEVEL_DEBUG);

    /* 6. ConfigStore (QSPI-backed). */
    (void) config_store_init(health_report);

    /* 7. ConfigService with factory defaults; apply any persisted blob. */
    (void) config_service_init(config_store);
    {
        config_blob_t      blob;
        uint32_t           len    = 0U;
        config_store_err_t cs_err = config_store->load(&blob, &len, sizeof(blob));
        if (cs_err == CONFIG_STORE_OK)
        {
            (void) config_service_apply_loaded(&blob, len);
        }
    }

    /* 8. ConsoleService — wires injected interfaces; task calls init_finalise(). */
    (void) console_service_init(debug_uart, sensor_service, config_provider,
                                config_manager, health_snapshot);

    /* 9. ModbusRegisterMap — wire all providers. */
    modbus_register_map_t *mrm = modbus_register_map_instance();
    (void) modbus_register_map_init(mrm, sensor_service, alarm_service,
                                    config_provider, (iconfig_manager_t *) config_manager,
                                    health_snapshot, (ihealth_report_t *) health_report,
                                    time_provider,
                                    &s_mb_stats_vtable, (imodbus_slave_t *) modbus_slave,
                                    &s_lc_ctrl_vtable);
    modbus_register_map_get_iface(mrm, &s_mrm_iface);

    /* 10. LifecycleController — sensor/alarm init is delegated to the LC
     *     init sub-state sequence (BringingUpSensors). */
    lifecycle_reset_cause_t reset_cause = lifecycle_detect_reset_cause();
    (void) lifecycle_controller_init(reset_cause,
                                     config_store,
                                     config_provider,
                                     config_manager,
                                     sensor_service,
                                     alarm_service,
                                     console_service,
                                     health_report,
                                     modbus_slave);

    /* 11. Pre-scheduler banner. */
    LOG_INFO("[IT-LC]", "===== LifecycleController integration test =====");
    LOG_INFO("[IT-LC]", "starting scheduler...");

    /* 12. Create tasks — LifecycleTask LAST (all other tasks gate on
     *     the start-gate bit set by LifecycleController on Operational). */
    (void) xTaskCreateStatic(console_task_body, "Console",
                             CONSOLE_TASK_STACK_WORDS, NULL,
                             CONSOLE_TASK_PRIORITY,
                             s_console_stack, &s_console_tcb);

    (void) xTaskCreateStatic(modbus_task, "ModbusTask",
                             MODBUS_TASK_STACK_WORDS, NULL,
                             MODBUS_TASK_PRIORITY,
                             s_modbus_stack, &s_modbus_tcb);

    it_lc_start();

    (void) xTaskCreateStatic(lifecycle_task_body, "LifecycleTask",
                             LC_TASK_STACK_WORDS, NULL,
                             LC_TASK_PRIORITY,
                             s_lc_stack, &s_lc_tcb);

    vTaskStartScheduler();

    for (;;) {}
    return 0;
}
