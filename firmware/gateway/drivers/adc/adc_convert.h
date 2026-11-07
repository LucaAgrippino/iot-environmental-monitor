#ifndef ADC_CONVERT_H
#define ADC_CONVERT_H

/**
 * @file adc_convert.h
 * @brief AdcDriver, Gateway: raw readings to temperature (pure integer functions).
 *
 * No register access, so the host tests exercise it directly. Design:
 * docs/lld/drivers/adc-driver.md section 3.1.
 */

#include <stdint.h>

/**
 * @brief Factory calibration of the part (read from system memory by adc.c).
 *
 * The temperature sensor was measured at 30 degC and 130 degC with VDDA = 3.0 V; the internal
 * reference was measured at 3.0 V too.
 */
typedef struct
{
    uint16_t ts_cal1;     /**< Sensor reading at 30 degC. */
    uint16_t ts_cal2;     /**< Sensor reading at 130 degC. */
    uint16_t vrefint_cal; /**< Internal reference reading. */
} adc_calibration_t;

/**
 * @brief Convert the raw readings of the sensor and of VREFINT to 0.1 degC.
 *
 * Normalises the sensor reading to the 3.0 V of the calibration (ts * vrefint_cal / vrefint),
 * then interpolates between the two calibration points: the board runs at 3.3 V, which would
 * otherwise read about 10 degC off.
 *
 * @param[in] cal         Factory calibration.
 * @param[in] raw_ts      12-bit reading of the temperature sensor.
 * @param[in] raw_vrefint 12-bit reading of VREFINT; 0 is refused.
 * @param[out] deci_c     Temperature in tenths of a degree Celsius.
 * @return 1 on success; 0 when raw_vrefint is 0 or the calibration points are equal.
 */
int adc_mcu_temp_from_raw(const adc_calibration_t *cal, uint32_t raw_ts, uint32_t raw_vrefint,
                          int16_t *deci_c);

#endif /* ADC_CONVERT_H */
