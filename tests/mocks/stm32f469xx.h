#ifndef STM32F469XX_H
#define STM32F469XX_H

#include <stdint.h>

/* ====================================================================== */
/* Mock CMSIS header for STM32F469 — host-side unit-test builds only.     */
/*                                                                        */
/* Each per-peripheral section contains, in order:                        */
/*   1. TypeDef (only the fields a driver under test actually touches)    */
/*   2. Mock storage extern (defined in mock_cmsis.c)                     */
/*   3. Pointer macro(s) that driver code dereferences                    */
/*   4. Bit constants used by the corresponding driver(s)                 */
/*                                                                        */
/* Adding a new driver:                                                   */
/*   - New peripheral → append a fresh §-section immediately BEFORE       */
/*     §NVIC at the end of the file.                                      */
/*   - Existing peripheral → extend the TypeDef in place (add the         */
/*     field) and append the new bit constants to that section.           */
/*   - RCC accumulates fields across drivers (AHB1ENR, APB1ENR, BDCR …).  */
/*     Extend it in §RCC; do not duplicate.                               */
/* ====================================================================== */

/* Self-define the device identifier so driver translation units that use
 * `#if defined(STM32F469xx)` for board-conditional compilation work against
 * the mock without requiring -DSTM32F469xx on the test build command line.
 * Production builds set this via -D before any device header is included;
 * the mock supplies it here to make the test environment self-contained. */
#ifndef STM32F469xx
#define STM32F469xx
#endif

/* ====================================================================== */
/* §RCC — Reset and Clock Control                                         */
/* ====================================================================== */

/* Fields accumulate as drivers need them. Current owners:                */
/*   AHB1ENR  — GpioDriver                                                */
/*   APB1ENR  — DebugUartDriver (USART3EN), RtcDriver (PWREN)             */
/*   BDCR     — RtcDriver (LSE, RTCSEL, RTCEN)                            */
typedef struct
{
    volatile uint32_t AHB1ENR;
    volatile uint32_t APB1ENR;
    volatile uint32_t BDCR;
} RCC_TypeDef;

extern RCC_TypeDef g_mock_rcc;

#define RCC (&g_mock_rcc)

/* --- AHB1ENR bits (GpioDriver) ---------------------------------------- */
#define RCC_AHB1ENR_GPIOAEN_Pos (0U)
#define RCC_AHB1ENR_GPIOBEN_Pos (1U)
#define RCC_AHB1ENR_GPIOCEN_Pos (2U)
#define RCC_AHB1ENR_GPIODEN_Pos (3U)
#define RCC_AHB1ENR_GPIOEEN_Pos (4U)
#define RCC_AHB1ENR_GPIOFEN_Pos (5U)
#define RCC_AHB1ENR_GPIOGEN_Pos (6U)
#define RCC_AHB1ENR_GPIOHEN_Pos (7U)
#define RCC_AHB1ENR_GPIOIEN_Pos (8U)
#define RCC_AHB1ENR_GPIOJEN_Pos (9U)
#define RCC_AHB1ENR_GPIOKEN_Pos (10U)

#define RCC_AHB1ENR_GPIOAEN (1UL << RCC_AHB1ENR_GPIOAEN_Pos)
#define RCC_AHB1ENR_GPIOBEN (1UL << RCC_AHB1ENR_GPIOBEN_Pos)
#define RCC_AHB1ENR_GPIOCEN (1UL << RCC_AHB1ENR_GPIOCEN_Pos)
#define RCC_AHB1ENR_GPIODEN (1UL << RCC_AHB1ENR_GPIODEN_Pos)
#define RCC_AHB1ENR_GPIOEEN (1UL << RCC_AHB1ENR_GPIOEEN_Pos)
#define RCC_AHB1ENR_GPIOFEN (1UL << RCC_AHB1ENR_GPIOFEN_Pos)
#define RCC_AHB1ENR_GPIOGEN (1UL << RCC_AHB1ENR_GPIOGEN_Pos)
#define RCC_AHB1ENR_GPIOHEN (1UL << RCC_AHB1ENR_GPIOHEN_Pos)
#define RCC_AHB1ENR_GPIOIEN (1UL << RCC_AHB1ENR_GPIOIEN_Pos)
#define RCC_AHB1ENR_GPIOJEN (1UL << RCC_AHB1ENR_GPIOJEN_Pos)
#define RCC_AHB1ENR_GPIOKEN (1UL << RCC_AHB1ENR_GPIOKEN_Pos)

