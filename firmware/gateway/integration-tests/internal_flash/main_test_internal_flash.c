/**
 * @file main_test_internal_flash.c
 * @brief Hardware bring-up test for the InternalFlashDriver (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. Add cloud_publisher (the current harness) to the `excluding=` list of the
 *      `integration-tests` source entry and remove internal_flash from it.
 *   2. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/internal_flash.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * DESTRUCTIVE, but only inside two scratch regions that flash-partition-layout.md reserves
 * and that lie far above this small image:
 *   bank 1: 0x0807_E000..0x0807_FFFF   (8 KB, 4 pages)
 *   bank 2: 0x080F_8000..0x080F_FFFF   (32 KB, 16 pages)
 * Nothing else in the flash is touched. No scheduler is started.
 *
 * Automated test sequence (docs/lld/drivers/internal-flash-driver.md §8):
 *   TC-HW-IFD-001  internal_flash_init() succeeds (the device is in dual-bank mode).
 *   TC-HW-IFD-002  Erase a bank-2 page; it reads back all 0xFF; time logged.
 *   TC-HW-IFD-003  Program a 2 KB pattern into it; it reads back identical; time logged.
 *   TC-HW-IFD-004  Program over the programmed page: PROGRAM error, data unchanged.
 *   TC-HW-IFD-005  The same sequence on a bank-1 page.
 *   TC-HW-IFD-006  Erase every page of the bank-2 scratch region; all 0xFF.
 *   TC-HW-IFD-007  Cache coherence: read a programmed page twice, erase it, read again: 0xFF.
 *   TC-HW-IFD-008  A 1 kHz TIM7 interrupt while erasing a bank-2 page, then a bank-1 page;
 *                  the longest gap between interrupts is logged for each. The vector table,
 *                  the ISR and the polling loop are all in bank 1.
 *   TC-HW-IFD-008b The same with the instruction and data caches switched off, so that every
 *                  fetch must reach the flash. Logged, not asserted.
 *   TC-HW-IFD-009  Program 2 KB in each bank with the 1 kHz interrupt running: no error; time
 *                  and longest gap logged.
 *
 * Integration checklist:
 *   [ ] Every erase reads back 0xFF and every program reads back its data
 *   [ ] Programming a non-erased double word is refused and leaves the data alone
 *   [ ] Record: erase time, 2 KB program time, longest interrupt gap per bank
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "internal_flash/internal_flash.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Board constants                                                         */
/* ---------------------------------------------------------------------- */

#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)
#define BRINGUP_TX_TIMEOUT_MS (100U)

#define US_PER_S (1000000UL)
#define GAP_TIMER_HZ (1000UL)

/* Scratch regions (flash-partition-layout.md §5, reserved). */
#define SCRATCH_BANK1 (0x0807E000UL)
#define SCRATCH_BANK1_PAGES (4U)
#define SCRATCH_BANK2 (0x080F8000UL)
#define SCRATCH_BANK2_PAGES (16U)

/* The firmware must end below the bank-1 scratch region. */
#define FIRMWARE_LIMIT (SCRATCH_BANK1)

#define PAGE (INTERNAL_FLASH_PAGE_SIZE_BYTES)

static uint8_t s_pattern[PAGE];
static uint8_t s_readback[PAGE];

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

