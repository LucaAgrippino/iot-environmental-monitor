/**
 * @file test_watchdog_driver.c
 * @brief Unity tests for WatchdogDriver (Field Device); IWDG and DBGMCU are the host mock structs.
 */

#include "stm32_cmsis_mock.h"
#include "unity.h"
#include "watchdog_driver.h"

void setUp(void)
{
    stm32_cmsis_mock_reset();
    watchdog_reset_for_test();
}

void tearDown(void)
{
}

void test_WDG_01_start_programs_prescaler_reload_and_refreshes(void)
{
    TEST_ASSERT_EQUAL(WATCHDOG_OK, watchdog_start(10000U));
    TEST_ASSERT_EQUAL_UINT32(6U, IWDG->PR);
    TEST_ASSERT_EQUAL_UINT32(1250U, IWDG->RLR);
    TEST_ASSERT_EQUAL_UINT32(0xAAAAU, IWDG->KR);
    TEST_ASSERT_TRUE(watchdog_is_running());
}

void test_WDG_02_out_of_range_timeouts_start_nothing(void)
{
    TEST_ASSERT_EQUAL(WATCHDOG_ERR_RANGE, watchdog_start(WATCHDOG_TIMEOUT_MIN_MS - 1U));
    TEST_ASSERT_EQUAL(WATCHDOG_ERR_RANGE, watchdog_start(WATCHDOG_TIMEOUT_MAX_MS + 1U));
    TEST_ASSERT_FALSE(watchdog_is_running());
    TEST_ASSERT_EQUAL_UINT32(0U, IWDG->RLR);
    TEST_ASSERT_EQUAL_UINT32(0U, IWDG->KR);
}

void test_WDG_03_kick_does_nothing_before_start_and_refreshes_after(void)
{
    watchdog_kick();
    TEST_ASSERT_EQUAL_UINT32(0U, IWDG->KR);

    TEST_ASSERT_EQUAL(WATCHDOG_OK, watchdog_start(10000U));
    IWDG->KR = 0U;
    watchdog_kick();
    TEST_ASSERT_EQUAL_UINT32(0xAAAAU, IWDG->KR);
}

void test_WDG_04_a_second_start_is_ignored(void)
{
    TEST_ASSERT_EQUAL(WATCHDOG_OK, watchdog_start(10000U));
    TEST_ASSERT_EQUAL(WATCHDOG_OK, watchdog_start(2000U));
    TEST_ASSERT_EQUAL_UINT32(1250U, IWDG->RLR);
}

void test_WDG_05_the_debug_freeze_bit_is_set(void)
{
    TEST_ASSERT_EQUAL(WATCHDOG_OK, watchdog_start(10000U));
    TEST_ASSERT_BITS(DBGMCU_APB1_FZ_DBG_IWDG_STOP, DBGMCU_APB1_FZ_DBG_IWDG_STOP,
                     DBGMCU->APB1FZ);
}

void test_WDG_06_the_longest_timeout_fits_the_12_bit_reload(void)
{
    TEST_ASSERT_EQUAL(WATCHDOG_OK, watchdog_start(WATCHDOG_TIMEOUT_MAX_MS));
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(4095U, IWDG->RLR);
    TEST_ASSERT_EQUAL_UINT32(3750U, IWDG->RLR);
}
