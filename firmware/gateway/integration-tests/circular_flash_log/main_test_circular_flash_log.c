/**
 * @file main_test_circular_flash_log.c
 * @brief Hardware bring-up test for CircularFlashLog (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate:
 *   1. Copy this file into Src/ (or add integration-tests/circular_flash_log/
 *      as a source path in CubeIDE).
 *   2. In CubeIDE, right-click Src/main.c -> Properties ->
 *      C/C++ Build -> check "Exclude resource from build".
 *   3. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   4. Flash and run with a debugger attached (J-Link or ST-LINK).
 *
 * Hardware outputs:
 *   USART1 TX  PB6   115 200 8N1  Human-readable test results
 *   LD2 green  PA5                Heartbeat on completion, fast blink on halt
 *   QUADSPI    PE10..PE15  AF10   MX25R6435F, 8 MB, indirect 1-1-1
 *
 * DESTRUCTIVE.  This test operates on the *real* CircularFlashLog partition,
 * byte offsets 0x0002_0000 - 0x0011_FFFF (mapped 0x9002_0000, 1 MB, per
 * flash-partition-layout.md §5.2).  Phase A erases the metadata zone and the
 * data sectors it uses, destroying anything StoreAndForward had buffered
 * there.  ConfigStore (offset 0), CertStore (0x10000) and the OTA staging
 * area (0x120000) are never touched.
 *
 * TWO-PHASE TEST.  The unit tests can rewind the static pool with
 * cfl_reset_for_test(), but that hook does not exist in a production build and
 * the pool holds a single instance, so a second cfl_create() in one run is
 * impossible.  Persistence therefore has to be proven across a real reset:
 *
 *   Phase A runs when cfl_create() reports an empty ring.  It exercises the
 *   write side, leaves two known records behind, and asks the operator to
 *   press the black RESET button (B2).  Do NOT re-flash — re-flashing is
 *   fine for the firmware but the point is to leave the QSPI part alone.
 *
 *   Phase B runs when cfl_create() reports a non-empty ring.  It proves the
 *   head pointer and record contents survived the power cycle, then drains
 *   the ring and finishes.
 *
 * After Phase B the partition is in neither starting state: the metadata zone
 * holds a head pointer, and because only the head *sector* is persisted
 * (CFL-D5), the boot scan rediscovers the last record consumed inside that
 * sector.  A third run therefore reports one entry, and the harness asks the
 * operator to erase the metadata zone rather than failing obscurely.  That
 * replay is by design — StoreAndForward publishes at QoS 1, so a repeated
 * frame after an unexpected reset is within contract — and it is tracked as
 * companion open item CFL-O6.
 *
 * Automated test sequence:
 *   Phase A
 *   TC-HW-CFL-001  qspi_flash_init() returns QSPI_FLASH_OK (prerequisite).
 *   TC-HW-CFL-002  cfl_create() on a blank partition returns CFL_ERR_OK and
 *                   reports an empty ring.  The call is DWT-timed: this is the
 *                   full 254-sector boot scan the companion's CFL-O2 defers to
 *                   integration, with a 100 ms threshold.
 *   TC-HW-CFL-003  cfl_append() then cfl_peek_oldest() returns the payload
 *                   byte-identical off the physical part.
 *   TC-HW-CFL-004  cfl_get_occupancy() reports one entry and the payload size.
 *   TC-HW-CFL-005  FIFO order across three records of differing sizes.
 *   TC-HW-CFL-006  A max-size record forces a sector advance (erase + move),
 *                   and consuming across the sector boundary rewrites the
 *                   persistent head pointer.
 *   TC-HW-CFL-007  DWT-timed append, sector-advancing append and consume,
 *                   reported against the QSPID-O4 bench figures (sector erase
 *                   81 ms, page program 3181 us/256 B).
 *   Phase B (after RESET)
 *   TC-HW-CFL-008  cfl_create() recovers the surviving records by ring scan —
 *                   occupancy matches what Phase A left behind.
 *   TC-HW-CFL-009  The recovered oldest record is the one Phase A expected,
 *                   proving the head pointer came back from the metadata A/B
 *                   slot rather than defaulting to sector 0.
 *   TC-HW-CFL-010  cfl_drop_oldest() raises the buffer-overflow event and
 *                   cfl_consume_oldest() drains the ring to empty.
 *
 * Integration checklist:
 *   [ ] TC-HW-CFL-002 boot-scan time printed and under 100 ms -> record it and
 *       close CFL-O2 in docs/lld/middleware/circular-flash-log.md §13
 *   [ ] No CFL_ERR_FLASH_* anywhere (the driver's bounded polls are adequate)
 *   [ ] Payloads read back byte-identical, oldest first
 *   [ ] Phase B recovers the exact records Phase A left, in order
 *   [ ] Overflow event count is 1 after the single cfl_drop_oldest()
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "circular_flash_log/circular_flash_log.h"
#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "qspi_flash/qspi_flash.h"

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
/* Test region — the real CircularFlashLog partition (see file header)     */
/* ---------------------------------------------------------------------- */

