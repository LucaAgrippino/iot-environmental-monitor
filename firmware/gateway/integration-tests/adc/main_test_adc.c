/**
 * @file main_test_adc.c
 * @brief AdcDriver hardware bring-up, Gateway (TC-HW-ADC-002).
 *
 * Reads the MCU die temperature ten times, one per 100 ms, and checks that
 *   - every read returns ADC_OK,
 *   - every value is between 0 and 85 degC (a board on a desk is near 30 to 50 degC),
 *   - the readings agree within 2 degC (the sensor is stable),
 *   - a read before adc_init() and a NULL output are refused.
 * Output on USART1 PB6 (ST-LINK virtual COM port, 115200 8N1). Clock tree from cpu_init().
 *
 * Build: see scripts/build-gateway-app.sh (MAIN override) or a one-off arm-none-eabi-gcc line.
 */

#include <stddef.h>
#include <stdint.h>

#include "adc/adc.h"
#include "cpu/cpu.h"
#include "stm32l475xx.h"

#define BRINGUP_UART_BRR (694U) /* 80 MHz / 115200 */
#define READ_COUNT (10U)
#define TEMP_MIN_DECI_C (0)
#define TEMP_MAX_DECI_C (850)
#define SPREAD_MAX_DECI_C (20)
#define SPACING_LOOPS (4000000U) /* about 0.5 s at 80 MHz, -O0 */

static void uart_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 12U)) | (2UL << 12U);
    GPIOB->OTYPER &= ~(1UL << 6U);
    GPIOB->OSPEEDR |= (3UL << 12U);
    GPIOB->PUPDR &= ~(3UL << 12U);
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(0xFUL << 24U)) | (7UL << 24U);
    USART1->CR1 = 0U;
    USART1->BRR = BRINGUP_UART_BRR;
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void putc_(char c)
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
        putc_(*s++);
    }
}

static void put_temp(int32_t deci_c)
{
    if (deci_c < 0)
    {
        putc_('-');
        deci_c = -deci_c;
    }
    char digits[8];
    uint32_t n = 0U;
    uint32_t whole = (uint32_t) deci_c / 10U;
    do
    {
        digits[n++] = (char) ('0' + (whole % 10U));
        whole /= 10U;
    } while (whole != 0U);
    while (n > 0U)
    {
        putc_(digits[--n]);
    }
    putc_('.');
    putc_((char) ('0' + ((uint32_t) deci_c % 10U)));
}

int main(void)
{
    int16_t v[READ_COUNT];
    int16_t lo = 0;
    int16_t hi = 0;
    uint32_t ok = 0U;
    int16_t dummy = 0;

    (void) cpu_init();
    uart_init();
    puts_("\r\n======= AdcDriver Hardware Bring-up (Gateway) =======\r\n");

    puts_((adc_read_mcu_temp_deci_c(&dummy) == ADC_ERR_NOT_INIT)
              ? "[PASS] TC-HW-ADC-002a read before init is refused\r\n"
              : "[FAIL] TC-HW-ADC-002a read before init was not refused\r\n");
    puts_((adc_init() == ADC_OK) ? "[PASS] TC-HW-ADC-002b adc_init\r\n"
                                 : "[FAIL] TC-HW-ADC-002b adc_init\r\n");
    puts_((adc_read_mcu_temp_deci_c(NULL) == ADC_ERR_NULL)
              ? "[PASS] TC-HW-ADC-002c NULL output is refused\r\n"
              : "[FAIL] TC-HW-ADC-002c NULL output was accepted\r\n");

    for (uint32_t i = 0U; i < READ_COUNT; i++)
    {
        if (adc_read_mcu_temp_deci_c(&v[i]) == ADC_OK)
        {
            ok++;
            lo = ((ok == 1U) || (v[i] < lo)) ? v[i] : lo;
            hi = ((ok == 1U) || (v[i] > hi)) ? v[i] : hi;
            puts_("  read ");
            put_temp(v[i]);
            puts_(" degC\r\n");
        }
        for (volatile uint32_t d = 0U; d < SPACING_LOOPS; d++)
        {
        }
    }

    puts_((ok == READ_COUNT) ? "[PASS] TC-HW-ADC-002d ten reads returned ADC_OK\r\n"
                             : "[FAIL] TC-HW-ADC-002d a read failed\r\n");
    puts_(((lo >= TEMP_MIN_DECI_C) && (hi <= TEMP_MAX_DECI_C))
              ? "[PASS] TC-HW-ADC-002e values are between 0 and 85 degC\r\n"
              : "[FAIL] TC-HW-ADC-002e a value is out of range\r\n");
    puts_(((hi - lo) <= SPREAD_MAX_DECI_C) ? "[PASS] TC-HW-ADC-002f readings agree within 2 degC\r\n"
                                           : "[FAIL] TC-HW-ADC-002f readings spread more than 2 degC\r\n");
    puts_("[DONE]\r\n");
    for (;;)
    {
    }
}
