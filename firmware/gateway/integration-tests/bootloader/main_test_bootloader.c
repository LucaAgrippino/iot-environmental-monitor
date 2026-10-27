/**
 * @file main_test_bootloader.c
 * @brief Hardware bring-up test for the Gateway bootloader (B-L475E-IOT01A).
 *
 * NOT part of the CubeIDE project. It is built by scripts/build-bootloader-bench.sh, which links
 * this one file twice, once for Slot A (0x0800_6000) and once for Slot B (0x0808_0000), so the
 * bootloader has two distinct, verifiable images to choose between. It plays the application's
 * part of the boot-record protocol (flash-partition-layout.md §7.1) with the same flash driver
 * the real FirmwareStore uses, so that every transition of docs/lld/drivers/bootloader.md §8 can
 * be driven from a terminal.
 *
 * DESTRUCTIVE. Erases and programs the two boot-record pages (0x0800_4000..0x0800_4FFF). It
 * never touches the bootloader, the descriptors or either slot.
 *
 * On every start it prints who it is, what the bootloader handed over (VTOR, clock state, after
 * cpu_init) and both boot records. USART1 PB6/PB7, 115 200 8N1, one command character at a time:
 *
 *   i  print the identity and both records again
 *   c  confirm: CONFIRMED on the copy that is not selected (what FirmwareStore.confirm writes)
 *   r  rollback: previous slot active, ROLLED_BACK, count + 1; then reset
 *   p  activate: the other slot active, TRIAL_PENDING; then reset (what activate writes)
 *   x  reset without confirming (a crash)
 *   w  start the independent watchdog and spin (a hang; the watchdog resets the board)
 *   e  erase both records (blank metadata); then reset
 *   t<d>  tear: write a record to the copy that is not selected, but reset the board <d> * 10 us
 *      (d = 0..9) after the final double word starts programming. That is a power cut in the
 *      middle of a program (TC-HW-BL-008). It only works from Slot B: from Slot A the CPU stalls
 *      on the same flash bank until the program completes. Reading the result is the
 *      bootloader's job.
 *
 * Test sequence (TC-HW-BL-nnn from docs/lld/drivers/bootloader.md §8) is in
 * scripts/bootloader_bench.py.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "config_store/config_store_crc.h"
#include "cpu/cpu.h"
#include "cpu/status.h"
#include "firmware_store/firmware_image_format.h"
#include "internal_flash/iinternal_flash.h"
#include "internal_flash/internal_flash.h"
#include "stm32l475xx.h"

#ifndef SLOT_APP_NAME
#define SLOT_APP_NAME "?"
#endif

#ifndef SLOT_APP_VERSION
#define SLOT_APP_VERSION (0x01000000UL)
#endif

#define UART_BAUD (115200UL)
#define PCLK2_HZ (80000000UL)
#define UART_TX_PIN (6U)
#define UART_RX_PIN (7U)
#define UART_AF (7UL)

#define IWDG_KEY_START (0xCCCCU)
#define IWDG_KEY_ACCESS (0x5555U)
#define IWDG_KEY_RELOAD (0xAAAAU)

/** Absorbed uncorrectable ECC errors; the bootloader has the same handler (BL-D8). */
static volatile uint32_t s_ecc_nmi;

/* ---------------------------------------------------------------------- */
/* Console                                                                 */
/* ---------------------------------------------------------------------- */

static void uart_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void) RCC->APB2ENR;

    GPIOB->MODER = (GPIOB->MODER & ~((3UL << (2U * UART_TX_PIN)) | (3UL << (2U * UART_RX_PIN)))) |
                   (2UL << (2U * UART_TX_PIN)) | (2UL << (2U * UART_RX_PIN));
    GPIOB->AFR[0] =
        (GPIOB->AFR[0] & ~((0xFUL << (4U * UART_TX_PIN)) | (0xFUL << (4U * UART_RX_PIN)))) |
        (UART_AF << (4U * UART_TX_PIN)) | (UART_AF << (4U * UART_RX_PIN));

    USART1->CR1 = 0U;
    USART1->BRR = (PCLK2_HZ + (UART_BAUD / 2U)) / UART_BAUD;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

