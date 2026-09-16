/**
 * @file main_test_qspi_flash.c
 * @brief Hardware bring-up test for QspiFlashDriver (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate:
 *   1. Copy this file into Src/ (or add integration-tests/qspi_flash/ as a
 *      source path in CubeIDE).
 *   2. In CubeIDE, right-click Src/main.c -> Properties ->
 *      C/C++ Build -> check "Exclude resource from build".
 *   3. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   4. Flash and run with a debugger attached (J-Link or ST-LINK).
 *
 * Hardware outputs:
 *   USART1 TX  PB6   115 200 8N1  Human-readable test results
 *   LD2 green  PA5                Heartbeat on completion, fast blink on halt
 *   QUADSPI    PE10..PE15  AF10   MX25R6435F, 8 MB, indirect 1-1-1 @ 26.67 MHz
 *
 * QspiFlashDriver configures PE10..PE15 itself via CMSIS (companion
 * QSPID-D10), so this test does not touch the QSPI pins — GpioDriver is
 * brought up only for the LED and the USART1 reporting channel.
 *
 * DESTRUCTIVE.  Erases and programs the 4 KB sector at byte offset
 * 0x0052_0000.  That offset is the start of the *(reserved)* region in
 * flash-partition-layout.md §5.2 (`0x9052_0000` - `0x907F_FFFF`): no
 * partition owns it, so ConfigStore, CertStore, CircularFlashLog and the
 * OTA staging area are all left intact.  Do not repoint the scratch base
 * at offset 0 — that is the live ConfigStore partition.
 *
 * Automated test sequence:
 *   TC-HW-QSPI-001  qspi_flash_init() returns QSPI_FLASH_OK on the physical
 *                    part.  Init verifies RDID against 0xC22817 internally,
 *                    so OK is positive confirmation of the Macronix
 *                    MX25R6435F ID -> closes companion QSPID-O3.
 *   TC-HW-QSPI-002  Second qspi_flash_init() is idempotent (returns OK
 *                    without re-touching the peripheral).
 *   TC-HW-QSPI-003  Sector erase, then read back: all bytes are 0xFF at
 *                    offsets 0, 0x800 and 0xFFF of the sector.  Proves the
 *                    erase reached the silicon, not just the registers.
 *   TC-HW-QSPI-004  Full 256-byte page program at the sector base, read
 *                    back byte-identical.
 *   TC-HW-QSPI-005  Second page at +0x100 with a different pattern; page A
 *                    re-verified afterwards to prove no page-wrap spill.
 *   TC-HW-QSPI-006  NOR 1->0 semantics: programming 0x0F over the existing
 *                    contents yields the bitwise AND of old and new.  Real
 *                    silicon only — a register mock has no memory array.
 *   TC-HW-QSPI-007  512-byte read spanning both pages returns the two
 *                    patterns contiguously (read is not page-limited).
 *   TC-HW-QSPI-008  Re-erase restores 0xFF across the whole 4 KB sector.
 *   TC-HW-QSPI-009  Validation cascade returns the documented error codes.
 *   TC-HW-QSPI-010  DWT-timed erase and page-program durations, reported and
 *                    checked against the MX25R6435F datasheet windows
 *                    (erase typ 40 ms / max 240 ms; page program max 10 ms)
 *                    and the driver's ~500 ms bounded WIP poll.  Supplies
 *                    the integration measurement QSPID-O4 defers to.
 *
 * Diagnostic limitation: qspi_flash_init() checks RDID internally but does
 * not expose the value it read, so a QSPI_FLASH_ERR_DEVICE result proves a
 * mismatch without reporting the observed ID.  If TC-HW-QSPI-001 fails,
 * break in prv_read_id() to recover the actual bytes.
 *
 * Integration checklist:
 *   [ ] TC-HW-QSPI-001 passes -> QSPID-O3 confirmed, close it in the companion
 *   [ ] No QSPI_FLASH_ERR_TIMEOUT anywhere (bounded polls are wide enough)
 *   [ ] Erase and page-program times inside the datasheet windows
 *   [ ] Erased sector reads 0xFF; written data reads back identically
 *   [ ] Validation cascade returns the documented error codes
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cpu/cpu.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "qspi_flash/qspi_flash.h"
#include "cpu/status.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

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

/* ---------------------------------------------------------------------- */
/* Test region — reserved partition, safe to erase (see file header)       */
/* ---------------------------------------------------------------------- */

