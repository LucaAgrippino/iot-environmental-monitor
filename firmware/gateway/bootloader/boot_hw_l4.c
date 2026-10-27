/**
 * @file boot_hw_l4.c
 * @brief Bootloader hardware seam for the STM32L475 (see boot_hw.h). CMSIS only, no HAL.
 *
 * @see docs/lld/drivers/bootloader.md §3.5, §3.6, §4
 */

#include "boot_hw.h"

#include <stdbool.h>

#include "stm32l475xx.h"

#define BOOT_UART_BAUD (115200UL)
#define BOOT_UART_TX_PIN (6U)
#define BOOT_UART_AF (7UL)
#define BOOT_LED_PIN (14U)

#define BOOT_POLL_LIMIT (2000000UL)
#define BOOT_BLINK_LOOPS (2000000UL)

#define BOOT_DEC_DIGITS (10U)

static uint32_t s_saved_acr;
static uint32_t s_saved_rcc_cr;
static uint32_t s_saved_demcr;
static uint32_t s_saved_dwt_ctrl;
static bool s_clocks_raised;
static bool s_console_up;
static bool s_dwt_up;
static volatile uint32_t s_ecc_events;

/** @brief Wait for MSIRDY, bounded. A few cycles pass first: a range change drops it briefly. */
static void wait_msi_ready(void)
{
    for (uint32_t i = 0U; i < 16U; i++)
    {
        __NOP();
    }

    for (uint32_t i = 0U; i < BOOT_POLL_LIMIT; i++)
    {
        if ((RCC->CR & RCC_CR_MSIRDY) != 0U)
        {
            return;
        }
    }
}

void boot_hw_clocks_fast(void)
{
    s_saved_acr = FLASH->ACR;
    s_saved_rcc_cr = RCC->CR;

    /* Wait states first, then the frequency (RM0351 §3.3.3: 2 WS up to 48 MHz in range 1). */
    FLASH->ACR = (s_saved_acr & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_2WS;

    for (uint32_t i = 0U; i < BOOT_POLL_LIMIT; i++)
    {
        if ((FLASH->ACR & FLASH_ACR_LATENCY) == FLASH_ACR_LATENCY_2WS)
        {
            break;
        }
    }

    RCC->CR = (RCC->CR & ~RCC_CR_MSIRANGE) | RCC_CR_MSIRANGE_11 | RCC_CR_MSIRGSEL;
    wait_msi_ready();

    /* The caches help the CRC loop; they are put back with the rest of ACR on the way out. */
    FLASH->ACR |= FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;

    s_clocks_raised = true;
}

static void clocks_restore(void)
{
    if (!s_clocks_raised)
    {
        return;
    }

    /* Frequency first, then the wait states. MSIRGSEL cannot be cleared by software, but range
     * 6 from RCC_CR is the same 4 MHz as the reset value from RCC_CSR. */
    RCC->CR = (RCC->CR & ~RCC_CR_MSIRANGE) | (s_saved_rcc_cr & RCC_CR_MSIRANGE);
    wait_msi_ready();
    FLASH->ACR = s_saved_acr;

    s_clocks_raised = false;
}

void boot_hw_console_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void) RCC->APB2ENR; /* the clock needs a cycle before the first access */

    GPIOB->MODER = (GPIOB->MODER & ~(3UL << (2U * BOOT_UART_TX_PIN))) |
                   (2UL << (2U * BOOT_UART_TX_PIN)); /* alternate function */
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(0xFUL << (4U * BOOT_UART_TX_PIN))) |
                    (BOOT_UART_AF << (4U * BOOT_UART_TX_PIN));
    GPIOB->OSPEEDR |= (3UL << (2U * BOOT_UART_TX_PIN));

    USART1->CR1 = 0U;
    USART1->BRR = (BOOT_HW_FAST_HZ + (BOOT_UART_BAUD / 2U)) / BOOT_UART_BAUD;
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE;

    s_console_up = true;
}