static void put_char(char c)
{
    while ((USART1->ISR & USART_ISR_TXE) == 0U)
    {
    }

    USART1->TDR = (uint32_t) (uint8_t) c;
}

static void puts_(const char *s)
{
    while (*s != '\0')
    {
        put_char(*s);
        s++;
    }
}

static void put_hex(uint32_t v)
{
    static const char k_hex[] = "0123456789ABCDEF";

    puts_("0x");

    for (int32_t shift = 28; shift >= 0; shift -= 4)
    {
        put_char(k_hex[(v >> (uint32_t) shift) & 0xFU]);
    }
}

static void put_dec(uint32_t v)
{
    char digits[10];
    uint32_t n = 0U;

    do
    {
        digits[n] = (char) ('0' + (char) (v % 10U));
        n++;
        v /= 10U;
    } while (v != 0U);

    while (n > 0U)
    {
        n--;
        put_char(digits[n]);
    }
}

static int32_t get_char(void)
{
    if ((USART1->ISR & (USART_ISR_ORE | USART_ISR_FE)) != 0U)
    {
        USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF;
    }

    if ((USART1->ISR & USART_ISR_RXNE) == 0U)
    {
        return -1;
    }

    return (int32_t) (USART1->RDR & 0xFFU);
}

/* ---------------------------------------------------------------------- */
/* Boot records, as the application sees them                              */
/* ---------------------------------------------------------------------- */

typedef struct
{
    bool valid;
    uint8_t copy;
    firmware_boot_meta_t rec;
} record_t;

static bool read_flash(uint32_t addr, void *buf, uint32_t len)
{
    return internal_flash_driver->read(addr, (uint8_t *) buf, len) == INTERNAL_FLASH_OK;
}

static bool record_valid(const firmware_boot_meta_t *r)
{
    return (r->magic == FIRMWARE_META_MAGIC) &&
           (r->crc32 == config_store_crc32((const uint8_t *) r, FIRMWARE_META_CRC_SPAN));
}

static uint32_t copy_addr(uint8_t copy)
{
    return (copy == 0U) ? FIRMWARE_META_A_ADDR : FIRMWARE_META_B_ADDR;
}

static record_t record_select(void)
{
    record_t best = {.valid = false, .copy = 0U};

    for (uint8_t c = 0U; c < 2U; c++)
    {
        firmware_boot_meta_t r;

        if (read_flash(copy_addr(c), &r, sizeof(r)) && record_valid(&r) &&
            (!best.valid || (r.generation > best.rec.generation)))
        {
            best.valid = true;
            best.copy = c;
            best.rec = r;
        }
    }

    return best;
}

/** @brief The record that follows @p cur with the given content. */
static firmware_boot_meta_t record_build(const record_t *cur, uint8_t active, uint8_t state,
                                         uint8_t previous, uint32_t count)
{
    firmware_boot_meta_t r = {
        .magic = FIRMWARE_META_MAGIC,
        .active_bank = active,
        .boot_state = state,
        .previous_bank = previous,
        .reserved = 0xFFU,
        .rollback_count = count,
        .generation = cur->valid ? (cur->rec.generation + 1U) : 1U,
        .pad = 0xFFFFFFFFUL,
    };

    r.crc32 = config_store_crc32((const uint8_t *) &r, FIRMWARE_META_CRC_SPAN);

    return r;
}

static uint8_t record_target(const record_t *cur)
{
    return (cur->valid && (cur->copy == 0U)) ? 1U : 0U;
}

/** @brief The application's write: the copy that is not selected, CRC double word last. */
static bool record_write(const record_t *cur, uint8_t active, uint8_t state, uint8_t previous,
                         uint32_t count)
{
    const uint32_t addr = copy_addr(record_target(cur));
    const firmware_boot_meta_t r = record_build(cur, active, state, previous, count);

    return internal_flash_driver->erase_page(addr) == INTERNAL_FLASH_OK &&
           internal_flash_driver->program(addr, (const uint8_t *) &r, 16U) == INTERNAL_FLASH_OK &&
           internal_flash_driver->program(addr + 16U, ((const uint8_t *) &r) + 16U, 8U) ==
               INTERNAL_FLASH_OK;
}

