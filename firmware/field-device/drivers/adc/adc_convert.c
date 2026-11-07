/**
 * @file adc_convert.c
 * @brief AdcDriver, Field Device: raw reading to temperature. See adc_convert.h.
 */

#include "adc_convert.h"

/** Supply of the board in millivolts (the reference of the conversion). */
#define ADC_VDDA_MV (3300U)
/** Datasheet typical sensor voltage at 25 degC, in millivolts. */
#define ADC_V25_MV (760)
/** Datasheet typical slope: 2.5 mV/degC, that is 4 tenths of a degree per millivolt. */
#define ADC_DECI_C_PER_MV (4)
/** Temperature at which the sensor gives V25, in tenths of a degree. */
#define ADC_T_REF_DECI_C (250)

int16_t adc_mcu_temp_from_raw(uint32_t raw)
{
    if (raw > ADC_RAW_MAX)
    {
        raw = ADC_RAW_MAX;
    }

    /* deci = (raw * 3300 / 4095 - 760) * 4 + 250, with the division moved last (rounded) so the
     * millivolt step of 0.8 mV does not become a 0.3 degC step in the result. */
    const int32_t scaled =
        (int32_t) (((raw * ADC_VDDA_MV * (uint32_t) ADC_DECI_C_PER_MV) + (ADC_RAW_MAX / 2U)) /
                   ADC_RAW_MAX);
    return (int16_t) (scaled - (ADC_V25_MV * ADC_DECI_C_PER_MV) + ADC_T_REF_DECI_C);
}
