/**
 * @file adc.c
 * @brief AdcDriver, Gateway (STM32L475). See adc.h.
 */

#include "adc.h"

#include <stdbool.h>
#include <stddef.h>

#include "adc_convert.h"
#include "stm32l475xx.h"

/** Factory calibration words in system memory (DS11929, RM0351 section 16.4.32). */
#define ADC_TS_CAL1_ADDR (0x1FFF75A8UL)
#define ADC_VREFINT_CAL_ADDR (0x1FFF75AAUL)
#define ADC_TS_CAL2_ADDR (0x1FFF75CAUL)

#define ADC_CH_VREFINT (0U)
#define ADC_CH_TEMP (17U)
/** SMPx = 7: 640.5 cycles, the longest (32 us at 20 MHz). */
#define ADC_SAMPLE_640_CYCLES (7U)
#define ADC_SMP_BITS (3U)
#define ADC_SMPR2_FIRST_CHANNEL (10U)
/** Passes of the regulator start-up wait (20 us at 80 MHz is 1600 cycles; a pass is >= 2). */
#define ADC_REGULATOR_LOOPS (2000U)
/** Upper bound of every ready and end-of-conversion wait, in loop passes. */
#define ADC_POLL_LIMIT (200000U)

static bool s_initialised;
static adc_calibration_t s_cal;

static bool wait_clear(const volatile uint32_t *reg, uint32_t mask)
{
    uint32_t guard = ADC_POLL_LIMIT;
    while (((*reg & mask) != 0U) && (guard > 0U))
    {
        guard--;
    }
    return guard > 0U;
}

static bool wait_set(const volatile uint32_t *reg, uint32_t mask)
{
    uint32_t guard = ADC_POLL_LIMIT;
    while (((*reg & mask) == 0U) && (guard > 0U))
    {
        guard--;
    }
    return guard > 0U;
}

adc_err_t adc_init(void)
{
    if (s_initialised)
    {
        return ADC_OK;
    }

    s_cal.ts_cal1 = *(const volatile uint16_t *) ADC_TS_CAL1_ADDR;
    s_cal.ts_cal2 = *(const volatile uint16_t *) ADC_TS_CAL2_ADDR;
    s_cal.vrefint_cal = *(const volatile uint16_t *) ADC_VREFINT_CAL_ADDR;

    RCC->AHB2ENR |= RCC_AHB2ENR_ADCEN;
    (void) RCC->AHB2ENR;
    RCC->CCIPR |= RCC_CCIPR_ADCSEL; /* ADC clock = SYSCLK */
    ADC123_COMMON->CCR = (ADC123_COMMON->CCR & ~(ADC_CCR_CKMODE | ADC_CCR_PRESC)) |
                         ADC_CCR_PRESC_1; /* asynchronous clock, /4 = 20 MHz */
    ADC123_COMMON->CCR |= ADC_CCR_VREFEN | ADC_CCR_TSEN;

    /* Leave deep power-down, start the regulator, calibrate, enable. */
    ADC1->CR &= ~ADC_CR_DEEPPWD;
    ADC1->CR |= ADC_CR_ADVREGEN;
    for (volatile uint32_t i = 0U; i < ADC_REGULATOR_LOOPS; i++)
    {
    }

    ADC1->CR |= ADC_CR_ADCAL;
    if (!wait_clear(&ADC1->CR, ADC_CR_ADCAL))
    {
        return ADC_ERR_TIMEOUT;
    }

    ADC1->ISR = ADC_ISR_ADRDY;
    ADC1->CR |= ADC_CR_ADEN;
    if (!wait_set(&ADC1->ISR, ADC_ISR_ADRDY))
    {
        return ADC_ERR_TIMEOUT;
    }

    ADC1->SMPR1 = (ADC_SAMPLE_640_CYCLES << (ADC_CH_VREFINT * ADC_SMP_BITS));
    ADC1->SMPR2 =
        (ADC_SAMPLE_640_CYCLES << ((ADC_CH_TEMP - ADC_SMPR2_FIRST_CHANNEL) * ADC_SMP_BITS));
    ADC1->CFGR = 0U; /* single conversion, software trigger, 12 bit, right aligned */

    s_initialised = true;
    return ADC_OK;
}

static adc_err_t convert(uint32_t channel, uint32_t *raw)
{
    ADC1->SQR1 = (channel << ADC_SQR1_SQ1_Pos); /* L = 0: one conversion */
    ADC1->ISR = ADC_ISR_EOC;
    ADC1->CR |= ADC_CR_ADSTART;
    if (!wait_set(&ADC1->ISR, ADC_ISR_EOC))
    {
        return ADC_ERR_TIMEOUT;
    }
    *raw = ADC1->DR; /* clears EOC */
    return ADC_OK;
}

adc_err_t adc_read_mcu_temp_deci_c(int16_t *deci_c)
{
    uint32_t raw_vref = 0U;
    uint32_t raw_ts = 0U;

    if (deci_c == NULL)
    {
        return ADC_ERR_NULL;
    }
    if (!s_initialised)
    {
        return ADC_ERR_NOT_INIT;
    }
    if ((convert(ADC_CH_VREFINT, &raw_vref) != ADC_OK) || (convert(ADC_CH_TEMP, &raw_ts) != ADC_OK))
    {
        return ADC_ERR_TIMEOUT;
    }
    /* a dead reference (raw 0) or unreadable calibration is a failed conversion */
    return (adc_mcu_temp_from_raw(&s_cal, raw_ts, raw_vref, deci_c) != 0) ? ADC_OK
                                                                          : ADC_ERR_TIMEOUT;
}