/** Base of the *(reserved)* QSPI region, 4 KB aligned. NOT ConfigStore. */
#define SCRATCH_BASE (0x00520000UL)

#define SCRATCH_PAGE_A (SCRATCH_BASE)
#define SCRATCH_PAGE_B (SCRATCH_BASE + QSPI_FLASH_PAGE_SIZE_BYTES)

/** Offsets sampled across the 4 KB sector to prove a full-sector erase. */
#define SECTOR_PROBE_LOW (0U)
#define SECTOR_PROBE_MID (0x800U)
#define SECTOR_PROBE_HIGH (QSPI_FLASH_SECTOR_SIZE_BYTES - 1U)

/** Datasheet windows for the MX25R6435F, used by TC-HW-QSPI-010. */
#define ERASE_MAX_MS (240U)
#define PAGE_PROGRAM_MAX_US (10000U)

/** Pattern byte programmed over page A to exercise NOR 1->0 semantics. */
#define NOR_AND_PATTERN (0x0FU)

static uint8_t s_page_a[QSPI_FLASH_PAGE_SIZE_BYTES];
static uint8_t s_page_b[QSPI_FLASH_PAGE_SIZE_BYTES];
static uint8_t s_readback[QSPI_FLASH_PAGE_SIZE_BYTES * 2U];

/** Elapsed times captured by the destructive cases, reported in TC-010. */
static uint32_t s_erase_ms;
static uint32_t s_program_us;

/* ---------------------------------------------------------------------- */
/* Reporting helpers — built on DebugUartDriver.                           */
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

static void bringup_info(const char *label)
{
    bringup_puts("[INFO] ");
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

/** Report a call that returned an unexpected error code, then halt. */
static void bringup_fail_err(const char *label, qspi_flash_err_t got, qspi_flash_err_t expected)
{
    char msg[96];
    (void) snprintf(msg, sizeof(msg), "%s (err=%d, expected=%d)", label, (int) got, (int) expected);
    bringup_fail(msg);
}

/** Halt unless err matches the documented expectation for this step. */
static void expect_err(const char *label, qspi_flash_err_t got, qspi_flash_err_t expected)
{
    if (got != expected)
    {
        bringup_fail_err(label, got, expected);
    }
}

/* ---------------------------------------------------------------------- */
/* DWT timing — cpu_init() enables the cycle counter (cpu.c step 8).       */
/* ---------------------------------------------------------------------- */

static uint32_t dwt_now(void)
{
    return DWT->CYCCNT;
}

static uint32_t dwt_elapsed_us(uint32_t start)
{
    const uint32_t cycles_per_us = cpu_get_sysclk_hz() / 1000000U;
    return (DWT->CYCCNT - start) / cycles_per_us;
}

/* ---------------------------------------------------------------------- */
/* Buffer helpers                                                          */
/* ---------------------------------------------------------------------- */

/** Compare two buffers; report the first mismatch and halt if they differ. */
static void expect_buffers_equal(const char *label, const uint8_t *expected, const uint8_t *actual,
                                 uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++)
    {
        if (expected[i] != actual[i])
        {
            char msg[112];
            (void) snprintf(msg, sizeof(msg), "%s mismatch at %lu (exp=0x%02X got=0x%02X)", label,
                            (unsigned long) i, (unsigned) expected[i], (unsigned) actual[i]);
            bringup_fail(msg);
        }
    }
}

/** Read one byte from the device, halting if the read itself fails. */
static uint8_t read_byte_at(uint32_t addr, const char *label)
{
    uint8_t byte = 0U;
    expect_err(label, qspi_flash_read(addr, &byte, 1U), QSPI_FLASH_OK);
    return byte;
}

