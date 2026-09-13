/**
 * @file main_test_wifi_task.c
 * @brief Hardware bring-up test for WifiTask (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default. To activate:
 *   1. Copy this file into Src/ (or add integration-tests/wifi_task/ as a
 *      source path in CubeIDE).
 *   2. In CubeIDE, right-click Src/main.c -> Properties ->
 *      C/C++ Build -> check "Exclude resource from build".
 *   3. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   4. Wire the ISM43362-M3G-L44 module (or use the on-board one on the
 *      B-L475E-IOT01A) and flash with a debugger attached.
 *   5. Create firmware/gateway/certs/bringup_secrets.h with your real
 *      BRINGUP_WIFI_SSID / BRINGUP_WIFI_PASSWORD / BRINGUP_REMOTE_HOST —
 *      see scripts/bringup_secrets.h.example for the template.
 *      bringup_secrets.h is gitignored and never committed; without
 *      it this is a placeholder-only build. Same
 *      test bench as main_test_wifi_driver.c (see that file's header for
 *      the phone-hotspot + TCP/UDP server app setup) — a dedicated router
 *      works too, but some networks silently drop device-to-device
 *      traffic (WIFI-O12).
 *   6. To exercise real data transfer (TC-HW-WIFITASK-005/006): same
 *      procedure as main_test_wifi_driver.c's step 6 — start the TCP/UDP
 *      server app first, set BRINGUP_REMOTE_HOST in bringup_secrets.h,
 *      watch for the live countdown before each connect attempt.
 *
 * Scope: unlike main_test_wifi_driver.c (which calls wifi_* functions
 * directly, proving WifiDriver alone), this bring-up calls wifitask_*()
 * functions exclusively — proving the request-queue/dispatch/notify round
 * trip between two real, separately-scheduled FreeRTOS tasks (this
 * bring-up task and WifiTask's own task, both spawned here) actually
 * works on hardware, not just against the freertos_mock.c host stubs the
 * unit tests use. WifiTask is a synchronous relocate (docs/lld/middleware/
 * wifi-task.md §5.2): every wifitask_*() call still blocks for the same
 * wall-clock duration the underlying wifi_*() call takes — what's new
 * here is that the blocking round trip now crosses two tasks via a real
 * queue and a real xTaskNotifyWait(), not a same-task direct call.
 *
 * Not exercised here: WIFITASK-O3 (MqttClient still calls WifiDriver
 * directly, not through WifiTask) — this bring-up is WifiTask in
 * isolation, same relationship main_test_wifi_driver.c has to WifiDriver
 * before MqttClient existed. WIFI-O15's liveness-check/reconnect path
 * (companion §5.3) is not driven to a failure here either — it would
 * need a real, physically-triggered AP drop mid-run to observe, which
 * this bring-up does not attempt; the heartbeat loop at the end runs long
 * enough for the periodic tick (WIFI_LIVENESS_CHECK_PERIOD_MS, 30 s) to
 * fire at least once with nothing to do, which is as far as this harness
 * goes toward proving that path is alive.
 *
 * Reports through the real Logger middleware, same pattern as
 * main_test_wifi_driver.c. wifi_create() (WifiDriver Phase 1) runs
 * pre-scheduler; wifitask_create() (which internally spawns WifiTask's
 * own task and registers the DATARDY callback) must run post-scheduler,
 * same constraint wifi_attach_datardy_callback() already has.
 *
 * Hardware outputs:
 *   USART1 TX  PB6   115 200 8N1  Logger output (LOG_INFO/LOG_ERROR lines)
 *   LD2 green  PA5                Heartbeat once the bring-up task is running
 *   SPI3 SCK/MISO/MOSI  PC10/PC11/PC12  AF6, 10 MHz, mode 0, 16-bit frames
 *   NSS   PE0  (GpioDriver, output, active low)
 *   DRDY  PE1  (GpioDriver input + ExtiDriver EXTI1, rising edge)
 *   RST   PE8  (GpioDriver, output, active low)
 *   WAKEUP PB13 (GpioDriver, output, active high)
 *   BOOT0 PB12 (GpioDriver, output, low = normal boot)
 *
 * Automated test sequence:
 *   TC-HW-WIFITASK-001  gpio_init() + spi_create() + wifi_create() all
 *                       succeed (WifiDriver Phase 1, identical to
 *                       main_test_wifi_driver.c TC-HW-WIFI-001)
 *   TC-HW-WIFITASK-002  wifitask_create() succeeds — spawns WifiTask's own
 *                       task and registers the DATARDY callback internally
 *   TC-HW-WIFITASK-003  wifitask_get_link_state() reports WIFI_LINK_DOWN
 *                       pre-connect — proves the request/dispatch/notify
 *                       round trip works for the simplest possible call
 *   TC-HW-WIFITASK-004  (only if BRINGUP_WIFI_SSID is non-empty)
 *                       wifitask_connect_ap() + wifitask_get_rssi(),
 *                       routed through WifiTask
 *   TC-HW-WIFITASK-005  Open a TCP socket via wifitask_open_socket(), send,
 *                       wait for a reply via wifitask_recv(), close
 *   TC-HW-WIFITASK-006  Same as 005, over UDP
 *
 * If cpu_init(), debug_uart_init(), or rtc_init() fail, Logger cannot be
 * trusted as the reporting channel yet — the board halts with a fast LED
 * blink instead (same convention as main_test_wifi_driver.c).
 */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "exti/exti_driver.h"
