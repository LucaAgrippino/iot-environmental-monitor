/**
 * @file main_test_store_and_forward.c
 * @brief Hardware bring-up test for StoreAndForward (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. Add cloud_publisher (the current harness) to the `excluding=` list of
 *      the `integration-tests` source entry and remove store_and_forward.
 *   2. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/store_and_forward.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * Hardware outputs:
 *   USART1 TX  PB6   115 200 8N1  Human-readable test results
 *   LD2 green  PA5                Heartbeat on completion, fast blink on halt
 *   QUADSPI    PE10..PE15  AF10   MX25R6435F, 8 MB, indirect 1-1-1
 *
 * DESTRUCTIVE.  Operates on the real CircularFlashLog partition, byte offsets
 * 0x0002_0000 - 0x0011_FFFF.  Phase A erases the metadata zone and the first
 * data sectors; Phase B fills the whole ring.  Anything StoreAndForward had
 * buffered there is lost.  ConfigStore, CertStore and the OTA staging area are
 * never touched.
 *
 * TWO-PHASE TEST.  The CircularFlashLog pool holds a single instance, so
 * persistence has to be proven across a real reset:
 *
 *   Phase A runs on a blank metadata zone.  It exercises enqueue, dequeue and
 *   confirm, leaves three known entries behind with the first one dequeued but
 *   NOT confirmed, and asks for a RESET (button B2, or an OpenOCD reset).
 *
 *   Phase B runs when the ring holds those three entries.  It proves they
 *   survived, that the unconfirmed entry is delivered again, drains the ring,
 *   then fills all 254 sectors to drive the drop-oldest policy.  It finishes by
 *   erasing the metadata zone so the next run starts at Phase A again.
 *
 *   Any other starting state is reset by erasing the partition head and asking
 *   for a RESET.
 *
 * Automated test sequence:
 *   Phase A
 *   TC-HW-SAF-001  qspi_flash_init() returns QSPI_FLASH_OK (prerequisite).
 *   TC-HW-SAF-002  cfl_create() + saf_create() on a blank partition: empty,
 *                   occupancy 0 pushed to HealthMonitor (read from its snapshot).
 *   TC-HW-SAF-003  enqueue -> dequeue -> confirm round trip is byte-identical
 *                   (topic, QoS, payload) off the physical part.
 *   TC-HW-SAF-004  FIFO order across three entries of differing sizes.
 *   TC-HW-SAF-005  A second dequeue before confirm returns the same entry.
 *   TC-HW-SAF-006  A maximum-size entry (64-char topic, SAF_PAYLOAD_MAX bytes)
 *                   round-trips, forcing a CFL sector advance.
 *   TC-HW-SAF-007  DWT timings for enqueue, enqueue-with-erase, dequeue and
 *                   confirm.
 *   Phase B (after RESET)
 *   TC-HW-SAF-008  saf_create() recovers three entries by ring scan.
 *   TC-HW-SAF-009  The entry dequeued but never confirmed is delivered again.
 *   TC-HW-SAF-010  The three entries drain in order, then SAF_ERR_EMPTY.
 *   TC-HW-SAF-011  Filling the ring (254 maximum-size entries) and enqueueing
 *                   one more drops exactly one entry and raises exactly one
 *                   CFL buffer-overflow event; occupancy stays at capacity.
 *
 * Integration checklist:
 *   [ ] Every entry reads back byte-identical, oldest first
 *   [ ] Phase B recovers the three entries and re-delivers the unconfirmed one
 *   [ ] Overflow event count is 1 after the 255th enqueue
 *   [ ] Record the TC-HW-SAF-007 timings against the CFL figures
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
#include "health_monitor/health_monitor.h"
#include "led/led_driver.h"
#include "qspi_flash/qspi_flash.h"
#include "store_and_forward/store_and_forward.h"

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
/* Test region and sizes                                                   */
/* ---------------------------------------------------------------------- */

/** Device byte offset of the CircularFlashLog partition. */
#define CFL_PARTITION_BASE (0x00020000UL)

/** Metadata sectors plus the first data sectors Phase A uses. */
#define PREP_SECTOR_COUNT (8U)

/** Worst-case entry count: one maximum-size entry per data sector. */
#define SAF_TEST_CAPACITY (254U)

#define PERSIST_COUNT (3U)

static const char *const PERSIST_TOPIC[PERSIST_COUNT] = {"gw/test/a", "gw/test/b", "gw/test/c"};
static const uint16_t PERSIST_LEN[PERSIST_COUNT] = {64U, 300U, 1000U};
static const uint8_t PERSIST_QOS[PERSIST_COUNT] = {0U, 1U, 0U};
static const uint8_t PERSIST_SEED[PERSIST_COUNT] = {0xA0U, 0xB0U, 0xC0U};

