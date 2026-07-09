/**
 * @file main_test_wifi_driver.c
 * @brief Hardware bring-up test for WifiDriver (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default. To activate:
 *   1. Copy this file into Src/ (or add integration-tests/wifi_driver/ as
 *      a source path in CubeIDE).
 *   2. In CubeIDE, right-click Src/main.c -> Properties ->
 *      C/C++ Build -> check "Exclude resource from build".
 *   3. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   4. Wire the ISM43362-M3G-L44 module (or use the on-board one on the
 *      B-L475E-IOT01A) and flash with a debugger attached.
 *   5. Optionally fill in BRINGUP_WIFI_SSID / BRINGUP_WIFI_PASSWORD below
 *      before flashing to exercise the association + socket path.
 *   6. To exercise real data transfer (TC-HW-WIFI-005/006), run SocketTest
 *      (https://sourceforge.net/projects/sockettest/, or any TCP/UDP
 *      listener) on a PC on the same LAN as the board:
 *        - Start it in TCP server (listen) mode on BRINGUP_SOCKETTEST_TCP_PORT
 *          before flashing, to catch TC-HW-WIFI-005.
 *        - The task pauses 5 s between TC-HW-WIFI-005 and TC-HW-WIFI-006 —
 *          switch SocketTest to UDP listen mode on BRINGUP_SOCKETTEST_UDP_PORT
 *          during that pause.
 *        - Fill in BRINGUP_SOCKETTEST_HOST with the PC's LAN IP address.
 *        - Watching the bytes actually arrive in SocketTest's receive
 *          window is the point: it proves data survives the full path
 *          (WifiDriver -> ISM43362 -> real network -> PC), not just that
 *          AT commands returned OK.
 *
 * Reports through the real Logger middleware (companion:
 * docs/lld/middleware/logger.md), not raw UART — same pattern as
 * integration-tests/logger/main_test_logger.c. wifi_create() itself
 * (companion Phase 1) runs pre-scheduler, so its diagnostics take
 * Logger's synchronous-write path; wifi_attach_datardy_callback() and the
 * connect/RSSI/socket exercise (companion Phase 2) run from a real
 * FreeRTOS task after vTaskStartScheduler(), exercising Logger's
 * queue + drain-task path as well as WifiDriver's own two-phase design.
 *
 * Hardware outputs:
 *   USART1 TX  PB6   115 200 8N1  Logger output (LOG_INFO/LOG_ERROR lines)
 *   LD2 green  PA5                Heartbeat once the wifi task is running
 *   SPI3 SCK/MISO/MOSI  PC10/PC11/PC12  AF6, 10 MHz, mode 0, 16-bit frames
 *   NSS   PE0  (GpioDriver, output, active low)
 *   DRDY  PE1  (GpioDriver input + ExtiDriver EXTI1, rising edge)
 *   RST   PE8  (GpioDriver, output, active low)
 *   WAKEUP PB13 (GpioDriver, output, active high)
 *   BOOT0 PB12 (GpioDriver, output, low = normal boot)
 *
 * Automated test sequence:
 *   TC-HW-WIFI-001  gpio_init() + spi_create() + wifi_create() all succeed
 *                   (exercises the full ISM43362 reset + AT handshake +
 *                   firmware version check against real hardware)
 *   TC-HW-WIFI-002  wifi_attach_datardy_callback() returns WIFI_ERR_OK
 *   TC-HW-WIFI-003  wifi_get_link_state() reports WIFI_LINK_DOWN pre-connect
 *   TC-HW-WIFI-004  (only if BRINGUP_WIFI_SSID is non-empty) connect to the
 *                   configured AP, read RSSI
 *   TC-HW-WIFI-005  Open a TCP socket to BRINGUP_SOCKETTEST_HOST:_TCP_PORT,
 *                   send a message, attempt to receive a reply, close
 *   TC-HW-WIFI-006  Same as 005, over UDP to BRINGUP_SOCKETTEST_UDP_PORT
 *
 * If cpu_init(), debug_uart_init(), or rtc_init() fail, Logger cannot be
 * trusted as the reporting channel yet — the board halts with a fast LED
 * blink instead (same convention as main_test_logger.c).
 *
 * Manual step (requires the DATARDY line and a logic analyser, or reliance
 * on the on-board module's real responses): probe PE1 during TC-HW-WIFI-001
 * to confirm the module asserts DATARDY in response to the AT handshake.
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
#include "wifi_driver/wifi_driver.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Fill these in before flashing to exercise the AP-connect + socket path. */
/* Leave BRINGUP_WIFI_SSID empty to skip TC-HW-WIFI-004/005/006.            */
/* ---------------------------------------------------------------------- */
#define BRINGUP_WIFI_SSID "Grove Island"
#define BRINGUP_WIFI_PASSWORD "island1234"

