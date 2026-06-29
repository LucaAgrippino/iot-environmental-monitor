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
/* §PWR storage (CpuDriver)                                               */
/* ====================================================================== */

PWR_TypeDef g_mock_pwr;


/* ====================================================================== */
/* §FLASH storage (CpuDriver)                                             */
/* ====================================================================== */

FLASH_TypeDef g_mock_flash;


/* ====================================================================== */
/* §CoreDebug storage (CpuDriver)                                         */
/* ====================================================================== */

CoreDebug_TypeDef g_mock_core_debug;


/* ====================================================================== */
/* §DWT storage (CpuDriver)                                               */
/* ====================================================================== */

DWT_TypeDef g_mock_dwt;


/* ====================================================================== */
/* §SCB storage (CpuDriver fault status)                                  */
/* ====================================================================== */

SCB_TypeDef g_mock_scb;


/* ====================================================================== */
/* §RTC backup-register storage (CpuDriver post-mortem)                  */
/* ====================================================================== */

RTC_TypeDef g_mock_rtc;


/* ====================================================================== */
/* §I2C2 storage (I2cDriver — L475 I2C v2)                               */
/* ====================================================================== */

I2C_TypeDef g_mock_i2c2;


/* ====================================================================== */
/* §UART4 storage (ModbusUartDriver GW)                                  */
/* ====================================================================== */

USART_L4_TypeDef g_mock_uart4;


/* ====================================================================== */
/* §USART1 storage (CpuDriver panic UART)                                 */
/* ====================================================================== */

USART_L4_TypeDef g_mock_usart1;


/* ====================================================================== */
/* §NVIC storage (L475)                                                   */
/* ====================================================================== */

uint32_t g_mock_nvic_enable_count[NVIC_IRQ_COUNT_MAX];
uint32_t g_mock_nvic_disable_count[NVIC_IRQ_COUNT_MAX];


/* ====================================================================== */
/* §CpuDriver hw-abstraction stub storage                                 */
/* ====================================================================== */

uint32_t g_cpu_hw_disable_irq_count;
uint32_t g_cpu_hw_breakpoint_count;
uint32_t g_cpu_hw_system_reset_count;


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
    g_mock_rcc_l4.CR       = 0;
    g_mock_rcc_l4.CFGR     = 0;
    g_mock_rcc_l4.PLLCFGR  = 0;
    g_mock_rcc_l4.AHB2ENR  = 0;
    g_mock_rcc_l4.APB1ENR1 = 0;
    g_mock_rcc_l4.APB2ENR  = 0;

    /* §PWR */
    g_mock_pwr.CR1 = 0;

    /* §FLASH */
    g_mock_flash.ACR = 0;

    /* §CoreDebug */
    g_mock_core_debug.DEMCR = 0;

    /* §DWT */
    g_mock_dwt.CTRL   = 0;
    g_mock_dwt.CYCCNT = 0;

    /* §SCB */
    g_mock_scb.CFSR  = 0;
    g_mock_scb.HFSR  = 0;
    g_mock_scb.BFAR  = 0;
    g_mock_scb.MMFAR = 0;

    /* §RTC */
    g_mock_rtc.BKP0R  = 0;
    g_mock_rtc.BKP1R  = 0;
    g_mock_rtc.BKP2R  = 0;
    g_mock_rtc.BKP3R  = 0;
    g_mock_rtc.BKP4R  = 0;
    g_mock_rtc.BKP5R  = 0;
    g_mock_rtc.BKP6R  = 0;
    g_mock_rtc.BKP7R  = 0;
    g_mock_rtc.BKP8R  = 0;
    g_mock_rtc.BKP9R  = 0;
    g_mock_rtc.BKP10R = 0;
    g_mock_rtc.BKP11R = 0;
    g_mock_rtc.BKP12R = 0;
    g_mock_rtc.BKP13R = 0;
    g_mock_rtc.BKP14R = 0;
    g_mock_rtc.BKP15R = 0;
    g_mock_rtc.BKP16R = 0;

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

    /* §USART1 */
    g_mock_usart1.CR1 = 0;
    g_mock_usart1.CR2 = 0;
    g_mock_usart1.CR3 = 0;
    g_mock_usart1.BRR = 0;
    g_mock_usart1.ISR = 0;
    g_mock_usart1.ICR = 0;
    g_mock_usart1.RDR = 0;
    g_mock_usart1.TDR = 0;

    /* §NVIC */
    for (uint32_t i = 0; i < NVIC_IRQ_COUNT_MAX; ++i)
    {
        g_mock_nvic_enable_count[i]  = 0;
        g_mock_nvic_disable_count[i] = 0;
    }

    /* §CpuDriver hw-abstraction counters */
    g_cpu_hw_disable_irq_count  = 0;
    g_cpu_hw_breakpoint_count   = 0;
    g_cpu_hw_system_reset_count = 0;
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


/* ====================================================================== */
/* §CpuDriver hw-abstraction stub implementations                         */
/* ====================================================================== */

void cpu_hw_disable_irq(void)
{
    g_cpu_hw_disable_irq_count++;
}

void cpu_hw_breakpoint(void)
{
    g_cpu_hw_breakpoint_count++;
}

void cpu_hw_system_reset(void)
{
    g_cpu_hw_system_reset_count++;
}
