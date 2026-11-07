/**
 * @file main_test_adc.c
 * @brief AdcDriver hardware bring-up, Field Device (TC-HW-ADC-001).
 *
 * Reads the MCU die temperature ten times, one per 100 ms, and checks that
 *   - every read returns ADC_OK,
 *   - every value is between 0 and 85 degC (a board on a desk is near 30 to 50 degC),
 *   - the readings agree within 2 degC (the sensor is stable),
 *   - a read before adc_init() and a NULL output are refused.
 * Output on USART3 (ST-LINK virtual COM port, 115200 8N1).
 *
 * Build: GUI=0 MAIN=firmware/field-device/integration-tests/adc/main_test_adc.c
 * scripts/build-fd-app.sh --flash
 */

#include <stddef.h>
#include <stdint.h>

#include "adc/adc.h"
#include "stm32f469xx.h"
#include "system_clock.h"

#define BRINGUP_UART_BRR (391U) /* 45 MHz / 115200 */
#define READ_COUNT (10U)
#define TEMP_MIN_DECI_C (0)
#define TEMP_MAX_DECI_C (850)
#define SPREAD_MAX_DECI_C (20)
#define SPACING_LOOPS (18000000U) /* about 100 ms at 180 MHz */

static void uart_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 20U)) | (2UL << 20U);
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~(0xFUL << 8U)) | (7UL << 8U);
    GPIOB->OSPEEDR |= (3UL << 20U);
    USART3->CR1 = 0U;
    USART3->BRR = BRINGUP_UART_BRR;
    USART3->CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void putc_(char c)
{
    while ((USART3->SR & USART_SR_TXE) == 0U)
    {
    }
    USART3->DR = (uint32_t) (uint8_t) c;
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

    system_clock_init();
    uart_init();
    puts_("\r\n======= AdcDriver Hardware Bring-up (Field Device) =======\r\n");

    puts_((adc_read_mcu_temp_deci_c(&dummy) == ADC_ERR_NOT_INIT)
              ? "[PASS] TC-HW-ADC-001a read before init is refused\r\n"
              : "[FAIL] TC-HW-ADC-001a read before init was not refused\r\n");
    puts_((adc_init() == ADC_OK) ? "[PASS] TC-HW-ADC-001b adc_init\r\n"
                                 : "[FAIL] TC-HW-ADC-001b adc_init\r\n");
    puts_((adc_read_mcu_temp_deci_c(NULL) == ADC_ERR_NULL)
              ? "[PASS] TC-HW-ADC-001c NULL output is refused\r\n"
              : "[FAIL] TC-HW-ADC-001c NULL output was accepted\r\n");

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

    puts_((ok == READ_COUNT) ? "[PASS] TC-HW-ADC-001d ten reads returned ADC_OK\r\n"
                             : "[FAIL] TC-HW-ADC-001d a read failed\r\n");
    puts_(((lo >= TEMP_MIN_DECI_C) && (hi <= TEMP_MAX_DECI_C))
              ? "[PASS] TC-HW-ADC-001e values are between 0 and 85 degC\r\n"
              : "[FAIL] TC-HW-ADC-001e a value is out of range\r\n");
    puts_(((hi - lo) <= SPREAD_MAX_DECI_C)
              ? "[PASS] TC-HW-ADC-001f readings agree within 2 degC\r\n"
              : "[FAIL] TC-HW-ADC-001f readings spread more than 2 degC\r\n");
    puts_("[DONE]\r\n");
    for (;;)
    {
    }
}
