/**
 * @file test_humidity_temp_driver.c
 * @brief Unit tests for HumidityTempDriver — simulated temperature and humidity readings.
 *
 * Covers T-HT-01 through T-HT-05 per
 * docs/lld/drivers/simulated-sensor-drivers.md §7.
 *
 * No stub headers needed — HumidityTempDriver has no downward dependencies.
 * Ceedling auto-links humidity_temp_driver.c via the included header.
 */

#include "unity.h"

#include "humidity_temp_driver.h"

void setUp(void)
{
    humidity_temp_reset_for_test();
}

void tearDown(void)
{
}

/* T-HT-01: humidity_temp_init resets both defaults and clears fault flag. */
void test_T_HT_01_init_sets_default_state(void)
{
    int32_t  temp     = 0;
    uint32_t humidity = 0U;
    bool     fault    = true;

    /* Pre-arm fault and corrupt state to prove init resets all fields. */
    humidity_temp_inject_fault(true);
    humidity_temp_set_temp_for_test(0);
    humidity_temp_set_humidity_for_test(0U);

    ht_err_t err = humidity_temp_init();

    humidity_temp_get_state_for_test(&temp, &humidity, &fault);

    TEST_ASSERT_EQUAL_INT(HT_ERR_OK, err);
    TEST_ASSERT_EQUAL_INT32(HT_DEFAULT_TEMPERATURE_X100, temp);
    TEST_ASSERT_EQUAL_UINT32(HT_DEFAULT_HUMIDITY_X100, humidity);
    TEST_ASSERT_FALSE(fault);
}

/* T-HT-02: 100 reads always produce temperature in [−4000, 8500] and
   humidity in [0, 10000]. */
void test_T_HT_02_read_100_times_stays_in_range(void)
{
    for (int32_t i = 0; i < 100; i++)
    {
        ht_reading_t r   = {0U};
        ht_err_t     err = humidity_temp_read(&r);
        TEST_ASSERT_EQUAL_INT(HT_ERR_OK, err);
        TEST_ASSERT_TRUE(r.temperature_x100 >= HT_TEMPERATURE_MIN_X100);
        TEST_ASSERT_TRUE(r.temperature_x100 <= HT_TEMPERATURE_MAX_X100);
        TEST_ASSERT_TRUE(r.humidity_x100 >= HT_HUMIDITY_MIN_X100);
        TEST_ASSERT_TRUE(r.humidity_x100 <= HT_HUMIDITY_MAX_X100);
    }
}

/* T-HT-03: read after inject_fault(true) returns HT_ERR_FAULT;
   reading pointer is not modified. */
void test_T_HT_03_read_with_fault_injected_returns_fault(void)
{
    ht_reading_t r;
    r.temperature_x100 = HT_DEFAULT_TEMPERATURE_X100;
    r.humidity_x100    = HT_DEFAULT_HUMIDITY_X100;

    humidity_temp_inject_fault(true);
    ht_err_t err = humidity_temp_read(&r);

    TEST_ASSERT_EQUAL_INT(HT_ERR_FAULT, err);
    TEST_ASSERT_EQUAL_INT32(HT_DEFAULT_TEMPERATURE_X100, r.temperature_x100);
    TEST_ASSERT_EQUAL_UINT32(HT_DEFAULT_HUMIDITY_X100, r.humidity_x100);
}

/* T-HT-04: inject_fault(false) after active fault — next read returns HT_ERR_OK. */
void test_T_HT_04_fault_cleared_read_resumes_ok(void)
{
    humidity_temp_inject_fault(true);
    humidity_temp_inject_fault(false);

    ht_reading_t r   = {0U};
    ht_err_t     err = humidity_temp_read(&r);

    TEST_ASSERT_EQUAL_INT(HT_ERR_OK, err);
}

/* T-HT-05: Both temperature and humidity advance on the same read call —
   neither lags the other. Verified by observing both fields change from their
   initial values within 20 reads (probability of no change in either over
   20 steps is negligible). */
void test_T_HT_05_both_simulations_advance_independently_per_read(void)
{
    int32_t  initial_temp     = HT_DEFAULT_TEMPERATURE_X100;
    uint32_t initial_humidity = HT_DEFAULT_HUMIDITY_X100;
    bool     temp_moved       = false;
    bool     humidity_moved   = false;

    for (int32_t i = 0; i < 20; i++)
    {
        ht_reading_t r   = {0U};
        ht_err_t     err = humidity_temp_read(&r);
        TEST_ASSERT_EQUAL_INT(HT_ERR_OK, err);

        if (r.temperature_x100 != initial_temp)
        {
            temp_moved = true;
            initial_temp = r.temperature_x100;
        }
        if (r.humidity_x100 != initial_humidity)
        {
            humidity_moved = true;
            initial_humidity = r.humidity_x100;
        }

        if (temp_moved && humidity_moved)
        {
            break;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(temp_moved, "temperature never moved in 20 reads");
    TEST_ASSERT_TRUE_MESSAGE(humidity_moved, "humidity never moved in 20 reads");
}
