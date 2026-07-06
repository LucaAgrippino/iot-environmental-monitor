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
 *
 * Hardware outputs:
 *   USART1 TX  PB6   115 200 8N1  Human-readable test results
 *   LD2 green  PA5                Heartbeat and pass/fail markers
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
 *                   configured AP, read RSSI, open+close a TCP socket
 *
 * Manual step (requires the DATARDY line and a logic analyser, or reliance
 * on the on-board module's real responses): probe PE1 during TC-HW-WIFI-001
 * to confirm the module asserts DATARDY in response to the AT handshake.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "exti/exti_driver.h"
#include "gpio/gpio_driver.h"
#include "spi/spi.h"
#include "wifi_driver/wifi_driver.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Fill these in before flashing to exercise the AP-connect + socket path. */
/* Leave BRINGUP_WIFI_SSID empty to skip TC-HW-WIFI-004.                    */
/* ---------------------------------------------------------------------- */
#define BRINGUP_WIFI_SSID ""
#define BRINGUP_WIFI_PASSWORD ""

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

#define BRINGUP_TX_TIMEOUT_MS (100U)

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

/* ---------------------------------------------------------------------- */
/* Reporting helpers — built on DebugUartDriver (USART1, PB6, 115 200 8N1). */
/* ---------------------------------------------------------------------- */

static void bringup_puts(const char *s)
{
    size_t len = 0U;
    while (s[len] != '\0')
    {
        ++len;
    }
    (void) debug_uart_send((const uint8_t *) s, len, BRINGUP_TX_TIMEOUT_MS);
}

static void bringup_pass(const char *label)
{
    bringup_puts("[PASS] ");
    bringup_puts(label);
    bringup_puts("\r\n");
}

static void bringup_raw_spin(uint32_t n)
{
    volatile uint32_t c = n;
    while (c > 0U)
    {
        --c;
    }
}

static void bringup_fail(const char *label)
{
    bringup_puts("[FAIL] ");
    bringup_puts(label);
    bringup_puts(" - HALTED\r\n");
    for (;;)
    {
        GPIOA->BSRR = (1UL << BRINGUP_LED_PIN);
        bringup_raw_spin(1000000U);
        GPIOA->BSRR = (1UL << (BRINGUP_LED_PIN + 16U));
        bringup_raw_spin(1000000U);
    }
}

/* ---------------------------------------------------------------------- */
/* DATARDY callback — pre-scheduler bring-up, so this is never actually   */
/* invoked from an ISR here (EXTI1 is enabled but nothing services it     */
/* without the scheduler + stm32l4xx_it.c wiring from WifiTask); it only  */
/* proves wifi_attach_datardy_callback() accepts a valid callback.        */
/* ---------------------------------------------------------------------- */

static volatile uint32_t s_datardy_calls;

static void bringup_datardy_cb(void *ctx)
{
    (void) ctx;
    s_datardy_calls++;
}

/* ---------------------------------------------------------------------- */
/* main()                                                                  */
/* ---------------------------------------------------------------------- */

int main(void)
{
    status_t init_st = cpu_init();
    if (init_st != STATUS_OK)
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

    if (gpio_init() != GPIO_OK)
    {
        for (;;)
        {
            GPIOA->BSRR = (1UL << BRINGUP_LED_PIN);
            bringup_raw_spin(100000U);
            GPIOA->BSRR = (1UL << (BRINGUP_LED_PIN + 16U));
            bringup_raw_spin(100000U);
        }
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
    (void) debug_uart_init();

    bringup_puts("\r\n======= WifiDriver Hardware Bring-up =======\r\n");
    bringup_puts("Board : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("Module: ISM43362-M3G-L44 via SPI3\r\n");
    bringup_puts("=============================================\r\n\r\n");

    /* SPI3 pins (SCK/MISO/MOSI, AF6). */
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

    /* ISM43362 control lines. */
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
                                     .pull = GPIO_PULL_NONE,
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
    bringup_pass("TC-HW-WIFI-001a  All GPIO pins configured (SPI3 + 5 control lines)");

    spi_config_t spi_config = {.instance = SPI3};
    spi_handle_t spi_handle = NULL;
    if (spi_create(&spi_config, &spi_handle) != SPI_ERR_OK)
    {
        bringup_fail("TC-HW-WIFI-001b  spi_create() failed");
    }
    bringup_pass("TC-HW-WIFI-001b  spi_create() returned SPI_ERR_OK");

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
        char err_msg[48];
        (void) snprintf(err_msg, sizeof(err_msg), "[INFO] wifi_create() error code: %d\r\n",
                        (int) wifi_err);
        bringup_puts(err_msg);
        bringup_fail("TC-HW-WIFI-001c  wifi_create() failed (reset sequence, AT "
                     "handshake, or firmware version check)");
    }
    bringup_pass("TC-HW-WIFI-001c  wifi_create() completed reset + AT handshake + "
                 "firmware check");

    /* TC-HW-WIFI-002 */
    if (wifi_attach_datardy_callback(wifi_handle, bringup_datardy_cb, NULL) != WIFI_ERR_OK)
    {
        bringup_fail("TC-HW-WIFI-002  wifi_attach_datardy_callback() failed");
    }
    bringup_pass("TC-HW-WIFI-002  wifi_attach_datardy_callback() returned WIFI_ERR_OK");

    /* TC-HW-WIFI-003 */
    wifi_link_state_t link_state;
    if ((wifi_get_link_state(wifi_handle, &link_state) != WIFI_ERR_OK) ||
        (link_state != WIFI_LINK_DOWN))
    {
        bringup_fail("TC-HW-WIFI-003  expected WIFI_LINK_DOWN before wifi_connect_ap()");
    }
    bringup_pass("TC-HW-WIFI-003  wifi_get_link_state() reports WIFI_LINK_DOWN pre-connect");

    /* TC-HW-WIFI-004 — only runs if credentials were filled in above. */
    if (sizeof(BRINGUP_WIFI_SSID) > 1U)
    {
        if (wifi_connect_ap(wifi_handle, BRINGUP_WIFI_SSID, BRINGUP_WIFI_PASSWORD) != WIFI_ERR_OK)
        {
            bringup_fail("TC-HW-WIFI-004  wifi_connect_ap() failed");
        }

        int8_t rssi_dbm = 0;
        if (wifi_get_rssi(wifi_handle, &rssi_dbm) != WIFI_ERR_OK)
        {
            bringup_fail("TC-HW-WIFI-004  wifi_get_rssi() failed after association");
        }

        wifi_socket_t sock = WIFI_INVALID_SOCKET;
        if (wifi_open_socket(wifi_handle, WIFI_SOCKET_TCP, "8.8.8.8", 53U, &sock) != WIFI_ERR_OK)
        {
            bringup_fail("TC-HW-WIFI-004  wifi_open_socket() failed");
        }
        (void) wifi_close_socket(wifi_handle, sock);
        bringup_pass("TC-HW-WIFI-004  connected, read RSSI, opened+closed a TCP socket");
    }
    else
    {
        bringup_puts("[INFO] BRINGUP_WIFI_SSID is empty — skipping TC-HW-WIFI-004 "
                     "(connect/RSSI/socket path).\r\n");
    }

    bringup_puts("\r\n[PASS] All automated tests complete.\r\n");
    bringup_puts("[INFO] Heartbeat: LD2 PA5 toggles every 500 ms.\r\n\r\n");

    for (;;)
    {
        gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        cpu_delay_ms(500U);
    }
}