static const char *state_name(uint8_t s)
{
    switch (s)
    {
    case FIRMWARE_META_STATE_CONFIRMED:
        return "CONFIRMED";
    case FIRMWARE_META_STATE_TRIAL_PENDING:
        return "TRIAL_PENDING";
    case FIRMWARE_META_STATE_TRIAL_RUNNING:
        return "TRIAL_RUNNING";
    case FIRMWARE_META_STATE_ROLLED_BACK:
        return "ROLLED_BACK";
    default:
        return "?";
    }
}

static void print_record(uint8_t copy)
{
    firmware_boot_meta_t r;

    puts_("  copy ");
    put_char((char) ('A' + (char) copy));
    puts_(": ");

    if (!read_flash(copy_addr(copy), &r, sizeof(r)))
    {
        puts_("unreadable\r\n");
        return;
    }

    if (!record_valid(&r))
    {
        puts_("invalid\r\n");
        return;
    }

    puts_(state_name(r.boot_state));
    puts_(" active=");
    put_char((r.active_bank == FIRMWARE_BANK_B) ? 'B' : 'A');
    puts_(" previous=");
    put_char((r.previous_bank == FIRMWARE_BANK_B) ? 'B' : 'A');
    puts_(" rb=");
    put_dec(r.rollback_count);
    puts_(" gen=");
    put_dec(r.generation);
    puts_("\r\n");
}

static void print_identity(status_t cpu_status)
{
    puts_("\r\nSLOT-APP ");
    puts_(SLOT_APP_NAME);
    puts_(" version=");
    put_hex(SLOT_APP_VERSION);
    puts_(" vtor=");
    put_hex(SCB->VTOR);
    puts_(" cpu_init=");
    put_dec((uint32_t) cpu_status);
    puts_(" sws=");
    put_dec((RCC->CFGR & RCC_CFGR_SWS) >> RCC_CFGR_SWS_Pos);
    puts_(" acr=");
    put_hex(FLASH->ACR);
    puts_(" ecc=");
    put_dec(s_ecc_nmi);
    puts_("\r\nRECORDS\r\n");
    print_record(0U);
    print_record(1U);
    puts_("READY\r\n");
}

/* ---------------------------------------------------------------------- */
/* Commands                                                                */
/* ---------------------------------------------------------------------- */

/**
 * @brief NMI: an uncorrectable flash ECC error is cleared and execution resumes.
 *
 * Without it a torn boot record would hang the application at its first read of the record, the
 * same hazard the bootloader's NMI handler removes (docs/lld/drivers/bootloader.md BL-O8).
 */
void NMI_Handler(void)
{
    if ((FLASH->ECCR & FLASH_ECCR_ECCD) != 0U)
    {
        s_ecc_nmi++;
        FLASH->ECCR = FLASH_ECCR_ECCD | (FLASH->ECCR & FLASH_ECCR_ECCIE);
        return;
    }

    for (;;)
    {
    }
}

/**
 * @brief Write a record like record_write(), but reset the board during its last program.
 *
 * Erase and the first 16 bytes go through the driver. The final double word (the CRC) is started
 * with raw register writes, and the reset is requested @p delay_us after the second store. The
 * flash controller needs about 85 us for a double word, so a shorter delay aborts it.
 */
static void tear_record(const record_t *cur, uint8_t active, uint8_t state, uint8_t previous,
                        uint32_t count, uint32_t delay_us)
{
    const uint32_t addr = copy_addr(record_target(cur));
    const firmware_boot_meta_t r = record_build(cur, active, state, previous, count);
    uint32_t last[2];

    (void) memcpy(last, ((const uint8_t *) &r) + 16U, sizeof(last));

    if ((internal_flash_driver->erase_page(addr) != INTERNAL_FLASH_OK) ||
        (internal_flash_driver->program(addr, (const uint8_t *) &r, 16U) != INTERNAL_FLASH_OK))
    {
        puts_("tear: FAILED\r\n");
        return;
    }

    FLASH->KEYR = 0x45670123UL;
    FLASH->KEYR = 0xCDEF89ABUL;

    while ((FLASH->SR & FLASH_SR_BSY) != 0U)
    {
    }

    puts_("tear: programming, resetting\r\n");
    while ((USART1->ISR & USART_ISR_TC) == 0U)
    {
    }

    FLASH->CR |= FLASH_CR_PG;
    *(volatile uint32_t *) (addr + 16U) = last[0];
    *(volatile uint32_t *) (addr + 20U) = last[1];

    if (delay_us > 0U)
    {
        cpu_delay_us(delay_us);
    }

    NVIC_SystemReset();
}

