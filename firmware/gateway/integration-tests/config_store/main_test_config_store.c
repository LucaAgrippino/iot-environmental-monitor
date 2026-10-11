/**
 * @file main_test_config_store.c
 * @brief Hardware bring-up test for ConfigStore (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. Add cloud_publisher (the current harness) to the `excluding=` list of
 *      the `integration-tests` source entry and remove config_store.
 *   2. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/config_store.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * Hardware outputs:
 *   USART1 TX  PB6   115 200 8N1  Human-readable test results
 *   QUADSPI    PE10..PE15  AF10   MX25R6435F, 8 MB, indirect 1-1-1
 *
 * DESTRUCTIVE.  Operates on the real ConfigStore partition, QSPI device offsets
 * 0x0000_0000 - 0x0000_FFFF (mapped at 0x9000_0000, 64 KB, two 32 KB slots).
 * Anything stored there is lost.  CertStore (0x1_0000), the CircularFlashLog
 * partition (0x2_0000) and the OTA staging area are never touched.
 *
 * Real QspiFlashDriver and real HealthMonitor.  The harness cannot replace
 * health_monitor_push_event() (HealthMonitor is linked), so health events are
 * not observed here: HEALTH_EVENT_CONFIG_WRITE_FAIL is checked through the
 * snapshot flag, and the FAULT pushes are covered by the unit tests. No
 * scheduler is started; the whole sequence runs in main().
 *
 * TWO-PHASE TEST.  Persistence is proven across a real reset:
 *
 *   Phase A runs when config_store_check_integrity() finds no valid slot.  It
 *   starts from a factory erase, exercises every public function, simulates two
 *   kinds of interrupted write, leaves a known blob behind and asks for a RESET
 *   (button B2, or an OpenOCD reset).
 *
 *   Phase B runs when a valid slot exists.  It proves the blob survived the
 *   reset and that the integrity check passes, then erases the partition so the
 *   next run starts at Phase A.  If the blob is not the one Phase A left (stale
 *   data from another run) it erases the partition and asks for a RESET.
 *
 * Automated test sequence:
 *   Phase A
 *   TC-HW-CS-001  qspi_flash_init() OK; HealthMonitor and ConfigStore created.
 *   TC-HW-CS-002  config_store_erase() leaves both slots erased; timed.
 *   TC-HW-CS-003  Blank partition: integrity and load report NO_VALID_SLOT.
 *   TC-HW-CS-004  save() of a small blob goes to slot A (sequence 1); the
 *                  stored CRC matches the contents; load() returns it.
 *   TC-HW-CS-005  A second save goes to slot B (sequence 2) and leaves slot A
 *                  intact; load() returns the newer blob.
 *   TC-HW-CS-006  A maximum-size blob (32 712 bytes) round-trips; timed.
 *   TC-HW-CS-007  Interrupted commit: clearing bits in the newest slot's CRC
 *                  makes load() fall back to the previous blob.
 *   TC-HW-CS-008  Torn write: a newer header with no CRC is never chosen, and
 *                  the next save still succeeds.
 *   TC-HW-CS-009  Error paths on silicon.
 *   TC-HW-CS-010  The persistence blob is saved for Phase B.
 *   Phase B (after RESET)
 *   TC-HW-CS-011  Integrity check passes and load() returns the Phase A blob.
 *   TC-HW-CS-012  Factory erase clears the partition; load() then reports
 *                  NO_VALID_SLOT. HealthMonitor saw no write failure.
 *
 * Integration checklist:
 *   [ ] Every blob reads back byte-identical
 *   [ ] A slot with a bad or missing CRC is never chosen over a valid one
 *   [ ] Phase B finds the blob Phase A left after the reset
 *   [ ] Record the save / load / erase timings
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config_store/config_store.h"
#include "config_store/config_store_config.h"
#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "health_monitor/health_monitor.h"
#include "led/led_driver.h"
#include "qspi_flash/qspi_flash.h"

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
#define US_PER_MS (1000UL)

/* ---------------------------------------------------------------------- */
/* Test data                                                               */
/* ---------------------------------------------------------------------- */

#define SMALL_BLOB_LEN (300U)
#define MEDIUM_BLOB_LEN (5000U)
#define PERSIST_BLOB_LEN (1234U)

#define SEED_SMALL (0x11U)
#define SEED_MEDIUM (0x22U)
#define SEED_MAX (0x33U)
#define SEED_FALLBACK (0x44U)
#define SEED_AFTER_TORN (0x55U)
#define SEED_PERSIST (0xC5U)

