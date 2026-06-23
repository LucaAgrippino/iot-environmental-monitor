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

USART_TypeDef g_mock_usart3; /* DebugUartDriver (USART3) */
USART_TypeDef g_mock_usart6; /* ModbusUartDriver FD (USART6) */


/* ====================================================================== */
/* §PWR storage                                                           */
/* ====================================================================== */

PWR_TypeDef g_mock_pwr;


/* ====================================================================== */
/* §I2C storage (I2cDriver — F469 I2C1)                                  */
/* ====================================================================== */

I2C_TypeDef g_mock_i2c1;

/* ====================================================================== */
/* §FMC Bank5_6 storage (SdramDriver)                                    */
/* ====================================================================== */

FMC_Bank5_6_TypeDef g_mock_fmc_bank5_6;


/* ====================================================================== */
/* §QUADSPI storage (QspiFlashDriver)                                    */
/* ====================================================================== */

QUADSPI_TypeDef g_mock_quadspi;
uint8_t         g_mock_quadspi_rx_fifo[QUADSPI_MOCK_FIFO_DEPTH];
uint32_t        g_mock_quadspi_rx_fifo_idx;


/* ====================================================================== */
/* §RTC storage                                                           */
/* ====================================================================== */

RTC_TypeDef g_mock_rtc;


/* ====================================================================== */
/* §SYSCFG storage (TouchscreenDriver)                                    */
/* ====================================================================== */

SYSCFG_TypeDef g_mock_syscfg;


/* ====================================================================== */
/* §LTDC storage (LcdDriver)                                             */
/* ====================================================================== */

LTDC_TypeDef g_mock_ltdc;


/* ====================================================================== */
/* §EXTI storage (TouchscreenDriver)                                      */
/* ====================================================================== */

EXTI_TypeDef g_mock_exti;


/* ====================================================================== */
/* §NVIC storage                                                          */
/* ====================================================================== */

uint32_t g_mock_nvic_enable_count[NVIC_IRQ_COUNT_MAX];
uint32_t g_mock_nvic_disable_count[NVIC_IRQ_COUNT_MAX];
uint32_t g_mock_nvic_priority[NVIC_IRQ_COUNT_MAX];


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
    g_mock_rcc.AHB2ENR = 0;
    g_mock_rcc.AHB3ENR = 0;
    g_mock_rcc.APB1ENR = 0;
    g_mock_rcc.APB2ENR = 0;
    g_mock_rcc.BDCR    = 0;

    /* §USART3 (DebugUartDriver) */
    g_mock_usart3.SR   = 0;
    g_mock_usart3.DR   = 0;
    g_mock_usart3.BRR  = 0;
    g_mock_usart3.CR1  = 0;
    g_mock_usart3.CR2  = 0;
    g_mock_usart3.CR3  = 0;
    g_mock_usart3.GTPR = 0;

    /* §USART6 (ModbusUartDriver FD) */
    g_mock_usart6.SR   = 0;
    g_mock_usart6.DR   = 0;
    g_mock_usart6.BRR  = 0;
    g_mock_usart6.CR1  = 0;
    g_mock_usart6.CR2  = 0;
    g_mock_usart6.CR3  = 0;
    g_mock_usart6.GTPR = 0;

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

    /* §FMC Bank5_6 */
    g_mock_fmc_bank5_6.SDCR[0] = 0;
    g_mock_fmc_bank5_6.SDCR[1] = 0;
    g_mock_fmc_bank5_6.SDTR[0] = 0;
    g_mock_fmc_bank5_6.SDTR[1] = 0;
    g_mock_fmc_bank5_6.SDCMR   = 0;
    g_mock_fmc_bank5_6.SDRTR   = 0;
    g_mock_fmc_bank5_6.SDSR    = 0;

    /* §QUADSPI */
    g_mock_quadspi.CR  = 0;
    g_mock_quadspi.DCR = 0;
    g_mock_quadspi.SR  = 0;
    g_mock_quadspi.FCR = 0;
    g_mock_quadspi.DLR = 0;
    g_mock_quadspi.CCR = 0;
    g_mock_quadspi.AR  = 0;
    g_mock_quadspi.ABR = 0;
    g_mock_quadspi.DR  = 0;
    for (uint32_t i = 0; i < QUADSPI_MOCK_FIFO_DEPTH; ++i)
    {
        g_mock_quadspi_rx_fifo[i] = 0;
    }
    g_mock_quadspi_rx_fifo_idx = 0;

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

    /* §LTDC */
    g_mock_ltdc.IER = 0;
    g_mock_ltdc.ISR = 0;
    g_mock_ltdc.ICR = 0;

    /* §SYSCFG */
    g_mock_syscfg.MEMRMP = 0;
    g_mock_syscfg.PMC    = 0;
    for (uint32_t i = 0; i < 4U; ++i)
    {
        g_mock_syscfg.EXTICR[i] = 0;
    }

    /* §EXTI */
    g_mock_exti.IMR   = 0;
    g_mock_exti.EMR   = 0;
    g_mock_exti.RTSR  = 0;
    g_mock_exti.FTSR  = 0;
    g_mock_exti.SWIER = 0;
    g_mock_exti.PR    = 0;

    /* §NVIC */
    for (uint32_t i = 0; i < NVIC_IRQ_COUNT_MAX; ++i)
    {
        g_mock_nvic_enable_count[i]  = 0;
        g_mock_nvic_disable_count[i] = 0;
        g_mock_nvic_priority[i]      = 0;
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

void NVIC_SetPriority(IRQn_Type irqn, uint32_t priority)
{
    if ((uint32_t) irqn < NVIC_IRQ_COUNT_MAX)
    {
        g_mock_nvic_priority[irqn] = priority;
    }
}
