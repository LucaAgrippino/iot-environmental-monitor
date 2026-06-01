#include "stm32_cmsis_mock.h"


/* ====================================================================== */
/* Mock storage definitions.                                              */
/*                                                                        */
/* One definition per peripheral declared `extern` in stm32f469xx.h.      */
/* Adding a new peripheral: append its instance in a fresh §-section      */
/* below, then extend stm32_cmsis_mock_reset() with its zero-init.        */
/* ====================================================================== */


/* ====================================================================== */
/* §GPIO storage                                                          */
/* ====================================================================== */

/* Storage is non-volatile; per-field volatile inside GPIO_TypeDef matches
 * real CMSIS pattern (volatile on registers, not on the struct). */
GPIO_TypeDef g_mock_gpio[MOCK_GPIO_PORT_COUNT];


/* ====================================================================== */
/* §RCC storage                                                           */
/* ====================================================================== */

RCC_TypeDef g_mock_rcc;


/* ====================================================================== */
/* §USART storage                                                         */
/* ====================================================================== */

USART_TypeDef g_mock_usart3;


/* ====================================================================== */
/* §PWR storage                                                           */
/* ====================================================================== */

PWR_TypeDef g_mock_pwr;


/* ====================================================================== */
/* §I2C storage (I2cDriver — F469 I2C1)                                  */
/* ====================================================================== */

I2C_TypeDef g_mock_i2c1;

/* ====================================================================== */
/* §RTC storage                                                           */
/* ====================================================================== */

RTC_TypeDef g_mock_rtc;


/* ====================================================================== */
/* §NVIC storage                                                          */
/* ====================================================================== */

uint32_t g_mock_nvic_enable_count[NVIC_IRQ_COUNT_MAX];
uint32_t g_mock_nvic_disable_count[NVIC_IRQ_COUNT_MAX];


/* ====================================================================== */
/* Reset routine — clears all mock peripheral state.                      */
/*                                                                        */
/* Explicit per-field assignment (rather than memset) avoids any UB       */
/* questions around memset on structs with volatile members, and keeps    */
/* the reset list serving as a checklist of every register the mock owns. */
/* ====================================================================== */

void stm32_cmsis_mock_reset(void)
{
    /* §GPIO */
    for (uint32_t i = 0; i < MOCK_GPIO_PORT_COUNT; ++i)
    {
        g_mock_gpio[i].MODER   = 0;
        g_mock_gpio[i].OTYPER  = 0;
        g_mock_gpio[i].OSPEEDR = 0;
        g_mock_gpio[i].PUPDR   = 0;
        g_mock_gpio[i].IDR     = 0;
        g_mock_gpio[i].ODR     = 0;
        g_mock_gpio[i].BSRR    = 0;
        g_mock_gpio[i].LCKR    = 0;
        g_mock_gpio[i].AFR[0]  = 0;
        g_mock_gpio[i].AFR[1]  = 0;
    }

    /* §RCC */
    g_mock_rcc.AHB1ENR = 0;
    g_mock_rcc.APB1ENR = 0;
    g_mock_rcc.BDCR    = 0;

    /* §USART */
    g_mock_usart3.SR   = 0;
    g_mock_usart3.DR   = 0;
    g_mock_usart3.BRR  = 0;
    g_mock_usart3.CR1  = 0;
    g_mock_usart3.CR2  = 0;
    g_mock_usart3.CR3  = 0;
    g_mock_usart3.GTPR = 0;

    /* §PWR */
    g_mock_pwr.CR = 0;

    /* §I2C1 */
    g_mock_i2c1.CR1   = 0;
    g_mock_i2c1.CR2   = 0;
    g_mock_i2c1.OAR1  = 0;
    g_mock_i2c1.OAR2  = 0;
    g_mock_i2c1.DR    = 0;
    g_mock_i2c1.SR1   = 0;
    g_mock_i2c1.SR2   = 0;
    g_mock_i2c1.CCR   = 0;
    g_mock_i2c1.TRISE = 0;

    /* §RTC */
    g_mock_rtc.TR    = 0;
    g_mock_rtc.DR    = 0;
    g_mock_rtc.CR    = 0;
    g_mock_rtc.ISR   = 0;
    g_mock_rtc.PRER  = 0;
    g_mock_rtc.WPR   = 0;
    g_mock_rtc.BKP0R = 0;
    for (uint32_t i = 0;
         i < (sizeof(g_mock_rtc.BKP1R_to_BKP19R) / sizeof(g_mock_rtc.BKP1R_to_BKP19R[0]));
         ++i)
    {
        g_mock_rtc.BKP1R_to_BKP19R[i] = 0;
    }

    /* §NVIC */
    for (uint32_t i = 0; i < NVIC_IRQ_COUNT_MAX; ++i)
    {
        g_mock_nvic_enable_count[i]  = 0;
        g_mock_nvic_disable_count[i] = 0;
    }
}


/* ====================================================================== */
/* §NVIC implementations                                                  */
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
