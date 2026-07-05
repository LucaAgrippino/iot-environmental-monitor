/**
 * @file main_test_spi_driver.c
 * @brief Hardware bring-up test for SpiDriver (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate:
 *   1. Copy this file into Src/ (or add integration-tests/spi_driver/ as
 *      a source path in CubeIDE).
 *   2. In CubeIDE, right-click Src/main.c → Properties →
 *      C/C++ Build → check "Exclude resource from build".
 *   3. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   4. Flash and run with a debugger attached (J-Link or ST-LINK).
 *
 * Hardware outputs:
 *   USART1 TX  PB6  115 200 8N1  Human-readable test results
 *   LD2 green  PA5               Heartbeat and pass/fail markers
 *   SPI3 SCK/MISO/MOSI  PC10/PC11/PC12  AF6, 10 MHz, mode 0, 16-bit frames
 *   NSS (ISM43362) PE0  configured OUTPUT push-pull via GpioDriver, held
 *                       de-asserted (HIGH) for this bring-up — no live
 *                       ISM43362 module response is expected/required.
 *
 * Automated test sequence:
 *   TC-HW-SPI-001  gpio_init() + spi_create() both return their OK codes
 *   TC-HW-SPI-002  spi_transceive() with NSS de-asserted: 4 words
 *                   exchanged, function returns SPI_ERR_OK (no ISM43362
 *                   response expected; this only proves the peripheral
 *                   clocks out words and returns without hanging)
 *   TC-HW-SPI-003  Validation cascade: NULL handle, both-buffers-NULL
 *
 * Manual step (requires a logic analyser or the ISM43362 module wired):
 *   Probe PC10 (SCK) — confirm 10 MHz clock bursts during TC-HW-SPI-002,
 *   mode 0 (idle low, sample on rising edge). Probe PC12 (MOSI) to
 *   confirm the transmitted words appear MSB-first.
 */

#include <stdbool.h>
#include <stdint.h>

#include "cpu/cpu.h"
#include "gpio/gpio_driver.h"
#include "spi_driver.h"
#include "status.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Board constants                                                         */
/* ---------------------------------------------------------------------- */

/** LD2 (green user LED) on B-L475E-IOT01A — active high. */
#define BRINGUP_LED_PORT GPIO_PORT_A
#define BRINGUP_LED_PIN (5U)

/** USART1 TX on PB6, alternate function AF7 — used only to report results. */
#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_TX_AF (7U)

/** SPI3 pins per companion §4.3 — UM2153 Table 11, pins 78-80. */
#define BRINGUP_SPI_SCK_PORT GPIO_PORT_C
#define BRINGUP_SPI_SCK_PIN (10U)
#define BRINGUP_SPI_MISO_PORT GPIO_PORT_C
#define BRINGUP_SPI_MISO_PIN (11U)
#define BRINGUP_SPI_MOSI_PORT GPIO_PORT_C
#define BRINGUP_SPI_MOSI_PIN (12U)
#define BRINGUP_SPI_AF (6U)

/** ISM43362 NSS — PE0, held de-asserted (HIGH) throughout this bring-up. */
#define BRINGUP_NSS_PORT GPIO_PORT_E
#define BRINGUP_NSS_PIN (0U)

/** USART1 BRR = PCLK2 / baud = 80 000 000 / 115 200 ~= 694 at 80 MHz. */
#define BRINGUP_UART_BRR (694U)

/** Number of 16-bit words exchanged in TC-HW-SPI-002. */
#define BRINGUP_TRANSFER_WORDS (4U)

/* ---------------------------------------------------------------------- */
/* UART helpers (USART1, PB6, 115 200 8N1) — reporting only.               */
/* The TX pin itself is configured through gpio_configure_pin().           */
/* ---------------------------------------------------------------------- */

static void bringup_uart_peripheral_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    USART1->CR1 = 0U;
    USART1->BRR = BRINGUP_UART_BRR;
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void bringup_putc(char c)
{
    while (!(USART1->ISR & USART_ISR_TXE))
    {
        /* spin */
    }
    USART1->TDR = (uint32_t) (uint8_t) c;
}

