/**
 * @file main_test_modbus_uart_driver.c
 * @brief Hardware bring-up test for ModbusUartDriver (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. Add the current harness (cloud_publisher) to the `excluding=` list of
 *      the `integration-tests` source entry and remove modbus_uart_driver.
 *   2. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/modbus_uart_driver.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * WIRING.  No RS-485 transceiver and no second board are needed:
 *   - Jumper Arduino D1 (PA0, UART4_TX) to D0 (PA1, UART4_RX).  The frames the
 *     driver transmits are received by the same UART, which exercises TX, the
 *     IDLE-line RX path and the receive-error path on silicon.
 *   - Optional: scope PA15 (UART4_RTS_DE) to see the hardware DE window.
 *     The harness also samples PA15 in software (TC-HW-MBUART-005).
 *
 * Hardware outputs:
 *   USART1 TX  PB6        115 200 8N1  Human-readable test results
 *   UART4      PA0/PA1    9600 8N1     Device under test (looped back)
 *   UART4 DE   PA15 (AF8)               Driver-enable, asserted during TX
 *
 * Automated test sequence:
 *   TC-HW-MBUART-001  modbus_uart_init(): UE/TE, CR3.DEM, BRR = 8333.
 *   TC-HW-MBUART-002  modbus_uart_attach_rx(): RE, RXNEIE, IDLEIE; NVIC enabled.
 *   TC-HW-MBUART-003  8-byte frame loops back as one RX_DONE; bytes identical;
 *                      TX time within 20 % of 8 x 10 bit times.
 *   TC-HW-MBUART-004  Baud-rate accuracy: 100 bytes take 104.2 ms +/- 2 %.
 *   TC-HW-MBUART-005  PA15 (DE) is high while transmitting and low afterwards.
 *   TC-HW-MBUART-006  Maximum frame: 256 bytes loop back as a valid frame.
 *   TC-HW-MBUART-007  Three back-to-back frames each give their own RX_DONE.
 *   TC-HW-MBUART-008  Real overrun (UART IRQ masked while receiving) gives
 *                      RX_ERROR and no RX_DONE; the next frame is clean.
 *   TC-HW-MBUART-009  Invalid arguments are rejected without touching the bus.
 *   TC-HW-MBUART-010  A second transmit() from inside the TXE wait returns BUSY.
 *
 * Integration checklist:
 *   [ ] Jumper D1-D0 fitted; every loopback frame read back byte-identical
 *   [ ] TX duration matches 9600 baud (TC-HW-MBUART-004)
 *   [ ] DE (PA15) high only while transmitting
 *   [ ] If a transceiver is later fitted: does it echo TX back into RX when
 *       /RE is not tied to DE?  (companion MBUART-O5)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cpu/cpu.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "modbus_uart_driver/modbus_uart_driver.h"
#include "cpu/status.h"
#include "stm32l475xx.h"

/* ---------------------------------------------------------------------- */
/* Board constants                                                         */
/* ---------------------------------------------------------------------- */

#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)
#define BRINGUP_TX_TIMEOUT_MS (100U)

#define DE_PIN (15U)
#define EXPECTED_BRR (8333UL)
#define MS_PER_S (1000UL)
#define US_PER_MS (1000UL)
#define BIT_TIMES_PER_BYTE (10UL)
#define RX_WAIT_MS (500U)
#define IDLE_SETTLE_MS (5U)
#define BAUD_TEST_BYTES (100U)
#define BAUD_TEST_EXPECTED_US (104167UL) /* 100 x 10 bits at 9600 baud */
#define BAUD_TOLERANCE_PERCENT (2UL)
#define FRAME_TIME_TOLERANCE_PERCENT (20UL)
#define PERCENT (100UL)
#define OVERRUN_BYTES (3U)
#define BACK_TO_BACK_FRAMES (3U)
#define SMALL_FRAME_LEN (8U)

/* ---------------------------------------------------------------------- */
/* State shared with the RX callback (ISR context)                         */
/* ---------------------------------------------------------------------- */

static volatile uint32_t s_done_count;
static volatile uint32_t s_error_count;