#define TORN_SEQUENCE (99UL)

static uint8_t s_blob[CONFIG_STORE_MAX_DATA_BYTES];
static uint8_t s_expected[CONFIG_STORE_MAX_DATA_BYTES];

/* ---------------------------------------------------------------------- */
/* Pin table — Gateway (HealthMonitor drives LD2)                          */
/* ---------------------------------------------------------------------- */

static const led_pin_t k_gw_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = (uint8_t) GPIO_PORT_B, .pin = 14U, .active_high = true, .fitted = true},
    [LED_RED] = {.port = (uint8_t) GPIO_PORT_A, .pin = 5U, .active_high = true, .fitted = false},
};

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

static void expect_cs(config_store_err_t got, config_store_err_t expected, const char *label)
{
    if (got != expected)
    {
        char msg[96];

        (void) snprintf(msg, sizeof(msg), "%s (err=%d, expected=%d)", label, (int) got,
                        (int) expected);
        bringup_fail(msg);
    }
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
    const uint32_t cycles_per_us = cpu_get_sysclk_hz() / US_PER_S;

    return (DWT->CYCCNT - start) / cycles_per_us;
}

static void report_ms(const char *label, uint32_t start)
{
    char msg[96];

    (void) snprintf(msg, sizeof(msg), "%s %lu ms", label,
                    (unsigned long) (dwt_elapsed_us(start) / US_PER_MS));
    bringup_info(msg);
}

/* ---------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ---------------------------------------------------------------------- */

static void fill_pattern(uint8_t *buf, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0U; i < len; i++)
    {
        buf[i] = (uint8_t) (seed + (uint8_t) (i * 3U));
    }
}

static uint32_t get_u32(const uint8_t *src)
{
    return (uint32_t) src[0] | ((uint32_t) src[1] << 8U) | ((uint32_t) src[2] << 16U) |
           ((uint32_t) src[3] << 24U);
}

/** @brief Read the 16-byte header of a slot straight from flash. */
static void read_header(uint8_t slot, uint8_t hdr[CONFIG_STORE_HEADER_SIZE])
{
    uint32_t addr = CONFIG_STORE_PARTITION_BASE + ((uint32_t) slot * CONFIG_STORE_SLOT_SIZE);

    check(qspi_flash_read(addr, hdr, CONFIG_STORE_HEADER_SIZE) == QSPI_FLASH_OK, "raw header read");
}

static uint32_t raw_seq(uint8_t slot)
{
    uint8_t hdr[CONFIG_STORE_HEADER_SIZE];

    read_header(slot, hdr);

    return (get_u32(&hdr[0]) == CONFIG_STORE_MAGIC) ? get_u32(&hdr[4]) : 0U;
}

/** @brief Load and compare against a pattern; halts on any mismatch. */
static void load_expect(config_store_handle_t cs, uint32_t len, uint8_t seed, const char *label)
{
    uint32_t got = 0U;

    expect_cs(config_store_load(cs, s_blob, &got, sizeof(s_blob)), CONFIG_STORE_ERR_OK, label);
    check(got == len, label);

    fill_pattern(s_expected, len, seed);
    for (uint32_t i = 0U; i < len; i++)
    {
        if (s_blob[i] != s_expected[i])
        {
            char msg[96];

            (void) snprintf(msg, sizeof(msg), "%s mismatch at %lu (exp=0x%02X got=0x%02X)", label,
                            (unsigned long) i, (unsigned) s_expected[i], (unsigned) s_blob[i]);
            bringup_fail(msg);
        }
    }
}

static void save_pattern(config_store_handle_t cs, uint32_t len, uint8_t seed, const char *label)
{
    fill_pattern(s_blob, len, seed);
    expect_cs(config_store_save(cs, s_blob, len), CONFIG_STORE_ERR_OK, label);
}

/* ---------------------------------------------------------------------- */
/* Phase A                                                                 */
/* ---------------------------------------------------------------------- */