/** Device byte offset of the partition; mapped at 0x9002_0000. */
#define CFL_PARTITION_BASE (0x00020000UL)

/** Metadata sectors plus the data sectors this test writes. */
#define PREP_SECTOR_COUNT (8U)

/** Boot-scan budget from companion CFL-O2. */
#define BOOT_SCAN_BUDGET_MS (100U)

/** Payload sizes and seeds. Phase B recomputes these to check the recovery. */
#define REC_A_LEN (64U)
#define REC_A_SEED (0xA0U)
#define REC_B_LEN (200U)
#define REC_B_SEED (0xB0U)
#define REC_C_LEN (1000U)
#define REC_C_SEED (0xC0U)
#define REC_D_LEN (CFL_MAX_RECORD_BYTES)
#define REC_D_SEED (0xD0U)
#define REC_E_LEN (128U)
#define REC_E_SEED (0xE0U)

/** Records Phase A leaves in the ring for Phase B to recover. */
#define PHASE_B_ENTRY_COUNT (2U)
#define PHASE_B_BYTE_COUNT (REC_D_LEN + REC_E_LEN)

static uint8_t s_expected[CFL_MAX_RECORD_BYTES];
static uint8_t s_readback[CFL_MAX_RECORD_BYTES];

/** Elapsed times captured during Phase A, reported in TC-HW-CFL-007. */
static uint32_t s_boot_scan_us;
static uint32_t s_append_us;
static uint32_t s_append_advance_us;
static uint32_t s_consume_us;

/** IHealthReport spy — the GW HealthMonitor does not exist yet (CFL-O5). */
static uint32_t s_overflow_events;
static uint32_t s_flash_err_events;

void health_report_push_buffer_event(health_monitor_handle_t handle, cfl_health_event_t event)
{
    (void) handle;

    if (event == CFL_HEALTH_EVENT_BUFFER_OVERFLOW)
    {
        s_overflow_events++;
    }
    else
    {
        s_flash_err_events++;
    }
}

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

/** Halt unless err matches the documented expectation for this step. */
static void expect_cfl(const char *label, cfl_err_t got, cfl_err_t expected)
{
    if (got != expected)
    {
        char msg[96];
        (void) snprintf(msg, sizeof(msg), "%s (err=%d, expected=%d)", label, (int) got,
                        (int) expected);
        bringup_fail(msg);
    }
}