#include "gpio/gpio_driver.h"
#include "logger/logger.h"
#include "rtc/rtc.h"
#include "spi/spi.h"
#include "wifi_task/wifi_task.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Fill these in before flashing to exercise the AP-connect + socket path. */
/* Leave BRINGUP_WIFI_SSID empty to skip TC-HW-WIFITASK-004/005/006.       */
/* ---------------------------------------------------------------------- */

/* Real values come from bringup_secrets.h (gitignored, never committed —
 * see scripts/bringup_secrets.h.example) when the file exists; otherwise
 * this is a placeholder-only build. Same __has_include mechanism as
 * bringup_certs.h below, so no build-setting toggle is needed and the
 * tracked .cproject builds cleanly on a machine without secrets (CI). */
#if __has_include("bringup_secrets.h")
#include "bringup_secrets.h"
#else
#define BRINGUP_WIFI_SSID ""
#define BRINGUP_WIFI_PASSWORD ""
#define BRINGUP_REMOTE_HOST ""
#endif

/* Peer TCP/UDP ports — not personal/sensitive, safe to leave hardcoded. */
#define BRINGUP_REMOTE_TCP_PORT (8080U)
#define BRINGUP_REMOTE_UDP_PORT (8081U)

#define BRINGUP_RECV_TIMEOUT_MS (30000U)
#define BRINGUP_COUNTDOWN_S (10U)

/* ---------------------------------------------------------------------- */
/* Board constants — identical wiring to main_test_wifi_driver.c.         */
/* ---------------------------------------------------------------------- */

#define BRINGUP_LED_PORT GPIO_PORT_A
#define BRINGUP_LED_PIN (5U)

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

#define BRINGUP_TASK_STACK_WORDS (384U)
#define BRINGUP_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)

/* ---------------------------------------------------------------------- */
/* Boot-failure halt — identical convention to main_test_wifi_driver.c.   */
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
    LOG_ERROR("WifiTask", "%s", label);
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

/* ---------------------------------------------------------------------- */
/* EXTI1 ISR — same requirement as main_test_wifi_driver.c: WifiDriver     */
/* still owns the DATARDY line (WifiTask doesn't change this wiring), and  */
/* stm32l4xx_it.c is CubeIDE-generated, not tracked in this repo.          */
/* ---------------------------------------------------------------------- */

void EXTI1_IRQHandler(void)
{
    exti_clear_pending(WIFI_DRDY_EXTI_LINE);
    wifi_datardy_irq_handler();
}

