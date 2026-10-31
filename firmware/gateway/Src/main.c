/**
 * @file main.c
 * @brief Gateway application entry point (WP4, docs/lld/application/gateway-main.md).
 *
 * Two phases, because WifiTask and everything that takes its handle can only be created in task
 * context (finding F-1):
 *
 *   Phase A, main(), before the scheduler: drivers, middleware, services, the tasks main() creates
 *            (ConsoleTask, the stack reporter, BootTask).
 *   Phase B, BootTask, after the scheduler starts: WifiTask, join the access point with a bounded
 *            retry (D-3), resolve the broker name (D-7), NtpClient, MqttClient, FirmwareStore,
 *            CircularFlashLog, StoreAndForward, TimeService (D-4), CloudPublisher with
 *            update_service_get_instance() (D-2), UpdateService, the lifecycle wiring, the gated
 *            ModbusPollerTask (F-9), then BootTask becomes LifecycleTask (D-6) and never returns.
 *
 * Not product-complete (open items in gateway-main.md): the Wi-Fi credentials, the MQTT client id
 * and the TLS material still come from the gitignored bring-up headers, the same ones the
 * harnesses use; nothing secret is committed. There is no watchdog (LC-O8, section 9 of the doc).
 *
 * Task stacks follow decision D-1 option F (1.5x the measured use). Every task's lowest free stack
 * is logged by the stack reporter, with a warning when it runs low.
 *
 * This file is excluded from the CubeIDE build (`excluding="main.c"` in .cproject, standing rule:
 * not edited by Claude); scripts/build-gateway-app.sh builds the image without CubeIDE.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "alarm_service/alarm_service.h"
#include "circular_flash_log/circular_flash_log.h"
#include "cloud_publisher/cloud_publisher.h"
#include "config_service/config_service.h"
#include "config_store/config_store.h"
#include "console_service/console_service.h"
#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "device_profile_registry/device_profile_registry.h"
#include "exti/exti_driver.h"
#include "firmware_store/firmware_store.h"
#include "gpio/gpio_driver.h"
#include "health_monitor/health_monitor.h"
#include "i2c/i2c_driver.h"
#include "internal_flash/internal_flash.h"
#include "led/led_driver.h"
#include "lifecycle_wiring/lifecycle_wiring.h"
#include "logger/logger.h"
#include "modbus_master/modbus_master.h"
#include "modbus_poller/modbus_poller.h"
#include "modbus_uart_driver/modbus_uart_driver.h"
#include "mqtt_client/mqtt_client.h"
#include "mqtt_client/tls_arena.h"
#include "ntp_client/ntp_client.h"
#include "qspi_flash/qspi_flash.h"
#include "rtc/rtc.h"
#include "sensor_service/sensor_service.h"
#include "sensor_service/sensor_task.h"
#include "spi/spi.h"
#include "store_and_forward/store_and_forward.h"
#include "time_provider/time_provider.h"
#include "time_service/time_service.h"
#include "update_service/update_service.h"
#include "wifi_driver/wifi_driver.h"
#include "wifi_task/wifi_task.h"

#include "stm32l475xx.h"

#if !__has_include("bringup_secrets.h") || !__has_include("bringup_certs.h")
#error                                                                                             \
    "firmware/gateway/certs/bringup_secrets.h and bringup_certs.h are required (see scripts/bringup_secrets.h.example)"
#endif
#include "bringup_certs.h"
#include "bringup_secrets.h"

/* ---------------------------------------------------------------------- */
/* Board constants                                                         */
/* ---------------------------------------------------------------------- */

#define LD2_PIN (14U)
#define UART_TX_PORT GPIO_PORT_B
#define UART_TX_PIN (6U)
#define UART_RX_PORT GPIO_PORT_B
#define UART_RX_PIN (7U)
#define UART_AF (7U)
#define SPI_SCK_PORT GPIO_PORT_C
#define SPI_SCK_PIN (10U)
#define SPI_MISO_PORT GPIO_PORT_C
#define SPI_MISO_PIN (11U)
#define SPI_MOSI_PORT GPIO_PORT_C
#define SPI_MOSI_PIN (12U)
#define SPI_AF (6U)
#define WIFI_NSS_PORT GPIO_PORT_E
#define WIFI_NSS_PIN (0U)
#define WIFI_DRDY_PORT GPIO_PORT_E
#define WIFI_DRDY_PIN (1U)
#define WIFI_RST_PORT GPIO_PORT_E
#define WIFI_RST_PIN (8U)
#define WIFI_WAKEUP_PORT GPIO_PORT_B
#define WIFI_WAKEUP_PIN (13U)
#define WIFI_BOOT0_PORT GPIO_PORT_B
#define WIFI_BOOT0_PIN (12U)

