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
 * Link status: the harness also brings WiFi and MQTT up (credentials from certs/bringup_secrets.h,
 * broker and certificates from certs/bringup_certs.h, as in the other harnesses) and a small
 * "link monitor" task stands in for the producers a Gateway main() will have: it feeds the WiFi
 * signal strength and the broker connect / disconnect events into HealthMonitor, so the banner
 * dots, the status panels and `wifi status` / `mqtt status` show the real links. The Modbus link
 * stays offline: there is no Field Device in this setup, which is the true state. If the secrets
 * are missing or WiFi cannot be reached the console still runs and the dots stay red.
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
#include "tls_arena.h"

#include "alarm_service/alarm_service.h"
#include "config_service/config_service.h"
#include "config_store/config_store.h"
#include "console_service/console_service.h"
#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "device_profile_registry/device_profile_registry.h"
#include "exti/exti_driver.h"
#include "gpio/gpio_driver.h"
#include "health_monitor/health_monitor.h"
#include "i2c/i2c_driver.h"
#include "led/led_driver.h"
#include "mqtt_client/mqtt_client.h"
#include "qspi_flash/qspi_flash.h"
#include "rtc/rtc.h"
#include "sensor_service/sensor_service.h"
#include "sensor_service/sensor_task.h"
#include "spi/spi.h"
#include "time_provider/time_provider.h"
#include "wifi_driver/wifi_driver.h"
#include "wifi_task/wifi_task.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

#if __has_include("bringup_secrets.h")
#include "bringup_secrets.h"
#else
#define BRINGUP_WIFI_SSID ""
#define BRINGUP_WIFI_PASSWORD ""
#endif

#if __has_include("bringup_certs.h")
#include "bringup_certs.h"
#define BRINGUP_MQTT_BROKER_ENDPOINT BRINGUP_CERTS_BROKER_ENDPOINT
#else
#define BRINGUP_MQTT_BROKER_ENDPOINT ""
static const uint8_t s_bringup_ca_cert_der[] = {0x00};
static const uint8_t s_bringup_client_cert_der[] = {0x00};
static const uint8_t s_bringup_client_key_der[] = {0x00};
#endif

#define BRINGUP_MQTT_BROKER_PORT (8883U)
#define BRINGUP_MQTT_CLIENT_ID "gw-console-bringup-001"
#define BRINGUP_MQTT_KEEP_ALIVE_S (60U)

/* ISM43362 on SPI3, same wiring as the other WiFi harnesses. */
#define BRINGUP_SPI_AF (6U)
#define BRINGUP_NSS_PORT GPIO_PORT_E
#define BRINGUP_NSS_PIN (0U)
#define BRINGUP_DRDY_PORT GPIO_PORT_E
#define BRINGUP_DRDY_PIN (1U)
#define BRINGUP_RST_PORT GPIO_PORT_E
#define BRINGUP_RST_PIN (8U)
#define BRINGUP_WAKEUP_PORT GPIO_PORT_B
#define BRINGUP_WAKEUP_PIN (13U)
#define BRINGUP_BOOT0_PORT GPIO_PORT_B
#define BRINGUP_BOOT0_PIN (12U)

#define LINK_TASK_STACK_WORDS (1024U)
#define LINK_TASK_PRIORITY (2U)
#define LINK_POLL_MS (2000U)
#define LINK_SETTLE_MS (200U)

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

static StaticTask_t s_link_tcb;
static StackType_t s_link_stack[LINK_TASK_STACK_WORDS];
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

void EXTI1_IRQHandler(void)
{
    exti_clear_pending(WIFI_DRDY_EXTI_LINE);
    wifi_datardy_irq_handler();
}

static void mqtt_msg_cb(const char *topic, uint16_t topic_len, const uint8_t *payload, uint32_t len)
{
    (void) topic;
    (void) topic_len;
    (void) payload;
    (void) len;
}

static void mqtt_disconnect_cb(void)
{
}