/* --- APB1ENR bits (DebugUartDriver, RtcDriver) ------------------------ */
#define RCC_APB1ENR_USART3EN_Pos (18U)
#define RCC_APB1ENR_USART3EN (1UL << RCC_APB1ENR_USART3EN_Pos)

#define RCC_APB1ENR_PWREN_Pos (28U)
#define RCC_APB1ENR_PWREN (1UL << RCC_APB1ENR_PWREN_Pos)

/* --- BDCR bits (RtcDriver) -------------------------------------------- */
#define RCC_BDCR_LSEON_Pos (0U)
#define RCC_BDCR_LSEON (1UL << RCC_BDCR_LSEON_Pos)
#define RCC_BDCR_LSERDY_Pos (1U)
#define RCC_BDCR_LSERDY (1UL << RCC_BDCR_LSERDY_Pos)
#define RCC_BDCR_RTCSEL_0_Pos (8U)
#define RCC_BDCR_RTCSEL_0 (1UL << RCC_BDCR_RTCSEL_0_Pos)
#define RCC_BDCR_RTCSEL_1_Pos (9U)
#define RCC_BDCR_RTCSEL_1 (1UL << RCC_BDCR_RTCSEL_1_Pos)
#define RCC_BDCR_RTCSEL (RCC_BDCR_RTCSEL_0 | RCC_BDCR_RTCSEL_1)
#define RCC_BDCR_RTCEN_Pos (15U)
#define RCC_BDCR_RTCEN (1UL << RCC_BDCR_RTCEN_Pos)
#define RCC_BDCR_BDRST_Pos (16U)
#define RCC_BDCR_BDRST (1UL << RCC_BDCR_BDRST_Pos)

/* ====================================================================== */
/* §GPIO                                                                  */
/* ====================================================================== */

#define MOCK_GPIO_PORT_COUNT (11U) /* F469: GPIOA..GPIOK */

/* Mirror the real GPIO_TypeDef layout. Field names must match exactly;
 * absolute offsets don't matter (driver only uses field-name access). */
typedef struct
{
    volatile uint32_t MODER;
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t LCKR;
    volatile uint32_t AFR[2];
} GPIO_TypeDef;

extern GPIO_TypeDef g_mock_gpio[MOCK_GPIO_PORT_COUNT];

#define GPIOA (&g_mock_gpio[0])
#define GPIOB (&g_mock_gpio[1])
#define GPIOC (&g_mock_gpio[2])
#define GPIOD (&g_mock_gpio[3])
#define GPIOE (&g_mock_gpio[4])
#define GPIOF (&g_mock_gpio[5])
#define GPIOG (&g_mock_gpio[6])
#define GPIOH (&g_mock_gpio[7])
#define GPIOI (&g_mock_gpio[8])
#define GPIOJ (&g_mock_gpio[9])
#define GPIOK (&g_mock_gpio[10])

/* ====================================================================== */
/* §USART (DebugUartDriver)                                               */
/* ====================================================================== */

/* F4 legacy USART layout, per RM0386 USART chapter. */
typedef struct
{
    volatile uint32_t SR;   /**< Status register,             offset 0x00. */
    volatile uint32_t DR;   /**< Data register,               offset 0x04. */
    volatile uint32_t BRR;  /**< Baud rate register,          offset 0x08. */
    volatile uint32_t CR1;  /**< Control register 1,          offset 0x0C. */
    volatile uint32_t CR2;  /**< Control register 2,          offset 0x10. */
    volatile uint32_t CR3;  /**< Control register 3,          offset 0x14. */
    volatile uint32_t GTPR; /**< Guard-time / prescaler,      offset 0x18. */
} USART_TypeDef;

extern USART_TypeDef g_mock_usart3;

#define USART3 (&g_mock_usart3)

/* --- USART_SR bits (status, used by ISR and send) --------------------- */
#define USART_SR_TXE_Pos (7U)
#define USART_SR_TXE (1UL << USART_SR_TXE_Pos)
#define USART_SR_RXNE_Pos (5U)
#define USART_SR_RXNE (1UL << USART_SR_RXNE_Pos)
#define USART_SR_ORE_Pos (3U)
#define USART_SR_ORE (1UL << USART_SR_ORE_Pos)
#define USART_SR_NE_Pos (2U)
#define USART_SR_NE (1UL << USART_SR_NE_Pos)
#define USART_SR_FE_Pos (1U)
#define USART_SR_FE (1UL << USART_SR_FE_Pos)
#define USART_SR_PE_Pos (0U)
#define USART_SR_PE (1UL << USART_SR_PE_Pos)