/* PC running SocketTest (or any TCP/UDP listener) on the same LAN — see
 * the file header for setup instructions. Leave BRINGUP_SOCKETTEST_HOST
 * empty to skip TC-HW-WIFI-005/006. */
#define BRINGUP_SOCKETTEST_HOST ""
#define BRINGUP_SOCKETTEST_TCP_PORT (5000U)
#define BRINGUP_SOCKETTEST_UDP_PORT (5001U)

/* ---------------------------------------------------------------------- */
/* Board constants                                                         */
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

/** ISM43362 control lines — UM2153 Table 11. */
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

#define WIFI_TASK_STACK_WORDS (384U)
#define WIFI_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)

/* ---------------------------------------------------------------------- */
/* Boot-failure halt — Logger is not yet trusted, so this bypasses it,     */
/* same convention as integration-tests/logger/main_test_logger.c.         */
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

/**
 * @brief Halt after Logger is up: log the reason, then blink via GpioDriver.
 *
 * Post-scheduler, this must actually yield (vTaskDelay), not busy-spin:
 * WIFI_TASK_PRIORITY is higher than LOGGER_DRAIN_TASK_PRIORITY, so a
 * non-yielding loop here starves the drain task forever, and every queued
 * message — including this call's own LOG_ERROR(), and anything logged
 * earlier in this same task that hadn't drained yet — is silently lost.
 * Pre-scheduler (e.g. a wifi_create() failure in main()), the scheduler
 * isn't running yet, so vTaskDelay would be invalid; fall back to the
 * raw spin there, same as before.
 */