static void bringup_puts(const char *s)
{
    while (*s != '\0')
    {
        bringup_putc(*s);
        ++s;
    }
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

/**
 * @brief Emit a FAIL banner and enter a fast-blink halt loop.
 *
 * Drives LD2 directly via BSRR, not gpio_write_pin() — by the time a
 * failure can be reported, GpioDriver may itself be implicated, so the
 * halt indicator must not depend on it.
 */
static void bringup_fail(const char *label)
{
    bringup_puts("[FAIL] ");
    bringup_puts(label);
    bringup_puts(" — HALTED\r\n");
    for (;;)
    {
        GPIOA->BSRR = (1UL << BRINGUP_LED_PIN);
        bringup_raw_spin(1000000U);
        GPIOA->BSRR = (1UL << (BRINGUP_LED_PIN + 16U));
        bringup_raw_spin(1000000U);
    }
}

/* ---------------------------------------------------------------------- */
/* main()                                                                  */
/* ---------------------------------------------------------------------- */

int main(void)
{
    /* cpu_init() first — clock to 80 MHz, DWT, fault handlers. */
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

    /* gpio_init() next — required before any gpio_configure_pin() call. */
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

    /* Bring up the UART TX pin through the driver, then the peripheral. */
    gpio_pin_config_t uart_tx_config = {
        .port = BRINGUP_UART_TX_PORT,
        .pin = BRINGUP_UART_TX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_NONE,
        .alternate = BRINGUP_UART_TX_AF,
    };
    (void) gpio_configure_pin(&uart_tx_config);
    bringup_uart_peripheral_init();

    bringup_puts("\r\n======= SpiDriver Hardware Bring-up =======\r\n");
    bringup_puts("Board : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("UART  : USART1 PB6 TX   115 200 8N1\r\n");
    bringup_puts("SPI   : SPI3  SCK=PC10 MISO=PC11 MOSI=PC12  AF6  10 MHz\r\n");
    bringup_puts("============================================\r\n\r\n");

    /* TC-HW-SPI-001a — gpio_init() already succeeded to get this far. */
    bringup_pass("TC-HW-SPI-001a  gpio_init() returned GPIO_OK");

    /* Configure SPI3 pins (SCK/MISO/MOSI, AF6) and NSS (PE0, output). */
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
            bringup_fail("TC-HW-SPI-001b  SPI3 pin configuration failed");
        }
    }

    gpio_pin_config_t nss_config = {
        .port = BRINGUP_NSS_PORT,
        .pin = BRINGUP_NSS_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 0,
    };
    if (gpio_configure_pin(&nss_config) != GPIO_OK)
    {
        bringup_fail("TC-HW-SPI-001b  NSS pin configuration failed");
    }
    /* NSS de-asserted (HIGH) — no live ISM43362 exchange in this bring-up. */
    (void) gpio_write_pin(BRINGUP_NSS_PORT, BRINGUP_NSS_PIN, GPIO_LEVEL_HIGH);
    bringup_pass("TC-HW-SPI-001b  SPI3 pins (SCK/MISO/MOSI) and NSS configured");

    /* TC-HW-SPI-001c — spi_create() */
    spi_config_t spi_config = {.instance = SPI3};
    spi_handle_t spi_handle = NULL;
    if (spi_create(&spi_config, &spi_handle) != SPI_ERR_OK)
    {
        bringup_fail("TC-HW-SPI-001c  spi_create() failed");
    }
    bringup_pass("TC-HW-SPI-001c  spi_create() returned SPI_ERR_OK");

    /* TC-HW-SPI-002 — spi_transceive() with NSS de-asserted. */
    const uint16_t tx_words[BRINGUP_TRANSFER_WORDS] = {0x0102u, 0x0304u, 0x0506u, 0x0708u};
    uint16_t rx_words[BRINGUP_TRANSFER_WORDS] = {0};
    if (spi_transceive(spi_handle, tx_words, rx_words, BRINGUP_TRANSFER_WORDS) != SPI_ERR_OK)
    {
        bringup_fail("TC-HW-SPI-002  spi_transceive() returned non-OK "
                     "(check SCK/MISO/MOSI wiring and pin AF)");
    }
    bringup_pass("TC-HW-SPI-002  spi_transceive() exchanged 4 words without timing out");

    /* TC-HW-SPI-003 — validation cascade. */
    if (spi_transceive(NULL, tx_words, rx_words, 1U) != SPI_ERR_NULL_PTR)
    {
        bringup_fail("TC-HW-SPI-003  spi_transceive(NULL handle) did not return SPI_ERR_NULL_PTR");
    }
    if (spi_transceive(spi_handle, NULL, NULL, 1U) != SPI_ERR_NULL_PTR)
    {
        bringup_fail(
            "TC-HW-SPI-003  spi_transceive(both buffers NULL) did not return SPI_ERR_NULL_PTR");
    }
    bringup_pass("TC-HW-SPI-003  validation cascade returned documented error codes");

    /* ------------------------------------------------------------------ */
    bringup_puts("\r\n[PASS] All automated tests complete.\r\n");
    bringup_puts("[INFO] Probe PC10 (SCK) for a 10 MHz burst on the next heartbeat "
                 "transceive to visually confirm mode 0 timing.\r\n");
    bringup_puts("[INFO] Heartbeat: LD2 PA5 toggles every 500 ms; a 4-word SPI3 "
                 "transceive runs on each toggle.\r\n\r\n");

    for (;;)
    {
        gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        (void) spi_transceive(spi_handle, tx_words, rx_words, BRINGUP_TRANSFER_WORDS);
        cpu_delay_ms(500U);
    }
}