static void phase_a(config_store_handle_t cs, health_monitor_handle_t hm)
{
    health_snapshot_t snap;
    uint32_t len = 0U;
    uint32_t t0;
    uint8_t hdr[CONFIG_STORE_HEADER_SIZE];

    bringup_puts("\r\n--- Phase A: write side ---\r\n");

    /* TC-HW-CS-002 */
    t0 = dwt_now();
    expect_cs(config_store_erase(cs), CONFIG_STORE_ERR_OK, "TC-HW-CS-002 erase");
    report_ms("TC-HW-CS-002 factory erase (16 sectors)", t0);
    check((raw_seq(0U) == 0U) && (raw_seq(1U) == 0U), "TC-HW-CS-002 slots blank");
    read_header(0U, hdr);
    check((get_u32(&hdr[0]) == 0xFFFFFFFFUL), "TC-HW-CS-002 slot A header erased");
    bringup_pass("TC-HW-CS-002 factory erase leaves both slots erased");

    /* TC-HW-CS-003 */
    expect_cs(config_store_check_integrity(cs), CONFIG_STORE_ERR_NO_VALID_SLOT,
              "TC-HW-CS-003 integrity");
    expect_cs(config_store_load(cs, s_blob, &len, sizeof(s_blob)), CONFIG_STORE_ERR_NO_VALID_SLOT,
              "TC-HW-CS-003 load");
    bringup_pass("TC-HW-CS-003 blank partition: NO_VALID_SLOT from integrity and load");

    /* TC-HW-CS-004 */
    t0 = dwt_now();
    save_pattern(cs, SMALL_BLOB_LEN, SEED_SMALL, "TC-HW-CS-004 save");
    report_ms("TC-HW-CS-004 save of 300 B (erase + program)", t0);
    check(raw_seq(0U) == 1U, "TC-HW-CS-004 slot A sequence 1");
    check(raw_seq(1U) == 0U, "TC-HW-CS-004 slot B untouched");
    t0 = dwt_now();
    expect_cs(config_store_check_integrity(cs), CONFIG_STORE_ERR_OK, "TC-HW-CS-004 integrity");
    report_ms("TC-HW-CS-004 integrity check", t0);
    t0 = dwt_now();
    load_expect(cs, SMALL_BLOB_LEN, SEED_SMALL, "TC-HW-CS-004 load");
    report_ms("TC-HW-CS-004 load", t0);
    bringup_pass("TC-HW-CS-004 first save lands in slot A, sequence 1, and reads back");

    /* TC-HW-CS-005 */
    save_pattern(cs, MEDIUM_BLOB_LEN, SEED_MEDIUM, "TC-HW-CS-005 save");
    check(raw_seq(0U) == 1U, "TC-HW-CS-005 slot A intact");
    check(raw_seq(1U) == 2U, "TC-HW-CS-005 slot B sequence 2");
    load_expect(cs, MEDIUM_BLOB_LEN, SEED_MEDIUM, "TC-HW-CS-005 load");
    bringup_pass("TC-HW-CS-005 second save lands in slot B and leaves slot A intact");

    /* TC-HW-CS-006 */
    t0 = dwt_now();
    save_pattern(cs, CONFIG_STORE_MAX_DATA_BYTES, SEED_MAX, "TC-HW-CS-006 save");
    report_ms("TC-HW-CS-006 save of 32 712 B", t0);
    check(raw_seq(0U) == 3U, "TC-HW-CS-006 back in slot A, sequence 3");
    t0 = dwt_now();
    load_expect(cs, CONFIG_STORE_MAX_DATA_BYTES, SEED_MAX, "TC-HW-CS-006 load");
    report_ms("TC-HW-CS-006 load of 32 712 B", t0);
    bringup_pass("TC-HW-CS-006 maximum-size blob round-trips (page splitting, 8 sectors)");

    /* TC-HW-CS-007 - an interrupted CRC commit: program zeros over the stored CRC. */
    save_pattern(cs, SMALL_BLOB_LEN, SEED_FALLBACK, "TC-HW-CS-007 save");
    check(raw_seq(1U) == 4U, "TC-HW-CS-007 newest blob in slot B, sequence 4");
    {
        static const uint8_t zeros[CONFIG_STORE_CRC_SIZE] = {0U, 0U, 0U, 0U};
        uint32_t crc_addr =
            CONFIG_STORE_PARTITION_BASE + CONFIG_STORE_SLOT_SIZE + CONFIG_STORE_CRC_OFFSET;

        check(qspi_flash_write_page(crc_addr, zeros, sizeof(zeros)) == QSPI_FLASH_OK,
              "TC-HW-CS-007 corrupt the CRC");
    }
    load_expect(cs, CONFIG_STORE_MAX_DATA_BYTES, SEED_MAX, "TC-HW-CS-007 fallback load");
    expect_cs(config_store_check_integrity(cs), CONFIG_STORE_ERR_OK, "TC-HW-CS-007 integrity");
    bringup_pass("TC-HW-CS-007 a slot with a bad CRC is skipped; the previous blob is returned");

    /* TC-HW-CS-008 - a torn write: erase slot B, leave only a newer header. */
    {
        uint32_t base = CONFIG_STORE_PARTITION_BASE + CONFIG_STORE_SLOT_SIZE;
        uint8_t torn[CONFIG_STORE_HEADER_SIZE] = {0U};
        uint8_t magic[4];

        for (uint32_t s = 0U; s < CONFIG_STORE_SECTORS_PER_SLOT; s++)
        {
            check(qspi_flash_erase_sector(base + (s * CONFIG_STORE_SECTOR_SIZE)) == QSPI_FLASH_OK,
                  "TC-HW-CS-008 erase slot B");
        }

        magic[0] = (uint8_t) (CONFIG_STORE_MAGIC & 0xFFU);
        magic[1] = (uint8_t) ((CONFIG_STORE_MAGIC >> 8U) & 0xFFU);
        magic[2] = (uint8_t) ((CONFIG_STORE_MAGIC >> 16U) & 0xFFU);
        magic[3] = (uint8_t) ((CONFIG_STORE_MAGIC >> 24U) & 0xFFU);
        (void) memcpy(&torn[0], magic, sizeof(magic));
        torn[4] = (uint8_t) TORN_SEQUENCE;
        torn[8] = 16U; /* data_len */

        check(qspi_flash_write_page(base, torn, sizeof(torn)) == QSPI_FLASH_OK,
              "TC-HW-CS-008 write the torn header");
    }
    check(raw_seq(1U) == TORN_SEQUENCE, "TC-HW-CS-008 torn header has the highest sequence");
    load_expect(cs, CONFIG_STORE_MAX_DATA_BYTES, SEED_MAX, "TC-HW-CS-008 load ignores it");
    save_pattern(cs, SMALL_BLOB_LEN, SEED_AFTER_TORN, "TC-HW-CS-008 save over it");
    load_expect(cs, SMALL_BLOB_LEN, SEED_AFTER_TORN, "TC-HW-CS-008 load the new blob");
    bringup_pass("TC-HW-CS-008 a newer header with no CRC is never chosen; the next save recovers");

    /* TC-HW-CS-009 */
    check(config_store_save(cs, NULL, 16U) == CONFIG_STORE_ERR_NULL_PTR, "TC-HW-CS-009 NULL data");
    check(config_store_save(cs, s_blob, CONFIG_STORE_MAX_DATA_BYTES + 1U) ==
              CONFIG_STORE_ERR_TOO_LARGE,
          "TC-HW-CS-009 oversize");
    check(config_store_load(cs, s_blob, &len, 8U) == CONFIG_STORE_ERR_TOO_LARGE,
          "TC-HW-CS-009 load into a small buffer");
    check(config_store_load(cs, NULL, &len, 8U) == CONFIG_STORE_ERR_NULL_PTR,
          "TC-HW-CS-009 NULL buffer");
    check(config_store_load(NULL, s_blob, &len, 8U) == CONFIG_STORE_ERR_NULL_PTR,
          "TC-HW-CS-009 NULL handle");
    {
        config_store_handle_t second = NULL;
        config_store_config_t cfg = {.flash = qspi_flash_driver, .health = hm};

        check(config_store_create(&cfg, &second) == CONFIG_STORE_ERR_NO_RESOURCE,
              "TC-HW-CS-009 second create");
    }
    check(health_monitor_get_snapshot(hm, &snap) == HEALTH_MONITOR_ERR_OK, "TC-HW-CS-009 snapshot");
    check(!snap.config_write_failed, "TC-HW-CS-009 no write failure was reported");
    bringup_pass("TC-HW-CS-009 error paths; HealthMonitor saw no write failure");

    /* TC-HW-CS-010 */
    save_pattern(cs, PERSIST_BLOB_LEN, SEED_PERSIST, "TC-HW-CS-010 save");
    bringup_pass("TC-HW-CS-010 persistence blob saved");

    bringup_puts("\r\n=====================================================\r\n");
    bringup_puts("Phase A complete. A 1234-byte blob is stored.\r\n");
    bringup_puts("ACTION: press the black RESET button (B2) to run Phase B.\r\n");
    bringup_puts("Do NOT erase the QSPI part between the two phases.\r\n");
    bringup_puts("=====================================================\r\n");
}

