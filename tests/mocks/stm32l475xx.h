#ifndef STM32L475XX_H
#define STM32L475XX_H

#include <stdint.h>

/* ====================================================================== */
/* Mock CMSIS header for STM32L475 — host-side unit-test builds only.     */
/*                                                                        */
/* Follows the same conventions as stm32f469xx.h. Each section contains: */
/*   1. TypeDef (only the fields a driver under test actually touches)    */
/*   2. Mock storage extern (defined in stm32l475_cmsis_mock.c)           */
/*   3. Pointer macro(s) that driver code dereferences                    */
/*   4. Bit constants used by the corresponding driver(s)                 */
/* ====================================================================== */

/* Self-define the device identifier so driver TUs that use               */
/* #if defined(STM32L475xx) work without a -D flag.                      */
#ifndef STM32L475xx
#define STM32L475xx
#endif

/* ====================================================================== */
/* §RCC — Reset and Clock Control (L4)                                    */
/* ====================================================================== */

/* Fields accumulate as drivers need them. Current owners:                */
/*   AHB2ENR  — GpioDriver (L4)                                           */
/*   APB1ENR1 — I2cDriver (I2C2EN)                                        */
typedef struct
{
    volatile uint32_t AHB2ENR;
    volatile uint32_t APB1ENR1;
} RCC_TypeDef;

extern RCC_TypeDef g_mock_rcc_l4;

#define RCC (&g_mock_rcc_l4)

/* --- AHB2ENR bits (GpioDriver L4) ------------------------------------ */
#define RCC_AHB2ENR_GPIOAEN_Pos (0U)
#define RCC_AHB2ENR_GPIOAEN     (1UL << RCC_AHB2ENR_GPIOAEN_Pos)
#define RCC_AHB2ENR_GPIOBEN_Pos (1U)
#define RCC_AHB2ENR_GPIOBEN     (1UL << RCC_AHB2ENR_GPIOBEN_Pos)

/* --- APB1ENR1 bits (I2cDriver) ---------------------------------------- */
#define RCC_APB1ENR1_I2C2EN_Pos (22U)
#define RCC_APB1ENR1_I2C2EN     (1UL << RCC_APB1ENR1_I2C2EN_Pos)

/* ====================================================================== */
/* §GPIO (L4)                                                              */
/* ====================================================================== */

#define MOCK_GPIO_PORT_COUNT_L4 (8U) /* L475: GPIOA..GPIOH */

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

extern GPIO_TypeDef g_mock_gpio_l4[MOCK_GPIO_PORT_COUNT_L4];

#define GPIOA (&g_mock_gpio_l4[0])
#define GPIOB (&g_mock_gpio_l4[1])
#define GPIOC (&g_mock_gpio_l4[2])
#define GPIOD (&g_mock_gpio_l4[3])
#define GPIOE (&g_mock_gpio_l4[4])
#define GPIOF (&g_mock_gpio_l4[5])
#define GPIOG (&g_mock_gpio_l4[6])
#define GPIOH (&g_mock_gpio_l4[7])

/* ====================================================================== */
/* §I2C (I2cDriver — L475 I2C v2, peripheral I2C2)                        */
/* ====================================================================== */

/* I2C v2 register layout per RM0351 §37. Only registers the driver uses. */
typedef struct
{
    volatile uint32_t CR1;      /**< Control register 1,             offset 0x00. */
    volatile uint32_t CR2;      /**< Control register 2,             offset 0x04. */
    volatile uint32_t OAR1;     /**< Own address 1,                  offset 0x08. */
    volatile uint32_t OAR2;     /**< Own address 2,                  offset 0x0C. */
    volatile uint32_t TIMINGR;  /**< Timing register,                offset 0x10. */
    volatile uint32_t TIMEOUTR; /**< Timeout register,               offset 0x14. */
    volatile uint32_t ISR;      /**< Interrupt and status register,  offset 0x18. */
    volatile uint32_t ICR;      /**< Interrupt clear register,       offset 0x1C. */
    volatile uint32_t PECR;     /**< PEC register,                   offset 0x20. */
    volatile uint32_t RXDR;     /**< Receive data register,          offset 0x24. */
    volatile uint32_t TXDR;     /**< Transmit data register,         offset 0x28. */
} I2C_TypeDef;

extern I2C_TypeDef g_mock_i2c2;

#define I2C2 (&g_mock_i2c2)

/* --- I2C_CR1 bits (v2) ------------------------------------------------ */
#define I2C_CR1_PE_Pos    (0U)
#define I2C_CR1_PE        (1UL << I2C_CR1_PE_Pos)

/* --- I2C_CR2 bits (v2) ------------------------------------------------ */
#define I2C_CR2_SADD_Pos    (0U)
#define I2C_CR2_SADD       (0x3FFUL << I2C_CR2_SADD_Pos)
#define I2C_CR2_RD_WRN_Pos  (10U)
#define I2C_CR2_RD_WRN      (1UL << I2C_CR2_RD_WRN_Pos)
#define I2C_CR2_START_Pos   (13U)
#define I2C_CR2_START       (1UL << I2C_CR2_START_Pos)
#define I2C_CR2_STOP_Pos    (14U)
#define I2C_CR2_STOP        (1UL << I2C_CR2_STOP_Pos)
#define I2C_CR2_NBYTES_Pos  (16U)
#define I2C_CR2_NBYTES      (0xFFUL << I2C_CR2_NBYTES_Pos)
#define I2C_CR2_RELOAD_Pos  (24U)
#define I2C_CR2_RELOAD      (1UL << I2C_CR2_RELOAD_Pos)
#define I2C_CR2_AUTOEND_Pos (25U)
#define I2C_CR2_AUTOEND     (1UL << I2C_CR2_AUTOEND_Pos)

/* --- I2C_ISR bits (v2) ------------------------------------------------ */
#define I2C_ISR_TXE_Pos    (0U)
#define I2C_ISR_TXE        (1UL << I2C_ISR_TXE_Pos)
#define I2C_ISR_TXIS_Pos   (1U)
#define I2C_ISR_TXIS       (1UL << I2C_ISR_TXIS_Pos)
#define I2C_ISR_RXNE_Pos   (2U)
#define I2C_ISR_RXNE       (1UL << I2C_ISR_RXNE_Pos)
#define I2C_ISR_NACKF_Pos  (4U)
#define I2C_ISR_NACKF      (1UL << I2C_ISR_NACKF_Pos)
#define I2C_ISR_STOPF_Pos  (5U)
#define I2C_ISR_STOPF      (1UL << I2C_ISR_STOPF_Pos)
#define I2C_ISR_TC_Pos     (6U)
#define I2C_ISR_TC         (1UL << I2C_ISR_TC_Pos)
#define I2C_ISR_BUSY_Pos   (15U)
#define I2C_ISR_BUSY       (1UL << I2C_ISR_BUSY_Pos)

/* --- I2C_ICR bits (v2) ------------------------------------------------ */
#define I2C_ICR_NACKCF_Pos (4U)
#define I2C_ICR_NACKCF     (1UL << I2C_ICR_NACKCF_Pos)
#define I2C_ICR_STOPCF_Pos (5U)
#define I2C_ICR_STOPCF     (1UL << I2C_ICR_STOPCF_Pos)

#endif /* STM32L475XX_H */