/** Halt unless the byte at addr reads as erased (0xFF). */
static void expect_erased_at(uint32_t offset)
{
    const uint8_t byte = read_byte_at(SCRATCH_BASE + offset, "sector probe read");
    if (byte != 0xFFU)
    {
        char msg[96];
        (void) snprintf(msg, sizeof(msg), "sector offset 0x%lX reads 0x%02X, expected 0xFF",
                        (unsigned long) offset, (unsigned) byte);
        bringup_fail(msg);
    }
}

/* ---------------------------------------------------------------------- */
/* Test cases                                                              */
/* ---------------------------------------------------------------------- */

/* TC-HW-QSPI-001 / -002: init on the physical part, and idempotency. */
static void test_init(void)
{
    const qspi_flash_err_t err = qspi_flash_init();
    if (err == QSPI_FLASH_ERR_DEVICE)
    {
        bringup_fail("TC-HW-QSPI-001  RDID mismatch - wrong device fitted or QSPI lines open "
                     "(expected 0xC22817)");
    }
    expect_err("TC-HW-QSPI-001  qspi_flash_init()", err, QSPI_FLASH_OK);
    bringup_pass("TC-HW-QSPI-001  init OK - RDID 0xC22817 confirmed on silicon (QSPID-O3)");

    expect_err("TC-HW-QSPI-002  second qspi_flash_init()", qspi_flash_init(), QSPI_FLASH_OK);
    bringup_pass("TC-HW-QSPI-002  init is idempotent");
}

/* TC-HW-QSPI-003: erase actually reaches the array, across the whole sector. */
static void test_erase_to_blank(void)
{
    const uint32_t start = dwt_now();
    const qspi_flash_err_t err = qspi_flash_erase_sector(SCRATCH_BASE);
    s_erase_ms = dwt_elapsed_us(start) / 1000U;

    expect_err("TC-HW-QSPI-003  erase_sector()", err, QSPI_FLASH_OK);

    expect_erased_at(SECTOR_PROBE_LOW);
    expect_erased_at(SECTOR_PROBE_MID);
    expect_erased_at(SECTOR_PROBE_HIGH);

    bringup_pass("TC-HW-QSPI-003  sector erased - 0xFF at offsets 0, 0x800, 0xFFF");
}

/* TC-HW-QSPI-004: full-page program and verified read-back. */
static void test_program_page_a(void)
{
    uint32_t start;

    for (uint32_t i = 0U; i < QSPI_FLASH_PAGE_SIZE_BYTES; i++)
    {
        s_page_a[i] = (uint8_t) (0xA0U + i);
    }

    start = dwt_now();
    expect_err("TC-HW-QSPI-004  write_page() page A",
               qspi_flash_write_page(SCRATCH_PAGE_A, s_page_a, QSPI_FLASH_PAGE_SIZE_BYTES),
               QSPI_FLASH_OK);
    s_program_us = dwt_elapsed_us(start);

    expect_err("TC-HW-QSPI-004  read() page A",
               qspi_flash_read(SCRATCH_PAGE_A, s_readback, QSPI_FLASH_PAGE_SIZE_BYTES),
               QSPI_FLASH_OK);
    expect_buffers_equal("TC-HW-QSPI-004  page A", s_page_a, s_readback,
                         QSPI_FLASH_PAGE_SIZE_BYTES);

    bringup_pass("TC-HW-QSPI-004  256-byte page programmed and verified");
}

