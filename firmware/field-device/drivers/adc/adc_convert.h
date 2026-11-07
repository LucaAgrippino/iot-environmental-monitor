#ifndef ADC_CONVERT_H
#define ADC_CONVERT_H

/**
 * @file adc_convert.h
 * @brief AdcDriver, Field Device: raw reading to temperature (pure integer functions).
 *
 * No register access, so the host tests exercise it directly. Design: docs/lld/drivers/adc-driver.md
 * section 3.1.
 */

#include <stdint.h>

/** 12-bit conversion range. */
#define ADC_RAW_MAX (4095U)

/**
 * @brief Convert a raw reading of the internal temperature sensor to 0.1 degC.
 *
 * The STM32F469 has no factory calibration, so the datasheet's typical values are used with the
 * 3.3 V supply of the board: V25 = 760 mV, slope 2.5 mV/degC. The result is indicative only
 * (part-to-part offset of several degrees, ADC-O2).
 *
 * @param[in] raw  12-bit conversion result; values above ADC_RAW_MAX are clamped.
 * @return Temperature in tenths of a degree Celsius (451 is 45.1 degC).
 */
int16_t adc_mcu_temp_from_raw(uint32_t raw);

#endif /* ADC_CONVERT_H */