/* WiFi pins, SpiDriver and WifiDriver, before the scheduler (same as the other WiFi harnesses). */
static bool wifi_hw_init(wifi_handle_t *out)
{
    static const gpio_pin_config_t spi_pins[3] = {
        {.port = GPIO_PORT_C,
         .pin = 10U,
         .mode = GPIO_MODE_ALTERNATE,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_VERY_HIGH,
         .pull = GPIO_PULL_NONE,
         .alternate = BRINGUP_SPI_AF},
        {.port = GPIO_PORT_C,
         .pin = 11U,
         .mode = GPIO_MODE_ALTERNATE,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_VERY_HIGH,
         .pull = GPIO_PULL_NONE,
         .alternate = BRINGUP_SPI_AF},
        {.port = GPIO_PORT_C,
         .pin = 12U,
         .mode = GPIO_MODE_ALTERNATE,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_VERY_HIGH,
         .pull = GPIO_PULL_NONE,
         .alternate = BRINGUP_SPI_AF},
    };
    static const gpio_pin_config_t control_pins[5] = {
        {.port = BRINGUP_NSS_PORT,
         .pin = BRINGUP_NSS_PIN,
         .mode = GPIO_MODE_OUTPUT,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_LOW,
         .pull = GPIO_PULL_NONE,
         .alternate = 0},
        {.port = BRINGUP_DRDY_PORT,
         .pin = BRINGUP_DRDY_PIN,
         .mode = GPIO_MODE_INPUT,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_LOW,
         .pull = GPIO_PULL_DOWN,
         .alternate = 0},
        {.port = BRINGUP_RST_PORT,
         .pin = BRINGUP_RST_PIN,
         .mode = GPIO_MODE_OUTPUT,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_LOW,
         .pull = GPIO_PULL_NONE,
         .alternate = 0},
        {.port = BRINGUP_WAKEUP_PORT,
         .pin = BRINGUP_WAKEUP_PIN,
         .mode = GPIO_MODE_OUTPUT,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_LOW,
         .pull = GPIO_PULL_NONE,
         .alternate = 0},
        {.port = BRINGUP_BOOT0_PORT,
         .pin = BRINGUP_BOOT0_PIN,
         .mode = GPIO_MODE_OUTPUT,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_LOW,
         .pull = GPIO_PULL_NONE,
         .alternate = 0},
    };

    for (uint8_t i = 0U; i < 3U; i++)
    {
        if (gpio_configure_pin(&spi_pins[i]) != GPIO_OK)
        {
            return false;
        }
    }
    for (uint8_t i = 0U; i < 5U; i++)
    {
        if (gpio_configure_pin(&control_pins[i]) != GPIO_OK)
        {
            return false;
        }
    }

    const spi_config_t spi_config = {.instance = SPI3};
    spi_handle_t spi_handle = NULL;

    if (spi_create(&spi_config, &spi_handle) != SPI_ERR_OK)
    {
        return false;
    }
    const wifi_config_t wifi_config = {.spi = spi_handle,
                                       .nss_port = BRINGUP_NSS_PORT,
                                       .nss_pin = BRINGUP_NSS_PIN,
                                       .drdy_port = BRINGUP_DRDY_PORT,
                                       .drdy_pin = BRINGUP_DRDY_PIN,
                                       .rst_port = BRINGUP_RST_PORT,
                                       .rst_pin = BRINGUP_RST_PIN,
                                       .wakeup_port = BRINGUP_WAKEUP_PORT,
                                       .wakeup_pin = BRINGUP_WAKEUP_PIN,
                                       .boot0_port = BRINGUP_BOOT0_PORT,
                                       .boot0_pin = BRINGUP_BOOT0_PIN};

    return wifi_create(&wifi_config, out) == WIFI_ERR_OK;
}

/* Stands in for the producers of link health that a Gateway main() will have (WifiTask polling,
 * CloudPublisher events): join the AP, connect the broker, then keep HealthMonitor current. */
/* Result codes of the link bring-up, readable with a debugger (the UART belongs to the console). */
volatile uint32_t
    g_link_diag[4]; /* [0] stage reached, [1] wifi create, [2] wifi connect, [3] mqtt */

