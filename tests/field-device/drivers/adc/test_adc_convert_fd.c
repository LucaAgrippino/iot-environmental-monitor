/**
 * @file test_adc_convert_fd.c
 * @brief Host tests of the Field Device AdcDriver conversion (adc_convert.c).
 *
 * T-ADC-02 of docs/lld/drivers/adc-driver.md. The register sequence of adc.c is covered on the
 * board (TC-HW-ADC-001): it has no logic worth a register mock.
 */

#include "unity.h"

#include "adc_convert.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* raw for a given millivolt at 3.3 V, 12 bit */
#define RAW_OF_MV(mv) (((mv) * 4095U + 1650U) / 3300U)

void test_TC_ADC_FD_001_760_millivolts_is_25_degrees(void)
{
    TEST_ASSERT_INT16_WITHIN(2, 250, adc_mcu_temp_from_raw(RAW_OF_MV(760U)));
}

void test_TC_ADC_FD_002_25_millivolts_above_is_10_degrees_warmer(void)
{
    /* 2.5 mV per degree */
    TEST_ASSERT_INT16_WITHIN(3, 350, adc_mcu_temp_from_raw(RAW_OF_MV(785U)));
}

void test_TC_ADC_FD_003_a_reading_below_the_25_degree_point_is_cooler(void)
{
    TEST_ASSERT_INT16_WITHIN(3, 150, adc_mcu_temp_from_raw(RAW_OF_MV(735U)));
}

void test_TC_ADC_FD_004_a_low_reading_gives_a_negative_temperature(void)
{
    /* 760 mV - 75 mV is 30 degC below the 25 degC point: -5.0 degC */
    TEST_ASSERT_INT16_WITHIN(3, -50, adc_mcu_temp_from_raw(RAW_OF_MV(685U)));
}

void test_TC_ADC_FD_005_the_result_rises_with_the_reading(void)
{
    int16_t prev = adc_mcu_temp_from_raw(0U);

    for (uint32_t raw = 64U; raw <= 4095U; raw += 64U)
    {
        const int16_t t = adc_mcu_temp_from_raw(raw);
        TEST_ASSERT_TRUE(t >= prev);
        prev = t;
    }
}

void test_TC_ADC_FD_006_a_value_above_12_bits_is_clamped(void)
{
    TEST_ASSERT_EQUAL_INT16(adc_mcu_temp_from_raw(4095U), adc_mcu_temp_from_raw(0xFFFFFFFFU));
}

void test_TC_ADC_FD_007_the_range_of_the_converter_fits_the_result_type(void)
{
    /* 0 -> about -279 degC, 4095 -> about +527 degC: both inside int16 tenths */
    TEST_ASSERT_INT16_WITHIN(1, -2790 + 250 - 250, adc_mcu_temp_from_raw(0U) + 0);
    TEST_ASSERT_TRUE(adc_mcu_temp_from_raw(4095U) > 5000);
}
