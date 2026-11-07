/**
 * @file test_adc_convert_gw.c
 * @brief Host tests of the Gateway AdcDriver conversion (adc_convert.c).
 *
 * T-ADC-01 of docs/lld/drivers/adc-driver.md. The register sequence of adc.c is covered on the
 * board (TC-HW-ADC-002).
 */

#include "unity.h"

#include "adc_convert.h"

/* Typical calibration words of an STM32L475 */
static const adc_calibration_t k_cal = {.ts_cal1 = 1040U, .ts_cal2 = 1370U, .vrefint_cal = 1650U};

void setUp(void)
{
}

void tearDown(void)
{
}

void test_TC_ADC_GW_001_the_first_calibration_point_is_30_degrees(void)
{
    int16_t t = 0;
    TEST_ASSERT_EQUAL(1, adc_mcu_temp_from_raw(&k_cal, 1040U, 1650U, &t));
    TEST_ASSERT_EQUAL_INT16(300, t);
}

void test_TC_ADC_GW_002_the_second_calibration_point_is_130_degrees(void)
{
    int16_t t = 0;
    TEST_ASSERT_EQUAL(1, adc_mcu_temp_from_raw(&k_cal, 1370U, 1650U, &t));
    TEST_ASSERT_EQUAL_INT16(1300, t);
}

void test_TC_ADC_GW_003_halfway_between_the_points_is_80_degrees(void)
{
    int16_t t = 0;
    TEST_ASSERT_EQUAL(1, adc_mcu_temp_from_raw(&k_cal, 1205U, 1650U, &t));
    TEST_ASSERT_EQUAL_INT16(800, t);
}

void test_TC_ADC_GW_004_a_3v3_supply_is_corrected_with_vrefint(void)
{
    int16_t t = 0;
    /* At VDDA = 3.3 V every raw value is 3.0/3.3 of its 3.0 V value, VREFINT included. */
    const uint32_t raw_vref = (1650U * 3000U) / 3300U;
    const uint32_t raw_ts = (1040U * 3000U) / 3300U; /* the chip is at 30 degC */

    TEST_ASSERT_EQUAL(1, adc_mcu_temp_from_raw(&k_cal, raw_ts, raw_vref, &t));
    TEST_ASSERT_INT16_WITHIN(5, 300, t);
}

void test_TC_ADC_GW_005_a_reading_below_the_first_point_gives_a_lower_temperature(void)
{
    int16_t t = 0;
    /* 99 counts below the 30 degC point is about 30 degC cooler: 0 degC */
    TEST_ASSERT_EQUAL(1, adc_mcu_temp_from_raw(&k_cal, 941U, 1650U, &t));
    TEST_ASSERT_INT16_WITHIN(5, 0, t);
}

void test_TC_ADC_GW_006_below_zero_the_result_is_negative(void)
{
    int16_t t = 0;
    TEST_ASSERT_EQUAL(1, adc_mcu_temp_from_raw(&k_cal, 842U, 1650U, &t));
    TEST_ASSERT_INT16_WITHIN(5, -300, t);
}

void test_TC_ADC_GW_007_a_dead_reference_is_refused(void)
{
    int16_t t = 77;
    TEST_ASSERT_EQUAL(0, adc_mcu_temp_from_raw(&k_cal, 1040U, 0U, &t));
    TEST_ASSERT_EQUAL_INT16(77, t);
}

void test_TC_ADC_GW_008_unusable_calibration_is_refused(void)
{
    const adc_calibration_t bad = {.ts_cal1 = 1370U, .ts_cal2 = 1370U, .vrefint_cal = 1650U};
    int16_t t = 0;

    TEST_ASSERT_EQUAL(0, adc_mcu_temp_from_raw(&bad, 1200U, 1650U, &t));
}

void test_TC_ADC_GW_009_null_arguments_are_refused(void)
{
    int16_t t = 0;

    TEST_ASSERT_EQUAL(0, adc_mcu_temp_from_raw(NULL, 1200U, 1650U, &t));
    TEST_ASSERT_EQUAL(0, adc_mcu_temp_from_raw(&k_cal, 1200U, 1650U, NULL));
}