/* The console owns the debug UART (460800 baud, a full-screen terminal UI) and shows the log inside
 * itself. GATEWAY_WITH_CONSOLE=0 leaves it out for a plain 115200 baud log while debugging; it also
 * frees the console's SRAM2 buffers and its task stack. */
#ifndef GATEWAY_WITH_CONSOLE
#define GATEWAY_WITH_CONSOLE (1)
#endif

#define UART4_IRQ_PRIORITY (6U)
#define USART1_IRQ_PRIORITY (6U)
#define MODBUS_RESPONSE_TIMEOUT_MS (200U)
#define MODBUS_MAX_RETRIES (1U)

/* ---------------------------------------------------------------------- */
/* Tasks main() creates (D-1 option F: 1.5x the measured use)               */
/* ---------------------------------------------------------------------- */

#define BOOT_STACK_WORDS (896U) /* becomes LifecycleTask (D-6); measured 566 words for Phase B */
#define BOOT_PRIORITY (1U)
#define POLLER_STACK_WORDS (768U) /* measured 507 of 512 words (99 %), no Field Device attached */
#define POLLER_PRIORITY (4U)
#define REPORT_STACK_WORDS (320U) /* measured 169 words used */
#define REPORT_PRIORITY (1U)
#define REPORT_PERIOD_MS (60000U)
#define REPORT_MAX_TASKS (20U)
#define REPORT_LOW_WORDS (48U) /* below this many free words a task gets a warning */

/* ---------------------------------------------------------------------- */
/* Boot policy                                                             */
/* ---------------------------------------------------------------------- */

#define JOIN_ATTEMPTS (3U)    /* D-3: the first join after a module reset usually fails */
#define RESOLVE_ATTEMPTS (3U) /* D-7: a name the resolver cannot answer takes about 10 s to fail   \
                               */
#define RETRY_DELAY_MS (2000U)
#define MQTT_CLIENT_ID "gw-bringup-001" /* placeholder until the device serial exists */
#define MQTT_KEEP_ALIVE_S (60U)
#define SAF_CAPACITY (254U)
#define FW_VERSION_FALLBACK (0x01000000UL)
#define FD_SLAVE_ADDRESS (1U)

/* ---------------------------------------------------------------------- */
/* Halt helpers                                                            */
/* ---------------------------------------------------------------------- */

