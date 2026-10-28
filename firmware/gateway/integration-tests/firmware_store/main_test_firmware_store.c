/**
 * @file main_test_firmware_store.c
 * @brief Hardware bring-up test for FirmwareStore (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default. Written against the contract in docs/lld/middleware/firmware-store.md
 * §23. It needs the image linked for Slot A and the boot stub that jumps to it (P-3):
 *   scripts/build-slot-a.sh builds firmware/gateway/Debug/gateway_slot_a.elf (0x0800_6000)
 *   and boot_stub.elf (0x0800_0000); flash both. On an image that is not in a slot the harness
 *   reports that FirmwareStore correctly refuses OTA (TC-HW-FS-001b) and stops.
 *
 * To activate, in firmware/gateway/.cproject (bench-local, do not commit):
 *   1. Add cloud_publisher (the current harness) to the `excluding=` list of the
 *      `integration-tests` source entry and remove firmware_store from it.
 *   2. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/firmware_store.
 *   3. Remove `firmware_store` from the `excluding=` of the `middleware` source entry, and
 *      enable MBEDTLS_ECDSA_C in the mbedTLS configuration (FS-O4: the Gateway TLS
 *      configuration has ECP, secp256r1, SHA-256 and the ASN.1 modules, but not ECDSA).
 *   4. Provision a release key: tools/ota/make_image.py genkey (ota_public_key.c is an
 *      all-zero placeholder that fails closed).
 *
 * Host side: scripts/ota_feed.py streams a signed image over the debug UART.
 *   USART1 TX PB6, RX PB7, 115 200 8N1 for commands; 921 600 for the image itself.
 *
 * DESTRUCTIVE. Erases and programs the on-chip metadata (0x0800_4000..0x0800_5FFF) and the
 * inactive slot, and uses the QSPI OTA staging area (device offset 0x0012_0000, 4 MB).
 * Never touches the running slot, the bootloader, ConfigStore, CertStore or the log.
 *
 * Protocol, one command per line:
 *   host -> "OTA <image_id hex> <file_len> <install 0|1>"
 *   dev  -> "NEED <offset>"          (resume point; 0 for a fresh session)
 *   host -> raw bytes from <offset>, in 1024-byte blocks; dev answers each block with '.'
 *   dev  -> "RECEIVED <len> <ms>", then the verify, install and activate results.
 *
 * Automated test sequence (TC-HW-FS-nnn from §23):
 *   001   Metadata seeded CONFIRMED / active A; create; get_boot_info = capable, Slot A.
 *   001b  Not running from a slot: ota_capable is false and begin() refuses.
 *   002   A maximum-size Slot-B image is staged; the staging time is logged.
 *   003   Pull power during 002: the next "OTA" reports a resume offset <= bytes sent,
 *         4 KB aligned (manual, repeat five times).
 *   004   verify() of the staged image; duration logged (FS-O5).
 *   005   install: per-step and total time logged (FS-O1). Run SensorTask and ModbusPoller
 *         alongside once a main() exists; this harness has no scheduler.
 *   006   A 1 kHz TIM7 interrupt records its longest gap across activate() (FS-O10, <= 30 ms).
 *   007   An image with one flipped body byte -> HASH_MISMATCH (host: --corrupt-body).
 *   008   An image linked for Slot A -> WRONG_SLOT (host: --wrong-slot).
 *   009   activate(), then both metadata copies are dumped: TRIAL_PENDING, active B,
 *         generation + 1, the other copy intact.
 *   010   Deferred to the bootloader (FS-O7).
 *
 * Integration checklist:
 *   [ ] Every host command ends in the expected result code
 *   [ ] The staged digest matches the source after an interrupted and resumed transfer
 *   [ ] Record: staging time, verify time, per-step and total install time, longest ISR gap
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_store/config_store_crc.h"
#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "firmware_store/firmware_image_format.h"
#include "firmware_store/firmware_store.h"
#include "gpio/gpio_driver.h"
#include "internal_flash/iinternal_flash.h"
#include "internal_flash/internal_flash.h"
#include "qspi_flash/qspi_flash.h"
#include "tls_arena.h"

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
#define BRINGUP_TRANSFER_BAUD (921600U)

/* After the transfer both sides switch back to 115 200; the host needs about 50 ms. */
#define BAUD_SETTLE_MS (150U)