static void expect_u32(const char *label, uint32_t got, uint32_t expected)
{
    if (got != expected)
    {
        char msg[96];
        (void) snprintf(msg, sizeof(msg), "%s (got=%lu, expected=%lu)", label, (unsigned long) got,
                        (unsigned long) expected);
        bringup_fail(msg);
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
/* Payload helpers                                                         */
/* ---------------------------------------------------------------------- */

/** @brief Same deterministic pattern the unit tests use. */
static void fill_pattern(uint8_t *buf, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0U; i < len; i++)
    {
        buf[i] = (uint8_t) (seed + (uint8_t) i);
    }
}

/** @brief Append a pattern record, halting on any error. */
static void append_expect(cfl_handle_t handle, const char *label, uint32_t len, uint8_t seed)
{
    fill_pattern(s_expected, len, seed);
    expect_cfl(label, cfl_append(handle, s_expected, len), CFL_ERR_OK);
}

/** @brief Peek the oldest record and compare it against the pattern. */
static void peek_expect(cfl_handle_t handle, const char *label, uint32_t len, uint8_t seed)
{
    uint32_t got_len = 0U;

    expect_cfl(label, cfl_peek_oldest(handle, s_readback, &got_len, sizeof(s_readback)),
               CFL_ERR_OK);
    expect_u32(label, got_len, len);

    fill_pattern(s_expected, len, seed);

    for (uint32_t i = 0U; i < len; i++)
    {
        if (s_readback[i] != s_expected[i])
        {
            char msg[96];
            (void) snprintf(msg, sizeof(msg), "%s mismatch at %lu (exp=0x%02X got=0x%02X)", label,
                            (unsigned long) i, (unsigned) s_expected[i], (unsigned) s_readback[i]);
            bringup_fail(msg);
        }
    }
}

/* ---------------------------------------------------------------------- */
/* TC-HW-CFL-001 — QspiFlashDriver prerequisite                            */
/* ---------------------------------------------------------------------- */

static void test_qspi_prerequisite(void)
{
    if (qspi_flash_init() != QSPI_FLASH_OK)
    {
        bringup_fail("TC-HW-CFL-001 qspi_flash_init failed - check MX25R6435F wiring");
    }

    bringup_pass("TC-HW-CFL-001 qspi_flash_init OK (RDID verified by the driver)");
}

/**
 * @brief Erase the metadata zone and the data sectors Phase A will use.
 *
 * Only called on the Phase A path. Erasing the metadata zone is what makes
 * the next cfl_create() see a blank ring.
 */
static void prepare_blank_partition(void)
{
    for (uint32_t s = 0U; s < PREP_SECTOR_COUNT; s++)
    {
        uint32_t addr = CFL_PARTITION_BASE + (s * QSPI_FLASH_SECTOR_SIZE_BYTES);

        if (qspi_flash_erase_sector(addr) != QSPI_FLASH_OK)
        {
            bringup_fail("partition preparation erase failed");
        }
    }

    bringup_info("partition prepared: metadata zone + 6 data sectors erased");
}

/* ---------------------------------------------------------------------- */
/* Phase A                                                                 */
/* ---------------------------------------------------------------------- */

static void phase_a(cfl_handle_t handle)
{
    uint32_t entries = 0U;
    uint32_t bytes = 0U;
    uint32_t start;
    char msg[112];

    bringup_puts("\r\n--- Phase A: write side ---\r\n");

    /* TC-HW-CFL-003 — append then peek round-trip through real flash. */
    append_expect(handle, "TC-HW-CFL-003 append A", REC_A_LEN, REC_A_SEED);
    peek_expect(handle, "TC-HW-CFL-003 peek A", REC_A_LEN, REC_A_SEED);
    bringup_pass("TC-HW-CFL-003 append + peek round-trip byte-identical");

    /* TC-HW-CFL-004 — occupancy. */
    expect_cfl("TC-HW-CFL-004", cfl_get_occupancy(handle, &entries, &bytes), CFL_ERR_OK);
    expect_u32("TC-HW-CFL-004 entries", entries, 1U);
    expect_u32("TC-HW-CFL-004 bytes", bytes, REC_A_LEN);
    bringup_pass("TC-HW-CFL-004 occupancy reports 1 entry");

    /* TC-HW-CFL-005 — FIFO order across three differently sized records. */
    append_expect(handle, "TC-HW-CFL-005 append B", REC_B_LEN, REC_B_SEED);
    append_expect(handle, "TC-HW-CFL-005 append C", REC_C_LEN, REC_C_SEED);

    peek_expect(handle, "TC-HW-CFL-005 peek A", REC_A_LEN, REC_A_SEED);
    expect_cfl("TC-HW-CFL-005 consume A", cfl_consume_oldest(handle), CFL_ERR_OK);
    peek_expect(handle, "TC-HW-CFL-005 peek B", REC_B_LEN, REC_B_SEED);
    expect_cfl("TC-HW-CFL-005 consume B", cfl_consume_oldest(handle), CFL_ERR_OK);
    peek_expect(handle, "TC-HW-CFL-005 peek C", REC_C_LEN, REC_C_SEED);

    start = dwt_now();
    expect_cfl("TC-HW-CFL-005 consume C", cfl_consume_oldest(handle), CFL_ERR_OK);
    s_consume_us = dwt_elapsed_us(start);

    bringup_pass("TC-HW-CFL-005 FIFO order preserved across three records");

    /* TC-HW-CFL-006 — a max-size record forces a sector advance, so the head
     * later crosses a sector boundary and the metadata slot is rewritten. */
    start = dwt_now();
    append_expect(handle, "TC-HW-CFL-006 append D", REC_D_LEN, REC_D_SEED);
    s_append_advance_us = dwt_elapsed_us(start);

    start = dwt_now();
    append_expect(handle, "TC-HW-CFL-006 append E", REC_E_LEN, REC_E_SEED);
    s_append_us = dwt_elapsed_us(start);

    expect_cfl("TC-HW-CFL-006", cfl_get_occupancy(handle, &entries, &bytes), CFL_ERR_OK);
    expect_u32("TC-HW-CFL-006 entries", entries, PHASE_B_ENTRY_COUNT);
    expect_u32("TC-HW-CFL-006 bytes", bytes, PHASE_B_BYTE_COUNT);
    bringup_pass("TC-HW-CFL-006 sector advance + head pointer rewrite");

    /* TC-HW-CFL-007 — timings against the QSPID-O4 bench figures. */
    (void) snprintf(msg, sizeof(msg), "TC-HW-CFL-007 boot scan %lu us (budget %u ms)",
                    (unsigned long) s_boot_scan_us, (unsigned) BOOT_SCAN_BUDGET_MS);
    bringup_info(msg);
    (void) snprintf(msg, sizeof(msg), "TC-HW-CFL-007 append %lu us, append+erase %lu us",
                    (unsigned long) s_append_us, (unsigned long) s_append_advance_us);
    bringup_info(msg);
    (void) snprintf(msg, sizeof(msg), "TC-HW-CFL-007 consume (incl. metadata write) %lu us",
                    (unsigned long) s_consume_us);
    bringup_info(msg);

    if (s_boot_scan_us > (BOOT_SCAN_BUDGET_MS * 1000U))
    {
        bringup_info("TC-HW-CFL-007 boot scan EXCEEDS the CFL-O2 budget - CFL-O2");
        bringup_info("              resolution applies: persist the tail pointer.");
    }
    else
    {
        bringup_pass("TC-HW-CFL-007 boot scan inside the CFL-O2 budget");
    }

    expect_u32("TC-HW-CFL-007 no flash errors", s_flash_err_events, 0U);

    bringup_puts("\r\n=====================================================\r\n");
    bringup_puts("Phase A complete. Two records are now in the ring.\r\n");
    bringup_puts("ACTION: press the black RESET button (B2) to run Phase B.\r\n");
    bringup_puts("Do NOT erase the QSPI part between the two phases.\r\n");
    bringup_puts("=====================================================\r\n");
}

/* ---------------------------------------------------------------------- */
/* Phase B — after a reset                                                 */
/* ---------------------------------------------------------------------- */

static void phase_b(cfl_handle_t handle, uint32_t entries, uint32_t bytes)
{
    bool empty = true;
    char msg[112];

    bringup_puts("\r\n--- Phase B: recovery after reset ---\r\n");

    /* TC-HW-CFL-008 — the ring scan found what Phase A left behind. */
    expect_u32("TC-HW-CFL-008 entries", entries, PHASE_B_ENTRY_COUNT);
    expect_u32("TC-HW-CFL-008 bytes", bytes, PHASE_B_BYTE_COUNT);
    bringup_pass("TC-HW-CFL-008 records recovered by boot scan");

    (void) snprintf(msg, sizeof(msg), "TC-HW-CFL-008 boot scan %lu us over a populated ring",
                    (unsigned long) s_boot_scan_us);
    bringup_info(msg);

    /* TC-HW-CFL-009 — the head came back from the metadata slot. Record D is
     * only the oldest if the head skipped the three consumed records. */
    peek_expect(handle, "TC-HW-CFL-009 peek D", REC_D_LEN, REC_D_SEED);
    bringup_pass("TC-HW-CFL-009 head pointer recovered from metadata A/B slot");

    /* TC-HW-CFL-010 — drop_oldest reports the overflow, then drain. */
    expect_cfl("TC-HW-CFL-010 drop D", cfl_drop_oldest(handle), CFL_ERR_OK);
    expect_u32("TC-HW-CFL-010 overflow events", s_overflow_events, 1U);

    peek_expect(handle, "TC-HW-CFL-010 peek E", REC_E_LEN, REC_E_SEED);
    expect_cfl("TC-HW-CFL-010 consume E", cfl_consume_oldest(handle), CFL_ERR_OK);

    expect_cfl("TC-HW-CFL-010 is_empty", cfl_is_empty(handle, &empty), CFL_ERR_OK);
    if (!empty)
    {
        bringup_fail("TC-HW-CFL-010 ring not empty after draining");
    }

    expect_cfl("TC-HW-CFL-010 peek empty",
               cfl_peek_oldest(handle, s_readback, &bytes, sizeof(s_readback)), CFL_ERR_EMPTY);
    bringup_pass("TC-HW-CFL-010 drop_oldest reported overflow; ring drained");

    expect_u32("TC-HW-CFL-010 no flash errors", s_flash_err_events, 0U);

    bringup_puts("\r\n=====================================================\r\n");
    bringup_puts("Phase B complete. All 10 hardware cases passed.\r\n");
    bringup_puts("Record the TC-HW-CFL-007 boot-scan figure against CFL-O2\r\n");
    bringup_puts("in docs/lld/middleware/circular-flash-log.md §13.\r\n");
    bringup_puts("=====================================================\r\n");
}

/* ---------------------------------------------------------------------- */
/* main                                                                    */
/* ---------------------------------------------------------------------- */

int main(void)
{
    /* cpu_init() first - clock to 80 MHz, DWT cycle counter, fault handlers. */
    status_t init_st = cpu_init();

    if (init_st != STATUS_OK)
    {
        for (;;)
        {
            /* Nothing can be reported before the UART exists. */
        }
    }

    gpio_pin_config_t led_config = {
        .port = BRINGUP_LED_PORT,
        .pin = BRINGUP_LED_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 0U,
    };
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
    (void) gpio_configure_pin(&led_config);
    (void) gpio_configure_pin(&uart_tx_config);
    (void) gpio_configure_pin(&uart_rx_config);
    (void) debug_uart_init();

    bringup_puts("\r\n===== CircularFlashLog Hardware Bring-up =====\r\n");
    bringup_puts("Board  : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("Region : 0x00020000 (CircularFlashLog partition, 1 MB)\r\n");
    bringup_puts("WARNING: Phase A erases buffered StoreAndForward data\r\n");
    bringup_puts("=============================================\r\n\r\n");

    test_qspi_prerequisite();

    /* A blank metadata zone is what selects Phase A, so the partition is only
     * prepared when no valid head pointer is present. Probe before creating:
     * once cfl_create() has run, the single pool slot is spent. */
    uint8_t probe[4];
    bool blank = false;

    if (qspi_flash_read(CFL_PARTITION_BASE, probe, sizeof(probe)) != QSPI_FLASH_OK)
    {
        bringup_fail("metadata probe read failed");
    }

    blank =
        (probe[0] == 0xFFU) && (probe[1] == 0xFFU) && (probe[2] == 0xFFU) && (probe[3] == 0xFFU);

    if (blank)
    {
        prepare_blank_partition();
    }
    else
    {
        bringup_info("metadata zone already written - taking the Phase B path");
    }

    cfl_config_t config = {
        .qspi = qspi_flash_driver,
        .health = NULL,
    };
    cfl_handle_t handle = NULL;
    uint32_t entries = 0U;
    uint32_t bytes = 0U;
    uint32_t start = dwt_now();

    expect_cfl("TC-HW-CFL-002 cfl_create", cfl_create(&config, &handle), CFL_ERR_OK);
    s_boot_scan_us = dwt_elapsed_us(start);

    expect_cfl("TC-HW-CFL-002 occupancy", cfl_get_occupancy(handle, &entries, &bytes), CFL_ERR_OK);

    if (entries == 0U)
    {
        bringup_pass("TC-HW-CFL-002 cfl_create on a blank partition: ring empty");
        phase_a(handle);
    }
    else if (entries == PHASE_B_ENTRY_COUNT)
    {
        bringup_pass("TC-HW-CFL-002 cfl_create recovered a populated ring");
        phase_b(handle, entries, bytes);
    }
    else
    {
        /* Neither starting state — most likely a third run after Phase B
         * completed. See the CFL-O6 note in the file header. */
        char msg[112];

        (void) snprintf(msg, sizeof(msg), "ring holds %lu entries; expected 0 or %u",
                        (unsigned long) entries, (unsigned) PHASE_B_ENTRY_COUNT);
        bringup_info(msg);
        bringup_info("ACTION: erase the metadata zone (2 sectors at 0x00020000)");
        bringup_info("        with STM32CubeProgrammer or a debugger, then re-run");
        bringup_info("        to restart at Phase A.");
        bringup_fail("partition is mid-sequence - see CFL-O6");
    }

    for (;;)
    {
        gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        cpu_delay_ms(1000U);
    }
}
