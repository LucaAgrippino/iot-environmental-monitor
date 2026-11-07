/**
 * @file adc_convert.c
 * @brief AdcDriver, Gateway: raw readings to temperature. See adc_convert.h.
 */

#include "adc_convert.h"

#include <stddef.h>

/** Temperature of the first calibration point, in tenths of a degree. */
#define ADC_CAL1_DECI_C (300)
/** Span between the calibration points (30 degC to 130 degC), in tenths of a degree. */
#define ADC_CAL_SPAN_DECI_C (1000)

int adc_mcu_temp_from_raw(const adc_calibration_t *cal, uint32_t raw_ts, uint32_t raw_vrefint,
                          int16_t *deci_c)
{
    if ((cal == NULL) || (deci_c == NULL) || (raw_vrefint == 0U) || (cal->ts_cal2 <= cal->ts_cal1))
    {
        return 0;
    }

    /* raw_ts <= 4095 and vrefint_cal <= 65535: the product fits 32 bits. Rounded division. */
    const uint32_t ts_norm = ((raw_ts * (uint32_t) cal->vrefint_cal) + (raw_vrefint / 2U)) /
                             raw_vrefint;
    const int32_t span = (int32_t) cal->ts_cal2 - (int32_t) cal->ts_cal1;
    const int32_t diff = ((int32_t) ts_norm - (int32_t) cal->ts_cal1) * ADC_CAL_SPAN_DECI_C;
    const int32_t half = (diff >= 0) ? (span / 2) : -(span / 2); /* round half away from zero */

    *deci_c = (int16_t) (((diff + half) / span) + ADC_CAL1_DECI_C);
    return 1;
}