/* ---------------------------------------------------------------------- */
/* State and spies                                                         */
/* ---------------------------------------------------------------------- */

static uint8_t s_payload[SAF_PAYLOAD_MAX];
static uint8_t s_expected[SAF_PAYLOAD_MAX];

/* ---------------------------------------------------------------------- */
/* Reporting helpers — built on DebugUartDriver.                           */
/* ---------------------------------------------------------------------- */

static void bringup_puts(const char *s)
{
    size_t len = strlen(s);

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

static void expect_saf(const char *label, saf_err_t got, saf_err_t expected)
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
/* HealthMonitor: the real one, read through its snapshot (WP1 PR-B).       */
/* The image also contains health_monitor.c, so a private spy with the real  */
/* function names would be a duplicate definition.                          */
/* ---------------------------------------------------------------------- */

#define LD2_PIN (14U)
#define LD2_RED_PIN (5U)

static health_monitor_handle_t s_hm;

static const led_pin_t k_gw_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = (uint8_t) GPIO_PORT_B,
                   .pin = (uint8_t) LD2_PIN,
                   .active_high = true,
                   .fitted = true},
    [LED_RED] = {.port = (uint8_t) GPIO_PORT_A,
                 .pin = LD2_RED_PIN,
                 .active_high = true,
                 .fitted = false}, /* LD1 / SB1 - shared with SPI1_SCK */
};

/** @brief Create the HealthMonitor the modules under test report to. */
static void bringup_health_start(void)
{
    const health_monitor_config_t hm_config = {.boot_reset_cause = LIFECYCLE_RESET_SOFT};

    if (led_init(k_gw_led_pins, (uint8_t) LED_COUNT) != LED_ERR_OK)
    {
        bringup_fail("led_init");
    }
    if (health_monitor_create(&hm_config, &s_hm) != HEALTH_MONITOR_ERR_OK)
    {
        bringup_fail("health_monitor_create");
    }
}

static health_snapshot_t s_snap;

/** @brief Buffer overflows HealthMonitor has counted (HEALTH_EVENT_BUFFER_OVERFLOW). */
static uint32_t hm_overflow_count(void)
{
    (void) health_monitor_get_snapshot(s_hm, &s_snap);
    return s_snap.buffer_overflow_count;
}

/** @brief Entries HealthMonitor was last told are buffered
 * (health_monitor_update_buffer_occupancy). */
static uint32_t hm_entry_count(void)
{
    (void) health_monitor_get_snapshot(s_hm, &s_snap);
    return s_snap.buffer_entry_count;
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
/* DWT timing — cpu_init() enables the cycle counter.                      */
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

static void report_us(const char *label, uint32_t start)
{
    char msg[96];

    (void) snprintf(msg, sizeof(msg), "%s %lu us", label, (unsigned long) dwt_elapsed_us(start));
    bringup_info(msg);
}

/* ---------------------------------------------------------------------- */
/* Payload helpers                                                         */
/* ---------------------------------------------------------------------- */

static void fill_pattern(uint8_t *buf, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0U; i < len; i++)
    {
        buf[i] = (uint8_t) (seed + (uint8_t) i);
    }
}

static void enqueue_expect(saf_handle_t h, const char *label, const char *topic, uint16_t len,
                           uint8_t qos, uint8_t seed)
{
    fill_pattern(s_payload, len, seed);
    expect_saf(label, saf_enqueue(h, topic, s_payload, len, qos), SAF_ERR_OK);
}

/** @brief Dequeue the oldest entry and compare it with the pattern. */
static void dequeue_expect(saf_handle_t h, const char *label, const char *topic, uint16_t len,
                           uint8_t qos, uint8_t seed)
{
    const saf_entry_t *e = NULL;

    expect_saf(label, saf_dequeue(h, &e), SAF_ERR_OK);

    if (strcmp(e->topic, topic) != 0)
    {
        bringup_fail(label);
    }

    expect_u32(label, e->qos, qos);
    expect_u32(label, e->payload_len, len);

    fill_pattern(s_expected, len, seed);

    for (uint32_t i = 0U; i < len; i++)
    {
        if (e->payload[i] != s_expected[i])
        {
            char msg[96];

            (void) snprintf(msg, sizeof(msg), "%s mismatch at %lu (exp=0x%02X got=0x%02X)", label,
                            (unsigned long) i, (unsigned) s_expected[i], (unsigned) e->payload[i]);
            bringup_fail(msg);
        }
    }
}