/* TC-HW-QSPI-005: adjacent page, and page A must survive it. */
static void test_program_page_b(void)
{
    for (uint32_t i = 0U; i < QSPI_FLASH_PAGE_SIZE_BYTES; i++)
    {
        s_page_b[i] = (uint8_t) (0x5AU ^ i);
    }

    expect_err("TC-HW-QSPI-005  write_page() page B",
               qspi_flash_write_page(SCRATCH_PAGE_B, s_page_b, QSPI_FLASH_PAGE_SIZE_BYTES),
               QSPI_FLASH_OK);

    expect_err("TC-HW-QSPI-005  read() page B",
               qspi_flash_read(SCRATCH_PAGE_B, s_readback, QSPI_FLASH_PAGE_SIZE_BYTES),
               QSPI_FLASH_OK);
    expect_buffers_equal("TC-HW-QSPI-005  page B", s_page_b, s_readback,
                         QSPI_FLASH_PAGE_SIZE_BYTES);

    /* Page A must be untouched — catches a page-wrap spill on real silicon. */
    expect_err("TC-HW-QSPI-005  re-read() page A",
               qspi_flash_read(SCRATCH_PAGE_A, s_readback, QSPI_FLASH_PAGE_SIZE_BYTES),
               QSPI_FLASH_OK);
    expect_buffers_equal("TC-HW-QSPI-005  page A after page B write", s_page_a, s_readback,
                         QSPI_FLASH_PAGE_SIZE_BYTES);

    bringup_pass("TC-HW-QSPI-005  adjacent page written; page A intact (no wrap)");
}

/* TC-HW-QSPI-007: a single read may span pages; only writes are page-bound. */
static void test_cross_page_read(void)
{
    const uint32_t span = QSPI_FLASH_PAGE_SIZE_BYTES * 2U;

    expect_err("TC-HW-QSPI-007  cross-page read()",
               qspi_flash_read(SCRATCH_PAGE_A, s_readback, span), QSPI_FLASH_OK);
    expect_buffers_equal("TC-HW-QSPI-007  first half", s_page_a, s_readback,
                         QSPI_FLASH_PAGE_SIZE_BYTES);
    expect_buffers_equal("TC-HW-QSPI-007  second half", s_page_b,
                         &s_readback[QSPI_FLASH_PAGE_SIZE_BYTES], QSPI_FLASH_PAGE_SIZE_BYTES);

    bringup_pass("TC-HW-QSPI-007  512-byte read spans both pages contiguously");
}

/* TC-HW-QSPI-006: NOR flash clears bits but cannot set them without erase. */
static void test_nor_and_semantics(void)
{
    uint8_t expected[QSPI_FLASH_PAGE_SIZE_BYTES];

    for (uint32_t i = 0U; i < QSPI_FLASH_PAGE_SIZE_BYTES; i++)
    {
        s_readback[i] = NOR_AND_PATTERN;
        /* Programming without erasing can only clear bits: old AND new. */
        expected[i] = (uint8_t) (s_page_a[i] & NOR_AND_PATTERN);
    }

    expect_err("TC-HW-QSPI-006  overwrite write_page()",
               qspi_flash_write_page(SCRATCH_PAGE_A, s_readback, QSPI_FLASH_PAGE_SIZE_BYTES),
               QSPI_FLASH_OK);

    expect_err("TC-HW-QSPI-006  read() after overwrite",
               qspi_flash_read(SCRATCH_PAGE_A, s_readback, QSPI_FLASH_PAGE_SIZE_BYTES),
               QSPI_FLASH_OK);
    expect_buffers_equal("TC-HW-QSPI-006  AND semantics", expected, s_readback,
                         QSPI_FLASH_PAGE_SIZE_BYTES);

    bringup_pass("TC-HW-QSPI-006  program-without-erase yields old AND new (1->0 only)");
}

/* TC-HW-QSPI-008: a second erase returns the sector to blank. */
static void test_re_erase(void)
{
    expect_err("TC-HW-QSPI-008  second erase_sector()", qspi_flash_erase_sector(SCRATCH_BASE),
               QSPI_FLASH_OK);

    expect_erased_at(SECTOR_PROBE_LOW);
    expect_erased_at(SECTOR_PROBE_MID);
    expect_erased_at(SECTOR_PROBE_HIGH);

    bringup_pass("TC-HW-QSPI-008  re-erase restored 0xFF across the sector");
}