static void bringup_countdown(const char *action)
{
    for (uint32_t s = BRINGUP_COUNTDOWN_S; s > 0U; s--)
    {
        LOG_INFO("WifiTask", "%s in %u...", action, (unsigned) s);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief Open a socket, send a hello, prompt for and wait on a reply,
 *        close — every step routed through wifitask_*(), not wifi_*()
 *        directly. Shared by TC-HW-WIFITASK-005 (TCP) and -006 (UDP).
 */
static void bringup_socket_roundtrip(wifitask_handle_t wifitask_handle, const char *tc_id,
                                     wifi_socket_type_t type, uint16_t port,
                                     const uint8_t *hello, size_t hello_len)
{
    const char *proto = (type == WIFI_SOCKET_TCP) ? "TCP" : "UDP";

    LOG_INFO("WifiTask", "%s  about to connect to %s:%u over %s", tc_id, BRINGUP_REMOTE_HOST,
             (unsigned) port, proto);
    LOG_INFO("WifiTask", "%s  make sure your %s server/listener app is running now", tc_id, proto);
    bringup_countdown("Connecting");

    wifi_socket_t sock = WIFI_INVALID_SOCKET;
    if (wifitask_open_socket(wifitask_handle, type, BRINGUP_REMOTE_HOST, port, &sock) !=
        WIFITASK_ERR_OK)
    {
        LOG_ERROR("WifiTask", "%s  wifitask_open_socket(%s) failed", tc_id, proto);
        bringup_fail(tc_id);
    }
    LOG_INFO("WifiTask", "%s  socket open (id=%u)", tc_id, (unsigned) sock);
    vTaskDelay(pdMS_TO_TICKS(200));

    if (wifitask_send(wifitask_handle, sock, hello, hello_len) != WIFITASK_ERR_OK)
    {
        LOG_ERROR("WifiTask", "%s  wifitask_send() failed", tc_id);
        bringup_fail(tc_id);
    }
    LOG_INFO("WifiTask", "%s  sent %u bytes - check your app's receive window", tc_id,
             (unsigned) hello_len);
    vTaskDelay(pdMS_TO_TICKS(200));

    LOG_INFO("WifiTask", "%s  type a reply in the app NOW and send it", tc_id);
    LOG_INFO("WifiTask", "%s  waiting up to %u s for a reply...", tc_id,
             (unsigned) (BRINGUP_RECV_TIMEOUT_MS / 1000U));
    vTaskDelay(pdMS_TO_TICKS(200));

    uint8_t rx_buf[64];
    size_t rx_len = 0U;
    wifitask_err_t recv_err = wifitask_recv(wifitask_handle, sock, rx_buf, sizeof(rx_buf) - 1U,
                                            &rx_len, BRINGUP_RECV_TIMEOUT_MS);
    if (recv_err == WIFITASK_ERR_OK)
    {
        rx_buf[rx_len] = (uint8_t) '\0';
        LOG_INFO("WifiTask", "%s  received %u bytes: %s", tc_id, (unsigned) rx_len,
                 (const char *) rx_buf);
    }
    else
    {
        LOG_INFO("WifiTask", "%s  no reply within %u s", tc_id,
                 (unsigned) (BRINGUP_RECV_TIMEOUT_MS / 1000U));
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    (void) wifitask_close_socket(wifitask_handle, sock);
    LOG_INFO("WifiTask", "%s  socket closed", tc_id);
    vTaskDelay(pdMS_TO_TICKS(200));
}

/* ---------------------------------------------------------------------- */
/* Bring-up task — Phase 2 (post-scheduler): create WifiTask, run          */
/* TC-HW-WIFITASK-002..006, then heartbeat.                                */
/* ---------------------------------------------------------------------- */

static StaticTask_t s_bringup_task_tcb;
static StackType_t s_bringup_task_stack[BRINGUP_TASK_STACK_WORDS];

static void wifitask_bringup_task(void *arg)
{
    wifi_handle_t wifi_handle = (wifi_handle_t) arg;

    /* TC-HW-WIFITASK-002 */
    wifitask_config_t wifitask_config = {.wifi = wifi_handle};
    wifitask_handle_t wifitask_handle = NULL;
    wifitask_err_t create_err = wifitask_create(&wifitask_config, &wifitask_handle);
    if (create_err != WIFITASK_ERR_OK)
    {
        LOG_ERROR("WifiTask", "wifitask_create() error code: %d", (int) create_err);
        bringup_fail("TC-HW-WIFITASK-002  wifitask_create() failed");
    }
    LOG_INFO("WifiTask", "TC-HW-WIFITASK-002  wifitask_create() returned WIFITASK_ERR_OK "
                        "(WifiTask's own task is now running)");

    /* TC-HW-WIFITASK-003 */
    wifi_link_state_t link_state;
    wifitask_err_t link_err = wifitask_get_link_state(wifitask_handle, &link_state);
    if ((link_err != WIFITASK_ERR_OK) || (link_state != WIFI_LINK_DOWN))
    {
        LOG_ERROR("WifiTask", "wifitask_get_link_state() error code: %d", (int) link_err);
        bringup_fail("TC-HW-WIFITASK-003  expected WIFI_LINK_DOWN before wifitask_connect_ap()");
    }
    LOG_INFO("WifiTask", "TC-HW-WIFITASK-003  link state is WIFI_LINK_DOWN pre-connect "
                        "(round trip through WifiTask confirmed)");

    /* TC-HW-WIFITASK-004 — only runs if credentials were filled in above. */
    if (sizeof(BRINGUP_WIFI_SSID) > 1U)
    {
        wifitask_err_t connect_err =
            wifitask_connect_ap(wifitask_handle, BRINGUP_WIFI_SSID, BRINGUP_WIFI_PASSWORD);
        if (connect_err != WIFITASK_ERR_OK)
        {
            LOG_ERROR("WifiTask", "wifitask_connect_ap() error code: %d", (int) connect_err);
            bringup_fail("TC-HW-WIFITASK-004  wifitask_connect_ap() failed");
        }

        int8_t rssi_dbm = 0;
        wifitask_err_t rssi_err = wifitask_get_rssi(wifitask_handle, &rssi_dbm);
        if (rssi_err != WIFITASK_ERR_OK)
        {
            LOG_ERROR("WifiTask", "wifitask_get_rssi() error code: %d", (int) rssi_err);
            bringup_fail("TC-HW-WIFITASK-004  wifitask_get_rssi() failed after association");
        }
        LOG_INFO("WifiTask", "TC-HW-WIFITASK-004  associated, RSSI=%d dBm", (int) rssi_dbm);
        vTaskDelay(pdMS_TO_TICKS(200));

        if (sizeof(BRINGUP_REMOTE_HOST) > 1U)
        {
            static const uint8_t tcp_hello[] = "WIFITASK-BRINGUP-TCP-HELLO\r\n";
            bringup_socket_roundtrip(wifitask_handle, "TC-HW-WIFITASK-005", WIFI_SOCKET_TCP,
                                     BRINGUP_REMOTE_TCP_PORT, tcp_hello, sizeof(tcp_hello) - 1U);

            LOG_INFO("WifiTask", "Switch to your UDP listener on port %u now",
                     (unsigned) BRINGUP_REMOTE_UDP_PORT);
            bringup_countdown("Continuing to TC-HW-WIFITASK-006");

            static const uint8_t udp_hello[] = "WIFITASK-BRINGUP-UDP-HELLO\r\n";
            bringup_socket_roundtrip(wifitask_handle, "TC-HW-WIFITASK-006", WIFI_SOCKET_UDP,
                                     BRINGUP_REMOTE_UDP_PORT, udp_hello, sizeof(udp_hello) - 1U);
        }
        else
        {
            LOG_INFO("WifiTask", "BRINGUP_REMOTE_HOST is empty - skipping "
                                "TC-HW-WIFITASK-005/006");
        }
    }
    else
    {
        LOG_INFO("WifiTask", "BRINGUP_WIFI_SSID is empty - skipping "
                            "TC-HW-WIFITASK-004/005/006");
    }

    LOG_INFO("WifiTask", "All automated tests complete. Heartbeat running — the periodic "
                        "liveness-check tick (WIFI-O15) fires every 30 s from here on "
                        "whenever WifiTask's request queue is idle.");

    for (;;)
    {
        (void) gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ---------------------------------------------------------------------- */
/* main()                                                                  */
/* ---------------------------------------------------------------------- */

int main(void)
{
    if (cpu_init() != STATUS_OK)
    {
        bringup_halt();
    }

    if (gpio_init() != GPIO_OK)
    {
        bringup_halt();
    }

    gpio_pin_config_t uart_tx_config = {
        .port = BRINGUP_UART_TX_PORT,
        .pin = BRINGUP_UART_TX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_NONE,
        .alternate = BRINGUP_UART_AF,
    };
    gpio_pin_config_t uart_rx_config = {
        .port = BRINGUP_UART_RX_PORT,
        .pin = BRINGUP_UART_RX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_UP,
        .alternate = BRINGUP_UART_AF,
    };
    (void) gpio_configure_pin(&uart_tx_config);
    (void) gpio_configure_pin(&uart_rx_config);
    if (debug_uart_init() != DEBUG_UART_OK)
    {
        bringup_halt();
    }
    if (rtc_init() != RTC_OK)
    {
        bringup_halt();
    }

    (void) logger_init(LOG_LEVEL_DEBUG);

    LOG_INFO("WifiTask", "===== WifiTask Hardware Bring-up =====");
    LOG_INFO("WifiTask", "Board : B-L475E-IOT01A (STM32L475VGTx)");
    LOG_INFO("WifiTask", "Module: ISM43362-M3G-L44 via SPI3");
    LOG_INFO("WifiTask", "SYSCLK=%lu Hz", (unsigned long) cpu_get_sysclk_hz());

    /* WifiDriver Phase 1 (pre-scheduler): pins, SpiDriver, wifi_create() —
     * identical to main_test_wifi_driver.c TC-HW-WIFI-001. WifiTask does
     * not change WifiDriver's own init sequence. */
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
            bringup_fail("TC-HW-WIFITASK-001  SPI3 pin configuration failed");
        }
    }

    gpio_pin_config_t nss_config = {.port = BRINGUP_NSS_PORT,
                                    .pin = BRINGUP_NSS_PIN,
                                    .mode = GPIO_MODE_OUTPUT,
                                    .otype = GPIO_OTYPE_PUSH_PULL,
                                    .speed = GPIO_SPEED_LOW,
                                    .pull = GPIO_PULL_NONE,
                                    .alternate = 0};
    gpio_pin_config_t drdy_config = {.port = BRINGUP_DRDY_PORT,
                                     .pin = BRINGUP_DRDY_PIN,
                                     .mode = GPIO_MODE_INPUT,
                                     .otype = GPIO_OTYPE_PUSH_PULL,
                                     .speed = GPIO_SPEED_LOW,
                                     .pull = GPIO_PULL_DOWN,
                                     .alternate = 0};
    gpio_pin_config_t rst_config = {.port = BRINGUP_RST_PORT,
                                    .pin = BRINGUP_RST_PIN,
                                    .mode = GPIO_MODE_OUTPUT,
                                    .otype = GPIO_OTYPE_PUSH_PULL,
                                    .speed = GPIO_SPEED_LOW,
                                    .pull = GPIO_PULL_NONE,
                                    .alternate = 0};
    gpio_pin_config_t wakeup_config = {.port = BRINGUP_WAKEUP_PORT,
                                       .pin = BRINGUP_WAKEUP_PIN,
                                       .mode = GPIO_MODE_OUTPUT,
                                       .otype = GPIO_OTYPE_PUSH_PULL,
                                       .speed = GPIO_SPEED_LOW,
                                       .pull = GPIO_PULL_NONE,
                                       .alternate = 0};
    gpio_pin_config_t boot0_config = {.port = BRINGUP_BOOT0_PORT,
                                      .pin = BRINGUP_BOOT0_PIN,
                                      .mode = GPIO_MODE_OUTPUT,
                                      .otype = GPIO_OTYPE_PUSH_PULL,
                                      .speed = GPIO_SPEED_LOW,
                                      .pull = GPIO_PULL_NONE,
                                      .alternate = 0};
    if ((gpio_configure_pin(&nss_config) != GPIO_OK) ||
        (gpio_configure_pin(&drdy_config) != GPIO_OK) ||
        (gpio_configure_pin(&rst_config) != GPIO_OK) ||
        (gpio_configure_pin(&wakeup_config) != GPIO_OK) ||
        (gpio_configure_pin(&boot0_config) != GPIO_OK))
    {
        bringup_fail("TC-HW-WIFITASK-001  ISM43362 control-line configuration failed");
    }

    spi_config_t spi_config = {.instance = SPI3};
    spi_handle_t spi_handle = NULL;
    if (spi_create(&spi_config, &spi_handle) != SPI_ERR_OK)
    {
        bringup_fail("TC-HW-WIFITASK-001  spi_create() failed");
    }

    wifi_config_t wifi_config = {
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
    wifi_err_t wifi_err = wifi_create(&wifi_config, &wifi_handle);
    if (wifi_err != WIFI_ERR_OK)
    {
        LOG_ERROR("WifiTask", "wifi_create() error code: %d", (int) wifi_err);
        bringup_fail("TC-HW-WIFITASK-001  wifi_create() failed (reset sequence, AT "
                     "handshake, or firmware version check)");
    }
    LOG_INFO("WifiTask", "TC-HW-WIFITASK-001  wifi_create() completed reset + AT handshake + "
                        "firmware check");
    LOG_INFO("WifiTask", "starting scheduler...");

    (void) xTaskCreateStatic(wifitask_bringup_task, "wifitask_bringup", BRINGUP_TASK_STACK_WORDS,
                             wifi_handle, BRINGUP_TASK_PRIORITY, s_bringup_task_stack,
                             &s_bringup_task_tcb);

    vTaskStartScheduler();

    for (;;)
    {
    }
}