static void halt_with(const char *msg)
{
    LOG_ERROR("Main", "FATAL %s", msg);
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

static void spin_forever(void)
{
    for (;;)
    {
    }
}

/* ---------------------------------------------------------------------- */
/* Hardware callbacks and stand-ins                                        */
/* ---------------------------------------------------------------------- */

void EXTI1_IRQHandler(void)
{
    exti_clear_pending(WIFI_DRDY_EXTI_LINE);
    wifi_datardy_irq_handler();
}

static uint32_t tick_ms(void)
{
    return (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static const led_pin_t k_gw_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = (uint8_t) GPIO_PORT_B,
                   .pin = (uint8_t) LD2_PIN,
                   .active_high = true,
                   .fitted = true},
    [LED_RED] = {.port = (uint8_t) GPIO_PORT_A, .pin = 5U, .active_high = true, .fitted = false},
};

#if GATEWAY_WITH_CONSOLE
/* Read side of DeviceProfileRegistry for the console's `profiles list`. */
static bool profile_next(const device_profile_t **p, uint8_t *index)
{
    return device_profile_get_next(p, index);
}

static const idevice_profile_provider_t k_profiles = {.get_next = profile_next};
#endif

/* UpdateService's self-check probes (GM-O4: there is no provider module yet; this adapter is it).
 */
static bool probe_sensors(void *ctx)
{
    (void) ctx;
    return sensor_service_is_ready();
}

static bool probe_modbus(void *ctx)
{
    (void) ctx;
    return modbus_poller_is_ready();
}

static const us_probe_if_t k_probes = {
    .ctx = NULL, .sensors_ready = probe_sensors, .modbus_ready = probe_modbus};

/* ---------------------------------------------------------------------- */
/* State shared between Phase A and BootTask                               */
/* ---------------------------------------------------------------------- */

static health_monitor_handle_t s_hm;
static config_store_handle_t s_cs;
static wifi_handle_t s_wifi;
static lifecycle_reset_cause_t s_reset_cause;

/* MqttClient keeps pointers into its connect configuration for the whole session (REQ-NF-302), so
 * the resolved address lives in static storage. */
static char s_broker_ip[WIFI_IPV4_STR_LEN];

static StaticTask_t s_boot_tcb;
static StackType_t s_boot_stack[BOOT_STACK_WORDS];
static StaticTask_t s_poller_tcb;
static StackType_t s_poller_stack[POLLER_STACK_WORDS];
static StaticTask_t s_report_tcb;
static StackType_t s_report_stack[REPORT_STACK_WORDS];
#if GATEWAY_WITH_CONSOLE
static StaticTask_t s_console_tcb;
static StackType_t s_console_stack[CONSOLE_TASK_STACK_WORDS];
#endif

/* ---------------------------------------------------------------------- */
/* Stack reporter (gateway-main.md section 10, step 2)                     */
/* ---------------------------------------------------------------------- */

static TaskStatus_t s_status[REPORT_MAX_TASKS];
static uint32_t s_min_free[REPORT_MAX_TASKS];
static char s_min_name[REPORT_MAX_TASKS][configMAX_TASK_NAME_LEN];

/* The lowest free stack seen for a task name across all reports (the kernel's own mark is already
 * an all-time minimum, this only survives a task being recreated). Returns the slot or
 * REPORT_MAX_TASKS. */
static uint32_t report_slot(const char *name)
{
    uint32_t empty = REPORT_MAX_TASKS;

    for (uint32_t k = 0U; k < REPORT_MAX_TASKS; k++)
    {
        if (strncmp(s_min_name[k], name, configMAX_TASK_NAME_LEN) == 0)
        {
            return k;
        }
        if ((empty == REPORT_MAX_TASKS) && (s_min_name[k][0] == '\0'))
        {
            empty = k;
        }
    }
    return empty;
}

static void report_task(void *arg)
{
    (void) arg;

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(REPORT_PERIOD_MS));

        const UBaseType_t n = uxTaskGetSystemState(s_status, REPORT_MAX_TASKS, NULL);

        LOG_INFO("Stack", "%u tasks, TLS arena peak %lu of %lu bytes", (unsigned) n,
                 (unsigned long) tls_arena_peak_bytes(), (unsigned long) TLS_ARENA_SIZE_BYTES);
        for (UBaseType_t i = 0U; i < n; i++)
        {
            const uint32_t free_words = (uint32_t) s_status[i].usStackHighWaterMark;
            const uint32_t slot = report_slot(s_status[i].pcTaskName);

            if (slot < REPORT_MAX_TASKS)
            {
                if (s_min_name[slot][0] == '\0')
                {
                    (void) strncpy(s_min_name[slot], s_status[i].pcTaskName,
                                   configMAX_TASK_NAME_LEN - 1U);
                    s_min_free[slot] = free_words;
                }
                else if (free_words < s_min_free[slot])
                {
                    s_min_free[slot] = free_words;
                }
            }
            if (free_words < REPORT_LOW_WORDS)
            {
                LOG_WARN("Stack", "%-16s prio %u free %4lu words: LOW", s_status[i].pcTaskName,
                         (unsigned) s_status[i].uxCurrentPriority, (unsigned long) free_words);
            }
            else
            {
                LOG_INFO("Stack", "%-16s prio %u free %4lu words", s_status[i].pcTaskName,
                         (unsigned) s_status[i].uxCurrentPriority, (unsigned long) free_words);
            }
            vTaskDelay(pdMS_TO_TICKS(20U)); /* let the logger drain between lines */
        }
    }
}

/* ---------------------------------------------------------------------- */
/* BootTask: Phase B, then becomes LifecycleTask (D-6)                     */
/* ---------------------------------------------------------------------- */