static void put_char(char c)
{
    if (!s_console_up)
    {
        return;
    }

    for (uint32_t i = 0U; i < BOOT_POLL_LIMIT; i++)
    {
        if ((USART1->ISR & USART_ISR_TXE) != 0U)
        {
            break;
        }
    }

    USART1->TDR = (uint32_t) (uint8_t) c;
}

void boot_hw_console_puts(const char *s)
{
    while (*s != '\0')
    {
        put_char(*s);
        s++;
    }
}

void boot_hw_console_dec(uint32_t v)
{
    char digits[BOOT_DEC_DIGITS];
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

void boot_hw_console_hex(uint32_t v)
{
    static const char k_hex[] = "0123456789ABCDEF";

    put_char('0');
    put_char('x');

    for (int32_t shift = 28; shift >= 0; shift -= 4)
    {
        put_char(k_hex[(v >> (uint32_t) shift) & 0xFU]);
    }
}

uint32_t boot_hw_cycles(void)
{
    if (!s_dwt_up)
    {
        s_saved_demcr = CoreDebug->DEMCR;
        s_saved_dwt_ctrl = DWT->CTRL;
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CYCCNT = 0U;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
        s_dwt_up = true;
    }

    return DWT->CYCCNT;
}

uint32_t boot_hw_ecc_events(void)
{
    return s_ecc_events;
}

static void console_drain(void)
{
    if (!s_console_up)
    {
        return;
    }

    for (uint32_t i = 0U; i < BOOT_POLL_LIMIT; i++)
    {
        if ((USART1->ISR & USART_ISR_TC) != 0U)
        {
            return;
        }
    }
}

void boot_hw_halt(void)
{
    console_drain();

    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << (2U * BOOT_LED_PIN))) | (1UL << (2U * BOOT_LED_PIN));

    for (;;)
    {
        GPIOB->ODR ^= (1UL << BOOT_LED_PIN);

        for (volatile uint32_t i = 0U; i < BOOT_BLINK_LOOPS; i++)
        {
        }
    }
}

void boot_hw_jump(uint32_t slot_base)
{
    const uint32_t *const vectors = (const uint32_t *) slot_base;
    const uint32_t sp = vectors[0];
    const uint32_t pc = vectors[1];

    console_drain();

    /* Peripherals back to reset: pulse their reset lines, then gate their clocks. */
    USART1->CR1 = 0U;
    RCC->APB2RSTR |= RCC_APB2RSTR_USART1RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_USART1RST;
    RCC->AHB2RSTR |= RCC_AHB2RSTR_GPIOBRST;
    RCC->AHB2RSTR &= ~RCC_AHB2RSTR_GPIOBRST;
    RCC->APB2ENR &= ~RCC_APB2ENR_USART1EN;
    RCC->AHB2ENR &= ~RCC_AHB2ENR_GPIOBEN;

    if (s_dwt_up)
    {
        DWT->CTRL = s_saved_dwt_ctrl;
        CoreDebug->DEMCR = s_saved_demcr;
    }

    clocks_restore();

    SCB->VTOR = slot_base;
    __DSB();
    __ISB();
    __set_MSP(sp);

    __asm volatile("bx %0" : : "r"(pc) : "memory");

    for (;;)
    {
    }
}

/**
 * @brief NMI: an uncorrectable flash ECC error is absorbed, anything else halts.
 *
 * The read that raised it completes with unreliable data, which the record and body CRCs then
 * reject (docs/lld/drivers/bootloader.md BL-D8).
 */
void boot_nmi_handler(void)
{
    if ((FLASH->ECCR & FLASH_ECCR_ECCD) != 0U)
    {
        s_ecc_events++;
        FLASH->ECCR = FLASH_ECCR_ECCD | (FLASH->ECCR & FLASH_ECCR_ECCIE);
        return;
    }

    boot_hw_halt();
}

void boot_fault_handler(void)
{
    boot_hw_console_puts("BL FAULT\r\n");
    boot_hw_halt();
}
