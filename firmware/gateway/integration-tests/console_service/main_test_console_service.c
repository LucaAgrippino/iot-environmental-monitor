/**
 * @file main_test_console_service.c
 * @brief Hardware bring-up for ConsoleService (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. In the `integration-tests` source entry's `excluding=` list, add the current harness
 *      (cloud_publisher) and remove console_service.
 *   2. In the `application` entry's `excluding=`, remove `sensor_service|alarm_service`: this
 *      harness uses the real ones (the cloud_publisher harness stubs them).
 *   3. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/console_service.
 *   Treat that swap as bench-local; do not commit it.
 *
 * This is the first image where the real services meet: SensorService (via SensorTask),
 * AlarmService, ConfigService on ConfigStore (QSPI), HealthMonitor and the console, wired the way
 * a Gateway main() will wire them. The stored configuration is loaded at boot, so a `set` made in
 * the console survives a reset.
 *
 * The console owns the ST-Link virtual COM port (USART1, 115 200 8N1), so nothing else is
 * printed. Drive it from a terminal that is at least 88 x 30 and supports truecolor, or from
 * scripts/console_bench.py, which sends keystrokes and renders the screen.
 *
 * Checks (manual or scripted, see the LLD §23):
 *   IT-GCS-001  the TUI appears within 2 s of reset: banner, menu, chart
 *   IT-GCS-003..007  navigation, focus, channel cycling
 *   IT-GCS-008  the chart cools / warms with the real sensor and `set t.hi <value near it>`
 *   IT-GCS-009..011  status, alarms, network panels filled from the live system
 *   IT-GCS-012..014  help, sensors, set
 *   IT-GCS-018  selftest
 *   IT-GCS-019  profiles list
 *   IT-GCS-024  reboot
 *   persistence: `set t.hi 31`, reset, `thresholds` still shows 31.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "alarm_service/alarm_service.h"
#include "config_service/config_service.h"
#include "config_store/config_store.h"
#include "console_service/console_service.h"
#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "device_profile_registry/device_profile_registry.h"
#include "gpio/gpio_driver.h"
#include "health_monitor/health_monitor.h"
#include "i2c/i2c_driver.h"
#include "led/led_driver.h"
#include "qspi_flash/qspi_flash.h"
#include "rtc/rtc.h"
#include "sensor_service/sensor_service.h"
#include "sensor_service/sensor_task.h"
#include "time_provider/time_provider.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

#define BRINGUP_LED_PORT GPIO_PORT_A
#define BRINGUP_LED_PIN (5U)
#define LD2_PIN (14U)
#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)
#define USART1_IRQ_PRIORITY                                                                        \
    (6U) /* below configMAX_SYSCALL_INTERRUPT_PRIORITY: calls FromISR APIs */

static StaticTask_t s_console_tcb;
static StackType_t s_console_stack[CONSOLE_TASK_STACK_WORDS];

static health_monitor_handle_t s_hm;
static config_store_handle_t s_cs;

static const led_pin_t k_gw_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = (uint8_t) GPIO_PORT_B,
                   .pin = (uint8_t) LD2_PIN,
                   .active_high = true,
                   .fitted = true},
    [LED_RED] = {.port = (uint8_t) GPIO_PORT_A, .pin = 5U, .active_high = true, .fitted = false},
};

/* Read side of DeviceProfileRegistry for `profiles list`. */
static bool profile_next(const device_profile_t **p, uint8_t *index)
{
    return device_profile_get_next(p, index);
}

static const idevice_profile_provider_t k_profiles = {.get_next = profile_next};

static void bringup_spin(uint32_t n)
{
    volatile uint32_t c = n;

    while (c > 0U)
    {
        --c;
    }
}

/* Nothing can be printed (the console owns the UART): blink the red LED pin and stop. */
static void bringup_fail(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    GPIOA->MODER &= ~(3UL << (BRINGUP_LED_PIN * 2U));
    GPIOA->MODER |= (1UL << (BRINGUP_LED_PIN * 2U));
    for (;;)
    {
        GPIOA->BSRR = (1UL << BRINGUP_LED_PIN);
        bringup_spin(200000U);
        GPIOA->BSRR = (1UL << (BRINGUP_LED_PIN + 16U));
        bringup_spin(200000U);
    }
}

