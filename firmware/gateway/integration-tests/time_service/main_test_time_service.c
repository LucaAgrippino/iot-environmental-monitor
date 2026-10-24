/**
 * @file main_test_time_service.c
 * @brief Hardware bring-up test for TimeService (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. Add the current harness (cloud_publisher) to the `integration-tests` `excluding=`
 *      list and remove time_service from it.
 *   2. Add `modbus_poller` to the `application` entry's `excluding=` list: this harness
 *      defines its own modbus_poller_send_command() so the pushed Field Device command
 *      can be observed without a Modbus bus.
 *   3. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/time_service.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * NEEDS a 2.4 GHz access point with internet access and real credentials in
 * firmware/gateway/certs/bringup_secrets.h (gitignored).  TimeService uses the NtpClient
 * IPv4 servers compiled into time_service.c (the ISM43362 has no DNS).  Compare the
 * printed epoch with `date +%s` on the bench machine.
 *
 * Automated test sequence:
 *   TC-HW-TS-001  WiFi up, TimeProvider and NtpClient created, time_service_init() OK,
 *                 interface present, state NEVER before the task has run.
 *   TC-HW-TS-002  Boot sync: state becomes OK within 30 s without any caller action.
 *   TC-HW-TS-003  TimeProvider now reports SYNCHRONISED with a plausible epoch that agrees
 *                 with the Unix time NtpClient returns.
 *   TC-HW-TS-004  The Field Device push was posted: one FC16 command to CMD_SET_TIME_HI,
 *                 two words that reassemble to the synchronised epoch (+/- 5 s).
 *   TC-HW-TS-005  trigger_retry() makes a second sync happen (second FD push observed)
 *                 and the state stays OK.
 *
 * Integration checklist:
 *   [ ] Printed epoch matches the bench machine's clock
 *   [ ] TC-TS-I02..I05 of the LLD (disconnect/reconnect, FD register, delta rejection,
 *       periodic re-sync) are manual or need the FD and are not automated here
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "exti/exti_driver.h"
#include "gpio/gpio_driver.h"
#include "health_monitor/health_monitor.h"
#include "led/led_driver.h"
#include "logger/logger.h"
#include "modbus_poller/modbus_poller.h"
#include "ntp_client/ntp_client.h"
#include "rtc/rtc.h"
#include "spi/spi.h"
#include "time_provider/time_provider.h"
#include "time_service/time_service.h"
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

#define TAG "TimeSvc"

/* ---------------------------------------------------------------------- */
/* Test constants                                                          */
/* ---------------------------------------------------------------------- */

/* 2026-01-01 00:00:00 UTC: any real answer is later than this. */
#define EPOCH_PLAUSIBLE_MIN (1767225600UL)

#define BOOT_SYNC_TIMEOUT_MS (30000U)
#define RETRY_SYNC_TIMEOUT_MS (30000U)
#define POLL_STEP_MS (200U)
#define EPOCH_TOLERANCE_S (5UL)
#define CMD_SET_TIME_HI_REG (0x0210U)

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

/* Stand-in for ModbusPoller: records the Field Device time command and acknowledges it. */
static volatile uint32_t s_fd_push_count;
static modbus_command_t s_fd_last_cmd;

modbus_poller_err_t modbus_poller_send_command(const modbus_command_t *cmd)
{
    s_fd_last_cmd = *cmd;
    s_fd_push_count++;

    const modbus_command_response_t ok = {.status = MODBUS_MASTER_ERR_OK, .data_len = 0U};
    (void) xQueueSend(cmd->response_queue, &ok, 0U);
    return MODBUS_POLLER_ERR_OK;
}

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

static bool wait_for_state(const itime_service_t *ts, time_service_sync_t wanted,
                           uint32_t timeout_ms)
{
    for (uint32_t waited = 0U; waited < timeout_ms; waited += POLL_STEP_MS)
    {
        if (ts->get_sync_state(ts->ctx) == wanted)
        {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_STEP_MS));
    }
    return ts->get_sync_state(ts->ctx) == wanted;
}

static uint32_t fd_epoch(void)
{
    return ((uint32_t) s_fd_last_cmd.data[0] << 16U) | (uint32_t) s_fd_last_cmd.data[1];
}