static void link_task(void *arg)
{
    wifitask_handle_t wifi = NULL;
    const wifitask_config_t wifitask_config = {.wifi = (wifi_handle_t) arg};
    mqtt_client_handle_t mqtt = NULL;
    bool mqtt_up = false;

    if ((sizeof(BRINGUP_WIFI_SSID) <= 1U) ||
        (wifitask_create(&wifitask_config, &wifi) != WIFITASK_ERR_OK) ||
        (wifitask_connect_ap(wifi, BRINGUP_WIFI_SSID, BRINGUP_WIFI_PASSWORD) != WIFITASK_ERR_OK))
    {
        vTaskSuspend(NULL); /* no WiFi: the console keeps running with the links shown down */
    }
    vTaskDelay(pdMS_TO_TICKS(LINK_SETTLE_MS));

    const mqtt_client_config_t mqtt_config = {
        .wifi = wifi, .msg_cb = mqtt_msg_cb, .disconnect_cb = mqtt_disconnect_cb};

    if ((sizeof(BRINGUP_MQTT_BROKER_ENDPOINT) > 1U) &&
        (mqtt_client_create(&mqtt_config, &mqtt) == MQTT_CLIENT_ERR_OK))
    {
        const mqtt_connect_cfg_t connect_cfg = {.broker_endpoint = BRINGUP_MQTT_BROKER_ENDPOINT,
                                                .broker_port = BRINGUP_MQTT_BROKER_PORT,
                                                .client_id = BRINGUP_MQTT_CLIENT_ID,
                                                .client_cert_der = s_bringup_client_cert_der,
                                                .client_cert_len =
                                                    sizeof(s_bringup_client_cert_der),
                                                .client_key_der = s_bringup_client_key_der,
                                                .client_key_len = sizeof(s_bringup_client_key_der),
                                                .ca_cert_der = s_bringup_ca_cert_der,
                                                .ca_cert_len = sizeof(s_bringup_ca_cert_der),
                                                .keep_alive_s = BRINGUP_MQTT_KEEP_ALIVE_S};

        g_link_diag[0] = 2U;
        g_link_diag[3] = (uint32_t) mqtt_client_connect(mqtt, &connect_cfg);
        g_link_diag[0] = 3U;
        if (g_link_diag[3] != (uint32_t) MQTT_CLIENT_ERR_OK)
        {
            mqtt = NULL; /* broker unreachable: report only the WiFi link */
        }
    }

    for (;;)
    {
        int8_t rssi = 0;

        if (wifitask_get_rssi(wifi, &rssi) == WIFITASK_ERR_OK)
        {
            (void) health_monitor_update_wifi_rssi(s_hm, rssi);
        }
        if (mqtt != NULL)
        {
            (void) mqtt_client_process(mqtt);
            const bool now = mqtt_client_is_connected(mqtt);

            if (now != mqtt_up)
            {
                (void) health_monitor_push_event(
                    s_hm, now ? HEALTH_EVENT_CLOUD_CONNECTED : HEALTH_EVENT_CLOUD_DISCONNECTED, 0U);
                mqtt_up = now;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(LINK_POLL_MS));
    }
}

/* DeviceProfileRegistry for `profiles list`: nothing is stored on this bench, so the default
 * profile is seeded; saving is not supported here. */
static bool profiles_load(device_profile_t *out, uint8_t max, uint8_t *count_out)
{
    (void) out;
    (void) max;
    (void) count_out;
    return false;
}

static bool profiles_save(const device_profile_t *in, uint8_t count)
{
    (void) in;
    (void) count;
    return false;
}

static const device_profile_storage_t k_profile_storage = {.load = profiles_load,
                                                           .save = profiles_save};

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

    tls_arena_init(); /* mbedTLS allocates only from the static arena (WP2) */
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

    /* WiFi link: hardware before the scheduler, the rest from link_task. A failure here only
     * leaves the links shown down. */
    wifi_handle_t wifi = NULL;

    if (wifi_hw_init(&wifi))
    {
        (void) xTaskCreateStatic(link_task, "link", LINK_TASK_STACK_WORDS, wifi, LINK_TASK_PRIORITY,
                                 s_link_stack, &s_link_tcb);
    }

    /* The USART1 vector must be below the FreeRTOS syscall ceiling: the receive callback calls
     * xTaskNotifyFromISR (DUART-O5). */
    NVIC_SetPriority(USART1_IRQn, USART1_IRQ_PRIORITY);

    if ((device_profile_registry_init(&k_profile_storage) != DEVICE_PROFILE_REGISTRY_OK) ||
        (device_profile_registry_load() != DEVICE_PROFILE_REGISTRY_OK))
    {
        for (;;)
        {
        }
    }

    const console_deps_t deps = {.uart = debug_uart,
                                 .sensors = sensor_service,
                                 .alarms = alarm_service,
                                 .cfg_read = config_service_get_provider(),
                                 .cfg_write = config_service_get_manager(),
                                 .health = s_hm,
                                 .prof_read = device_profile_provider};

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
