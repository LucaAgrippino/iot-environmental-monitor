/**
 * @file test_barometer_driver.c
 * @brief Unit tests for BarometerDriver — simulated pressure readings.
 *
 * Covers T-BARO-01 through T-BARO-07 per
 * docs/lld/drivers/simulated-sensor-drivers.md §7.
 *
 * No stub headers needed — BarometerDriver has no downward dependencies.
 * Ceedling auto-links barometer_driver.c via the included header.
 */

#include "unity.h"

#include "barometer_driver.h"

void setUp(void)
{
    barometer_reset_for_test();
}

void tearDown(void)
{
}

/* T-BARO-01: barometer_init resets pressure to 10132 and clears fault flag. */
void test_T_BARO_01_init_sets_default_state(void)
{
    int32_t pressure = 0;
    bool    fault    = true;

    /* Pre-arm fault and corrupt pressure to prove init resets both. */
    barometer_inject_fault(true);
    barometer_set_pressure_for_test(0);

    baro_err_t err = barometer_init();

    barometer_get_state_for_test(&pressure, &fault);

    TEST_ASSERT_EQUAL_INT(BARO_ERR_OK, err);
    TEST_ASSERT_EQUAL_INT32(BARO_DEFAULT_PRESSURE_X10, pressure);
    TEST_ASSERT_FALSE(fault);
}

/* T-BARO-02: 100 reads always produce values within [3000, 11000]. */
void test_T_BARO_02_read_100_times_stays_in_range(void)
{
    for (int32_t i = 0; i < 100; i++)
    {
        baro_reading_t r   = {0};
        baro_err_t     err = barometer_read(&r);
        TEST_ASSERT_EQUAL_INT(BARO_ERR_OK, err);
        TEST_ASSERT_TRUE(r.pressure_x10 >= BARO_PRESSURE_MIN_X10);
        TEST_ASSERT_TRUE(r.pressure_x10 <= BARO_PRESSURE_MAX_X10);
    }
}

/* T-BARO-03: read after inject_fault(true) returns BARO_ERR_FAULT;
   reading pointer is not modified. */
void test_T_BARO_03_read_with_fault_injected_returns_fault(void)
{
    baro_reading_t r;
    r.pressure_x10 = BARO_DEFAULT_PRESSURE_X10;

    barometer_inject_fault(true);
    baro_err_t err = barometer_read(&r);

    TEST_ASSERT_EQUAL_INT(BARO_ERR_FAULT, err);
    TEST_ASSERT_EQUAL_INT32(BARO_DEFAULT_PRESSURE_X10, r.pressure_x10);
}

/* T-BARO-04: inject_fault(false) after active fault — next read returns OK. */
void test_T_BARO_04_fault_cleared_read_resumes_ok(void)
{
    barometer_inject_fault(true);
    barometer_inject_fault(false);

    baro_reading_t r   = {0};
    baro_err_t     err = barometer_read(&r);

    TEST_ASSERT_EQUAL_INT(BARO_ERR_OK, err);
}

/* T-BARO-05: NULL reading pointer returns BARO_ERR_FAULT.
   Design choice: NULL returns the same error as a hardware fault — the caller
   (SensorService) treats both paths identically (marks sample invalid). */
void test_T_BARO_05_null_reading_pointer_returns_fault(void)
{
    baro_err_t err = barometer_read(NULL);
    TEST_ASSERT_EQUAL_INT(BARO_ERR_FAULT, err);
}

/* T-BARO-06: force pressure to 10999 — one read must not exceed 11000. */
void test_T_BARO_06_clamping_at_upper_bound(void)
{
    barometer_set_pressure_for_test(10999);

    baro_reading_t r   = {0};
    baro_err_t     err = barometer_read(&r);

    TEST_ASSERT_EQUAL_INT(BARO_ERR_OK, err);
    TEST_ASSERT_TRUE(r.pressure_x10 <= BARO_PRESSURE_MAX_X10);
}

/* T-BARO-07: force pressure to 3001 — one read must not go below 3000. */
void test_T_BARO_07_clamping_at_lower_bound(void)
{
    barometer_set_pressure_for_test(3001);

    baro_reading_t r   = {0};
    baro_err_t     err = barometer_read(&r);

    TEST_ASSERT_EQUAL_INT(BARO_ERR_OK, err);
    TEST_ASSERT_TRUE(r.pressure_x10 >= BARO_PRESSURE_MIN_X10);
}
