#include "stm32_cmsis_mock.h"

/* Storage is non-volatile; per-field volatile inside GPIO_TypeDef matches
 * real CMSIS pattern (volatile on registers, not on the struct). */
GPIO_TypeDef g_mock_gpio[MOCK_GPIO_PORT_COUNT];
RCC_TypeDef g_mock_rcc;
USART_TypeDef g_mock_usart3;
/* Add to storage definitions, alongside g_mock_gpio and g_mock_rcc: */
uint32_t g_mock_nvic_enable_count[NVIC_IRQ_COUNT_MAX];
uint32_t g_mock_nvic_disable_count[NVIC_IRQ_COUNT_MAX];

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
    g_mock_rcc.APB1ENR = 0; /* <-- add */

    /* Zero USART3 registers field-by-field (consistent with GPIO pattern). */
    g_mock_usart3.SR = 0;
    g_mock_usart3.DR = 0;
    g_mock_usart3.BRR = 0;
    g_mock_usart3.CR1 = 0;
    g_mock_usart3.CR2 = 0;
    g_mock_usart3.CR3 = 0;
    g_mock_usart3.GTPR = 0;

    /* Zero NVIC counters. */
    for (uint32_t i = 0; i < NVIC_IRQ_COUNT_MAX; i++)
    {
        g_mock_nvic_enable_count[i] = 0;
        g_mock_nvic_disable_count[i] = 0;
    }
}

/* NVIC mock implementations: */
void NVIC_EnableIRQ(IRQn_Type irqn)
{
    if ((uint32_t) irqn < NVIC_IRQ_COUNT_MAX)
    {
        g_mock_nvic_enable_count[irqn]++;
    }
}

void NVIC_DisableIRQ(IRQn_Type irqn)
{
    if ((uint32_t) irqn < NVIC_IRQ_COUNT_MAX)
    {
        g_mock_nvic_disable_count[irqn]++;
    }
}
