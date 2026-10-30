/**
 * @file main_bench_stack_marks.c
 * @brief Bench image for the WP4 D-1 gate: the real Gateway task set running together, with every
 *        task's stack high-water mark logged. NOT product code; it is the draft of the Gateway main().
 *
 * It follows docs/lld/application/gateway-main.md section 4 and the Pass H decisions:
 *   Phase A (before the scheduler): drivers, middleware, services, the tasks main() creates
 *            (ModbusPollerTask gated, ConsoleTask), BootTask, the stats task.
 *   Phase B (BootTask, after the scheduler starts): WifiTask, join with a bounded retry (D-3),
 *            NtpClient, MqttClient, FirmwareStore, CircularFlashLog, StoreAndForward, TimeService (D-4),
 *            CloudPublisher with update_service_get_instance() (D-2), UpdateService, the lifecycle wiring,
 *            then BootTask becomes LifecycleTask (D-6).
 *
 * Every task name and declared stack size is printed with its high-water mark, every STATS_PERIOD_MS.
 * "used" is declared minus the lowest free words ever seen, so it only grows. Exercise the paths:
 * let the TLS session come up, send console commands (scripts/console_bench.py), publish a /control
 * message. UpdateService's verify path is not exercised here (its mark is known from the UpdateService
 * bench: 586 of 1024 words free).
 *
 * The whole task set does not fit the chip: it needs about 147 KB of static RAM and the chip has
 * 96 KB + 32 KB (that is what D-1 is about). So the bench runs in two partitions, each of which fits:
 *   BENCH_WITH_CONSOLE=0, BENCH_WITH_CLOUD=1 (mode A): everything except the console. Without the
 *     console's 24.6 KB of SRAM2 buffers, SRAM2 is free, and the bench linker script places the task
 *     stacks (about 27 KB) there so the main bank can hold the arena, pools and state.
 *   BENCH_WITH_CONSOLE=1, BENCH_WITH_CLOUD=0 (mode B): the console and the sensors only.
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

#if __has_include("bringup_secrets.h")
#include "bringup_secrets.h"
#else
#define BRINGUP_WIFI_SSID ""
#define BRINGUP_WIFI_PASSWORD ""
#endif
#include "bringup_certs.h"

/* ---------------------------------------------------------------------- */
/* Board constants (same values as the bring-up mains)                     */
/* ---------------------------------------------------------------------- */

#define LD2_PIN (14U)
#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)
#define BRINGUP_SPI_SCK_PORT GPIO_PORT_C
#define BRINGUP_SPI_SCK_PIN (10U)
#define BRINGUP_SPI_MISO_PORT GPIO_PORT_C
#define BRINGUP_SPI_MISO_PIN (11U)
#define BRINGUP_SPI_MOSI_PORT GPIO_PORT_C
#define BRINGUP_SPI_MOSI_PIN (12U)
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

#define UART4_IRQ_PRIORITY (6U)
#define USART1_IRQ_PRIORITY (6U)
#define RESPONSE_TIMEOUT_MS (200U)
#define MAX_RETRIES (1U)

#ifndef BENCH_WITH_CONSOLE
#define BENCH_WITH_CONSOLE (0)
#endif
#ifndef BENCH_WITH_CLOUD
#define BENCH_WITH_CLOUD (1)
#endif

#define BOOT_STACK_WORDS (2048U) /* becomes LifecycleTask (D-6) */
#define BOOT_PRIORITY (1U)
#define POLLER_STACK_WORDS (512U)
#define POLLER_PRIORITY (4U)
#define STATS_STACK_WORDS (512U)
#define STATS_PRIORITY (1U)
#define STATS_PERIOD_MS (15000U)
#define STATS_MAX_TASKS (20U)

#define JOIN_ATTEMPTS (3U)
#define MQTT_CLIENT_ID "gw-bringup-001"
#define MQTT_KEEP_ALIVE_S (60U)
#define SAF_CAPACITY (254U)
#define FW_VERSION (0x01000000UL)

/* ---------------------------------------------------------------------- */
/* Halt helpers                                                            */
/* ---------------------------------------------------------------------- */