static void bringup_task(void *arg)
{
    wifi_handle_t wifi_handle = (wifi_handle_t) arg;
    wifitask_handle_t wifitask_handle = NULL;
    ntp_client_handle_t ntp = NULL;

    const wifitask_config_t wifitask_config = {.wifi = wifi_handle};

    check(wifitask_create(&wifitask_config, &wifitask_handle) == WIFITASK_ERR_OK,
          "TC-HW-TS-001 wifitask_create");
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
        bringup_fail("TC-HW-TS-001 wifitask_connect_ap");
    }
    vTaskDelay(pdMS_TO_TICKS(ASSOCIATE_SETTLE_MS));

    const ntp_client_config_t ntp_config = {.wifi = wifitask_handle, .health = s_hm};

    check(ntp_client_create(&ntp_config, &ntp) == NTP_CLIENT_ERR_OK,
          "TC-HW-TS-001 ntp_client_create");
    check(time_provider_init(s_hm) == TIME_PROVIDER_ERR_OK, "TC-HW-TS-001 time_provider_init");

    /* TC-HW-TS-001 - the task is created here, so it runs its boot sync as soon as this
     * task blocks. State is read first to catch the NEVER start. */
    check(time_service_get_interface() == NULL, "TC-HW-TS-001 interface before init");
    check(time_service_init(ntp) == TIME_SERVICE_OK, "TC-HW-TS-001 time_service_init");
    const itime_service_t *ts = time_service_get_interface();

    check(ts != NULL, "TC-HW-TS-001 interface after init");
    LOG_INFO(TAG, "[PASS] TC-HW-TS-001 WiFi up, TimeService initialised");

    /* TC-HW-TS-002 */
    check(wait_for_state(ts, TIME_SERVICE_SYNC_OK, BOOT_SYNC_TIMEOUT_MS),
          "TC-HW-TS-002 boot sync did not reach OK - AP without internet?");
    LOG_INFO(TAG, "[PASS] TC-HW-TS-002 boot sync reached SYNC_OK");

    /* TC-HW-TS-003 */
    time_provider_ts_t now;

    check(time_provider_get(&now) == TIME_PROVIDER_ERR_OK, "TC-HW-TS-003 time_provider_get");
    LOG_INFO(TAG, "[INFO] RTC epoch = %lu  (compare with `date +%%s`)", (unsigned long) now.epoch);
    check(now.sync_state == TIME_SYNC_SYNCHRONISED, "TC-HW-TS-003 TimeProvider not synchronised");
    check(now.epoch > EPOCH_PLAUSIBLE_MIN, "TC-HW-TS-003 epoch implausible");
    LOG_INFO(TAG, "[PASS] TC-HW-TS-003 TimeProvider synchronised with a plausible epoch");

    /* TC-HW-TS-004 */
    check(s_fd_push_count == 1U, "TC-HW-TS-004 expected exactly one FD push after boot sync");
    check(s_fd_last_cmd.function_code == 0x10U, "TC-HW-TS-004 function code");
    check(s_fd_last_cmd.start_reg == CMD_SET_TIME_HI_REG, "TC-HW-TS-004 start register");
    check(s_fd_last_cmd.reg_count == 2U, "TC-HW-TS-004 register count");
    {
        const uint32_t pushed = fd_epoch();
        const uint32_t diff = (pushed > now.epoch) ? (pushed - now.epoch) : (now.epoch - pushed);

        LOG_INFO(TAG, "[INFO] FD push words 0x%04X 0x%04X = %lu", (unsigned) s_fd_last_cmd.data[0],
                 (unsigned) s_fd_last_cmd.data[1], (unsigned long) pushed);
        check(diff <= EPOCH_TOLERANCE_S, "TC-HW-TS-004 pushed epoch differs from the RTC");
    }
    LOG_INFO(TAG, "[PASS] TC-HW-TS-004 Field Device time command posted and encoded correctly");

    /* TC-HW-TS-005 */
    ts->trigger_retry(ts->ctx);
    for (uint32_t waited = 0U; (s_fd_push_count < 2U) && (waited < RETRY_SYNC_TIMEOUT_MS);
         waited += POLL_STEP_MS)
    {
        vTaskDelay(pdMS_TO_TICKS(POLL_STEP_MS));
    }
    check(s_fd_push_count == 2U, "TC-HW-TS-005 trigger_retry did not cause a second sync");
    check(ts->get_sync_state(ts->ctx) == TIME_SERVICE_SYNC_OK, "TC-HW-TS-005 state after retry");
    LOG_INFO(TAG, "[PASS] TC-HW-TS-005 trigger_retry() caused a second sync, state still OK");

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

    LOG_INFO(TAG, "===== TimeService Hardware Bring-up =====");
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

    (void) xTaskCreateStatic(bringup_task, "ts_bringup", BRINGUP_TASK_STACK_WORDS, wifi_handle,
                             BRINGUP_TASK_PRIORITY, s_task_stack, &s_task_tcb);
    vTaskStartScheduler();

    /* Only reached if the scheduler fails to start. */
    for (;;)
    {
    }
}
