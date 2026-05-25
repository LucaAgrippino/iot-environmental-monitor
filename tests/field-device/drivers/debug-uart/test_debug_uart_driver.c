#include "unity.h"
#include "stm32_cmsis_mock.h"
#include "debug_uart_driver.h"

extern void debug_uart_reset_for_test(void);

void setUp(void)
{
    stm32_cmsis_mock_reset();
    debug_uart_reset_for_test();
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

void test_debug_uart_init_succeeds_first_call(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());
}

void test_debug_uart_init_idempotent_second_call_returns_ok(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());
}

void test_debug_uart_init_enables_usart_and_gpio_clocks(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    /* AHB1ENR must have GPIOBEN set. */
    TEST_ASSERT_BITS_HIGH(RCC_AHB1ENR_GPIOBEN, RCC->AHB1ENR);

    /* APB1ENR must have USART3EN set. */
    TEST_ASSERT_BITS_HIGH(RCC_APB1ENR_USART3EN, RCC->APB1ENR);
}

void test_debug_uart_init_configures_pin_alternate_function(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    /* MODER bits [21:20] = pin 10, [23:22] = pin 11, both = 0b10. */
    TEST_ASSERT_EQUAL_HEX32((0x2U << 20) | (0x2U << 22),
                            GPIOB->MODER & ((0x3U << 20) | (0x3U << 22)));

    /* AFR[1] bits [11:8] = pin 10, [15:12] = pin 11, both = 7. */
    TEST_ASSERT_EQUAL_HEX32((0x7U << 8) | (0x7U << 12),
                            GPIOB->AFR[1] & ((0xFU << 8) | (0xFU << 12)));
}

void test_debug_uart_init_programs_baud_rate_for_pclk(void)
{
    TEST_ASSERT_EQUAL_INT(DEBUG_UART_OK, debug_uart_init());

    /* BRR = round(PCLK1 / BAUD) = round(45000000 / 115200) = 391 = 0x187. */
    TEST_ASSERT_EQUAL_HEX32(0x187u, USART3->BRR);
}