/** @brief Wait a moment for one more character; -1 if none arrives. */
static int32_t get_char_wait(void)
{
    for (uint32_t i = 0U; i < 4000000UL; i++)
    {
        const int32_t c = get_char();

        if (c >= 0)
        {
            return c;
        }
    }

    return -1;
}

static void watchdog_hang(void)
{
    puts_("WDG start\r\n");
    IWDG->KR = IWDG_KEY_START;
    IWDG->KR = IWDG_KEY_ACCESS;
    IWDG->PR = 0U;    /* LSI / 4 */
    IWDG->RLR = 400U; /* about 50 ms */
    IWDG->KR = IWDG_KEY_RELOAD;

    for (;;)
    {
    }
}

static void do_command(int32_t c)
{
    record_t cur = record_select();

    switch (c)
    {
    case 'i':
        print_identity(STATUS_OK);
        break;

    case 'c':
        if (!cur.valid)
        {
            puts_("confirm: no valid record\r\n");
        }
        else
        {
            puts_(record_write(&cur, cur.rec.active_bank, FIRMWARE_META_STATE_CONFIRMED,
                               cur.rec.previous_bank, cur.rec.rollback_count)
                      ? "confirm: ok\r\n"
                      : "confirm: FAILED\r\n");
        }
        break;

    case 'r':
        if (!cur.valid)
        {
            puts_("rollback: no valid record\r\n");
        }
        else if (record_write(&cur, cur.rec.previous_bank, FIRMWARE_META_STATE_ROLLED_BACK,
                              cur.rec.active_bank, cur.rec.rollback_count + 1U))
        {
            puts_("rollback: ok, resetting\r\n");
            NVIC_SystemReset();
        }
        else
        {
            puts_("rollback: FAILED\r\n");
        }
        break;

    case 'p':
        if (!cur.valid)
        {
            puts_("activate: no valid record\r\n");
        }
        else if (record_write(&cur, cur.rec.previous_bank, FIRMWARE_META_STATE_TRIAL_PENDING,
                              cur.rec.active_bank, cur.rec.rollback_count))
        {
            puts_("activate: ok, resetting\r\n");
            NVIC_SystemReset();
        }
        else
        {
            puts_("activate: FAILED\r\n");
        }
        break;

    case 'x':
        puts_("reset\r\n");
        NVIC_SystemReset();
        break;

    case 'w':
        watchdog_hang();
        break;

    case 't':
    {
        const int32_t d = get_char_wait();

        if (!cur.valid || (d < '0') || (d > '9'))
        {
            puts_("tear: no valid record or no delay digit\r\n");
        }
        else
        {
            tear_record(&cur, cur.rec.active_bank, cur.rec.boot_state, cur.rec.previous_bank,
                        cur.rec.rollback_count, (uint32_t) (d - '0') * 10U);
        }
        break;
    }

    case 'e':
        (void) internal_flash_driver->erase_page(FIRMWARE_META_A_ADDR);
        (void) internal_flash_driver->erase_page(FIRMWARE_META_B_ADDR);
        puts_("records erased, resetting\r\n");
        NVIC_SystemReset();
        break;

    default:
        break;
    }
}

int main(void)
{
    const status_t cpu_status = cpu_init();

    uart_init();
    (void) internal_flash_init();
    print_identity(cpu_status);

    for (;;)
    {
        const int32_t c = get_char();

        if (c >= 0)
        {
            do_command(c);
        }
    }
}
