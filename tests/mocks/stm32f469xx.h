#ifndef STM32F469XX_H
#define STM32F469XX_H

#include <stdint.h>

/* Peripheral pointer macros — driver code writes through these. */
#define GPIOA  (&g_mock_gpio[0])
#define GPIOB  (&g_mock_gpio[1])
#define GPIOC  (&g_mock_gpio[2])
#define GPIOD  (&g_mock_gpio[3])
#define GPIOE  (&g_mock_gpio[4])
#define GPIOF  (&g_mock_gpio[5])
#define GPIOG  (&g_mock_gpio[6])
#define GPIOH  (&g_mock_gpio[7])
#define GPIOI  (&g_mock_gpio[8])
#define GPIOJ  (&g_mock_gpio[9])
#define GPIOK  (&g_mock_gpio[10])
#define RCC    (&g_mock_rcc)

/* Bit-position macros — copy values from ST's stm32f469xx.h. */
#define RCC_AHB1ENR_GPIOAEN_Pos  (0U)
#define RCC_AHB1ENR_GPIOBEN_Pos  (1U)
#define RCC_AHB1ENR_GPIOCEN_Pos  (2U)
#define RCC_AHB1ENR_GPIODEN_Pos  (3U)
#define RCC_AHB1ENR_GPIOEEN_Pos  (4U)
#define RCC_AHB1ENR_GPIOFEN_Pos  (5U)
#define RCC_AHB1ENR_GPIOGEN_Pos  (6U)
#define RCC_AHB1ENR_GPIOHEN_Pos  (7U)
#define RCC_AHB1ENR_GPIOIEN_Pos  (8U)
#define RCC_AHB1ENR_GPIOJEN_Pos  (9U)
#define RCC_AHB1ENR_GPIOKEN_Pos  (10U)

#define RCC_AHB1ENR_GPIOAEN      (1UL << RCC_AHB1ENR_GPIOAEN_Pos)
#define RCC_AHB1ENR_GPIOBEN      (1UL << RCC_AHB1ENR_GPIOBEN_Pos)
#define RCC_AHB1ENR_GPIOCEN      (1UL << RCC_AHB1ENR_GPIOCEN_Pos)
#define RCC_AHB1ENR_GPIODEN      (1UL << RCC_AHB1ENR_GPIODEN_Pos)
#define RCC_AHB1ENR_GPIOEEN      (1UL << RCC_AHB1ENR_GPIOEEN_Pos)
#define RCC_AHB1ENR_GPIOFEN      (1UL << RCC_AHB1ENR_GPIOFEN_Pos)
#define RCC_AHB1ENR_GPIOGEN      (1UL << RCC_AHB1ENR_GPIOGEN_Pos)
#define RCC_AHB1ENR_GPIOHEN      (1UL << RCC_AHB1ENR_GPIOHEN_Pos)
#define RCC_AHB1ENR_GPIOIEN      (1UL << RCC_AHB1ENR_GPIOIEN_Pos)
#define RCC_AHB1ENR_GPIOJEN      (1UL << RCC_AHB1ENR_GPIOJEN_Pos)
#define RCC_AHB1ENR_GPIOKEN      (1UL << RCC_AHB1ENR_GPIOKEN_Pos)

#define MOCK_GPIO_PORT_COUNT     (11U)    /* F469: GPIOA..GPIOK */

/* Mirror the real GPIO_TypeDef layout. Field names must match exactly;
 * absolute offsets don't matter (driver only uses field-name access). */
typedef struct {
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

typedef struct {
    /* TODO: GpioDriver only touches AHB1ENR on F469.
     * Minimum required field below; add others only if a future
     * driver needs them. */
    volatile uint32_t AHB1ENR;
} RCC_TypeDef;

/* Storage lives in the .c — declared extern here. */
extern GPIO_TypeDef g_mock_gpio[MOCK_GPIO_PORT_COUNT];   /* GPIOA..GPIOK */
extern RCC_TypeDef  g_mock_rcc;

#endif /* STM32F469XX_H */