static void bringup_fail(const char *label)
{
    LOG_ERROR("Wifi", "%s", label);
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
/* EXTI1 ISR — overrides the weak default handler from the startup file.  */
/* wifi_driver.h documents this as living in stm32l4xx_it.c, which is the */
/* real production wiring once this code is merged into a CubeIDE        */
/* project — but that file is CubeIDE-generated and not tracked in this   */
/* repo, so this standalone bring-up main provides it directly, same as  */
/* integration-tests/exti/main_test_exti.c does for ExtiDriver's own      */
/* bring-up. Without this, wifi_attach_datardy_callback()'s exti_enable() */
/* leaves the vector at its weak default (Default_Handler -> infinite     */
/* loop), which fires on DRDY's first real rising edge after that point   */
/* — in practice, the first AT command sent from the post-scheduler task. */
/* ---------------------------------------------------------------------- */

void EXTI1_IRQHandler(void)
{
    exti_clear_pending(WIFI_DRDY_EXTI_LINE);
    wifi_datardy_irq_handler();
}

/* ---------------------------------------------------------------------- */
/* DATARDY callback — this bring-up never enables real WifiTask-style      */
/* notification plumbing (xTaskNotifyFromISR); it only proves              */
/* wifi_attach_datardy_callback() accepts a valid callback and enables     */
/* EXTI1 without error.                                                    */
/* ---------------------------------------------------------------------- */

static volatile uint32_t s_datardy_calls;

static void bringup_datardy_cb(void *ctx)
{
    (void) ctx;
    s_datardy_calls++;
}

/* ---------------------------------------------------------------------- */
/* WiFi task — Phase 2 (post-scheduler): attach callback, run TC-HW-WIFI-  */
/* 002..004, then heartbeat.                                               */
/* ---------------------------------------------------------------------- */

static StaticTask_t s_wifi_task_tcb;
static StackType_t s_wifi_task_stack[WIFI_TASK_STACK_WORDS];

static void wifi_bringup_task(void *arg)
{
    wifi_handle_t wifi_handle = (wifi_handle_t) arg;

    /* TC-HW-WIFI-002 */
    if (wifi_attach_datardy_callback(wifi_handle, bringup_datardy_cb, NULL) != WIFI_ERR_OK)
    {
        bringup_fail("TC-HW-WIFI-002  wifi_attach_datardy_callback() failed");
    }
    LOG_INFO("Wifi", "TC-HW-WIFI-002  wifi_attach_datardy_callback() returned WIFI_ERR_OK");

    /* TC-HW-WIFI-003 */
    wifi_link_state_t link_state;
    if ((wifi_get_link_state(wifi_handle, &link_state) != WIFI_ERR_OK) ||
        (link_state != WIFI_LINK_DOWN))
    {
        bringup_fail("TC-HW-WIFI-003  expected WIFI_LINK_DOWN before wifi_connect_ap()");
    }
    LOG_INFO("Wifi", "TC-HW-WIFI-003  link state is WIFI_LINK_DOWN pre-connect");

    /* TC-HW-WIFI-004 — only runs if credentials were filled in above. */
    if (sizeof(BRINGUP_WIFI_SSID) > 1U)
    {
        wifi_err_t connect_err =
            wifi_connect_ap(wifi_handle, BRINGUP_WIFI_SSID, BRINGUP_WIFI_PASSWORD);
        if (connect_err != WIFI_ERR_OK)
        {
            LOG_ERROR("Wifi", "wifi_connect_ap() error code: %d", (int) connect_err);
            bringup_fail("TC-HW-WIFI-004  wifi_connect_ap() failed");
        }

        int8_t rssi_dbm = 0;
        wifi_err_t rssi_err = wifi_get_rssi(wifi_handle, &rssi_dbm);
        if (rssi_err != WIFI_ERR_OK)
        {
            LOG_ERROR("Wifi", "wifi_get_rssi() error code: %d", (int) rssi_err);
            bringup_fail("TC-HW-WIFI-004  wifi_get_rssi() failed after association");
        }
        LOG_INFO("Wifi", "TC-HW-WIFI-004  associated, RSSI=%d dBm", (int) rssi_dbm);

        if (sizeof(BRINGUP_SOCKETTEST_HOST) > 1U)
        {
            /* TC-HW-WIFI-005 — TCP round trip against SocketTest. */
            wifi_socket_t tcp_sock = WIFI_INVALID_SOCKET;
            if (wifi_open_socket(wifi_handle, WIFI_SOCKET_TCP, BRINGUP_SOCKETTEST_HOST,
                                 BRINGUP_SOCKETTEST_TCP_PORT, &tcp_sock) != WIFI_ERR_OK)
            {
                bringup_fail("TC-HW-WIFI-005  wifi_open_socket(TCP) failed");
            }
            LOG_INFO("Wifi", "TC-HW-WIFI-005  TCP socket open (id=%u) - check SocketTest",
                     (unsigned) tcp_sock);

            static const uint8_t tcp_hello[] = "WIFI-BRINGUP-TCP-HELLO\r\n";
            if (wifi_send(wifi_handle, tcp_sock, tcp_hello, sizeof(tcp_hello) - 1U) != WIFI_ERR_OK)
            {
                bringup_fail("TC-HW-WIFI-005  wifi_send() failed");
            }
            LOG_INFO("Wifi", "TC-HW-WIFI-005  sent - confirm it appeared in SocketTest's window");

            uint8_t rx_buf[64];
            size_t rx_len = 0U;
            wifi_err_t recv_err =
                wifi_recv(wifi_handle, tcp_sock, rx_buf, sizeof(rx_buf) - 1U, &rx_len, 3000U);
            if (recv_err == WIFI_ERR_OK)
            {
                rx_buf[rx_len] = (uint8_t) '\0';
                LOG_INFO("Wifi", "TC-HW-WIFI-005  received %u bytes: %s", (unsigned) rx_len,
                         (const char *) rx_buf);
            }
            else
            {
                LOG_INFO("Wifi", "TC-HW-WIFI-005  no reply within timeout (type something in "
                                 "SocketTest and resend to exercise this path)");
            }

            (void) wifi_close_socket(wifi_handle, tcp_sock);
            LOG_INFO("Wifi", "TC-HW-WIFI-005  TCP socket closed");

            LOG_INFO("Wifi",
                     "Switch SocketTest to UDP listen mode on port %u now - "
                     "5 s pause...",
                     (unsigned) BRINGUP_SOCKETTEST_UDP_PORT);
            vTaskDelay(pdMS_TO_TICKS(5000));

            /* TC-HW-WIFI-006 — UDP round trip against SocketTest. */
            wifi_socket_t udp_sock = WIFI_INVALID_SOCKET;
            if (wifi_open_socket(wifi_handle, WIFI_SOCKET_UDP, BRINGUP_SOCKETTEST_HOST,
                                 BRINGUP_SOCKETTEST_UDP_PORT, &udp_sock) != WIFI_ERR_OK)
            {
                bringup_fail("TC-HW-WIFI-006  wifi_open_socket(UDP) failed");
            }
            LOG_INFO("Wifi", "TC-HW-WIFI-006  UDP socket open (id=%u) - check SocketTest",
                     (unsigned) udp_sock);

            static const uint8_t udp_hello[] = "WIFI-BRINGUP-UDP-HELLO\r\n";
            if (wifi_send(wifi_handle, udp_sock, udp_hello, sizeof(udp_hello) - 1U) != WIFI_ERR_OK)
            {
                bringup_fail("TC-HW-WIFI-006  wifi_send() failed");
            }
            LOG_INFO("Wifi", "TC-HW-WIFI-006  sent - confirm it appeared in SocketTest's window");

            recv_err =
                wifi_recv(wifi_handle, udp_sock, rx_buf, sizeof(rx_buf) - 1U, &rx_len, 3000U);
            if (recv_err == WIFI_ERR_OK)
            {
                rx_buf[rx_len] = (uint8_t) '\0';
                LOG_INFO("Wifi", "TC-HW-WIFI-006  received %u bytes: %s", (unsigned) rx_len,
                         (const char *) rx_buf);
            }
            else
            {
                LOG_INFO("Wifi", "TC-HW-WIFI-006  no reply within timeout (type something in "
                                 "SocketTest and resend to exercise this path)");
            }

            (void) wifi_close_socket(wifi_handle, udp_sock);
            LOG_INFO("Wifi", "TC-HW-WIFI-006  UDP socket closed");
        }
        else
        {
            LOG_INFO("Wifi", "BRINGUP_SOCKETTEST_HOST is empty - skipping TC-HW-WIFI-005/006");
        }
    }
    else
    {
        LOG_INFO("Wifi", "BRINGUP_WIFI_SSID is empty - skipping TC-HW-WIFI-004/005/006");
    }

    LOG_INFO("Wifi", "All automated tests complete.");

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
    /* 1. Clock tree -> 80 MHz, DWT, fault handlers, LSE for the RTC. */
    if (cpu_init() != STATUS_OK)
    {
        bringup_halt();
    }

    /* 2. Drivers Logger depends on. */
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

    /* 3. Logger — creates the queue and drain task statically; log calls
     *    before vTaskStartScheduler() take the synchronous-write path. */
    (void) logger_init(LOG_LEVEL_DEBUG);

    LOG_INFO("Wifi", "===== WifiDriver Hardware Bring-up =====");
    LOG_INFO("Wifi", "Board : B-L475E-IOT01A (STM32L475VGTx)");
    LOG_INFO("Wifi", "Module: ISM43362-M3G-L44 via SPI3");
    /* Diagnostic: wifi_create()'s internal timeouts (100 ms DRDY, 5000 ms
     * response) are cpu_delay_us() cycle counts derived from this value.
     * If it's not ~80000000, every "millisecond" timeout below actually
     * takes proportionally longer in real time. */
    LOG_INFO("Wifi", "SYSCLK=%lu Hz", (unsigned long) cpu_get_sysclk_hz());

    /* 4. WifiDriver Phase 1 (pre-scheduler): pins, SpiDriver, wifi_create(). */
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
            bringup_fail("TC-HW-WIFI-001  SPI3 pin configuration failed");
        }
    }

    gpio_pin_config_t nss_config = {.port = BRINGUP_NSS_PORT,
                                    .pin = BRINGUP_NSS_PIN,
                                    .mode = GPIO_MODE_OUTPUT,
                                    .otype = GPIO_OTYPE_PUSH_PULL,
                                    .speed = GPIO_SPEED_LOW,
                                    .pull = GPIO_PULL_NONE,
                                    .alternate = 0};
    /* Pull-down, not GPIO_PULL_NONE: an unpulled input floats to whatever
     * stray capacitance/leakage holds it at until the module actually
     * drives it, which the pre-reset diagnostic below showed reads as a
     * false HIGH. That fools every "wait DRDY high" into succeeding
     * instantly without a real signal, while every "wait DRDY low" then
     * times out for real against a pin nothing is driving low. */
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
        bringup_fail("TC-HW-WIFI-001  ISM43362 control-line configuration failed");
    }
    LOG_INFO("Wifi", "TC-HW-WIFI-001a  All GPIO pins configured (SPI3 + 5 control lines)");

    /* Diagnostic: DRDY idle level before any reset pulse. A floating/pulled
     * input reading HIGH here would make the first "wait DRDY high"
     * inside wifi_create() succeed trivially without the module actually
     * being ready, masking the real failure until a later, longer wait. */
    gpio_level_t drdy_idle_level;
    (void) gpio_read_pin(BRINGUP_DRDY_PORT, BRINGUP_DRDY_PIN, &drdy_idle_level);
    LOG_INFO("Wifi", "DRDY idle level (pre-reset) = %s",
             (drdy_idle_level == GPIO_LEVEL_HIGH) ? "HIGH" : "LOW");

    spi_config_t spi_config = {.instance = SPI3};
    spi_handle_t spi_handle = NULL;
    if (spi_create(&spi_config, &spi_handle) != SPI_ERR_OK)
    {
        bringup_fail("TC-HW-WIFI-001b  spi_create() failed");
    }
    LOG_INFO("Wifi", "TC-HW-WIFI-001b  spi_create() returned SPI_ERR_OK");

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
        LOG_ERROR("Wifi", "wifi_create() error code: %d", (int) wifi_err);
        bringup_fail("TC-HW-WIFI-001c  wifi_create() failed (reset sequence, AT "
                     "handshake, or firmware version check)");
    }
    LOG_INFO("Wifi", "TC-HW-WIFI-001c  wifi_create() completed reset + AT handshake + "
                     "firmware check");
    LOG_INFO("Wifi", "starting scheduler...");

    /* 5. Phase 2 (post-scheduler): attach callback, connect, heartbeat. */
    (void) xTaskCreateStatic(wifi_bringup_task, "wifi_bringup", WIFI_TASK_STACK_WORDS, wifi_handle,
                             WIFI_TASK_PRIORITY, s_wifi_task_stack, &s_wifi_task_tcb);

    /* 6. Start the scheduler. Does not return under normal operation. */
    vTaskStartScheduler();

    /* 7. Only reached if the scheduler fails to start. */
    for (;;)
    {
    }
}