static void halt_with(const char *msg)
{
    LOG_ERROR("Bench", "FATAL %s", msg);
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

#if BENCH_WITH_CONSOLE
/* Read side of DeviceProfileRegistry for the console's `profiles list`. */
static bool profile_next(const device_profile_t **p, uint8_t *index)
{
    return device_profile_get_next(p, index);
}

static const idevice_profile_provider_t k_profiles = {.get_next = profile_next};
#endif

/* UpdateService's self-check probes (GM-O4: no production provider exists yet). */
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

static StaticTask_t s_boot_tcb;
static StackType_t s_boot_stack[BOOT_STACK_WORDS];
static StaticTask_t s_poller_tcb;
static StackType_t s_poller_stack[POLLER_STACK_WORDS];
static StaticTask_t s_stats_tcb;
static StackType_t s_stats_stack[STATS_STACK_WORDS];
#if BENCH_WITH_CONSOLE
static StaticTask_t s_console_tcb;
static StackType_t s_console_stack[CONSOLE_TASK_STACK_WORDS];
#endif

/* ---------------------------------------------------------------------- */
/* Stack reporter                                                          */
/* ---------------------------------------------------------------------- */

typedef struct
{
    const char *name;
    uint32_t declared_words;
} declared_stack_t;

/* Declared sizes, by task name (the code constants, see gateway-main.md section 5). */
static const declared_stack_t k_declared[] = {
    {"SensorTask", SENSOR_TASK_STACK_WORDS},
    {"ModbusPoller", POLLER_STACK_WORDS},
    {"WifiTask", 256U},
    {"CloudPublisherT", 768U}, /* FreeRTOS keeps 15 characters of the task name */
    {"TS", 768U},
    {"UpdateServiceTa", 1024U},
    {"console", CONSOLE_TASK_STACK_WORDS},
    {"lifecycle", BOOT_STACK_WORDS},
    {"log_drain", 256U},
    {"stats", STATS_STACK_WORDS},
    {"IDLE", configMINIMAL_STACK_SIZE},
    {"Tmr Svc", 256U},
};

static uint32_t declared_for(const char *name)
{
    for (size_t i = 0U; i < (sizeof(k_declared) / sizeof(k_declared[0])); i++)
    {
        if (strcmp(k_declared[i].name, name) == 0)
        {
            return k_declared[i].declared_words;
        }
    }
    return 0U;
}

static TaskStatus_t s_status[STATS_MAX_TASKS];
static uint32_t s_min_free[STATS_MAX_TASKS];
static char s_min_name[STATS_MAX_TASKS][configMAX_TASK_NAME_LEN];

static void stats_task(void *arg)
{
    (void) arg;

    for (uint32_t round = 1U;; round++)
    {
        vTaskDelay(pdMS_TO_TICKS(STATS_PERIOD_MS));

        const UBaseType_t n = uxTaskGetSystemState(s_status, STATS_MAX_TASKS, NULL);

        LOG_INFO("Stack", "===== round %lu, %u tasks, arena peak %lu of %lu =====",
                 (unsigned long) round, (unsigned) n, (unsigned long) tls_arena_peak_bytes(),
                 (unsigned long) TLS_ARENA_SIZE_BYTES);
        for (UBaseType_t i = 0U; i < n; i++)
        {
            const char *name = s_status[i].pcTaskName;
            const uint32_t free_words = (uint32_t) s_status[i].usStackHighWaterMark;
            const uint32_t declared = declared_for(name);

            /* Keep the lowest free count seen per task name across rounds. */
            uint32_t slot = STATS_MAX_TASKS;
            for (uint32_t k = 0U; k < STATS_MAX_TASKS; k++)
            {
                if (strncmp(s_min_name[k], name, configMAX_TASK_NAME_LEN) == 0)
                {
                    slot = k;
                    break;
                }
                if ((slot == STATS_MAX_TASKS) && (s_min_name[k][0] == '\0'))
                {
                    slot = k;
                }
            }
            if (slot < STATS_MAX_TASKS)
            {
                if (s_min_name[slot][0] == '\0')
                {
                    (void) strncpy(s_min_name[slot], name, configMAX_TASK_NAME_LEN - 1U);
                    s_min_free[slot] = free_words;
                }
                else if (free_words < s_min_free[slot])
                {
                    s_min_free[slot] = free_words;
                }
            }
            LOG_INFO("Stack", "%-18s prio %u declared %4lu free %4lu used %4lu words (%lu B)", name,
                     (unsigned) s_status[i].uxCurrentPriority, (unsigned long) declared,
                     (unsigned long) free_words,
                     (unsigned long) ((declared > free_words) ? (declared - free_words) : 0U),
                     (unsigned long) (((declared > free_words) ? (declared - free_words) : 0U) * 4U));
            vTaskDelay(pdMS_TO_TICKS(20U)); /* let the logger drain between lines */
        }
    }
}

/* ---------------------------------------------------------------------- */
/* BootTask: Phase B, then becomes LifecycleTask (D-6)                     */
/* ---------------------------------------------------------------------- */

static void boot_task(void *arg)
{
    (void) arg;

#if !BENCH_WITH_CLOUD
    LOG_INFO("Bench", "console-only mode: no cloud chain, no lifecycle");
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
#endif
    LOG_INFO("Bench", "BootTask: Phase B starts");

    /* WifiTask is created in task context, after the scheduler (F-1). */
    const wifitask_config_t wifitask_cfg = {.wifi = s_wifi};
    wifitask_handle_t wifitask = NULL;

    if (wifitask_create(&wifitask_cfg, &wifitask) != WIFITASK_ERR_OK)
    {
        halt_with("wifitask_create");
    }

    /* D-3: bounded retry; the first join after a module reset usually fails. */
    wifitask_err_t join = WIFITASK_ERR_NO_RESOURCE;

    for (unsigned attempt = 1U; (attempt <= JOIN_ATTEMPTS) && (join != WIFITASK_ERR_OK); attempt++)
    {
        join = wifitask_connect_ap(wifitask, BRINGUP_WIFI_SSID, BRINGUP_WIFI_PASSWORD);
        LOG_INFO("Bench", "join attempt %u -> %d", attempt, (int) join);
        if (join != WIFITASK_ERR_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(2000U));
        }
    }
    if (join != WIFITASK_ERR_OK)
    {
        halt_with("wifi join");
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
    saf_handle_t saf = NULL;
    const saf_config_t saf_cfg = {.cfl = NULL, .health = s_hm, .capacity = SAF_CAPACITY};
    saf_config_t saf_cfg_bound = saf_cfg;

    if (cfl_create(&cfl_cfg, &cfl) != CFL_ERR_OK)
    {
        halt_with("cfl_create");
    }
    saf_cfg_bound.cfl = cfl;
    if (saf_create(&saf_cfg_bound, &saf) != SAF_ERR_OK)
    {
        halt_with("saf_create");
    }

    /* D-4: TimeService before CloudPublisher, so its interface pointer is not NULL. */
    if (time_service_init(ntp) != TIME_SERVICE_OK)
    {
        halt_with("time_service_init");
    }

    /* Shorter intervals than the 60 s / 600 s defaults, so the publish paths run during the bench. */
    {
        const uint32_t telemetry_ms = 10000U;
        const uint32_t health_ms = 20000U;

        (void) config_service_set_param_deferred(CONFIG_PARAM_TELEMETRY_INTERVAL, &telemetry_ms);
        (void) config_service_set_param_deferred(CONFIG_PARAM_HEALTH_INTERVAL, &health_ms);
    }

    const mqtt_connect_cfg_t connect_cfg = {
        .broker_endpoint = BRINGUP_CERTS_BROKER_ENDPOINT,
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
        .fd_slave_addr = 1U,
        .fw_version = (boot_info.running_version != 0U) ? boot_info.running_version : FW_VERSION,
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

    LOG_INFO("Bench", "BootTask becomes LifecycleTask");
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

    gpio_pin_config_t uart_tx_config = {.port = BRINGUP_UART_TX_PORT,
                                        .pin = BRINGUP_UART_TX_PIN,
                                        .mode = GPIO_MODE_ALTERNATE,
                                        .otype = GPIO_OTYPE_PUSH_PULL,
                                        .speed = GPIO_SPEED_VERY_HIGH,
                                        .pull = GPIO_PULL_NONE,
                                        .alternate = BRINGUP_UART_AF};
    gpio_pin_config_t uart_rx_config = {.port = BRINGUP_UART_RX_PORT,
                                        .pin = BRINGUP_UART_RX_PIN,
                                        .mode = GPIO_MODE_ALTERNATE,
                                        .otype = GPIO_OTYPE_PUSH_PULL,
                                        .speed = GPIO_SPEED_VERY_HIGH,
                                        .pull = GPIO_PULL_UP,
                                        .alternate = BRINGUP_UART_AF};

    if ((gpio_init() != GPIO_OK) || (gpio_configure_pin(&uart_tx_config) != GPIO_OK) ||
        (gpio_configure_pin(&uart_rx_config) != GPIO_OK) || (debug_uart_init() != DEBUG_UART_OK) ||
        (rtc_init() != RTC_OK) || (qspi_flash_init() != QSPI_FLASH_OK) ||
        (internal_flash_init() != INTERNAL_FLASH_OK))
    {
        spin_forever();
    }
    (void) logger_init(LOG_LEVEL_INFO);
    LOG_INFO("Bench", "===== Gateway stack high-water bench (WP4 D-1) =====");
    LOG_INFO("Bench", "SYSCLK=%lu Hz, arena %lu bytes", (unsigned long) cpu_get_sysclk_hz(),
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
        (modbus_master_init(RESPONSE_TIMEOUT_MS, MAX_RETRIES) != MODBUS_MASTER_ERR_OK))
    {
        spin_forever();
    }
    modbus_uart_set_tick_source(tick_ms);

#if BENCH_WITH_CONSOLE
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
        {.port = BRINGUP_SPI_SCK_PORT,
         .pin = BRINGUP_SPI_SCK_PIN,
         .mode = GPIO_MODE_ALTERNATE,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_VERY_HIGH,
         .pull = GPIO_PULL_NONE,
         .alternate = BRINGUP_SPI_AF},
        {.port = BRINGUP_SPI_MISO_PORT,
         .pin = BRINGUP_SPI_MISO_PIN,
         .mode = GPIO_MODE_ALTERNATE,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_VERY_HIGH,
         .pull = GPIO_PULL_NONE,
         .alternate = BRINGUP_SPI_AF},
        {.port = BRINGUP_SPI_MOSI_PORT,
         .pin = BRINGUP_SPI_MOSI_PIN,
         .mode = GPIO_MODE_ALTERNATE,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_VERY_HIGH,
         .pull = GPIO_PULL_NONE,
         .alternate = BRINGUP_SPI_AF},
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
    gpio_pin_config_t drdy_config = {.port = BRINGUP_DRDY_PORT,
                                     .pin = BRINGUP_DRDY_PIN,
                                     .mode = GPIO_MODE_INPUT,
                                     .otype = GPIO_OTYPE_PUSH_PULL,
                                     .speed = GPIO_SPEED_LOW,
                                     .pull = GPIO_PULL_DOWN,
                                     .alternate = 0};

    nss_config.port = BRINGUP_NSS_PORT;
    nss_config.pin = BRINGUP_NSS_PIN;
    rst_config.port = BRINGUP_RST_PORT;
    rst_config.pin = BRINGUP_RST_PIN;
    wakeup_config.port = BRINGUP_WAKEUP_PORT;
    wakeup_config.pin = BRINGUP_WAKEUP_PIN;
    boot0_config.port = BRINGUP_BOOT0_PORT;
    boot0_config.pin = BRINGUP_BOOT0_PIN;
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

    if (wifi_create(&wifi_cfg, &s_wifi) != WIFI_ERR_OK)
    {
        spin_forever();
    }

    /* Tasks main() creates. ModbusPollerTask is NOT here: it waits on the start gate, and the gate is
     * created by lifecycle_controller_init(), which runs in BootTask after the scheduler has started
     * (finding F-9: created here it dereferences a NULL event group at scheduler start). */
#if BENCH_WITH_CONSOLE
    (void) xTaskCreateStatic(console_task_body, "console", CONSOLE_TASK_STACK_WORDS, NULL,
                             CONSOLE_TASK_PRIORITY, s_console_stack, &s_console_tcb);
#endif
    (void) xTaskCreateStatic(stats_task, "stats", STATS_STACK_WORDS, NULL, STATS_PRIORITY,
                             s_stats_stack, &s_stats_tcb);
    (void) xTaskCreateStatic(boot_task, "lifecycle", BOOT_STACK_WORDS, NULL, BOOT_PRIORITY,
                             s_boot_stack, &s_boot_tcb);

    vTaskStartScheduler();

    spin_forever();
}
