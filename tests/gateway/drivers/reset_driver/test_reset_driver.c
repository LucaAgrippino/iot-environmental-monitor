/**
 * @file test_reset_driver.c
 * @brief Unity tests for ResetDriver (Gateway).
 *
 * NVIC_SystemReset() and __DSB() are replaced by the instrumented hooks in
 * stm32l475_cmsis_mock.c, so the test can see them without resetting the host.
 * Test IDs follow companion section 7 (T-RST-01, T-RST-02); T-RST-03.. are additions.
 */

#include "cpu/cpu_hw.h"
#include "reset_driver.h"
#include "stm32l475_cmsis_mock.h"
#include "unity.h"

/* T-RST-02 is the compile-time half: this file includes no FreeRTOS header, and the driver header
 * must not pull one in. */
#ifdef FREERTOS_H
#error "reset_driver.h must not include FreeRTOS.h"
#endif

void setUp(void)
{
    stm32l475_cmsis_mock_reset();
}

void tearDown(void)
{
}

void test_RST_01_reset_trigger_requests_a_system_reset(void)
{
    reset_trigger();
    TEST_ASSERT_EQUAL_UINT32(1U, g_cpu_hw_system_reset_count);
}

void test_RST_01b_a_data_barrier_is_issued_before_the_reset(void)
{
    reset_trigger();
    TEST_ASSERT_EQUAL_UINT32(1U, g_cpu_hw_data_barrier_count);
    TEST_ASSERT_EQUAL_UINT32(1U, g_cpu_hw_barriers_at_reset);
}

void test_RST_03_the_driver_touches_nothing_else(void)
{
    reset_trigger();
    TEST_ASSERT_EQUAL_UINT32(0U, g_cpu_hw_disable_irq_count);
    TEST_ASSERT_EQUAL_UINT32(0U, g_cpu_hw_breakpoint_count);
}

void test_RST_04_the_driver_is_stateless_so_a_second_call_resets_again(void)
{
    reset_trigger();
    reset_trigger();
    TEST_ASSERT_EQUAL_UINT32(2U, g_cpu_hw_system_reset_count);
    TEST_ASSERT_EQUAL_UINT32(2U, g_cpu_hw_data_barrier_count);
}