static bool join_access_point(wifitask_handle_t wifitask)
{
    wifitask_err_t join = WIFITASK_ERR_NO_RESOURCE;

    for (unsigned attempt = 1U; (attempt <= JOIN_ATTEMPTS) && (join != WIFITASK_ERR_OK); attempt++)
    {
        join = wifitask_connect_ap(wifitask, BRINGUP_WIFI_SSID, BRINGUP_WIFI_PASSWORD);
        LOG_INFO("Main", "join attempt %u of %u -> %d", attempt, (unsigned) JOIN_ATTEMPTS,
                 (int) join);
        if (join != WIFITASK_ERR_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        }
    }
    return join == WIFITASK_ERR_OK;
}

/* D-7: the module's socket command takes an address, so BootTask resolves the broker name once
 * after the join. It calls the driver directly, not through WifiTask: WifiTask polls only sockets
 * that are open, none is yet, and BootTask is its only user at this point. A failing lookup holds
 * the module for about 10 s before it answers ERROR (WIFI-O18), hence the long driver timeout and
 * few attempts. */
static bool resolve_broker(void)
{
    for (unsigned attempt = 1U; attempt <= RESOLVE_ATTEMPTS; attempt++)
    {
        const wifi_err_t r =
            wifi_resolve(s_wifi, BRINGUP_CERTS_BROKER_ENDPOINT, s_broker_ip, sizeof(s_broker_ip));

        LOG_INFO("Main", "resolve %s attempt %u of %u -> %d (%s)", BRINGUP_CERTS_BROKER_ENDPOINT,
                 attempt, (unsigned) RESOLVE_ATTEMPTS, (int) r,
                 (r == WIFI_ERR_OK) ? s_broker_ip : "");
        if (r == WIFI_ERR_OK)
        {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }
    return false;
}

static void boot_task(void *arg)
{
    (void) arg;

    LOG_INFO("Main", "BootTask: Phase B starts");

    /* WifiTask is created in task context, after the scheduler (F-1). */
    const wifitask_config_t wifitask_cfg = {.wifi = s_wifi};
    wifitask_handle_t wifitask = NULL;

    if (wifitask_create(&wifitask_cfg, &wifitask) != WIFITASK_ERR_OK)
    {
        halt_with("wifitask_create");
    }
    if (!join_access_point(wifitask))
    {
        halt_with("wifi join"); /* GM-O17: what the device does once the retries are exhausted */
    }
    if (!resolve_broker())
    {
        halt_with("broker name resolution");
    }

    const ntp_client_config_t ntp_cfg = {.wifi = wifitask, .health = s_hm};
    ntp_client_handle_t ntp = NULL;

    if (ntp_client_create(&ntp_cfg, &ntp) != NTP_CLIENT_ERR_OK)
    {
        halt_with("ntp_client_create");
    }

    const mqtt_client_config_t mqtt_cfg = {.wifi = wifitask,
                                           .msg_cb = cloud_publisher_on_message,
                                           .disconnect_cb = cloud_publisher_on_disconnect};
    mqtt_client_handle_t mqtt = NULL;

    if (mqtt_client_create(&mqtt_cfg, &mqtt) != MQTT_CLIENT_ERR_OK)
    {
        halt_with("mqtt_client_create");
    }

    const firmware_store_config_t fs_cfg = {.qspi = qspi_flash_driver,
                                            .iflash = internal_flash_driver,
                                            .running_image_base = cpu_get_vector_table_base()};
    firmware_store_t *fs = NULL;

    if (firmware_store_create(&fs_cfg, &fs) != FIRMWARE_STORE_ERR_OK)
    {
        halt_with("firmware_store_create");
    }
    firmware_boot_info_t boot_info;

    (void) memset(&boot_info, 0, sizeof(boot_info));
    (void) firmware_store_boot_ops.get_boot_info(fs, &boot_info);

    const cfl_config_t cfl_cfg = {.qspi = qspi_flash_driver, .health = s_hm};
    cfl_handle_t cfl = NULL;

    if (cfl_create(&cfl_cfg, &cfl) != CFL_ERR_OK)
    {
        halt_with("cfl_create");
    }
    const saf_config_t saf_cfg = {.cfl = cfl, .health = s_hm, .capacity = SAF_CAPACITY};
    saf_handle_t saf = NULL;

    if (saf_create(&saf_cfg, &saf) != SAF_ERR_OK)
    {
        halt_with("saf_create");
    }

    /* D-4: TimeService before CloudPublisher, so its interface pointer is not NULL (idempotent: the
     * Lifecycle adapter's later call is a no-op). */
    if (time_service_init(ntp) != TIME_SERVICE_OK)
    {
        halt_with("time_service_init");
    }

    /* D-7: connect to the resolved address, check the certificate against the endpoint's name. */
    const mqtt_connect_cfg_t connect_cfg = {
        .broker_endpoint = s_broker_ip,
        .tls_server_name = BRINGUP_CERTS_BROKER_ENDPOINT,
        .broker_port = 8883U,
        .client_id = MQTT_CLIENT_ID,
        .client_cert_der = s_bringup_client_cert_der,
        .client_cert_len = sizeof(s_bringup_client_cert_der),
        .client_key_der = s_bringup_client_key_der,
        .client_key_len = sizeof(s_bringup_client_key_der),
        .ca_cert_der = s_bringup_ca_cert_der,
        .ca_cert_len = sizeof(s_bringup_ca_cert_der),
        .keep_alive_s = MQTT_KEEP_ALIVE_S,
    };

    /* D-2: CloudPublisher takes the UpdateService handle before UpdateService exists. */
    const cloud_publisher_config_t cp_cfg = {
        .mqtt = mqtt,
        .saf = saf,
        .health = s_hm,
        .cfg_read = config_service_get_provider(),
        .cfg_write = config_service_get_manager(),
        .time = time_service_get_interface(),
        .lifecycle = lifecycle_controller,
        .control_handler = lifecycle_handle_remote,
        .fd_slave_addr = FD_SLAVE_ADDRESS,
        .fw_version =
            (boot_info.running_version != 0U) ? boot_info.running_version : FW_VERSION_FALLBACK,
        .update_ops = &update_service_ops,
        .update = update_service_get_instance(),
        .mqtt_connect_cfg = connect_cfg,
    };
    cloud_publisher_handle_t cp = NULL;

    if (cloud_publisher_create(&cp_cfg, &cp) != CP_ERR_OK)
    {
        halt_with("cloud_publisher_create");
    }

    iota_transport_t ota;

    if (cloud_publisher_get_ota_transport(cp, &ota) != CP_ERR_OK)
    {
        halt_with("cloud_publisher_get_ota_transport");
    }
    const update_service_config_t us_cfg = {.fs_image = &firmware_store_image_ops,
                                            .fs_boot = &firmware_store_boot_ops,
                                            .fs = fs,
                                            .transport = &ota,
                                            .probes = &k_probes,
                                            .lifecycle = lifecycle_controller};
    update_service_t *us = NULL;

    if (update_service_create(&us_cfg, &us) != UPDATE_SERVICE_ERR_OK)
    {
        halt_with("update_service_create");
    }

    const lifecycle_wiring_config_t wiring_cfg = {.health = s_hm,
                                                  .config_store = s_cs,
                                                  .cloud = cp,
                                                  .ntp = ntp,
                                                  .fault_on_no_config = false,
                                                  .update_ops = &update_control_ops,
                                                  .update = us,
                                                  .firmware_ops = &firmware_store_boot_ops,
                                                  .firmware = fs};
    lifecycle_deps_t deps;

    if ((lifecycle_wiring_bind(&wiring_cfg, &deps) != LIFECYCLE_ERR_OK) ||
        (lifecycle_controller_init(s_reset_cause, &deps) != LIFECYCLE_ERR_OK))
    {
        halt_with("lifecycle wiring / init");
    }

    /* The start gate exists now, so the gated poller can be created (F-9). */
    (void) xTaskCreateStatic(lifecycle_wiring_poller_task, "ModbusPoller", POLLER_STACK_WORDS, NULL,
                             POLLER_PRIORITY, s_poller_stack, &s_poller_tcb);

    LOG_INFO("Main", "BootTask becomes LifecycleTask");
    lifecycle_task_body(NULL); /* never returns */
}

/* ---------------------------------------------------------------------- */
/* main: Phase A                                                           */
/* ---------------------------------------------------------------------- */

int main(void)
{
    s_reset_cause = lifecycle_detect_reset_cause(); /* before anything can disturb the flags */

    if (cpu_init() != STATUS_OK)
    {
        spin_forever();
    }
    tls_arena_init(); /* before any mbedTLS call (WP2) */

    gpio_pin_config_t uart_tx_config = {.port = UART_TX_PORT,
                                        .pin = UART_TX_PIN,
                                        .mode = GPIO_MODE_ALTERNATE,
                                        .otype = GPIO_OTYPE_PUSH_PULL,
                                        .speed = GPIO_SPEED_VERY_HIGH,
                                        .pull = GPIO_PULL_NONE,
                                        .alternate = UART_AF};
    gpio_pin_config_t uart_rx_config = {.port = UART_RX_PORT,
                                        .pin = UART_RX_PIN,
                                        .mode = GPIO_MODE_ALTERNATE,
                                        .otype = GPIO_OTYPE_PUSH_PULL,
                                        .speed = GPIO_SPEED_VERY_HIGH,
                                        .pull = GPIO_PULL_UP,
                                        .alternate = UART_AF};

    if ((gpio_init() != GPIO_OK) || (gpio_configure_pin(&uart_tx_config) != GPIO_OK) ||
        (gpio_configure_pin(&uart_rx_config) != GPIO_OK) || (debug_uart_init() != DEBUG_UART_OK) ||
        (rtc_init() != RTC_OK) || (qspi_flash_init() != QSPI_FLASH_OK) ||
        (internal_flash_init() != INTERNAL_FLASH_OK))
    {
        spin_forever();
    }
    (void) logger_init(LOG_LEVEL_INFO);
    LOG_INFO("Main", "===== Gateway =====");
    LOG_INFO("Main", "SYSCLK=%lu Hz, TLS arena %lu bytes", (unsigned long) cpu_get_sysclk_hz(),
             (unsigned long) TLS_ARENA_SIZE_BYTES);

    const health_monitor_config_t hm_cfg = {.boot_reset_cause = s_reset_cause};

    if ((led_init(k_gw_led_pins, (uint8_t) LED_COUNT) != LED_ERR_OK) ||
        (health_monitor_create(&hm_cfg, &s_hm) != HEALTH_MONITOR_ERR_OK) ||
        (time_provider_init(s_hm) != TIME_PROVIDER_ERR_OK))
    {
        spin_forever();
    }

    const config_store_config_t cs_cfg = {.flash = qspi_flash_driver, .health = s_hm};

    if ((config_store_create(&cs_cfg, &s_cs) != CONFIG_STORE_ERR_OK) ||
        (config_service_init(s_cs) != CONFIG_SERVICE_OK))
    {
        spin_forever();
    }

    /* Sensors and alarms are initialised before the scheduler (SS-D9). */
    if ((i2c_driver->init() != I2C_ERR_OK) || (sensor_task_create(s_hm) != SENSOR_TASK_ERR_OK))
    {
        spin_forever();
    }

    NVIC_SetPriority(UART4_IRQn, UART4_IRQ_PRIORITY);
    if ((modbus_uart_init() != MODBUS_UART_OK) ||
        (modbus_master_init(MODBUS_RESPONSE_TIMEOUT_MS, MODBUS_MAX_RETRIES) !=
         MODBUS_MASTER_ERR_OK))
    {
        spin_forever();
    }
    modbus_uart_set_tick_source(tick_ms);

#if GATEWAY_WITH_CONSOLE
    NVIC_SetPriority(USART1_IRQn, USART1_IRQ_PRIORITY);
    const console_deps_t console_deps = {.uart = debug_uart,
                                         .sensors = sensor_service,
                                         .alarms = alarm_service,
                                         .cfg_read = config_service_get_provider(),
                                         .cfg_write = config_service_get_manager(),
                                         .health = s_hm,
                                         .prof_read = &k_profiles};

    if (console_service_init(&console_deps) != CONSOLE_SERVICE_ERR_OK)
    {
        spin_forever();
    }
#endif

    /* The ISM43362 and its SPI lines: the driver only, WifiTask comes in Phase B. */
    gpio_pin_config_t spi_pins[3] = {
        {.port = SPI_SCK_PORT,
         .pin = SPI_SCK_PIN,
         .mode = GPIO_MODE_ALTERNATE,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_VERY_HIGH,
         .pull = GPIO_PULL_NONE,
         .alternate = SPI_AF},
        {.port = SPI_MISO_PORT,
         .pin = SPI_MISO_PIN,
         .mode = GPIO_MODE_ALTERNATE,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_VERY_HIGH,
         .pull = GPIO_PULL_NONE,
         .alternate = SPI_AF},
        {.port = SPI_MOSI_PORT,
         .pin = SPI_MOSI_PIN,
         .mode = GPIO_MODE_ALTERNATE,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_VERY_HIGH,
         .pull = GPIO_PULL_NONE,
         .alternate = SPI_AF},
    };

    for (uint8_t i = 0U; i < 3U; ++i)
    {
        if (gpio_configure_pin(&spi_pins[i]) != GPIO_OK)
        {
            spin_forever();
        }
    }
    const gpio_pin_config_t out_low = {.mode = GPIO_MODE_OUTPUT,
                                       .otype = GPIO_OTYPE_PUSH_PULL,
                                       .speed = GPIO_SPEED_LOW,
                                       .pull = GPIO_PULL_NONE,
                                       .alternate = 0};
    gpio_pin_config_t nss_config = out_low;
    gpio_pin_config_t rst_config = out_low;
    gpio_pin_config_t wakeup_config = out_low;
    gpio_pin_config_t boot0_config = out_low;
    gpio_pin_config_t drdy_config = {.port = WIFI_DRDY_PORT,
                                     .pin = WIFI_DRDY_PIN,
                                     .mode = GPIO_MODE_INPUT,
                                     .otype = GPIO_OTYPE_PUSH_PULL,
                                     .speed = GPIO_SPEED_LOW,
                                     .pull = GPIO_PULL_DOWN,
                                     .alternate = 0};

    nss_config.port = WIFI_NSS_PORT;
    nss_config.pin = WIFI_NSS_PIN;
    rst_config.port = WIFI_RST_PORT;
    rst_config.pin = WIFI_RST_PIN;
    wakeup_config.port = WIFI_WAKEUP_PORT;
    wakeup_config.pin = WIFI_WAKEUP_PIN;
    boot0_config.port = WIFI_BOOT0_PORT;
    boot0_config.pin = WIFI_BOOT0_PIN;
    if ((gpio_configure_pin(&nss_config) != GPIO_OK) ||
        (gpio_configure_pin(&drdy_config) != GPIO_OK) ||
        (gpio_configure_pin(&rst_config) != GPIO_OK) ||
        (gpio_configure_pin(&wakeup_config) != GPIO_OK) ||
        (gpio_configure_pin(&boot0_config) != GPIO_OK))
    {
        spin_forever();
    }
    const spi_config_t spi_cfg = {.instance = SPI3};
    spi_handle_t spi = NULL;

    if (spi_create(&spi_cfg, &spi) != SPI_ERR_OK)
    {
        spin_forever();
    }
    const wifi_config_t wifi_cfg = {.spi = spi,
                                    .nss_port = WIFI_NSS_PORT,
                                    .nss_pin = WIFI_NSS_PIN,
                                    .drdy_port = WIFI_DRDY_PORT,
                                    .drdy_pin = WIFI_DRDY_PIN,
                                    .rst_port = WIFI_RST_PORT,
                                    .rst_pin = WIFI_RST_PIN,
                                    .wakeup_port = WIFI_WAKEUP_PORT,
                                    .wakeup_pin = WIFI_WAKEUP_PIN,
                                    .boot0_port = WIFI_BOOT0_PORT,
                                    .boot0_pin = WIFI_BOOT0_PIN};

    if (wifi_create(&wifi_cfg, &s_wifi) != WIFI_ERR_OK)
    {
        spin_forever();
    }

    /* Tasks main() creates. ModbusPollerTask is NOT here: it waits on the start gate, and the gate
     * is created by lifecycle_controller_init(), which runs in BootTask after the scheduler has
     * started (finding F-9: created here it dereferences a NULL event group at scheduler start). */
#if GATEWAY_WITH_CONSOLE
    (void) xTaskCreateStatic(console_task_body, "console", CONSOLE_TASK_STACK_WORDS, NULL,
                             CONSOLE_TASK_PRIORITY, s_console_stack, &s_console_tcb);
#endif
    (void) xTaskCreateStatic(report_task, "stack_report", REPORT_STACK_WORDS, NULL, REPORT_PRIORITY,
                             s_report_stack, &s_report_tcb);
    (void) xTaskCreateStatic(boot_task, "lifecycle", BOOT_STACK_WORDS, NULL, BOOT_PRIORITY,
                             s_boot_stack, &s_boot_tcb);

    vTaskStartScheduler();

    spin_forever();
}