/* Stand-in for what LifecycleController will do (SD-00): load the stored configuration. */
static void load_stored_config(void)
{
    config_blob_t blob;
    uint32_t len = 0U;

    if ((config_store_check_integrity(s_cs) == CONFIG_STORE_ERR_OK) &&
        (config_store_load(s_cs, &blob, &len, (uint32_t) sizeof(blob)) == CONFIG_STORE_ERR_OK))
    {
        (void) config_service_apply_loaded(&blob, len);
    }
}

int main(void)
{
    if (cpu_init() != STATUS_OK)
    {
        bringup_fail();
    }
    (void) gpio_init();

    const gpio_pin_config_t uart_tx = {.port = BRINGUP_UART_TX_PORT,
                                       .pin = BRINGUP_UART_TX_PIN,
                                       .mode = GPIO_MODE_ALTERNATE,
                                       .otype = GPIO_OTYPE_PUSH_PULL,
                                       .speed = GPIO_SPEED_VERY_HIGH,
                                       .pull = GPIO_PULL_NONE,
                                       .alternate = BRINGUP_UART_AF};
    const gpio_pin_config_t uart_rx = {.port = BRINGUP_UART_RX_PORT,
                                       .pin = BRINGUP_UART_RX_PIN,
                                       .mode = GPIO_MODE_ALTERNATE,
                                       .otype = GPIO_OTYPE_PUSH_PULL,
                                       .speed = GPIO_SPEED_VERY_HIGH,
                                       .pull = GPIO_PULL_UP,
                                       .alternate = BRINGUP_UART_AF};

    (void) gpio_configure_pin(&uart_tx);
    (void) gpio_configure_pin(&uart_rx);
    if ((debug_uart_init() != DEBUG_UART_OK) || (rtc_init() != RTC_OK) ||
        (qspi_flash_init() != QSPI_FLASH_OK))
    {
        bringup_fail();
    }

    const health_monitor_config_t hm_config = {.boot_reset_cause = LIFECYCLE_RESET_SOFT};

    if ((led_init(k_gw_led_pins, (uint8_t) LED_COUNT) != LED_ERR_OK) ||
        (health_monitor_create(&hm_config, &s_hm) != HEALTH_MONITOR_ERR_OK) ||
        (time_provider_init(s_hm) != TIME_PROVIDER_ERR_OK))
    {
        bringup_fail();
    }

    const config_store_config_t cs_cfg = {.flash = qspi_flash_driver, .health = s_hm};

    if ((config_store_create(&cs_cfg, &s_cs) != CONFIG_STORE_ERR_OK) ||
        (config_service_init(s_cs) != CONFIG_SERVICE_OK))
    {
        bringup_fail();
    }
    load_stored_config();

    if ((i2c_driver->init() != I2C_ERR_OK) || (sensor_task_create(s_hm) != SENSOR_TASK_ERR_OK))
    {
        bringup_fail();
    }

    /* The USART1 vector must be below the FreeRTOS syscall ceiling: the receive callback calls
     * xTaskNotifyFromISR (DUART-O5). */
    NVIC_SetPriority(USART1_IRQn, USART1_IRQ_PRIORITY);

    const console_deps_t deps = {.uart = debug_uart,
                                 .sensors = sensor_service,
                                 .alarms = alarm_service,
                                 .cfg_read = config_service_get_provider(),
                                 .cfg_write = config_service_get_manager(),
                                 .health = s_hm,
                                 .prof_read = &k_profiles};

    if (console_service_init(&deps) != CONSOLE_SERVICE_ERR_OK)
    {
        bringup_fail();
    }
    const TaskHandle_t console =
        xTaskCreateStatic(console_task_body, "console", CONSOLE_TASK_STACK_WORDS, NULL,
                          CONSOLE_TASK_PRIORITY, s_console_stack, &s_console_tcb);

    (void) health_monitor_register_task(s_hm, "console", console);
    vTaskStartScheduler();

    for (;;)
    {
    }
}