#define USART_SR_ERR_MASK (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)

/* --- USART_CR1 bits --------------------------------------------------- */
#define USART_CR1_UE_Pos (13U)
#define USART_CR1_UE (1UL << USART_CR1_UE_Pos)
#define USART_CR1_TE_Pos (3U)
#define USART_CR1_TE (1UL << USART_CR1_TE_Pos)
#define USART_CR1_RE_Pos (2U)
#define USART_CR1_RE (1UL << USART_CR1_RE_Pos)
#define USART_CR1_RXNEIE_Pos (5U)
#define USART_CR1_RXNEIE (1UL << USART_CR1_RXNEIE_Pos)

/* --- Debug-UART line endings (consumed by DebugUartDriver) ------------ */
#define DEBUG_UART_CR ((uint8_t) '\r')
#define DEBUG_UART_LF ((uint8_t) '\n')

/* ====================================================================== */
/* §PWR (RtcDriver)                                                       */
/* ====================================================================== */

/* Minimal layout — RtcDriver only touches CR. */
typedef struct
{
    volatile uint32_t CR; /**< Power control register, offset 0x00. */
} PWR_TypeDef;

extern PWR_TypeDef g_mock_pwr;

#define PWR (&g_mock_pwr)

/* --- PWR_CR bits ------------------------------------------------------ */
#define PWR_CR_DBP_Pos (8U)
#define PWR_CR_DBP (1UL << PWR_CR_DBP_Pos)

/* ====================================================================== */
/* §RTC (RtcDriver)                                                       */
/* ====================================================================== */

/* Minimal layout: only the registers RtcDriver reads or writes. The real
 * RM0386 §27 layout has additional registers between WPR and the backup
 * block (WUTR, CALIBR, ALRMAR, …); they are not mocked.
 *
 * Backup registers MUST stay contiguous starting at BKP0R: the driver
 * uses pointer arithmetic (&RTC->BKP0R + idx) for indexed access. The
 * trailing array therefore covers BKP1R..BKP19R as a single block. */
typedef struct
{
    volatile uint32_t TR;                  /**< Time register. */
    volatile uint32_t DR;                  /**< Date register. */
    volatile uint32_t CR;                  /**< Control register. */
    volatile uint32_t ISR;                 /**< Init / status register. */
    volatile uint32_t PRER;                /**< Prescaler register. */
    volatile uint32_t WPR;                 /**< Write-protection register. */
    volatile uint32_t BKP0R;               /**< Backup register 0. */
    volatile uint32_t BKP1R_to_BKP19R[19]; /**< Contiguous with BKP0R. */
} RTC_TypeDef;

extern RTC_TypeDef g_mock_rtc;

#define RTC (&g_mock_rtc)

/* --- RTC_ISR bits ----------------------------------------------------- */
#define RTC_ISR_INIT_Pos (7U)
#define RTC_ISR_INIT (1UL << RTC_ISR_INIT_Pos)
#define RTC_ISR_INITF_Pos (6U)
#define RTC_ISR_INITF (1UL << RTC_ISR_INITF_Pos)
#define RTC_ISR_RSF_Pos (5U)
#define RTC_ISR_RSF (1UL << RTC_ISR_RSF_Pos)
#define RTC_ISR_INITS_Pos (4U)
#define RTC_ISR_INITS (1UL << RTC_ISR_INITS_Pos)

/* --- RTC_CR bits ------------------------------------------------------ */
#define RTC_CR_FMT_Pos (6U)
#define RTC_CR_FMT (1UL << RTC_CR_FMT_Pos)

/* ====================================================================== */
/* §NVIC — must stay last; extended per driver                            */
/* ====================================================================== */

typedef enum
{
    USART3_IRQn = 39 /* Per stm32f469xx.h CMSIS canonical value. */
    /* Add more IRQn values as future drivers need them. */
} IRQn_Type;

/* Mock NVIC tracks call counts per IRQn. Tests inspect counters directly;
 * current-enabled state is derivable as (enable_count > disable_count). */
#define NVIC_IRQ_COUNT_MAX (128U)

extern uint32_t g_mock_nvic_enable_count[NVIC_IRQ_COUNT_MAX];
extern uint32_t g_mock_nvic_disable_count[NVIC_IRQ_COUNT_MAX];

void NVIC_EnableIRQ(IRQn_Type irqn);
void NVIC_DisableIRQ(IRQn_Type irqn);

#endif /* STM32F469XX_H */