/* TC-HW-QSPI-009: every guard in the driver's validation cascade. */
static void test_validation_cascade(void)
{
    uint8_t byte = 0U;

    expect_err("TC-HW-QSPI-009  read(NULL)", qspi_flash_read(SCRATCH_BASE, NULL, 1U),
               QSPI_FLASH_ERR_NULL_POINTER);
    expect_err("TC-HW-QSPI-009  write_page(NULL)", qspi_flash_write_page(SCRATCH_BASE, NULL, 1U),
               QSPI_FLASH_ERR_NULL_POINTER);

    expect_err("TC-HW-QSPI-009  read(len=0)", qspi_flash_read(SCRATCH_BASE, &byte, 0U),
               QSPI_FLASH_ERR_LEN);
    expect_err("TC-HW-QSPI-009  write_page(len=0)",
               qspi_flash_write_page(SCRATCH_BASE, s_page_a, 0U), QSPI_FLASH_ERR_LEN);
    expect_err("TC-HW-QSPI-009  write_page(len=257)",
               qspi_flash_write_page(SCRATCH_BASE, s_page_a, QSPI_FLASH_PAGE_SIZE_BYTES + 1U),
               QSPI_FLASH_ERR_LEN);

    /* addr 0x..F8 + 16 bytes ends at 0x..107 — crosses the 256-byte page. */
    expect_err("TC-HW-QSPI-009  write_page() across page boundary",
               qspi_flash_write_page(SCRATCH_BASE + 0xF8U, s_page_a, 16U), QSPI_FLASH_ERR_LEN);

    expect_err("TC-HW-QSPI-009  read() past device end",
               qspi_flash_read(QSPI_FLASH_DEVICE_SIZE_BYTES, &byte, 1U), QSPI_FLASH_ERR_ADDR);
    expect_err("TC-HW-QSPI-009  erase_sector() past device end",
               qspi_flash_erase_sector(QSPI_FLASH_DEVICE_SIZE_BYTES), QSPI_FLASH_ERR_ADDR);

    bringup_pass("TC-HW-QSPI-009  validation cascade returned the documented error codes");
}

/* TC-HW-QSPI-010: report the measured timings against the datasheet. */
static void test_report_timings(void)
{
    char msg[112];

    (void) snprintf(msg, sizeof(msg),
                    "TC-HW-QSPI-010  erase %lu ms (max %u), page program %lu us (max %u)",
                    (unsigned long) s_erase_ms, ERASE_MAX_MS, (unsigned long) s_program_us,
                    PAGE_PROGRAM_MAX_US);
    bringup_info(msg);

    if (s_erase_ms > ERASE_MAX_MS)
    {
        bringup_fail("TC-HW-QSPI-010  sector erase exceeded the datasheet maximum");
    }
    if (s_program_us > PAGE_PROGRAM_MAX_US)
    {
        bringup_fail("TC-HW-QSPI-010  page program exceeded the datasheet maximum");
    }

    bringup_pass("TC-HW-QSPI-010  erase and program times within datasheet windows");
}

/* ---------------------------------------------------------------------- */
/* main()                                                                  */
/* ---------------------------------------------------------------------- */

int main(void)
{
    /* cpu_init() first - clock to 80 MHz, DWT cycle counter, fault handlers. */
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

    /* gpio_init() + DebugUartDriver (USART1) — reporting channel only.
     * The QSPI pins are owned by QspiFlashDriver itself (QSPID-D10). */
    (void) gpio_init();

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

    bringup_puts("\r\n===== QspiFlashDriver Hardware Bring-up =====\r\n");
    bringup_puts("Board  : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("Device : MX25R6435F 8 MB, QUADSPI PE10..PE15 AF10\r\n");
    bringup_puts("Scratch: 0x00520000 (reserved region - NOT ConfigStore)\r\n");
    bringup_puts("WARNING: this test erases that 4 KB sector\r\n");
    bringup_puts("=============================================\r\n\r\n");

    test_init();
    test_erase_to_blank();
    test_program_page_a();
    test_program_page_b();
    test_cross_page_read();
    test_nor_and_semantics();
    test_re_erase();
    test_validation_cascade();
    test_report_timings();

    bringup_puts("\r\n[PASS] All automated tests complete.\r\n");
    bringup_puts("[INFO] TC-HW-QSPI-001 passing confirms QSPID-O3 - record the\r\n");
    bringup_puts("       result in docs/lld/drivers/qspi-flash-driver.md §8.\r\n\r\n");

    for (;;)
    {
        gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        cpu_delay_ms(1000U);
    }
}
