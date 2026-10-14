/**
 * @file test_stm32l4xx_it.c
 * @brief Unit tests for the Gateway EXTI vectors (firmware/gateway/Src/stm32l4xx_it.c).
 *
 * The driver ISR entry points and exti_clear_pending() are fakes defined here; the
 * pending register is the L475 EXTI mock (PR1). Each test sets the pending bits a
 * real interrupt would have raised and calls the vector directly.
 */

#include "unity.h"

#include <stdint.h>

#include "stm32l475_cmsis_mock.h"
#include "stm32l4xx_it.h"

#define LINE_WIFI (1U)
#define LINE_MAG (8U)
#define LINE_IMU (11U)

static uint32_t s_wifi_calls;
static uint32_t s_mag_calls;
static uint32_t s_imu_calls;
static uint32_t s_clear_calls;
static uint8_t s_clear_last;
static uint32_t s_pr1_at_wifi;
static uint32_t s_pr1_at_mag;
static uint32_t s_pr1_at_imu;

void exti_clear_pending(uint8_t line)
{
    s_clear_calls++;
    s_clear_last = line;
    EXTI->PR1 &= ~(1UL << line); /* write-1-to-clear on the real part */
}

void wifi_datardy_irq_handler(void)
{
    s_wifi_calls++;
    s_pr1_at_wifi = EXTI->PR1;
}

void magnetometer_drdy_irq_handler(void)
{
    s_mag_calls++;
    s_pr1_at_mag = EXTI->PR1;
}

void imu_int1_irq_handler(void)
{
    s_imu_calls++;
    s_pr1_at_imu = EXTI->PR1;
}

void setUp(void)
{
    stm32l475_cmsis_mock_reset();
    s_wifi_calls = 0U;
    s_mag_calls = 0U;
    s_imu_calls = 0U;
    s_clear_calls = 0U;
    s_clear_last = 0xFFU;
    s_pr1_at_wifi = 0U;
    s_pr1_at_mag = 0U;
    s_pr1_at_imu = 0U;
}

void tearDown(void)
{
}

/* TC-IT-001: EXTI1 -> WifiDriver, pending cleared before the driver runs. */
void test_TC_IT_001_exti1_dispatches_wifi(void)
{
    EXTI->PR1 = (1UL << LINE_WIFI);

    EXTI1_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(1U, s_wifi_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pr1_at_wifi); /* cleared first */
    TEST_ASSERT_EQUAL_UINT32(1U, s_clear_calls);
    TEST_ASSERT_EQUAL_UINT8(LINE_WIFI, s_clear_last);
}

/* TC-IT-002: EXTI9_5 with line 8 -> MagnetometerDriver. */
void test_TC_IT_002_exti9_5_dispatches_magnetometer(void)
{
    EXTI->PR1 = (1UL << LINE_MAG);

    EXTI9_5_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(1U, s_mag_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pr1_at_mag);
    TEST_ASSERT_EQUAL_UINT8(LINE_MAG, s_clear_last);
    TEST_ASSERT_EQUAL_UINT32(0U, s_wifi_calls + s_imu_calls);
}

/* TC-IT-003: EXTI15_10 with line 11 -> ImuDriver. */
void test_TC_IT_003_exti15_10_dispatches_imu(void)
{
    EXTI->PR1 = (1UL << LINE_IMU);

    EXTI15_10_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(1U, s_imu_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_pr1_at_imu);
    TEST_ASSERT_EQUAL_UINT8(LINE_IMU, s_clear_last);
    TEST_ASSERT_EQUAL_UINT32(0U, s_wifi_calls + s_mag_calls);
}

/* TC-IT-004: a pending line this vector does not own is neither cleared nor dispatched. */
void test_TC_IT_004_unowned_lines_are_left_alone(void)
{
    EXTI->PR1 = (1UL << 9U); /* shares EXTI9_5 with line 8, owned by nobody yet */
    EXTI9_5_IRQHandler();
    TEST_ASSERT_EQUAL_UINT32(0U, s_mag_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_clear_calls);
    TEST_ASSERT_EQUAL_UINT32((1UL << 9U), EXTI->PR1);

    EXTI->PR1 = (1UL << 13U); /* shares EXTI15_10 with line 11 */
    EXTI15_10_IRQHandler();
    TEST_ASSERT_EQUAL_UINT32(0U, s_imu_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_clear_calls);
}

/* TC-IT-005: only the owned line is cleared when others are pending too. */
void test_TC_IT_005_shared_vector_clears_only_its_line(void)
{
    EXTI->PR1 = (1UL << LINE_MAG) | (1UL << 6U);

    EXTI9_5_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(1U, s_mag_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_clear_calls);
    TEST_ASSERT_EQUAL_UINT32((1UL << 6U), EXTI->PR1);
}

/* TC-IT-006: a vector with nothing pending does nothing. */
void test_TC_IT_006_nothing_pending(void)
{
    EXTI1_IRQHandler();
    EXTI9_5_IRQHandler();
    EXTI15_10_IRQHandler();

    TEST_ASSERT_EQUAL_UINT32(0U, s_wifi_calls + s_mag_calls + s_imu_calls + s_clear_calls);
}