static uint8_t s_tx[MODBUS_UART_BUF_SIZE];
static uint8_t s_rx[MODBUS_UART_BUF_SIZE];

static void on_rx_event(modbus_uart_event_t event, void *context)
{
    (void) context;
    if (event == MODBUS_UART_EVENT_RX_DONE)
    {
        s_done_count++;
    }
    else
    {
        s_error_count++;
    }
}

/* ---------------------------------------------------------------------- */
/* Reporting helpers — built on DebugUartDriver.                           */
/* ---------------------------------------------------------------------- */

static void bringup_puts(const char *s)
{
    (void) debug_uart_send((const uint8_t *) s, strlen(s), BRINGUP_TX_TIMEOUT_MS);
}

static void bringup_pass(const char *label)
{
    bringup_puts("[PASS] ");
    bringup_puts(label);
    bringup_puts("\r\n");
}

static void bringup_fail(const char *label)
{
    bringup_puts("[FAIL] ");
    bringup_puts(label);
    bringup_puts(" - HALTED\r\n");
    for (;;)
    {
    }
}

static void check(bool condition, const char *label)
{
    if (!condition)
    {
        bringup_fail(label);
    }
}

static void put_u32(const char *label, uint32_t value, const char *unit)
{
    char digits[11];
    size_t n = 0U;

    do
    {
        digits[n] = (char) ('0' + (value % 10U));
        value /= 10U;
        n++;
    } while (value > 0U);

    bringup_puts("[INFO] ");
    bringup_puts(label);
    bringup_puts(" = ");
    while (n > 0U)
    {
        n--;
        (void) debug_uart_send((const uint8_t *) &digits[n], 1U, BRINGUP_TX_TIMEOUT_MS);
    }
    bringup_puts(unit);
    bringup_puts("\r\n");
}

/* ---------------------------------------------------------------------- */
/* DWT-based time — cpu_init() enables the cycle counter.                  */
/* ---------------------------------------------------------------------- */

static uint32_t s_last_cycles;
static uint32_t s_cycle_remainder;
static uint32_t s_elapsed_ms;

/** Monotonic ms without the 53 s wrap of CYCCNT / cycles_per_ms. */
static uint32_t now_ms(void)
{
    const uint32_t cycles_per_ms = cpu_get_sysclk_hz() / MS_PER_S;
    const uint32_t cycles = DWT->CYCCNT;

    s_cycle_remainder += cycles - s_last_cycles;
    s_last_cycles = cycles;
    s_elapsed_ms += s_cycle_remainder / cycles_per_ms;
    s_cycle_remainder %= cycles_per_ms;
    return s_elapsed_ms;
}

static uint32_t dwt_now(void)
{
    return DWT->CYCCNT;
}

static uint32_t dwt_elapsed_us(uint32_t start)
{
    return (DWT->CYCCNT - start) / (cpu_get_sysclk_hz() / (MS_PER_S * US_PER_MS));
}

/** Wait until @p count reaches @p target; false on timeout. */
static bool wait_for(const volatile uint32_t *count, uint32_t target, uint32_t timeout_ms)
{
    const uint32_t start = now_ms();

    while (*count < target)
    {
        if ((now_ms() - start) >= timeout_ms)
        {
            return false;
        }
    }
    return true;
}

static void fill_pattern(uint8_t *buf, uint16_t len, uint8_t seed)
{
    for (uint16_t i = 0U; i < len; i++)
    {
        buf[i] = (uint8_t) (seed + i);
    }
}

/** Transmit @p len bytes and require one clean loopback frame back. */
static void loop_frame(uint16_t len, uint8_t seed, const char *label)
{
    const uint32_t done_before = s_done_count;
    uint16_t rx_len = 0U;

    fill_pattern(s_tx, len, seed);
    memset(s_rx, 0, sizeof(s_rx));

    check(modbus_uart_transmit(s_tx, len) == MODBUS_UART_OK, label);
    check(wait_for(&s_done_count, done_before + 1U, RX_WAIT_MS),
          "no RX_DONE - is the D1-D0 loopback jumper fitted?");
    check(modbus_uart_get_rx_frame(s_rx, &rx_len) == MODBUS_UART_OK, label);
    check(rx_len == len, "loopback length mismatch");
    check(memcmp(s_tx, s_rx, len) == 0, "loopback bytes differ");
}

