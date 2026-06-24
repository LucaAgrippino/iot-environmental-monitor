/**
 * @file test_console_service_main.c
 * @brief ConsoleService integration test — STM32F469I-DISCO (Field Device).
 *
 * Wires all ConsoleService dependencies, starts ConsoleTask, and presents the
 * operator serial console.  Connect a terminal (115200 8N1) to the ST-Link VCP
 * and interact using the commands listed in the visual checklist below.
 *
 * Activation in CubeIDE:
 *   - Right-click firmware/main.c → Resource Configurations →
 *     Exclude from Build → All Configurations.
 *   - Add integration-tests/console_service/ to project source paths
 *     (Project Properties → C/C++ General → Paths and Symbols).
 *   - Build, flash, open PuTTY at 115200/8N1 on the ST-Link VCP.
 *
 * VISUAL CHECKLIST
 * ================
 * | # | What to observe                                                | Verifies                               |
 * |---|----------------------------------------------------------------|----------------------------------------|
 * | 1 | Boot message "ConsoleService integration test" then "fd>" prompt | Init succeeds; prompt emitted        |
 * | 2 | Type "help" → all command tokens listed (help, version, sensors, status, alarms, selftest, config, prov, modbus) | Dispatch table complete |
 * | 3 | Type "version" → firmware version string printed               | cmd_version handler works              |
 * | 4 | Type "serial" → "UID: XXXXXXXX-XXXXXXXX-XXXXXXXX" (12 hex bytes) | MCU UID read correctly              |
 * | 5 | Type "sensors" → temperature, humidity, pressure values or INVALID | ISensorService wired correctly    |
 * | 6 | Type "status" → uptime_s and sensor_fail_count fields          | IHealthSnapshot wired correctly        |
 * | 7 | Type "alarms" → "No active alarms" (normal state)             | Alarm scan works; clear = no output    |
 * | 8 | Type "selftest" → Sensors/Comms/Flash rows each PASS or FAIL, then Overall | All three probes exercised  |
 * | 9 | Type "selftest-result" → same table as step 8                 | Last result stored and retrievable     |
 * | 10| Type "config list" → polling-interval-ms and all alarm thresholds | IConfigProvider.get_params wired    |
 * | 11| Type "config set polling-interval-ms 2000" → "[OK] staged"    | Staging works; dirty flag set          |
 * | 12| Type "config commit" → "Apply? [y/N]:" prompt; type y → "[OK] Config applied." | Confirm + apply path |
 * | 13| Type "config set polling-interval-ms xyz" → "[ERR] invalid value" | Non-numeric rejection          |
 * | 14| Type "config set unknown-key 1" → "[ERR] unknown key"         | Unknown-key path                       |
 * | 15| Type "config discard" → "[INFO] Discarded."                   | Discard path clears staging            |
 * | 16| Type "prov set modbus-addr 50" → "[OK] staged"                | Prov staging works                     |
 * | 17| Type "prov commit" → "Apply? [y/N]:" prompt; type y → "[OK] Provisioning applied." | Prov commit + flush |
 * | 18| Type "modbus status" → valid/CRC/mismatch counters            | FD-specific command compiled in        |
 * | 19| Type "frobnicate" → "[ERR] unknown command 'frobnicate'. Type 'help'." | Unknown-command path |
 */

#include "stm32f469xx.h"

#include "FreeRTOS.h"
#include "task.h"

#include "system_clock.h"
#include "gpio/gpio_driver.h"
#include "qspi_flash_driver/qspi_flash_driver.h"
#include "debug-uart/debug_uart_driver.h"
#include "rtc/rtc_driver.h"
#include "logger/logger.h"
#include "health_monitor/health_monitor.h"
#include "config_store/config_store.h"
#include "config_service/config_service.h"
#include "sensor_service/sensor_service.h"
#include "console_service/console_service.h"

/* ========================================================================= */
/* Board-specific LED macros (STM32F469I-DISCO)                              */
/* ========================================================================= */

#define LED_GREEN_PIN (1UL << 13U) /* PG13 — active high */
#define LED_RED_PIN   (1UL << 5U)  /* PD5  — active high */

static void led_green_on(void)
{
    GPIOG->BSRR = LED_GREEN_PIN;
}
static void led_red_on(void)
{
    GPIOD->BSRR = LED_RED_PIN;
}
static void led_green_off(void)
{
    GPIOG->BSRR = (LED_GREEN_PIN << 16U);
}

/* ========================================================================= */
/* ConsoleTask static storage (companion §1: 512 words = 2 KiB)             */
/* ========================================================================= */

#define CONSOLE_TASK_STACK_WORDS (512U)
#define CONSOLE_TASK_PRIORITY    (tskIDLE_PRIORITY + 1U)

static StackType_t  s_console_stack[CONSOLE_TASK_STACK_WORDS];
static StaticTask_t s_console_tcb;

/* ========================================================================= */
/* Integration test tag                                                      */
/* ========================================================================= */

#define IT_TAG "IT"

/* ========================================================================= */
/* main                                                                      */
/* ========================================================================= */

int main(void)
{
    /* 1. Clock tree — 180 MHz. */
    system_clock_init();

    /* 2. GPIO peripheral clocks — required by QSPI and board LEDs. */
    (void) gpio_init();

    /* 3. Drivers that have no inter-driver dependencies. */
    (void) debug_uart_init();
    (void) rtc_init();
    (void) qspi_flash_init();

    /* 4. HealthMonitor must precede ConfigStore because ConfigStore
     *    reports write faults through IHealthReport. */
    (void) health_monitor_init();

    /* 5. Logger.  Pre-scheduler calls take the synchronous-write path. */
    (void) logger_init(LOG_LEVEL_DEBUG);

    /* 6. ConfigStore (backed by QSPI flash). */
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

    /* 8. SensorService — initialises the sensor driver chain internally. */
    (void) sensor_service_init();

    /* 9. ConsoleService — wires all injected interfaces. */
    (void) console_service_init(debug_uart, sensor_service, config_provider,
                                config_manager, health_snapshot);

    /* 10. Pre-scheduler diagnostic so the operator knows the boot completed. */
    LOG_INFO(IT_TAG, "ConsoleService integration test — type 'help' for commands");

    /* 11. Create ConsoleTask (static allocation — no heap). */
    (void) xTaskCreateStatic(console_task_body, "Console", CONSOLE_TASK_STACK_WORDS, NULL,
                             CONSOLE_TASK_PRIORITY, s_console_stack, &s_console_tcb);

    /* 12. Signal board-ready: green LED on, red LED off. */
    led_green_on();
    led_green_off(); /* toggle briefly to show startup complete */
    led_green_on();

    vTaskStartScheduler();

    /* Should never reach here — indicate error via red LED. */
    led_red_on();
    for (;;) {}

    return 0;
}