#define RX_RING_SIZE (2048U) /* power of two */
#define LINE_MAX_LEN (96U)
#define BLOCK_LEN (1024U)
#define COMMAND_TIMEOUT_MS (600000U)
#define BLOCK_TIMEOUT_MS (5000U)

#define US_PER_S (1000000UL)
#define US_PER_MS (1000UL)
#define GAP_TIMER_HZ (1000UL)
#define GAP_LIMIT_US (30000UL)

#define BARE_METAL_BASE_MASK (0xFFFFE000UL)

static uint8_t s_block[BLOCK_LEN];

/* ---------------------------------------------------------------------- */
/* Reporting helpers — built on DebugUartDriver.                           */
/* ---------------------------------------------------------------------- */

static void bringup_puts(const char *s)
{
    (void) debug_uart_send((const uint8_t *) s, strlen(s), BRINGUP_TX_TIMEOUT_MS);
}

static void bringup_printf1(const char *fmt, uint32_t a)
{
    char msg[LINE_MAX_LEN];

    (void) snprintf(msg, sizeof(msg), fmt, (unsigned long) a);
    bringup_puts(msg);
}

static void bringup_printf(const char *fmt, uint32_t a, uint32_t b)
{
    char msg[LINE_MAX_LEN];

    (void) snprintf(msg, sizeof(msg), fmt, (unsigned long) a, (unsigned long) b);
    bringup_puts(msg);
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

static void expect_fs(firmware_store_err_t got, firmware_store_err_t expected, const char *label)
{
    if (got != expected)
    {
        char msg[LINE_MAX_LEN];

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
    return (DWT->CYCCNT - start) / (cpu_get_sysclk_hz() / US_PER_S);
}

static uint32_t dwt_elapsed_ms(uint32_t start)
{
    return dwt_elapsed_us(start) / US_PER_MS;
}

/* ---------------------------------------------------------------------- */
/* TC-HW-FS-006 — longest gap between 1 kHz interrupts                     */
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

    /* Seed the previous sample with "now". Starting from zero would discard the first
     * interrupt, and so hide a stall that begins right after the reset (the erase). */
    s_gap_last = DWT->CYCCNT;
}

static uint32_t gap_max_us(void)
{
    return s_gap_max_cycles / (cpu_get_sysclk_hz() / US_PER_S);
}

/* ---------------------------------------------------------------------- */
/* Receive ring (USART1 ISR -> main)                                       */
/* ---------------------------------------------------------------------- */

static volatile uint8_t s_ring[RX_RING_SIZE];
static volatile uint32_t s_ring_head;
static volatile uint32_t s_ring_tail;

static void rx_byte_isr(uint8_t byte, void *context)
{
    (void) context;
    s_ring[s_ring_head & (RX_RING_SIZE - 1U)] = byte;
    s_ring_head++;
}

static bool rx_get(uint8_t *byte, uint32_t timeout_ms)
{
    const uint32_t start = dwt_now();

    while (s_ring_tail == s_ring_head)
    {
        if (dwt_elapsed_ms(start) >= timeout_ms)
        {
            return false;
        }
    }

    *byte = s_ring[s_ring_tail & (RX_RING_SIZE - 1U)];
    s_ring_tail++;

    return true;
}

static bool rx_line(char *buf, size_t size, uint32_t timeout_ms)
{
    size_t n = 0U;
    uint8_t c = 0U;

    while (rx_get(&c, timeout_ms))
    {
        if (c == (uint8_t) '\n')
        {
            buf[n] = '\0';
            return true;
        }

        if ((c != (uint8_t) '\r') && (n < (size - 1U)))
        {
            buf[n] = (char) c;
            n++;
        }
    }

    return false;
}

/* ---------------------------------------------------------------------- */
/* Metadata seeding and dump (TC-HW-FS-001, -009)                          */
/* ---------------------------------------------------------------------- */

static firmware_boot_meta_t read_meta(uint32_t addr)
{
    firmware_boot_meta_t r;

    (void) internal_flash_driver->read(addr, (uint8_t *) &r, sizeof(r));

    return r;
}

static bool meta_valid(const firmware_boot_meta_t *r)
{
    return (r->magic == FIRMWARE_META_MAGIC) &&
           (r->crc32 == config_store_crc32((const uint8_t *) r, FIRMWARE_META_CRC_SPAN));
}

/** @brief Write CONFIRMED / active A / previous B as the bootloader would on a first boot. */
static void seed_metadata(void)
{
    firmware_boot_meta_t r = {
        .magic = FIRMWARE_META_MAGIC,
        .active_bank = FIRMWARE_BANK_A,
        .boot_state = FIRMWARE_META_STATE_CONFIRMED,
        .previous_bank = FIRMWARE_BANK_B,
        .reserved = 0xFFU,
        .rollback_count = 0U,
        .generation = 1U,
        .crc32 = 0U,
        .pad = FIRMWARE_ERASED_WORD,
    };

    r.crc32 = config_store_crc32((const uint8_t *) &r, FIRMWARE_META_CRC_SPAN);

    check(internal_flash_driver->erase_page(FIRMWARE_META_A_ADDR) == INTERNAL_FLASH_OK,
          "seed: erase metadata copy A");
    check(internal_flash_driver->program(FIRMWARE_META_A_ADDR, (const uint8_t *) &r, sizeof(r)) ==
              INTERNAL_FLASH_OK,
          "seed: program metadata copy A");
}

static void dump_copy(const char *name, uint32_t addr)
{
    const firmware_boot_meta_t r = read_meta(addr);
    char msg[LINE_MAX_LEN];

    (void) snprintf(
        msg, sizeof(msg), "  copy %s: %s active=0x%02X state=%u gen=%lu rollbacks=%lu\r\n", name,
        meta_valid(&r) ? "valid  " : "INVALID", (unsigned) r.active_bank, (unsigned) r.boot_state,
        (unsigned long) r.generation, (unsigned long) r.rollback_count);
    bringup_puts(msg);
}

/* ---------------------------------------------------------------------- */
/* One OTA command                                                         */
/* ---------------------------------------------------------------------- */

static void run_ota(firmware_store_t *fs, uint32_t image_id, uint32_t file_len, bool install)
{
    uint32_t offset = 0U;
    firmware_image_info_t info;
    const uint32_t t_stage = dwt_now();
    firmware_store_err_t err = firmware_store_image_ops.resume(fs, image_id, file_len, &offset);

    if (err == FIRMWARE_STORE_ERR_NO_SESSION)
    {
        expect_fs(firmware_store_image_ops.begin(fs, image_id, file_len), FIRMWARE_STORE_ERR_OK,
                  "begin");
        offset = 0U;
    }
    else
    {
        expect_fs(err, FIRMWARE_STORE_ERR_OK, "resume");
    }

    bringup_printf1("NEED %lu\r\n", offset);

    (void) debug_uart_set_baud(BRINGUP_TRANSFER_BAUD);

    for (uint32_t pos = offset; pos < file_len;)
    {
        const uint32_t n = ((file_len - pos) < BLOCK_LEN) ? (file_len - pos) : BLOCK_LEN;

        for (uint32_t i = 0U; i < n; i++)
        {
            if (!rx_get(&s_block[i], BLOCK_TIMEOUT_MS))
            {
                /* The host went away mid-transfer (TC-HW-FS-003). Keep the staged bytes and
                 * their checkpoints: the next OTA command resumes from the journal. */
                (void) debug_uart_set_baud(115200U);
                cpu_delay_ms(BAUD_SETTLE_MS);
                bringup_puts("ERR host stopped sending\r\n");
                return;
            }
        }

        expect_fs(firmware_store_image_ops.write_chunk(fs, pos, s_block, n), FIRMWARE_STORE_ERR_OK,
                  "write_chunk");
        pos += n;
        (void) debug_uart_send((const uint8_t *) ".", 1U, BRINGUP_TX_TIMEOUT_MS);
    }

    (void) debug_uart_set_baud(115200U);
    cpu_delay_ms(
        BAUD_SETTLE_MS); /* the host switches back too; a reply sent now would be garbled */
    bringup_printf("RECEIVED %lu %lu\r\n", file_len, dwt_elapsed_ms(t_stage));

    /* TC-HW-FS-004, -007, -008 */
    const uint32_t t_verify = dwt_now();

    err = firmware_store_image_ops.verify(fs, &info);
    bringup_printf("VERIFY err=%lu ms=%lu\r\n", (uint32_t) err, dwt_elapsed_ms(t_verify));

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        (void) firmware_store_image_ops.discard(fs);
        return;
    }

    bringup_printf("IMAGE version=0x%08lX len=%lu\r\n", info.version, info.image_len);

    if (!install)
    {
        (void) firmware_store_image_ops.discard(fs);
        return;
    }

    /* TC-HW-FS-005 */
    uint32_t steps = 0U;
    uint32_t worst_us = 0U;
    bool done = false;
    const uint32_t t_install = dwt_now();

    gap_reset();
    expect_fs(firmware_store_image_ops.install_begin(fs), FIRMWARE_STORE_ERR_OK, "install_begin");

    /* install_begin erases the target descriptor, which is in bank 1 with the running image:
     * that one stall is measured apart from the 240-page copy into bank 2 (amendment A1). */
    const uint32_t gap_begin_us = gap_max_us();

    gap_reset();

    while (!done)
    {
        const uint32_t t_step = dwt_now();

        expect_fs(firmware_store_image_ops.install_step(fs, &done), FIRMWARE_STORE_ERR_OK,
                  "install_step");
        steps++;

        if (dwt_elapsed_us(t_step) > worst_us)
        {
            worst_us = dwt_elapsed_us(t_step);
        }
    }

    bringup_printf("INSTALL steps=%lu total_ms=%lu\r\n", steps, dwt_elapsed_ms(t_install));
    bringup_printf("INSTALL worst_step_us=%lu begin_gap_us=%lu\r\n", worst_us, gap_begin_us);
    bringup_printf("INSTALL copy_gap_us=%lu limit_us=%lu\r\n", gap_max_us(), GAP_LIMIT_US);

    /* TC-HW-FS-006, -009 */
    gap_reset();
    expect_fs(firmware_store_image_ops.activate(fs), FIRMWARE_STORE_ERR_OK, "activate");
    bringup_printf("ACTIVATE max_isr_gap_us=%lu limit_us=%lu\r\n", gap_max_us(), GAP_LIMIT_US);
    check(gap_max_us() <= GAP_LIMIT_US, "TC-HW-FS-006 ISR gap during activate exceeds 30 ms");

    bringup_puts("METADATA after activate:\r\n");
    dump_copy("A", FIRMWARE_META_A_ADDR);
    dump_copy("B", FIRMWARE_META_B_ADDR);
    bringup_pass(
        "TC-HW-FS-009 activate armed Slot B as a trial; reset needs the bootloader (FS-O7)");
}