/** @brief Erase the metadata zone and the first data sectors. */
static void erase_partition_head(void)
{
    for (uint32_t s = 0U; s < PREP_SECTOR_COUNT; s++)
    {
        uint32_t addr = CFL_PARTITION_BASE + (s * QSPI_FLASH_SECTOR_SIZE_BYTES);

        if (qspi_flash_erase_sector(addr) != QSPI_FLASH_OK)
        {
            bringup_fail("partition erase failed");
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Phase A                                                                 */
/* ---------------------------------------------------------------------- */

static void phase_a(saf_handle_t h)
{
    const saf_entry_t *e = NULL;
    char max_topic[SAF_TOPIC_MAX + 1U];
    uint32_t t0;

    bringup_puts("\r\n--- Phase A: write side ---\r\n");

    /* TC-HW-SAF-003 */
    enqueue_expect(h, "TC-HW-SAF-003 enqueue", "gw/test/rt", 100U, 1U, 0x31U);
    dequeue_expect(h, "TC-HW-SAF-003 dequeue", "gw/test/rt", 100U, 1U, 0x31U);
    expect_saf("TC-HW-SAF-003 confirm", saf_confirm(h), SAF_ERR_OK);
    bringup_pass("TC-HW-SAF-003 enqueue/dequeue/confirm round-trip byte-identical");

    /* TC-HW-SAF-004 and 005 */
    enqueue_expect(h, "TC-HW-SAF-004 enqueue 1", "gw/test/f1", 50U, 0U, 0x41U);
    enqueue_expect(h, "TC-HW-SAF-004 enqueue 2", "gw/test/f2", 500U, 1U, 0x42U);
    enqueue_expect(h, "TC-HW-SAF-004 enqueue 3", "gw/test/f3", 2000U, 0U, 0x43U);
    dequeue_expect(h, "TC-HW-SAF-004 dequeue 1", "gw/test/f1", 50U, 0U, 0x41U);
    expect_saf("TC-HW-SAF-004 confirm 1", saf_confirm(h), SAF_ERR_OK);
    dequeue_expect(h, "TC-HW-SAF-004 dequeue 2", "gw/test/f2", 500U, 1U, 0x42U);

    expect_saf("TC-HW-SAF-005 re-dequeue", saf_dequeue(h, &e), SAF_ERR_OK);
    expect_u32("TC-HW-SAF-005 re-dequeue length", e->payload_len, 500U);
    bringup_pass("TC-HW-SAF-005 dequeue before confirm returns the same entry");

    expect_saf("TC-HW-SAF-004 confirm 2", saf_confirm(h), SAF_ERR_OK);
    dequeue_expect(h, "TC-HW-SAF-004 dequeue 3", "gw/test/f3", 2000U, 0U, 0x43U);
    expect_saf("TC-HW-SAF-004 confirm 3", saf_confirm(h), SAF_ERR_OK);
    expect_saf("TC-HW-SAF-004 empty", saf_dequeue(h, &e), SAF_ERR_EMPTY);
    bringup_pass("TC-HW-SAF-004 FIFO order preserved across three entries");

    /* TC-HW-SAF-007 - small-entry timings. */
    fill_pattern(s_payload, 200U, 0x71U);
    t0 = dwt_now();
    expect_saf("TC-HW-SAF-007 enqueue", saf_enqueue(h, "gw/test/tm", s_payload, 200U, 0U),
               SAF_ERR_OK);
    report_us("TC-HW-SAF-007 enqueue", t0);

    t0 = dwt_now();
    expect_saf("TC-HW-SAF-007 dequeue", saf_dequeue(h, &e), SAF_ERR_OK);
    report_us("TC-HW-SAF-007 dequeue", t0);

    t0 = dwt_now();
    expect_saf("TC-HW-SAF-007 confirm", saf_confirm(h), SAF_ERR_OK);
    report_us("TC-HW-SAF-007 confirm", t0);

    /* TC-HW-SAF-006 - maximum-size entry.  It also separates the entries above
     * from the persistence entries below, so none of them can be replayed after
     * the reset (CFL-O6). */
    (void) memset(max_topic, 'm', SAF_TOPIC_MAX);
    max_topic[SAF_TOPIC_MAX] = '\0';
    fill_pattern(s_payload, SAF_PAYLOAD_MAX, 0x61U);

    t0 = dwt_now();
    expect_saf("TC-HW-SAF-006 enqueue max",
               saf_enqueue(h, max_topic, s_payload, SAF_PAYLOAD_MAX, 1U), SAF_ERR_OK);
    report_us("TC-HW-SAF-007 enqueue with sector erase", t0);

    dequeue_expect(h, "TC-HW-SAF-006 dequeue max", max_topic, SAF_PAYLOAD_MAX, 1U, 0x61U);
    expect_saf("TC-HW-SAF-006 confirm max", saf_confirm(h), SAF_ERR_OK);
    bringup_pass("TC-HW-SAF-006 maximum-size entry round-trips (sector advance)");
    bringup_pass("TC-HW-SAF-007 timings reported");

    /* Persistence entries for Phase B. */
    for (uint32_t i = 0U; i < PERSIST_COUNT; i++)
    {
        enqueue_expect(h, "persistence enqueue", PERSIST_TOPIC[i], PERSIST_LEN[i], PERSIST_QOS[i],
                       PERSIST_SEED[i]);
    }

    dequeue_expect(h, "persistence dequeue", PERSIST_TOPIC[0], PERSIST_LEN[0], PERSIST_QOS[0],
                   PERSIST_SEED[0]);

    bringup_puts("\r\n=====================================================\r\n");
    bringup_puts("Phase A complete. Three entries are in the ring; the first\r\n");
    bringup_puts("was dequeued and deliberately NOT confirmed.\r\n");
    bringup_puts("ACTION: press the black RESET button (B2) to run Phase B.\r\n");
    bringup_puts("Do NOT erase the QSPI part between the two phases.\r\n");
    bringup_puts("=====================================================\r\n");
}

/* ---------------------------------------------------------------------- */
/* Phase B                                                                 */
/* ---------------------------------------------------------------------- */

static void phase_b(saf_handle_t h)
{
    char msg[96];
    const saf_entry_t *e = NULL;
    char max_topic[SAF_TOPIC_MAX + 1U];
    uint32_t count = 0U;
    uint32_t cap = 0U;
    uint32_t t0;

    bringup_puts("\r\n--- Phase B: recovery after reset ---\r\n");

    /* TC-HW-SAF-008 */
    expect_saf("TC-HW-SAF-008 occupancy", saf_get_occupancy(h, &count, &cap), SAF_ERR_OK);
    expect_u32("TC-HW-SAF-008 entry count", count, PERSIST_COUNT);
    bringup_pass("TC-HW-SAF-008 three entries recovered by ring scan");

    /* TC-HW-SAF-009 and 010 */
    for (uint32_t i = 0U; i < PERSIST_COUNT; i++)
    {
        dequeue_expect(h, (i == 0U) ? "TC-HW-SAF-009 redelivery" : "TC-HW-SAF-010 drain",
                       PERSIST_TOPIC[i], PERSIST_LEN[i], PERSIST_QOS[i], PERSIST_SEED[i]);
        expect_saf("confirm", saf_confirm(h), SAF_ERR_OK);

        if (i == 0U)
        {
            bringup_pass("TC-HW-SAF-009 unconfirmed entry delivered again");
        }
    }

    expect_saf("TC-HW-SAF-010 empty", saf_dequeue(h, &e), SAF_ERR_EMPTY);
    bringup_pass("TC-HW-SAF-010 entries drained in order, then SAF_ERR_EMPTY");

    /* TC-HW-SAF-011 - fill the ring, then overflow it once.  One maximum-size
     * entry per data sector, so 254 entries fill it. */
    (void) memset(max_topic, 'o', SAF_TOPIC_MAX);
    max_topic[SAF_TOPIC_MAX] = '\0';
    const uint32_t overflow_base = hm_overflow_count();

    bringup_info("TC-HW-SAF-011 filling 254 sectors - about a minute");
    t0 = dwt_now();

    for (uint32_t i = 0U; i < SAF_TEST_CAPACITY; i++)
    {
        fill_pattern(s_payload, SAF_PAYLOAD_MAX, (uint8_t) i);
        expect_saf("TC-HW-SAF-011 fill", saf_enqueue(h, max_topic, s_payload, SAF_PAYLOAD_MAX, 0U),
                   SAF_ERR_OK);
    }

    (void) snprintf(msg, sizeof(msg), "TC-HW-SAF-011 filled in %lu ms",
                    (unsigned long) (dwt_elapsed_us(t0) / 1000U));
    bringup_info(msg);

    expect_u32("TC-HW-SAF-011 no overflow while filling", hm_overflow_count() - overflow_base, 0U);
    expect_u32("TC-HW-SAF-011 occupancy at capacity", hm_entry_count(), SAF_TEST_CAPACITY);

    fill_pattern(s_payload, SAF_PAYLOAD_MAX, 0xEEU);
    expect_saf("TC-HW-SAF-011 overflow enqueue",
               saf_enqueue(h, max_topic, s_payload, SAF_PAYLOAD_MAX, 0U), SAF_ERR_OK);

    expect_u32("TC-HW-SAF-011 one overflow event", hm_overflow_count() - overflow_base, 1U);
    expect_u32("TC-HW-SAF-011 occupancy stays at capacity", hm_entry_count(), SAF_TEST_CAPACITY);

    /* The oldest entry (seed 1) must now lead; seed 0 was dropped. */
    dequeue_expect(h, "TC-HW-SAF-011 oldest after drop", max_topic, SAF_PAYLOAD_MAX, 0U, 0x01U);
    bringup_pass("TC-HW-SAF-011 full ring dropped exactly one entry, oldest first");

    /* Leave the partition blank so the next run starts at Phase A. */
    erase_partition_head();

    bringup_puts("\r\n=====================================================\r\n");
    bringup_puts("Phase B complete. All 11 hardware cases passed.\r\n");
    bringup_puts("The metadata zone was erased; the next run starts at Phase A.\r\n");
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

    bringup_puts("\r\n===== StoreAndForward Hardware Bring-up =====\r\n");
    bringup_puts("Board  : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("Region : 0x00020000 (CircularFlashLog partition, 1 MB)\r\n");
    bringup_puts("WARNING: erases buffered StoreAndForward data\r\n");
    bringup_puts("==============================================\r\n\r\n");

    if (qspi_flash_init() != QSPI_FLASH_OK)
    {
        bringup_fail("TC-HW-SAF-001 qspi_flash_init failed - check MX25R6435F wiring");
    }

    bringup_pass("TC-HW-SAF-001 qspi_flash_init OK (RDID verified by the driver)");

    /* A blank metadata zone selects Phase A.  Probe before cfl_create(): once
     * it has run, the single pool slot is spent. */
    uint8_t probe[4];

    if (qspi_flash_read(CFL_PARTITION_BASE, probe, sizeof(probe)) != QSPI_FLASH_OK)
    {
        bringup_fail("metadata probe read failed");
    }

    bool blank =
        (probe[0] == 0xFFU) && (probe[1] == 0xFFU) && (probe[2] == 0xFFU) && (probe[3] == 0xFFU);

    if (blank)
    {
        erase_partition_head();
        bringup_info("partition prepared: metadata zone + 6 data sectors erased");
    }
    else
    {
        bringup_info("metadata zone already written - checking for the Phase B state");
    }

    bringup_health_start();

    cfl_config_t cfl_config = {
        .qspi = qspi_flash_driver,
        .health = s_hm,
    };
    cfl_handle_t cfl = NULL;

    if (cfl_create(&cfl_config, &cfl) != CFL_ERR_OK)
    {
        bringup_fail("cfl_create failed");
    }

    saf_config_t saf_config = {
        .cfl = cfl,
        .health = s_hm,
        .capacity = SAF_TEST_CAPACITY,
    };
    saf_handle_t saf = NULL;

    expect_saf("TC-HW-SAF-002 saf_create", saf_create(&saf_config, &saf), SAF_ERR_OK);

    uint32_t entries = 0U;
    uint32_t cap = 0U;

    expect_saf("TC-HW-SAF-002 occupancy", saf_get_occupancy(saf, &entries, &cap), SAF_ERR_OK);
    expect_u32("TC-HW-SAF-002 capacity", cap, SAF_TEST_CAPACITY);

    if (blank && (entries == 0U))
    {
        expect_u32("TC-HW-SAF-002 pushed count", hm_entry_count(), 0U);
        bringup_pass("TC-HW-SAF-002 create on a blank partition: empty, occupancy 0 pushed");
        phase_a(saf);
    }
    else if (entries == PERSIST_COUNT)
    {
        bringup_pass("TC-HW-SAF-002 create recovered a populated ring");
        phase_b(saf);
    }
    else
    {
        /* Neither starting state - leftovers from another test, or a run that
         * stopped part-way.  The RAM state is stale after the erase, so a RESET
         * is needed before Phase A. */
        char msg[96];

        (void) snprintf(msg, sizeof(msg), "ring holds %lu entries; expected 0 or %u",
                        (unsigned long) entries, (unsigned) PERSIST_COUNT);
        bringup_info(msg);
        bringup_info("erasing the metadata zone and the first data sectors");
        erase_partition_head();
        bringup_info("ACTION: press RESET (B2) to start at Phase A.");
    }

    for (;;)
    {
        gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        cpu_delay_ms(1000U);
    }
}
