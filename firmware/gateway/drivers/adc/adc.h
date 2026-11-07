#ifndef ADC_H
#define ADC_H

/**
 * @file adc.h
 * @brief AdcDriver, Gateway (STM32L475): the internal temperature sensor.
 *
 * Direct API, same names as the Gateway. Design: docs/lld/drivers/adc-driver.md.
 */

#include <stdint.h>

typedef enum
{
    ADC_OK = 0,
    ADC_ERR_NOT_INIT = 1, /**< adc_init() has not succeeded. */
    ADC_ERR_NULL = 2,     /**< A required pointer is NULL. */
    ADC_ERR_TIMEOUT = 3   /**< The ADC did not finish a conversion in time. */
} adc_err_t;

/**
 * @brief Enable ADC1 and the temperature sensor. Idempotent.
 * @return ADC_OK.
 */
adc_err_t adc_init(void);

/**
 * @brief Read the MCU die temperature.
 *
 * One polled conversion of ADC1 channels 0 (VREFINT) and 17 (temperature), a few tens of
 * microseconds. One calling task only.
 *
 * @param[out] deci_c  Temperature in tenths of a degree Celsius.
 * @return ADC_OK, ADC_ERR_NOT_INIT, ADC_ERR_NULL or ADC_ERR_TIMEOUT.
 */
adc_err_t adc_read_mcu_temp_deci_c(int16_t *deci_c);

#endif /* ADC_H */
