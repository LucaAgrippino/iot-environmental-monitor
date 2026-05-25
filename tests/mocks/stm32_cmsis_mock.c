#include "stm32_cmsis_mock.h"

/* Storage is non-volatile; per-field volatile inside GPIO_TypeDef matches
 * real CMSIS pattern (volatile on registers, not on the struct). */
GPIO_TypeDef g_mock_gpio[MOCK_GPIO_PORT_COUNT];
RCC_TypeDef g_mock_rcc;

void stm32_cmsis_mock_reset(void)
{
    /* Explicit per-field assignment loop: no UB, but more verbose. */
    for (uint8_t i = 0; i < MOCK_GPIO_PORT_COUNT; ++i)
    {
        g_mock_gpio[i].MODER = 0;
        g_mock_gpio[i].OTYPER = 0;
        g_mock_gpio[i].OSPEEDR = 0;
        g_mock_gpio[i].PUPDR = 0;
        g_mock_gpio[i].IDR = 0;
        g_mock_gpio[i].ODR = 0;
        g_mock_gpio[i].BSRR = 0;
        g_mock_gpio[i].LCKR = 0;
        g_mock_gpio[i].AFR[0] = 0;
        g_mock_gpio[i].AFR[1] = 0;
    }

    g_mock_rcc.AHB1ENR = 0;
}
