#include "unity.h"
#include "stm32_cmsis_mock.h"
#include "gpio_driver.h"


extern void gpio_driver_reset_for_test(void);

void setUp(void)
{
    stm32_cmsis_mock_reset();
    gpio_driver_reset_for_test();
}

void tearDown(void)
{
}

/* Proves: the GPIOA macro resolves to a real, writable, readable backing
 * cell, and the mock storage is volatile-correct (host compiler doesn't
 * optimise the write away). */
void test_mock_gpio_write_read_round_trip(void)
{
    GPIOA->MODER = 0xDEADBEEFu;
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, GPIOA->MODER);
}

/* Proves: stm32_cmsis_mock_reset() clears GPIO state between tests.
 * If this fails, GPIO bug 2 (missing RCC zeroing) had a GPIO cousin. */
void test_mock_reset_clears_gpio_moder(void)
{
    GPIOK->MODER = 0xFFFFFFFFu;
    stm32_cmsis_mock_reset();
    TEST_ASSERT_EQUAL_HEX32(0u, GPIOK->MODER);
}

/* Proves: the RCC fix works. Catches the bug we just patched. */
void test_mock_reset_clears_rcc_ahb1enr(void)
{
    RCC->AHB1ENR = RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOHEN;
    stm32_cmsis_mock_reset();
    TEST_ASSERT_EQUAL_HEX32(0u, RCC->AHB1ENR);
}

/* Proves: GPIO initialisation succeeds on the first call. */
void test_gpio_init_succeeds_first_call(void)
{
    TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_init());
}

/* Proves: GPIO initialisation idempotent after the first call. */
void test_gpio_init_idempotent_second_call_returns_ok(void)
{
	TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_init());
	TEST_ASSERT_EQUAL_INT(GPIO_OK, gpio_init());
}

/* Proves: The clock initialization for each gpio port*/
void test_gpio_init_sets_rcc_ahb1enr_gpio_a_through_k_bits(void)
{
	gpio_init();

	// 0x7FF = first 11 bits setted
	TEST_ASSERT_EQUAL_HEX32(0x7FF, RCC->AHB1ENR);
}
