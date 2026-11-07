/**
 * @file adc.c
 * @brief AdcDriver, Field Device (STM32F469). See adc.h.
 */

#include "adc.h"

#include <stdbool.h>
#include <stddef.h>

#include "adc_convert.h"
#include "stm32f469xx.h"

/** Channel of the internal temperature sensor on ADC1 (shared with VBAT, selected by VBATE). */
#define ADC_TEMP_CHANNEL (18U)
/** SMPx = 7: 480 cycles, the longest; the sensor needs at least 10 us. */
#define ADC_SAMPLE_480_CYCLES (7U)
/** Upper bound of the end-of-conversion wait, in loop passes (a conversion takes about 50 us). */
#define ADC_POLL_LIMIT (100000U)

static bool s_initialised;

adc_err_t adc_init(void)
{
    if (s_initialised)
    {
        return ADC_OK;
    }

    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    (void) RCC->APB2ENR; /* read back: the clock is running before the next access */

    /* PCLK2 is 90 MHz; /4 gives 22.5 MHz, under the 36 MHz limit (480 cycles take 21 us). */
    ADC123_COMMON->CCR = (ADC123_COMMON->CCR & ~ADC_CCR_ADCPRE) | ADC_CCR_ADCPRE_0;
    ADC123_COMMON->CCR |= ADC_CCR_TSVREFE;
    ADC123_COMMON->CCR &= ~ADC_CCR_VBATE;

    ADC1->CR1 = 0U;
    ADC1->CR2 = ADC_CR2_ADON;
    ADC1->SMPR1 = (ADC1->SMPR1 & ~ADC_SMPR1_SMP18) | (ADC_SAMPLE_480_CYCLES << ADC_SMPR1_SMP18_Pos);
    ADC1->SQR1 = 0U; /* one conversion */
    ADC1->SQR3 = ADC_TEMP_CHANNEL;

    s_initialised = true;
    return ADC_OK;
}

adc_err_t adc_read_mcu_temp_deci_c(int16_t *deci_c)
{
    if (deci_c == NULL)
    {
        return ADC_ERR_NULL;
    }
    if (!s_initialised)
    {
        return ADC_ERR_NOT_INIT;
    }

    ADC1->SR = 0U;
    ADC1->CR2 |= ADC_CR2_SWSTART;

    uint32_t guard = ADC_POLL_LIMIT;
    while (((ADC1->SR & ADC_SR_EOC) == 0U) && (guard > 0U))
    {
        guard--;
    }
    if (guard == 0U)
    {
        return ADC_ERR_TIMEOUT;
    }

    *deci_c = adc_mcu_temp_from_raw(ADC1->DR); /* reading DR clears EOC */
    return ADC_OK;
}
