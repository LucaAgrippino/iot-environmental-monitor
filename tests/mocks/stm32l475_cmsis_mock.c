#include "stm32l475_cmsis_mock.h"

/* ====================================================================== */
/* Mock storage definitions.                                              */
/*                                                                        */
/* One definition per peripheral declared `extern` in stm32l475xx.h.      */
/* Adding a new peripheral: append its instance in a fresh section below, */
/* then extend stm32l475_cmsis_mock_reset() with its zero-init.           */
/* ====================================================================== */


/* ====================================================================== */
/* §GPIO storage (L4)                                                     */
/* ====================================================================== */

GPIO_TypeDef g_mock_gpio_l4[MOCK_GPIO_PORT_COUNT_L4];


/* ====================================================================== */
/* §RCC storage (L4)                                                      */
/* ====================================================================== */

RCC_TypeDef g_mock_rcc_l4;


/* ====================================================================== */
/* §I2C2 storage (I2cDriver — L475 I2C v2)                               */
/* ====================================================================== */

I2C_TypeDef g_mock_i2c2;


/* ====================================================================== */
/* §UART4 storage (ModbusUartDriver GW)                                  */
/* ====================================================================== */

USART_L4_TypeDef g_mock_uart4;


/* ====================================================================== */
/* §NVIC storage (L475)                                                   */
/* ====================================================================== */

uint32_t g_mock_nvic_enable_count[NVIC_IRQ_COUNT_MAX];
uint32_t g_mock_nvic_disable_count[NVIC_IRQ_COUNT_MAX];


/* ====================================================================== */
/* Reset routine — clears all L475 mock peripheral state.                 */
/* ====================================================================== */

void stm32l475_cmsis_mock_reset(void)
{
    /* §GPIO */
    for (uint32_t i = 0; i < MOCK_GPIO_PORT_COUNT_L4; ++i)
    {
        g_mock_gpio_l4[i].MODER   = 0;
        g_mock_gpio_l4[i].OTYPER  = 0;
        g_mock_gpio_l4[i].OSPEEDR = 0;
        g_mock_gpio_l4[i].PUPDR   = 0;
        g_mock_gpio_l4[i].IDR     = 0;
        g_mock_gpio_l4[i].ODR     = 0;
        g_mock_gpio_l4[i].BSRR    = 0;
        g_mock_gpio_l4[i].LCKR    = 0;
        g_mock_gpio_l4[i].AFR[0]  = 0;
        g_mock_gpio_l4[i].AFR[1]  = 0;
    }

    /* §RCC */
    g_mock_rcc_l4.AHB2ENR  = 0;
    g_mock_rcc_l4.APB1ENR1 = 0;

    /* §I2C2 */
    g_mock_i2c2.CR1      = 0;
    g_mock_i2c2.CR2      = 0;
    g_mock_i2c2.OAR1     = 0;
    g_mock_i2c2.OAR2     = 0;
    g_mock_i2c2.TIMINGR  = 0;
    g_mock_i2c2.TIMEOUTR = 0;
    g_mock_i2c2.ISR      = 0;
    g_mock_i2c2.ICR      = 0;
    g_mock_i2c2.PECR     = 0;
    g_mock_i2c2.RXDR     = 0;
    g_mock_i2c2.TXDR     = 0;

    /* §UART4 */
    g_mock_uart4.CR1 = 0;
    g_mock_uart4.CR2 = 0;
    g_mock_uart4.CR3 = 0;
    g_mock_uart4.BRR = 0;
    g_mock_uart4.ISR = 0;
    g_mock_uart4.ICR = 0;
    g_mock_uart4.RDR = 0;
    g_mock_uart4.TDR = 0;

    /* §NVIC */
    for (uint32_t i = 0; i < NVIC_IRQ_COUNT_MAX; ++i)
    {
        g_mock_nvic_enable_count[i]  = 0;
        g_mock_nvic_disable_count[i] = 0;
    }
}


/* ====================================================================== */
/* §NVIC implementations (L475)                                           */
/* ====================================================================== */

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
