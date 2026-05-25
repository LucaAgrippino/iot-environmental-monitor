#ifndef STM32F469XX_H
#define STM32F469XX_H

#include <stdint.h>

/* Peripheral pointer macros — driver code writes through these. */
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
#define RCC (&g_mock_rcc)

/* Bit-position macros — copy values from ST's stm32f469xx.h. */
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

typedef struct
{
    /* TODO: GpioDriver only touches AHB1ENR on F469.
     * Minimum required field below; add others only if a future
     * driver needs them. */
    volatile uint32_t AHB1ENR;
    volatile uint32_t APB1ENR;
} RCC_TypeDef;

/* Storage lives in the .c — declared extern here. */
extern GPIO_TypeDef g_mock_gpio[MOCK_GPIO_PORT_COUNT]; /* GPIOA..GPIOK */
extern RCC_TypeDef g_mock_rcc;

/* ------------------------------------------------------------------ */
/* USART_TypeDef (F4 legacy USART — used by USART3)                    */
/* Per RM0386 USART chapter.                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    volatile uint32_t SR;    /**< Status register, offset 0x00. */
    volatile uint32_t DR;    /**< Data register, offset 0x04. */
    volatile uint32_t BRR;   /**< Baud rate register, offset 0x08. */
    volatile uint32_t CR1;   /**< Control register 1, offset 0x0C. */
    volatile uint32_t CR2;   /**< Control register 2, offset 0x10. */
    volatile uint32_t CR3;   /**< Control register 3, offset 0x14. */
    volatile uint32_t GTPR;  /**< Guard-time / prescaler, offset 0x18. */
} USART_TypeDef;

extern USART_TypeDef g_mock_usart3;
#define USART3  (&g_mock_usart3)

/* RCC: extend with APB1ENR for USART3. Note: real RCC has APB1ENR at
 * offset 0x40, separate from AHB1ENR at 0x30. Field-name access means
 * absolute offsets don't matter; we just append. */
/* NOTE: also add 'volatile uint32_t APB1ENR;' to RCC_TypeDef above.
 * Per the GpioDriver lesson, struct storage is non-volatile but
 * register fields are. */

#define RCC_APB1ENR_USART3EN_Pos  (18U)
#define RCC_APB1ENR_USART3EN      (1UL << RCC_APB1ENR_USART3EN_Pos)

/* ------------------------------------------------------------------ */
/* NVIC (would normally come from core_cm4.h)                          */
/* ------------------------------------------------------------------ */
typedef enum {
    USART3_IRQn = 39   /* Per stm32f469xx.h CMSIS canonical value. */
    /* Add more IRQn values as future drivers need them. */
} IRQn_Type;

/* Mock NVIC tracks call counts per IRQn. Tests inspect counters
 * directly; current-enabled state is derivable as
 * (enable_count > disable_count). */
#define NVIC_IRQ_COUNT_MAX  (128U)

extern uint32_t g_mock_nvic_enable_count[NVIC_IRQ_COUNT_MAX];
extern uint32_t g_mock_nvic_disable_count[NVIC_IRQ_COUNT_MAX];

void NVIC_EnableIRQ(IRQn_Type irqn);
void NVIC_DisableIRQ(IRQn_Type irqn);

#endif /* STM32F469XX_H */
