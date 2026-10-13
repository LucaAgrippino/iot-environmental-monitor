/**
 * @file main_test_ntp_client.c
 * @brief Hardware bring-up test for NtpClient (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. Add the current harness (cloud_publisher) to the `excluding=` list of
 *      the `integration-tests` source entry and remove ntp_client.
 *   2. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/ntp_client.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * NEEDS a 2.4 GHz access point with internet access and real credentials in
 * firmware/gateway/certs/bringup_secrets.h (gitignored; see
 * scripts/bringup_secrets.h.example).  Servers are IPv4 literals because the
 * ISM43362 has no DNS resolver.  Compare the printed epoch with `date +%s` on the
 * bench machine: they should agree to within a few seconds.
 *
 * Automated test sequence:
 *   TC-HW-NTP-001  WifiDriver + WifiTask up, AP joined, ntp_client_create() succeeds.
 *   TC-HW-NTP-002  Query two public servers: OK, plausible Unix epoch (after 2026-01-01).
 *   TC-HW-NTP-003  First server unreachable (TEST-NET-1), second good: failover works.
 *   TC-HW-NTP-004  Only an unreachable server: ALL_FAILED, HealthMonitor counts one failure.
 *   TC-HW-NTP-005  Argument errors are rejected without touching the network.
 *   TC-HW-NTP-006  A successful query after the failures still works (sockets were closed).
 *
 * Integration checklist:
 *   [ ] Printed epoch matches the bench machine's clock
 *   [ ] TC-HW-NTP-003 / -004 elapsed times noted for NTP-O1 (query timeout validation)
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "exti/exti_driver.h"
#include "gpio/gpio_driver.h"
#include "health_monitor/health_monitor.h"
#include "led/led_driver.h"
#include "logger/logger.h"
#include "ntp_client/ntp_client.h"
#include "rtc/rtc.h"
#include "spi/spi.h"
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

#define TAG "Ntp"

/* ---------------------------------------------------------------------- */
/* Test constants                                                          */
/* ---------------------------------------------------------------------- */

#define SERVER_CLOUDFLARE "162.159.200.1"
#define SERVER_GOOGLE "216.239.35.0"
#define SERVER_BLACKHOLE "192.0.2.1" /* TEST-NET-1: never answers */

/* 2026-01-01 00:00:00 UTC: any real answer is later than this. */
#define EPOCH_PLAUSIBLE_MIN (1767225600UL)

/* ---------------------------------------------------------------------- */
/* Board constants — identical wiring to main_test_mqtt_client.c.          */
/* ---------------------------------------------------------------------- */

#define BRINGUP_LED_PORT GPIO_PORT_A
#define BRINGUP_LED_PIN (5U)
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

#define BRINGUP_TASK_STACK_WORDS (1024U)
#define BRINGUP_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define ASSOCIATE_SETTLE_MS (200U)

static StaticTask_t s_task_tcb;
static StackType_t s_task_stack[BRINGUP_TASK_STACK_WORDS];

static health_monitor_handle_t s_hm;

static const led_pin_t k_gw_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = (uint8_t) GPIO_PORT_B,
                   .pin = (uint8_t) LD2_PIN,
                   .active_high = true,
                   .fitted = true},
    [LED_RED] = {.port = (uint8_t) GPIO_PORT_A,
                 .pin = 5U,
                 .active_high = true,
                 .fitted = false}, /* LD1 / SB1 - shared with SPI1_SCK */
};

/* ---------------------------------------------------------------------- */
/* Reporting helpers                                                       */
/* ---------------------------------------------------------------------- */

static void bringup_raw_spin(uint32_t n)
{
    volatile uint32_t c = n;
    while (c > 0U)
    {
        --c;
    }
}

static void bringup_halt(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    GPIOA->MODER &= ~(3UL << (BRINGUP_LED_PIN * 2U));
    GPIOA->MODER |= (1UL << (BRINGUP_LED_PIN * 2U));
    for (;;)
    {
        GPIOA->BSRR = (1UL << BRINGUP_LED_PIN);
        bringup_raw_spin(200000U);
        GPIOA->BSRR = (1UL << (BRINGUP_LED_PIN + 16U));
        bringup_raw_spin(200000U);
    }
}