/* ---------------------------------------------------------------------- */
/* Phase B                                                                 */
/* ---------------------------------------------------------------------- */

static void phase_b(config_store_handle_t cs, health_monitor_handle_t hm)
{
    health_snapshot_t snap;
    uint32_t got = 0U;
    bool matches;

    bringup_puts("\r\n--- Phase B: recovery after reset ---\r\n");

    /* A valid slot is only Phase B if it holds the blob Phase A left. */
    matches = (config_store_load(cs, s_blob, &got, sizeof(s_blob)) == CONFIG_STORE_ERR_OK) &&
              (got == PERSIST_BLOB_LEN);
    if (matches)
    {
        fill_pattern(s_expected, PERSIST_BLOB_LEN, SEED_PERSIST);
        matches = (memcmp(s_blob, s_expected, PERSIST_BLOB_LEN) == 0);
    }

    if (!matches)
    {
        bringup_info("a valid blob is stored but it is not Phase A's - stale state");
        expect_cs(config_store_erase(cs), CONFIG_STORE_ERR_OK, "stale erase");
        bringup_info("partition erased. ACTION: press RESET (B2) to start at Phase A.");
        return;
    }

    /* TC-HW-CS-011 */
    expect_cs(config_store_check_integrity(cs), CONFIG_STORE_ERR_OK, "TC-HW-CS-011 integrity");
    load_expect(cs, PERSIST_BLOB_LEN, SEED_PERSIST, "TC-HW-CS-011 load");
    bringup_pass("TC-HW-CS-011 the blob survived the reset; integrity passes");

    /* TC-HW-CS-012 */
    expect_cs(config_store_erase(cs), CONFIG_STORE_ERR_OK, "TC-HW-CS-012 erase");
    expect_cs(config_store_load(cs, s_blob, &got, sizeof(s_blob)), CONFIG_STORE_ERR_NO_VALID_SLOT,
              "TC-HW-CS-012 load after erase");
    check((raw_seq(0U) == 0U) && (raw_seq(1U) == 0U), "TC-HW-CS-012 slots blank");
    check(health_monitor_get_snapshot(hm, &snap) == HEALTH_MONITOR_ERR_OK, "TC-HW-CS-012 snapshot");
    check(!snap.config_write_failed, "TC-HW-CS-012 no write failure was reported");
    bringup_pass("TC-HW-CS-012 factory erase clears the partition; the next run starts at Phase A");

    bringup_puts("\r\n=====================================================\r\n");
    bringup_puts("Phase B complete. All 12 hardware cases passed.\r\n");
    bringup_puts("=====================================================\r\n");
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

    bringup_puts("\r\n===== ConfigStore Hardware Bring-up =====\r\n");
    bringup_puts("Board  : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("Region : 0x00000000 (ConfigStore partition, 64 KB, slots A/B)\r\n");
    bringup_puts("WARNING: erases the ConfigStore partition\r\n");
    bringup_puts("=========================================\r\n\r\n");

    /* TC-HW-CS-001 */
    if (qspi_flash_init() != QSPI_FLASH_OK)
    {
        bringup_fail("TC-HW-CS-001 qspi_flash_init failed - check MX25R6435F wiring");
    }

    if (led_init(k_gw_led_pins, (uint8_t) LED_COUNT) != LED_ERR_OK)
    {
        bringup_fail("TC-HW-CS-001 led_init failed");
    }

    health_monitor_config_t hm_cfg = {.boot_reset_cause = LIFECYCLE_RESET_SOFT};
    health_monitor_handle_t hm = NULL;
    config_store_config_t cs_cfg = {.flash = qspi_flash_driver, .health = NULL};
    config_store_handle_t cs = NULL;

    expect_cs((health_monitor_create(&hm_cfg, &hm) == HEALTH_MONITOR_ERR_OK)
                  ? CONFIG_STORE_ERR_OK
                  : CONFIG_STORE_ERR_NOT_INIT,
              CONFIG_STORE_ERR_OK, "TC-HW-CS-001 health_monitor_create");

    cs_cfg.health = hm;
    expect_cs(config_store_create(&cs_cfg, &cs), CONFIG_STORE_ERR_OK, "TC-HW-CS-001 create");
    bringup_pass("TC-HW-CS-001 qspi_flash_init OK; HealthMonitor and ConfigStore created");

    /* The boot integrity check selects the phase (LifecycleController does this
     * during Init.CheckingIntegrity). */
    config_store_err_t integrity = config_store_check_integrity(cs);

    if (integrity == CONFIG_STORE_ERR_NO_VALID_SLOT)
    {
        phase_a(cs, hm);
    }
    else if (integrity == CONFIG_STORE_ERR_OK)
    {
        phase_b(cs, hm);
    }
    else
    {
        bringup_fail("boot integrity check returned a flash error");
    }

    for (;;)
    {
        /* Nothing more to do; the result is on the UART. */
    }
}
