/**
 * @file main_test_reset_driver.c
 * @brief Hardware bring-up test for ResetDriver (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. Add cloud_publisher (the current harness) to the `excluding=` list of the
 *      `integration-tests` source entry and remove reset_driver.
 *   2. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/reset_driver.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * No scheduler and no other module: gpio, debug UART and the RTC (for a backup register that
 * survives the reset). The test is one reset and two boots:
 *
 *   boot 1  BKP31R holds no marker. Write the marker, print it, call reset_trigger().
 *   boot 2  RCC->CSR reports a software reset AND BKP31R still holds the marker: the reset was
 *           ours, not a power cycle or the flash tool's.
 *
 * Automated test sequence:
 *   TC-HW-RST-001  reset_trigger() resets the MCU: the next banner appears.
 *   TC-HW-RST-002  The next boot reports SFTRSTF (L4 RCC->CSR bit 28) and not PINRSTF-only.
 *   TC-HW-RST-003  The backup-domain marker written before the reset is intact (the reset
 *                  spared the backup domain, as the companion section 4 says).
 *   TC-HW-RST-004  RAM state is not carried over: a .bss marker written before the reset reads zero
 *                 (the startup code re-zeroes .bss; the reset itself does not clear SRAM).
 *
 * Integration checklist:
 *   [ ] Both boots print their banner; the second ends with "ALL TESTS PASSED"
 *   [ ] ST-Link stayed attached across the reset (the debug interface survives, companion section
 * 4)
 *
 * BKP31R is used only by this bench test; index 0 belongs to TimeProvider (LLD-D16).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "reset_driver/reset_driver.h"
#include "rtc/rtc.h"

#include "stm32l475xx.h"

#define BKP_BENCH_IDX (31U)
#define BKP_BENCH_MAGIC (0x5E5E1234UL)
#define RAM_BENCH_MAGIC (0xCAFEF00DUL)

#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)
#define BRINGUP_TX_TIMEOUT_MS (100U)
#define SETTLE_LOOPS (2000000UL)

/* Lives in .bss, so the startup code zeroes it; a value written before the reset must not return.
 */
static volatile uint32_t s_ram_marker;

static void bringup_puts(const char *s)
{
    (void) debug_uart_send((const uint8_t *) s, strlen(s), BRINGUP_TX_TIMEOUT_MS);
}

static void bringup_halt(const char *label)
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
        bringup_halt(label);
    }
}

int main(void)
{
    /* Read the reset flags first, then clear them for the next boot. */
    const uint32_t csr = RCC->CSR;
    RCC->CSR |= RCC_CSR_RMVF;

    if (cpu_init() != STATUS_OK)
    {
        for (;;)
        {
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

    if ((gpio_init() != GPIO_OK) || (gpio_configure_pin(&uart_tx_config) != GPIO_OK) ||
        (gpio_configure_pin(&uart_rx_config) != GPIO_OK) || (debug_uart_init() != DEBUG_UART_OK) ||
        (rtc_init() != RTC_OK))
    {
        for (;;)
        {
        }
    }

    bringup_puts("\r\n===== ResetDriver Hardware Bring-up =====\r\n");
    bringup_puts("Board : B-L475E-IOT01A  (STM32L475VGTx)\r\n");

    uint32_t marker = 0U;
    check(rtc_read_backup(BKP_BENCH_IDX, &marker) == RTC_OK, "rtc_read_backup");

    const bool soft = ((csr & RCC_CSR_SFTRSTF) != 0U);
    const bool ours = soft && (marker == BKP_BENCH_MAGIC);

    if (!ours)
    {
        /* Boot 1 (or a boot the test did not cause): arm the marker and reset. */
        bringup_puts(soft ? "Boot  : software reset, no marker (not ours)\r\n"
                          : "Boot  : not a software reset\r\n");
        check(rtc_write_backup(BKP_BENCH_IDX, BKP_BENCH_MAGIC) == RTC_OK, "rtc_write_backup");
        s_ram_marker = RAM_BENCH_MAGIC;
        bringup_puts("[INFO] backup and RAM markers written; calling reset_trigger() ...\r\n");

        /* Let the UART finish shifting the last byte out. */
        for (volatile uint32_t i = 0U; i < SETTLE_LOOPS; i++)
        {
        }
        reset_trigger();

        /* Not reached on the target. */
        bringup_halt("TC-HW-RST-001 reset_trigger() returned");
    }

    /* Boot 2: the reset was ours. */
    bringup_puts("Boot  : software reset, marker present\r\n");
    bringup_puts(
        "[PASS] TC-HW-RST-001 reset_trigger() reset the MCU (this banner is the second boot)\r\n");
    check(((csr & RCC_CSR_SFTRSTF) != 0U), "TC-HW-RST-002 SFTRSTF not set");
    bringup_puts("[PASS] TC-HW-RST-002 RCC->CSR reports a software reset (SFTRSTF)\r\n");
    check(marker == BKP_BENCH_MAGIC, "TC-HW-RST-003 backup marker lost");
    bringup_puts("[PASS] TC-HW-RST-003 the backup domain survived the reset\r\n");
    check(s_ram_marker != RAM_BENCH_MAGIC, "TC-HW-RST-004 RAM marker survived");
    bringup_puts("[PASS] TC-HW-RST-004 RAM state was not carried over (.bss re-zeroed)\r\n");

    /* Clear the marker so the next power-up starts boot 1 again. */
    (void) rtc_write_backup(BKP_BENCH_IDX, 0U);
    bringup_puts("\r\n===== ALL TESTS PASSED =====\r\n");

    for (;;)
    {
    }
}