static void bringup_fail(const char *label)
{
    LOG_ERROR(TAG, "[FAIL] %s - HALTED", label);
    for (;;)
    {
        (void) gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
        {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else
        {
            bringup_raw_spin(500000U);
        }
    }
}

static void check(bool condition, const char *label)
{
    if (!condition)
    {
        bringup_fail(label);
    }
}

void EXTI1_IRQHandler(void)
{
    exti_clear_pending(WIFI_DRDY_EXTI_LINE);
    wifi_datardy_irq_handler();
}

/* ---------------------------------------------------------------------- */
/* Bring-up task                                                           */
/* ---------------------------------------------------------------------- */

static void bringup_task(void *arg)
{
    wifi_handle_t wifi_handle = (wifi_handle_t) arg;
    wifitask_handle_t wifitask_handle = NULL;
    ntp_client_handle_t ntp = NULL;
    uint32_t epoch = 0U;

    /* TC-HW-NTP-001 */
    const wifitask_config_t wifitask_config = {.wifi = wifi_handle};

    check(wifitask_create(&wifitask_config, &wifitask_handle) == WIFITASK_ERR_OK,
          "TC-HW-NTP-001 wifitask_create");
    if (sizeof(BRINGUP_WIFI_SSID) <= 1U)
    {
        LOG_INFO(TAG, "BRINGUP_WIFI_SSID is empty - cannot continue without an AP");
        for (;;)
        {
            (void) gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    const wifitask_err_t connect_err =
        wifitask_connect_ap(wifitask_handle, BRINGUP_WIFI_SSID, BRINGUP_WIFI_PASSWORD);

    if (connect_err != WIFITASK_ERR_OK)
    {
        LOG_ERROR(TAG, "wifitask_connect_ap() error code: %d", (int) connect_err);
        bringup_fail("TC-HW-NTP-001 wifitask_connect_ap");
    }
    vTaskDelay(pdMS_TO_TICKS(ASSOCIATE_SETTLE_MS));

    const ntp_client_config_t config = {.wifi = wifitask_handle, .health = s_hm};

    check(ntp_client_create(&config, &ntp) == NTP_CLIENT_ERR_OK, "TC-HW-NTP-001 ntp_client_create");
    LOG_INFO(TAG, "[PASS] TC-HW-NTP-001 WiFi up, AP joined, NtpClient created");

    /* TC-HW-NTP-002 */
    {
        const char *const servers[] = {SERVER_CLOUDFLARE, SERVER_GOOGLE};
        const uint32_t start = (uint32_t) xTaskGetTickCount();

        check(ntp_client_query(ntp, servers, 2U, &epoch) == NTP_CLIENT_ERR_OK,
              "TC-HW-NTP-002 query failed - AP without internet?");
        LOG_INFO(TAG, "[INFO] epoch = %lu  (compare with `date +%%s`), took %lu ms",
                 (unsigned long) epoch, (unsigned long) ((uint32_t) xTaskGetTickCount() - start));
        check(epoch > EPOCH_PLAUSIBLE_MIN, "TC-HW-NTP-002 epoch implausible");
        LOG_INFO(TAG, "[PASS] TC-HW-NTP-002 public servers answer; epoch is plausible");
    }

    /* TC-HW-NTP-003 */
    {
        const char *const servers[] = {SERVER_BLACKHOLE, SERVER_GOOGLE};
        const uint32_t start = (uint32_t) xTaskGetTickCount();

        epoch = 0U;
        check(ntp_client_query(ntp, servers, 2U, &epoch) == NTP_CLIENT_ERR_OK,
              "TC-HW-NTP-003 failover query");
        LOG_INFO(TAG, "[INFO] failover took %lu ms",
                 (unsigned long) ((uint32_t) xTaskGetTickCount() - start));
        check(epoch > EPOCH_PLAUSIBLE_MIN, "TC-HW-NTP-003 epoch implausible");
        LOG_INFO(TAG, "[PASS] TC-HW-NTP-003 unreachable first server, second one used");
    }

    /* TC-HW-NTP-004 */
    {
        const char *const servers[] = {SERVER_BLACKHOLE};
        health_snapshot_t before;
        health_snapshot_t after;
        const uint32_t start = (uint32_t) xTaskGetTickCount();

        check(health_monitor_get_snapshot(s_hm, &before) == HEALTH_MONITOR_ERR_OK,
              "TC-HW-NTP-004 snapshot before");
        check(ntp_client_query(ntp, servers, 1U, &epoch) == NTP_CLIENT_ERR_ALL_FAILED,
              "TC-HW-NTP-004 expected ALL_FAILED");
        LOG_INFO(TAG, "[INFO] all-failed query took %lu ms",
                 (unsigned long) ((uint32_t) xTaskGetTickCount() - start));
        check(health_monitor_get_snapshot(s_hm, &after) == HEALTH_MONITOR_ERR_OK,
              "TC-HW-NTP-004 snapshot after");
        check(after.ntp_sync_fail_count == (before.ntp_sync_fail_count + 1U),
              "TC-HW-NTP-004 HealthMonitor did not count exactly one failure");
        LOG_INFO(TAG, "[PASS] TC-HW-NTP-004 ALL_FAILED reported once to HealthMonitor");
    }

    /* TC-HW-NTP-005 */
    {
        const char *const servers[] = {SERVER_GOOGLE};

        check(ntp_client_query(ntp, servers, 0U, &epoch) == NTP_CLIENT_ERR_NULL_PTR,
              "TC-HW-NTP-005 zero count");
        check(ntp_client_query(ntp, servers, 1U, NULL) == NTP_CLIENT_ERR_NULL_PTR,
              "TC-HW-NTP-005 NULL output");
        check(ntp_client_query(ntp, servers, NTP_CLIENT_MAX_SERVERS + 1U, &epoch) ==
                  NTP_CLIENT_ERR_INVALID_ARG,
              "TC-HW-NTP-005 too many servers");
        LOG_INFO(TAG, "[PASS] TC-HW-NTP-005 argument errors rejected");
    }

    /* TC-HW-NTP-006 */
    {
        const char *const servers[] = {SERVER_GOOGLE};

        check(ntp_client_query(ntp, servers, 1U, &epoch) == NTP_CLIENT_ERR_OK,
              "TC-HW-NTP-006 query after failures - socket leak?");
        LOG_INFO(TAG, "[PASS] TC-HW-NTP-006 still works after failed queries (sockets closed)");
    }

    LOG_INFO(TAG, "===== ALL TESTS PASSED =====");
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------------------------------------------------------------------- */
/* main()                                                                  */
/* ---------------------------------------------------------------------- */

int main(void)
{
    /* cpu_init() first - clock to 80 MHz, DWT cycle counter, fault handlers. */
    if (cpu_init() != STATUS_OK)
    {
        bringup_halt();
    }

    const gpio_pin_config_t uart_tx_config = {.port = BRINGUP_UART_TX_PORT,
                                              .pin = BRINGUP_UART_TX_PIN,
                                              .mode = GPIO_MODE_ALTERNATE,
                                              .otype = GPIO_OTYPE_PUSH_PULL,
                                              .speed = GPIO_SPEED_VERY_HIGH,
                                              .pull = GPIO_PULL_NONE,
                                              .alternate = BRINGUP_UART_AF};
    const gpio_pin_config_t uart_rx_config = {.port = BRINGUP_UART_RX_PORT,
                                              .pin = BRINGUP_UART_RX_PIN,
                                              .mode = GPIO_MODE_ALTERNATE,
                                              .otype = GPIO_OTYPE_PUSH_PULL,
                                              .speed = GPIO_SPEED_VERY_HIGH,
                                              .pull = GPIO_PULL_UP,
                                              .alternate = BRINGUP_UART_AF};

    if ((gpio_init() != GPIO_OK) || (gpio_configure_pin(&uart_tx_config) != GPIO_OK) ||
        (gpio_configure_pin(&uart_rx_config) != GPIO_OK) || (debug_uart_init() != DEBUG_UART_OK) ||
        (rtc_init() != RTC_OK))
    {
        bringup_halt();
    }
    (void) logger_init(LOG_LEVEL_DEBUG);

    LOG_INFO(TAG, "===== NtpClient Hardware Bring-up =====");
    LOG_INFO(TAG, "Board : B-L475E-IOT01A (STM32L475VGTx)");

    /* WifiDriver phase 1 (pre-scheduler): pins, SpiDriver, wifi_create(). */
    const gpio_pin_config_t spi_pins[3] = {
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
            bringup_fail("SPI3 pin configuration failed");
        }
    }

    const gpio_pin_config_t control_pins[5] = {
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

    for (uint8_t i = 0U; i < 5U; ++i)
    {
        if (gpio_configure_pin(&control_pins[i]) != GPIO_OK)
        {
            bringup_fail("ISM43362 control-line configuration failed");
        }
    }

    const spi_config_t spi_config = {.instance = SPI3};
    spi_handle_t spi_handle = NULL;

    if (spi_create(&spi_config, &spi_handle) != SPI_ERR_OK)
    {
        bringup_fail("spi_create");
    }

    const wifi_config_t wifi_config = {
        .spi = spi_handle,
        .nss_port = BRINGUP_NSS_PORT,
        .nss_pin = BRINGUP_NSS_PIN,
        .drdy_port = BRINGUP_DRDY_PORT,
        .drdy_pin = BRINGUP_DRDY_PIN,
        .rst_port = BRINGUP_RST_PORT,
        .rst_pin = BRINGUP_RST_PIN,
        .wakeup_port = BRINGUP_WAKEUP_PORT,
        .wakeup_pin = BRINGUP_WAKEUP_PIN,
        .boot0_port = BRINGUP_BOOT0_PORT,
        .boot0_pin = BRINGUP_BOOT0_PIN,
    };
    wifi_handle_t wifi_handle = NULL;

    if (wifi_create(&wifi_config, &wifi_handle) != WIFI_ERR_OK)
    {
        bringup_fail("wifi_create");
    }

    /* NtpClient reports failures to HealthMonitor, which drives LD2 through LedDriver. */
    const health_monitor_config_t hm_config = {.boot_reset_cause = LIFECYCLE_RESET_SOFT};

    if (led_init(k_gw_led_pins, (uint8_t) LED_COUNT) != LED_ERR_OK)
    {
        bringup_fail("led_init");
    }
    if (health_monitor_create(&hm_config, &s_hm) != HEALTH_MONITOR_ERR_OK)
    {
        bringup_fail("health_monitor_create");
    }

    (void) xTaskCreateStatic(bringup_task, "ntp_bringup", BRINGUP_TASK_STACK_WORDS, wifi_handle,
                             BRINGUP_TASK_PRIORITY, s_task_stack, &s_task_tcb);
    vTaskStartScheduler();

    /* Only reached if the scheduler fails to start. */
    for (;;)
    {
    }
}