static void bringup_info(const char *label)
{
    bringup_puts("[INFO] ");
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

static void expect(internal_flash_err_t got, internal_flash_err_t expected, const char *label)
{
    if (got != expected)
    {
        char msg[96];

        (void) snprintf(msg, sizeof(msg), "%s (err=%d, expected=%d)", label, (int) got,
                        (int) expected);
        bringup_fail(msg);
    }
}

static void report(const char *fmt, uint32_t a, uint32_t b)
{
    char msg[96];

    (void) snprintf(msg, sizeof(msg), fmt, (unsigned long) a, (unsigned long) b);
    bringup_info(msg);
}

/* ---------------------------------------------------------------------- */
/* DWT timing — cpu_init() enables the cycle counter.                      */
/* ---------------------------------------------------------------------- */

static uint32_t dwt_now(void)
{
    return DWT->CYCCNT;
}

static uint32_t dwt_elapsed_us(uint32_t start)
{
    return (DWT->CYCCNT - start) / (cpu_get_sysclk_hz() / US_PER_S);
}

/* ---------------------------------------------------------------------- */
/* TC-HW-IFD-008, -009 — longest gap between 1 kHz interrupts              */
/* ---------------------------------------------------------------------- */

static volatile uint32_t s_gap_last;
static volatile uint32_t s_gap_max_cycles;

void TIM7_IRQHandler(void)
{
    const uint32_t now = DWT->CYCCNT;
    const uint32_t gap = now - s_gap_last;

    TIM7->SR = 0U;

    if ((s_gap_last != 0U) && (gap > s_gap_max_cycles))
    {
        s_gap_max_cycles = gap;
    }

    s_gap_last = now;
}

static void gap_timer_start(void)
{
    RCC->APB1ENR1 |= RCC_APB1ENR1_TIM7EN;
    (void) RCC->APB1ENR1; /* read back: the clock is on before the first access */

    TIM7->PSC = (uint16_t) ((cpu_get_pclk1_hz() / US_PER_S) - 1U); /* 1 MHz */
    TIM7->ARR = (uint16_t) ((US_PER_S / GAP_TIMER_HZ) - 1U);
    TIM7->DIER = TIM_DIER_UIE;
    TIM7->CR1 = TIM_CR1_CEN;

    NVIC_EnableIRQ(TIM7_IRQn);
}

static void gap_reset(void)
{
    s_gap_max_cycles = 0U;
    s_gap_last = 0U;
}

static uint32_t gap_max_us(void)
{
    return s_gap_max_cycles / (cpu_get_sysclk_hz() / US_PER_S);
}

/* ---------------------------------------------------------------------- */
/* Flash helpers                                                           */
/* ---------------------------------------------------------------------- */

static void fill_pattern(uint32_t seed)
{
    for (uint32_t i = 0U; i < PAGE; i++)
    {
        s_pattern[i] = (uint8_t) (((i * 131U) + seed) ^ (i >> 8));
    }
}

static bool page_is_erased(uint32_t addr)
{
    expect(internal_flash_read(addr, s_readback, PAGE), INTERNAL_FLASH_OK, "read page");

    for (uint32_t i = 0U; i < PAGE; i++)
    {
        if (s_readback[i] != 0xFFU)
        {
            return false;
        }
    }

    return true;
}

static bool page_equals_pattern(uint32_t addr)
{
    expect(internal_flash_read(addr, s_readback, PAGE), INTERNAL_FLASH_OK, "read page");

    return memcmp(s_readback, s_pattern, PAGE) == 0;
}

/** @brief Erase, program and re-check one page; log the times. Used for TC-HW-IFD-002..004. */
static void exercise_page(uint32_t addr, const char *bank, uint32_t seed)
{
    char label[96];

    fill_pattern(seed);

    uint32_t t = dwt_now();

    expect(internal_flash_erase_page(addr), INTERNAL_FLASH_OK, "erase page");
    report("  erase page: %lu us (addr 0x%08lX)", dwt_elapsed_us(t), addr);
    (void) snprintf(label, sizeof(label), "%s erased page reads back all 0xFF", bank);
    check(page_is_erased(addr), label);
    bringup_pass(label);

    t = dwt_now();
    expect(internal_flash_program(addr, s_pattern, PAGE), INTERNAL_FLASH_OK, "program page");
    report("  program 2 KB: %lu us (%lu double words)", dwt_elapsed_us(t), PAGE / 8U);
    (void) snprintf(label, sizeof(label), "%s programmed page reads back identical", bank);
    check(page_equals_pattern(addr), label);
    bringup_pass(label);

    /* Programming again onto the same double words must be refused and change nothing. */
    expect(internal_flash_program(addr, s_pattern, 16U), INTERNAL_FLASH_ERR_PROGRAM,
           "program over programmed data");
    (void) snprintf(label, sizeof(label), "%s second program refused, data unchanged", bank);
    check(page_equals_pattern(addr), label);
    bringup_pass(label);
}

/* ---------------------------------------------------------------------- */
/* main                                                                    */
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
        (gpio_configure_pin(&uart_rx_config) != GPIO_OK) || (debug_uart_init() != DEBUG_UART_OK))
    {
        for (;;)
        {
            /* No sink to report to. */
        }
    }

    bringup_puts("\r\n===== InternalFlashDriver Hardware Bring-up =====\r\n");
    bringup_puts("Board  : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("Scratch: 0x0807E000 (bank 1, 8 KB), 0x080F8000 (bank 2, 32 KB)\r\n");
    bringup_puts("WARNING: erases and programs those two regions only\r\n");
    bringup_puts("=================================================\r\n\r\n");

    /* This image runs from bank 1; it must end below the bank-1 scratch region. */
    extern const uint32_t _etext;
    check((uint32_t) &_etext < FIRMWARE_LIMIT, "firmware reaches the bank-1 scratch region");

    /* TC-HW-IFD-001 */
    expect(internal_flash_init(), INTERNAL_FLASH_OK, "TC-HW-IFD-001 internal_flash_init");
    bringup_pass("TC-HW-IFD-001 init OK: the device is in dual-bank mode");

    /* TC-HW-IFD-002..004 */
    bringup_info("TC-HW-IFD-002..004 bank 2");
    exercise_page(SCRATCH_BANK2, "bank-2", 0x21U);

    /* TC-HW-IFD-005 */
    bringup_info("TC-HW-IFD-005 bank 1");
    exercise_page(SCRATCH_BANK1, "bank-1", 0x17U);

    /* TC-HW-IFD-006 */
    for (uint32_t p = 0U; p < SCRATCH_BANK2_PAGES; p++)
    {
        expect(internal_flash_erase_page(SCRATCH_BANK2 + (p * PAGE)), INTERNAL_FLASH_OK,
               "erase every bank-2 scratch page");
    }

    for (uint32_t p = 0U; p < SCRATCH_BANK2_PAGES; p++)
    {
        check(page_is_erased(SCRATCH_BANK2 + (p * PAGE)), "a bank-2 scratch page is not erased");
    }

    bringup_pass("TC-HW-IFD-006 all 16 bank-2 scratch pages erase and read back 0xFF");

    /* TC-HW-IFD-007: a cached read of old contents must not survive an erase. */
    fill_pattern(0x5CU);
    expect(internal_flash_program(SCRATCH_BANK2, s_pattern, PAGE), INTERNAL_FLASH_OK, "program");
    check(page_equals_pattern(SCRATCH_BANK2), "TC-HW-IFD-007 first read");
    check(page_equals_pattern(SCRATCH_BANK2), "TC-HW-IFD-007 second read");
    expect(internal_flash_erase_page(SCRATCH_BANK2), INTERNAL_FLASH_OK, "erase");
    check(page_is_erased(SCRATCH_BANK2), "TC-HW-IFD-007 read after erase returned stale data");
    bringup_pass("TC-HW-IFD-007 the read after an erase is 0xFF, not the cached old data");

    /* TC-HW-IFD-008 */
    gap_timer_start();
    cpu_delay_ms(20U); /* let the gap meter settle */

    gap_reset();
    expect(internal_flash_erase_page(SCRATCH_BANK2), INTERNAL_FLASH_OK, "erase bank 2 under load");
    cpu_delay_ms(5U);
    const uint32_t gap_bank2_erase = gap_max_us();

    gap_reset();
    expect(internal_flash_erase_page(SCRATCH_BANK1), INTERNAL_FLASH_OK, "erase bank 1 under load");
    cpu_delay_ms(5U);
    const uint32_t gap_bank1_erase = gap_max_us();

    report("  longest 1 kHz interrupt gap: erase in bank 2 = %lu us, in bank 1 = %lu us",
           gap_bank2_erase, gap_bank1_erase);
    bringup_pass("TC-HW-IFD-008 gaps recorded");

    /* TC-HW-IFD-008b: nothing is cached, so the fetches of the ISR and of the polling loop
     * must reach the flash. A fetch from the bank being erased waits for the erase. */
    FLASH->ACR &= ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN);

    gap_reset();
    expect(internal_flash_erase_page(SCRATCH_BANK2), INTERNAL_FLASH_OK, "erase bank 2, no cache");
    cpu_delay_ms(5U);
    const uint32_t gap_bank2_cold = gap_max_us();

    gap_reset();
    expect(internal_flash_erase_page(SCRATCH_BANK1), INTERNAL_FLASH_OK, "erase bank 1, no cache");
    cpu_delay_ms(5U);
    const uint32_t gap_bank1_cold = gap_max_us();

    FLASH->ACR |= FLASH_ACR_ICEN | FLASH_ACR_DCEN;

    report("  caches OFF, longest gap: erase in bank 2 = %lu us, in bank 1 = %lu us",
           gap_bank2_cold, gap_bank1_cold);
    bringup_pass(
        "TC-HW-IFD-008b recorded (no assertion: see IFD-O2 for how to read these figures)");

    /* TC-HW-IFD-009 */
    fill_pattern(0x3AU);

    gap_reset();
    expect(internal_flash_program(SCRATCH_BANK2, s_pattern, PAGE), INTERNAL_FLASH_OK,
           "program bank 2 under load");
    cpu_delay_ms(5U);
    const uint32_t gap_bank2_prog = gap_max_us();

    gap_reset();
    expect(internal_flash_program(SCRATCH_BANK1, s_pattern, PAGE), INTERNAL_FLASH_OK,
           "program bank 1 under load");
    cpu_delay_ms(5U);
    const uint32_t gap_bank1_prog = gap_max_us();

    report("  longest gap while programming 2 KB: bank 2 = %lu us, bank 1 = %lu us", gap_bank2_prog,
           gap_bank1_prog);
    bringup_pass("TC-HW-IFD-009 programming under a running 1 kHz interrupt: no error");

    /* Leave the scratch regions erased. */
    expect(internal_flash_erase_page(SCRATCH_BANK1), INTERNAL_FLASH_OK, "final erase bank 1");
    expect(internal_flash_erase_page(SCRATCH_BANK2), INTERNAL_FLASH_OK, "final erase bank 2");

    bringup_puts("\r\n===== ALL TESTS PASSED =====\r\n");

    for (;;)
    {
    }
}