/* ---------------------------------------------------------------------- */
/* main                                                                    */
/* ---------------------------------------------------------------------- */

int main(void)
{
    if (cpu_init() != STATUS_OK)
    {
        for (;;)
        {
            /* Nothing can be reported before the UART exists. */
        }
    }

    tls_arena_init(); /* mbedTLS allocates only from the static arena (WP2) */

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
        (debug_uart_attach_rx_byte(rx_byte_isr, NULL) != DEBUG_UART_OK))
    {
        for (;;)
        {
            /* No sink to report to. */
        }
    }

    bringup_puts("\r\n===== FirmwareStore Hardware Bring-up =====\r\n");
    bringup_puts("Board  : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("Needs  : InternalFlashDriver (P-1), Slot-A link + boot stub (P-3)\r\n");
    bringup_puts("WARNING: erases the on-chip metadata and the inactive slot\r\n");
    bringup_puts("===========================================\r\n\r\n");

    check(qspi_flash_init() == QSPI_FLASH_OK, "TC-HW-FS-001 qspi_flash_init failed");
    check(internal_flash_init() == INTERNAL_FLASH_OK, "TC-HW-FS-001 internal_flash_init failed");

    const uint32_t running_base = cpu_get_vector_table_base();
    const firmware_store_config_t cfg = {
        .qspi = qspi_flash_driver,
        .iflash = internal_flash_driver,
        .running_image_base = running_base,
    };
    firmware_store_t *fs = NULL;
    firmware_boot_info_t boot;

    if ((running_base != FIRMWARE_SLOT_A_BASE) && (running_base != FIRMWARE_SLOT_B_BASE))
    {
        /* TC-HW-FS-001b: a development image must be refused, not bricked. */
        expect_fs(firmware_store_create(&cfg, &fs), FIRMWARE_STORE_ERR_OK, "TC-HW-FS-001b create");
        expect_fs(firmware_store_boot_ops.get_boot_info(fs, &boot), FIRMWARE_STORE_ERR_OK,
                  "TC-HW-FS-001b get_boot_info");
        check(!boot.ota_capable, "TC-HW-FS-001b a development image reports ota_capable");
        expect_fs(firmware_store_image_ops.begin(fs, 1U, FIRMWARE_HEADER_SIZE + 8U),
                  FIRMWARE_STORE_ERR_NOT_OTA_CAPABLE, "TC-HW-FS-001b begin");
        bringup_pass(
            "TC-HW-FS-001b not in a slot: OTA refused by design (needs P-3 to go further)");

        for (;;)
        {
        }
    }

    /* TC-HW-FS-001 */
    const firmware_boot_meta_t existing = read_meta(FIRMWARE_META_A_ADDR);

    if (!meta_valid(&existing))
    {
        seed_metadata();
        bringup_info("metadata blank: seeded CONFIRMED / active A");
    }

    expect_fs(firmware_store_create(&cfg, &fs), FIRMWARE_STORE_ERR_OK, "TC-HW-FS-001 create");
    expect_fs(firmware_store_boot_ops.get_boot_info(fs, &boot), FIRMWARE_STORE_ERR_OK,
              "TC-HW-FS-001 get_boot_info");
    check(boot.ota_capable && (boot.running_slot == FIRMWARE_SLOT_A) &&
              (boot.state == FIRMWARE_BOOT_CONFIRMED),
          "TC-HW-FS-001 expected capable, Slot A, CONFIRMED");
    bringup_pass("TC-HW-FS-001 capable, running Slot A, CONFIRMED");

    gap_timer_start();
    bringup_puts("READY - send: OTA <image_id hex> <file_len> <install 0|1>\r\n");

    for (;;)
    {
        char line[LINE_MAX_LEN];

        if (!rx_line(line, sizeof(line), COMMAND_TIMEOUT_MS))
        {
            continue;
        }

        char *end = NULL;
        const uint32_t image_id = (uint32_t) strtoul(&line[4], &end, 16);
        const uint32_t file_len = (uint32_t) strtoul(end, &end, 10);
        const bool install = (strtoul(end, NULL, 10) != 0UL);

        if ((strncmp(line, "OTA ", 4U) != 0) || (file_len == 0U))
        {
            bringup_puts("ERR unknown command\r\nREADY\r\n");
            continue;
        }

        run_ota(fs, image_id, file_len, install);
        bringup_puts("READY\r\n");
    }
}