/* ---------------------------------------------------------------------- */
/* DE sampling from inside the TXE/TC wait loops                           */
/* ---------------------------------------------------------------------- */

static volatile bool s_de_seen_high;

static uint32_t sampling_tick(void)
{
    if ((GPIOA->IDR & (1UL << DE_PIN)) != 0U)
    {
        s_de_seen_high = true;
    }
    return now_ms();
}

/* ---------------------------------------------------------------------- */
/* Re-entrancy probe for the BUSY guard                                    */
/* ---------------------------------------------------------------------- */

static volatile bool s_reentered;
static volatile modbus_uart_err_t s_reentrant_result;

static uint32_t reentrant_tick(void)
{
    if (!s_reentered)
    {
        s_reentered = true;
        s_reentrant_result = modbus_uart_transmit(s_tx, 1U);
    }
    return now_ms();
}

/* ---------------------------------------------------------------------- */
/* main()                                                                  */
/* ---------------------------------------------------------------------- */

int main(void)
{
    /* cpu_init() first - clock to 80 MHz, DWT cycle counter, fault handlers. */
    if (cpu_init() != STATUS_OK)
    {
        for (;;)
        {
            /* Nothing can be reported before the UART exists. */
        }
    }
    s_last_cycles = DWT->CYCCNT;

    const gpio_pin_config_t uart_tx_config = {
        .port = BRINGUP_UART_TX_PORT,
        .pin = BRINGUP_UART_TX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_NONE,
        .alternate = BRINGUP_UART_AF,
    };
    const gpio_pin_config_t uart_rx_config = {
        .port = BRINGUP_UART_RX_PORT,
        .pin = BRINGUP_UART_RX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_UP,
        .alternate = BRINGUP_UART_AF,
    };

    if ((gpio_init() != GPIO_OK) || (gpio_configure_pin(&uart_tx_config) != GPIO_OK) ||
        (gpio_configure_pin(&uart_rx_config) != GPIO_OK) || (debug_uart_init() != DEBUG_UART_OK))
    {
        for (;;)
        {
            /* No sink to report to. */
        }
    }

    bringup_puts("\r\n===== ModbusUartDriver Hardware Bring-up =====\r\n");
    bringup_puts("Board  : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("UART4  : PA0 TX / PA1 RX, RTS_DE PA15, 9600 8N1\r\n");
    bringup_puts("WIRING : jumper Arduino D1 (PA0) to D0 (PA1) for loopback\r\n");
    bringup_puts("==============================================\r\n\r\n");

    modbus_uart_set_tick_source(now_ms);

    /* TC-HW-MBUART-001 */
    check(modbus_uart_init() == MODBUS_UART_OK, "TC-HW-MBUART-001 modbus_uart_init");
    check((UART4->CR1 & (USART_CR1_UE | USART_CR1_TE)) == (USART_CR1_UE | USART_CR1_TE),
          "TC-HW-MBUART-001 UE/TE not set");
    check((UART4->CR3 & USART_CR3_DEM) != 0U, "TC-HW-MBUART-001 CR3.DEM not set");
    check(UART4->BRR == EXPECTED_BRR, "TC-HW-MBUART-001 BRR != 8333");
    bringup_pass("TC-HW-MBUART-001 init: UE/TE, DEM, BRR = 8333");

    /* TC-HW-MBUART-002 */
    modbus_uart_attach_rx(on_rx_event, NULL);
    check((UART4->CR1 & (USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_IDLEIE)) ==
              (USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_IDLEIE),
          "TC-HW-MBUART-002 RE/RXNEIE/IDLEIE not set");
    check((NVIC->ISER[UART4_IRQn / 32U] & (1UL << (UART4_IRQn % 32U))) != 0U,
          "TC-HW-MBUART-002 UART4 IRQ not enabled in NVIC");
    bringup_pass("TC-HW-MBUART-002 attach_rx: RE, RXNEIE, IDLEIE, NVIC");

    /* TC-HW-MBUART-003 */
    {
        const uint32_t start = dwt_now();
        const uint32_t done_before = s_done_count;
        uint16_t rx_len = 0U;

        fill_pattern(s_tx, SMALL_FRAME_LEN, 0x10U);
        check(modbus_uart_transmit(s_tx, SMALL_FRAME_LEN) == MODBUS_UART_OK,
              "TC-HW-MBUART-003 transmit");
        const uint32_t tx_us = dwt_elapsed_us(start);
        const uint32_t expect_us =
            (SMALL_FRAME_LEN * BIT_TIMES_PER_BYTE * MS_PER_S * US_PER_MS) / MODBUS_UART_BAUD;

        check(wait_for(&s_done_count, done_before + 1U, RX_WAIT_MS),
              "TC-HW-MBUART-003 no RX_DONE - is the D1-D0 loopback jumper fitted?");
        check(modbus_uart_get_rx_frame(s_rx, &rx_len) == MODBUS_UART_OK,
              "TC-HW-MBUART-003 get_rx_frame");
        check((rx_len == SMALL_FRAME_LEN) && (memcmp(s_tx, s_rx, SMALL_FRAME_LEN) == 0),
              "TC-HW-MBUART-003 loopback frame differs");
        check((tx_us * PERCENT) < (expect_us * (PERCENT + FRAME_TIME_TOLERANCE_PERCENT)),
              "TC-HW-MBUART-003 TX took too long");
        put_u32("8-byte TX time", tx_us, " us");
        bringup_pass("TC-HW-MBUART-003 8-byte frame loops back as one RX_DONE");
    }

    /* TC-HW-MBUART-004 */
    {
        const uint32_t done_before = s_done_count;
        uint16_t rx_len = 0U;
        uint32_t start;
        uint32_t tx_us;

        fill_pattern(s_tx, BAUD_TEST_BYTES, 0x20U);
        start = dwt_now();
        check(modbus_uart_transmit(s_tx, BAUD_TEST_BYTES) == MODBUS_UART_OK,
              "TC-HW-MBUART-004 transmit");
        tx_us = dwt_elapsed_us(start);

        check(wait_for(&s_done_count, done_before + 1U, RX_WAIT_MS), "TC-HW-MBUART-004 no RX_DONE");
        check(modbus_uart_get_rx_frame(s_rx, &rx_len) == MODBUS_UART_OK,
              "TC-HW-MBUART-004 get_rx_frame");

        put_u32("100-byte TX time", tx_us, " us (ideal 104167)");
        check((tx_us * PERCENT) > (BAUD_TEST_EXPECTED_US * (PERCENT - BAUD_TOLERANCE_PERCENT)),
              "TC-HW-MBUART-004 baud rate too fast");
        check((tx_us * PERCENT) < (BAUD_TEST_EXPECTED_US * (PERCENT + BAUD_TOLERANCE_PERCENT)),
              "TC-HW-MBUART-004 baud rate too slow");
        bringup_pass("TC-HW-MBUART-004 baud rate within 2 % of 9600");
    }

    /* TC-HW-MBUART-005 */
    {
        const uint32_t done_before = s_done_count;
        uint16_t rx_len = 0U;

        s_de_seen_high = false;
        modbus_uart_set_tick_source(sampling_tick);
        fill_pattern(s_tx, SMALL_FRAME_LEN, 0x30U);
        check(modbus_uart_transmit(s_tx, SMALL_FRAME_LEN) == MODBUS_UART_OK,
              "TC-HW-MBUART-005 transmit");
        modbus_uart_set_tick_source(now_ms);

        check(s_de_seen_high, "TC-HW-MBUART-005 DE (PA15) never seen high during TX");
        check((GPIOA->IDR & (1UL << DE_PIN)) == 0U,
              "TC-HW-MBUART-005 DE (PA15) still high after TC");

        (void) wait_for(&s_done_count, done_before + 1U, RX_WAIT_MS);
        (void) modbus_uart_get_rx_frame(s_rx, &rx_len);
        bringup_pass("TC-HW-MBUART-005 DE high during TX, low after TC");
    }

    /* TC-HW-MBUART-006 */
    loop_frame(MODBUS_UART_BUF_SIZE, 0x40U, "TC-HW-MBUART-006 transmit 256");
    bringup_pass("TC-HW-MBUART-006 256-byte frame is accepted as a valid frame");

    /* TC-HW-MBUART-007 */
    for (uint8_t i = 0U; i < BACK_TO_BACK_FRAMES; i++)
    {
        loop_frame(SMALL_FRAME_LEN + i, (uint8_t) (0x50U + i), "TC-HW-MBUART-007 frame");
    }
    bringup_pass("TC-HW-MBUART-007 three back-to-back frames, three RX_DONE");

    /* TC-HW-MBUART-008 */
    {
        const uint32_t done_before = s_done_count;
        const uint32_t error_before = s_error_count;

        NVIC_DisableIRQ(UART4_IRQn);
        fill_pattern(s_tx, OVERRUN_BYTES, 0x60U);
        check(modbus_uart_transmit(s_tx, OVERRUN_BYTES) == MODBUS_UART_OK,
              "TC-HW-MBUART-008 transmit");
        cpu_delay_ms(IDLE_SETTLE_MS);
        check((UART4->ISR & USART_ISR_ORE) != 0U,
              "TC-HW-MBUART-008 no overrun produced - loopback jumper fitted?");
        NVIC_EnableIRQ(UART4_IRQn);

        check(wait_for(&s_error_count, error_before + 1U, RX_WAIT_MS),
              "TC-HW-MBUART-008 no RX_ERROR after overrun");
        cpu_delay_ms(IDLE_SETTLE_MS);
        check(s_done_count == done_before, "TC-HW-MBUART-008 RX_DONE reported for a damaged frame");
        check((UART4->ISR & USART_ISR_ORE) == 0U, "TC-HW-MBUART-008 ORE not cleared");

        loop_frame(SMALL_FRAME_LEN, 0x70U, "TC-HW-MBUART-008 recovery frame");
        bringup_pass("TC-HW-MBUART-008 overrun -> RX_ERROR, then a clean frame");
    }

    /* TC-HW-MBUART-009 */
    {
        uint16_t rx_len = 0U;

        check(modbus_uart_transmit(NULL, 1U) == MODBUS_UART_ERR_INVALID_ARG,
              "TC-HW-MBUART-009 NULL frame");
        check(modbus_uart_transmit(s_tx, 0U) == MODBUS_UART_ERR_INVALID_ARG,
              "TC-HW-MBUART-009 zero length");
        check(modbus_uart_transmit(s_tx, MODBUS_UART_BUF_SIZE + 1U) == MODBUS_UART_ERR_INVALID_ARG,
              "TC-HW-MBUART-009 oversize length");
        check(modbus_uart_get_rx_frame(NULL, &rx_len) == MODBUS_UART_ERR_INVALID_ARG,
              "TC-HW-MBUART-009 NULL buffer");
        check(modbus_uart_get_rx_frame(s_rx, NULL) == MODBUS_UART_ERR_INVALID_ARG,
              "TC-HW-MBUART-009 NULL length");
        bringup_pass("TC-HW-MBUART-009 invalid arguments rejected");
    }

    /* TC-HW-MBUART-010 */
    {
        uint16_t rx_len = 0U;

        s_reentered = false;
        s_reentrant_result = MODBUS_UART_OK;
        modbus_uart_set_tick_source(reentrant_tick);
        fill_pattern(s_tx, 1U, 0x80U);
        check(modbus_uart_transmit(s_tx, 1U) == MODBUS_UART_OK, "TC-HW-MBUART-010 outer transmit");
        modbus_uart_set_tick_source(now_ms);

        check(s_reentered, "TC-HW-MBUART-010 tick source was never called");
        check(s_reentrant_result == MODBUS_UART_ERR_BUSY, "TC-HW-MBUART-010 inner call not BUSY");
        cpu_delay_ms(IDLE_SETTLE_MS);
        (void) modbus_uart_get_rx_frame(s_rx, &rx_len);
        bringup_pass("TC-HW-MBUART-010 re-entrant transmit returns BUSY");
    }

    bringup_puts("\r\n===== ALL TESTS PASSED =====\r\n");
    for (;;)
    {
    }
}
