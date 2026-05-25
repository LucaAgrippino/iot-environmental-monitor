#include "unity.h"
#include "stm32_cmsis_mock.h"

void setUp(void)
{
    stm32_cmsis_mock_reset();
}

void tearDown(void)
{
}

/* Proves: USART3 macro resolves to writable storage, fields are accessible
 * by their RM0386 names, and the volatile-on-fields pattern works. */
void test_mock_usart3_round_trip(void)
{
    USART3->BRR = 0x1869u;   /* Some plausible BRR value */
    USART3->CR1 = (1u << 13) | (1u << 3);   /* UE | TE */

    TEST_ASSERT_EQUAL_HEX32(0x1869u, USART3->BRR);
    TEST_ASSERT_EQUAL_HEX32((1u << 13) | (1u << 3), USART3->CR1);
}

/* Proves: APB1ENR field exists and is independently writable from AHB1ENR. */
void test_mock_rcc_apb1enr_independent_from_ahb1enr(void)
{
    RCC->AHB1ENR = 0xDEADBEEFu;
    RCC->APB1ENR = RCC_APB1ENR_USART3EN;

    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, RCC->AHB1ENR);
    TEST_ASSERT_EQUAL_HEX32(1u << 18, RCC->APB1ENR);
}

/* Proves: NVIC mock records enable/disable calls and the reset clears them. */
void test_mock_nvic_records_enable_and_disable(void)
{
    NVIC_EnableIRQ(USART3_IRQn);
    NVIC_EnableIRQ(USART3_IRQn);
    NVIC_DisableIRQ(USART3_IRQn);

    TEST_ASSERT_EQUAL_UINT32(2u, g_mock_nvic_enable_count[USART3_IRQn]);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_nvic_disable_count[USART3_IRQn]);
}

/* Proves: reset zeroes USART3 storage and NVIC counters. */
void test_mock_reset_clears_usart3_and_nvic(void)
{
    USART3->CR1 = 0xFFFFFFFFu;
    NVIC_EnableIRQ(USART3_IRQn);

    stm32_cmsis_mock_reset();

    TEST_ASSERT_EQUAL_HEX32(0u, USART3->CR1);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_nvic_enable_count[USART3_IRQn]);
}