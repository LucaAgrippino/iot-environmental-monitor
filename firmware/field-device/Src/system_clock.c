/**
 * @file system_clock.c
 * @brief F469 system clock initialisation — 180 MHz via HSE + PLL.
 *
 * References:
 *   - RM0386 §6.3 RCC
 *   - RM0386 §5.4.1 PWR_CR (over-drive)
 *   - RM0386 §3.5.1 FLASH_ACR (latency)
 */

#include "system_clock.h"

#include "stm32f469xx.h"

void system_clock_init(void)
{
    /* 1. Enable HSE (8 MHz crystal on the Discovery board) and wait. */
    RCC->CR |= RCC_CR_HSEON;
    while ((RCC->CR & RCC_CR_HSERDY) == 0U)
    {
        /* Blocks forever if HSE is missing — debugger will land here. */
    }

    /* 2. Enable PWR clock so we can configure the voltage scale and
     *    over-drive mode. Then select Scale 1 (VOS[1:0] = 11). */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR |= PWR_CR_VOS;

    /* 3. Configure Flash for 180 MHz operation at 3.3 V:
     *    5 wait states, prefetch enabled, instruction + data caches on. */
    FLASH->ACR = FLASH_ACR_LATENCY_5WS | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;

    /* 4. Set bus prescalers BEFORE switching SYSCLK to PLL, otherwise
     *    APB1/APB2 would briefly run at 180 MHz (over-spec). */
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2)) |
                RCC_CFGR_HPRE_DIV1     /* AHB  = SYSCLK / 1 = 180 MHz */
                | RCC_CFGR_PPRE1_DIV4  /* APB1 = HCLK   / 4 =  45 MHz */
                | RCC_CFGR_PPRE2_DIV2; /* APB2 = HCLK   / 2 =  90 MHz */

    /* 5. Configure the PLL.
     *      VCO input  = HSE / M  =   8 MHz / 8 = 1 MHz
     *      VCO output = VCO_in × N = 1 MHz × 360 = 360 MHz
     *      SYSCLK     = VCO_out / P = 360 / 2 = 180 MHz
     *      48 MHz dom = VCO_out / Q = 360 / 7 ≈ 51.4 MHz (unused, USB off)
     *
     *    PLLP encoding: 00 → /2, 01 → /4, 10 → /6, 11 → /8 — so /2 is
     *    written as zero in the PLLP field. */
    RCC->PLLCFGR = (8U << RCC_PLLCFGR_PLLM_Pos) | (360U << RCC_PLLCFGR_PLLN_Pos) |
                   (0U << RCC_PLLCFGR_PLLP_Pos) | (7U << RCC_PLLCFGR_PLLQ_Pos) |
                   RCC_PLLCFGR_PLLSRC_HSE;

    /* 6. Enable PLL and wait for lock. */
    RCC->CR |= RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) == 0U)
    {
    }

    /* 7. Enable over-drive mode (mandatory for SYSCLK > 168 MHz).
     *    Two-step: ODEN → wait ODRDY; ODSWEN → wait ODSWRDY. */
    PWR->CR |= PWR_CR_ODEN;
    while ((PWR->CSR & PWR_CSR_ODRDY) == 0U)
    {
    }
    PWR->CR |= PWR_CR_ODSWEN;
    while ((PWR->CSR & PWR_CSR_ODSWRDY) == 0U)
    {
    }

    /* 8. Switch SYSCLK source to PLL and wait until SWS reflects it. */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL)
    {
    }

    /* 9. Enable External low-speed oscillator */
    PWR->CR |= PWR_CR_DBP; // enable RCC->BDCR write
    RCC->BDCR |= RCC_BDCR_LSEON;

    /* Drivers should reference SYSTEM_PCLK1_HZ / SYSTEM_PCLK2_HZ from
     * system_clock.h for their baud-rate / timing maths — not the CMSIS
     * SystemCoreClock global, which is informational only here. */
}

void system_clock_enable_dwt(void)
{
    /* Enable trace subsystem — gates clock to DWT. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

    /* Reset and enable the cycle counter. */
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
