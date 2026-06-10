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

/* --- APB1ENR1 bits (I2cDriver, ModbusUartDriver) ---------------------- */
#define RCC_APB1ENR1_I2C2EN_Pos  (22U)
#define RCC_APB1ENR1_I2C2EN      (1UL << RCC_APB1ENR1_I2C2EN_Pos)

#define RCC_APB1ENR1_UART4EN_Pos (19U)
#define RCC_APB1ENR1_UART4EN     (1UL << RCC_APB1ENR1_UART4EN_Pos)

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
/* §USART — L4 family (ModbusUartDriver GW, peripheral UART4)             */
/* ====================================================================== */

/* L4 USART register layout per RM0351. Only fields the driver accesses.  */
typedef struct
{
    volatile uint32_t CR1; /**< Control register 1,              offset 0x00. */
    volatile uint32_t CR2; /**< Control register 2,              offset 0x04. */
    volatile uint32_t CR3; /**< Control register 3,              offset 0x08. */
    volatile uint32_t BRR; /**< Baud rate register,              offset 0x0C. */
    volatile uint32_t ISR; /**< Interrupt and status register,   offset 0x1C. */
    volatile uint32_t ICR; /**< Interrupt clear register,        offset 0x20. */
    volatile uint32_t RDR; /**< Receive data register,           offset 0x24. */
    volatile uint32_t TDR; /**< Transmit data register,          offset 0x28. */
} USART_L4_TypeDef;

extern USART_L4_TypeDef g_mock_uart4;

#define UART4 (&g_mock_uart4)

/* --- USART_CR1 bits (L4) — note UE at bit 0, differs from F4 bit 13 -- */
#define USART_CR1_UE_Pos     (0U)
#define USART_CR1_UE         (1UL << USART_CR1_UE_Pos)
#define USART_CR1_RE_Pos     (2U)
#define USART_CR1_RE         (1UL << USART_CR1_RE_Pos)
#define USART_CR1_TE_Pos     (3U)
#define USART_CR1_TE         (1UL << USART_CR1_TE_Pos)
#define USART_CR1_IDLEIE_Pos (4U)
#define USART_CR1_IDLEIE     (1UL << USART_CR1_IDLEIE_Pos)
#define USART_CR1_RXNEIE_Pos (5U)
#define USART_CR1_RXNEIE     (1UL << USART_CR1_RXNEIE_Pos)

/* --- USART_CR3 bits (L4, same bit position as F4) --------------------- */
#define USART_CR3_DEM_Pos (14U)
#define USART_CR3_DEM     (1UL << USART_CR3_DEM_Pos)

/* --- USART_ISR bits (L4 status register — replaces F4 SR) ------------- */
#define USART_ISR_FE_Pos   (1U)
#define USART_ISR_FE       (1UL << USART_ISR_FE_Pos)
#define USART_ISR_NE_Pos   (2U)
#define USART_ISR_NE       (1UL << USART_ISR_NE_Pos)
#define USART_ISR_ORE_Pos  (3U)
#define USART_ISR_ORE      (1UL << USART_ISR_ORE_Pos)
#define USART_ISR_IDLE_Pos (4U)
#define USART_ISR_IDLE     (1UL << USART_ISR_IDLE_Pos)
#define USART_ISR_RXNE_Pos (5U)
#define USART_ISR_RXNE     (1UL << USART_ISR_RXNE_Pos)
#define USART_ISR_TC_Pos   (6U)
#define USART_ISR_TC       (1UL << USART_ISR_TC_Pos)
#define USART_ISR_TXE_Pos  (7U)
#define USART_ISR_TXE      (1UL << USART_ISR_TXE_Pos)

/* --- USART_ICR bits (L4 clear register) ------------------------------- */
#define USART_ICR_FECF_Pos   (1U)
#define USART_ICR_FECF       (1UL << USART_ICR_FECF_Pos)
#define USART_ICR_NECF_Pos   (2U)
#define USART_ICR_NECF       (1UL << USART_ICR_NECF_Pos)
#define USART_ICR_ORECF_Pos  (3U)
#define USART_ICR_ORECF      (1UL << USART_ICR_ORECF_Pos)
#define USART_ICR_IDLECF_Pos (4U)
#define USART_ICR_IDLECF     (1UL << USART_ICR_IDLECF_Pos)

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

/* ====================================================================== */
/* §NVIC — must stay last; extended per driver                            */
/* ====================================================================== */

typedef enum
{
    UART4_IRQn = 52 /* Per stm32l475xx.h CMSIS canonical value. */
} IRQn_Type;

#define NVIC_IRQ_COUNT_MAX (128U)

extern uint32_t g_mock_nvic_enable_count[NVIC_IRQ_COUNT_MAX];
extern uint32_t g_mock_nvic_disable_count[NVIC_IRQ_COUNT_MAX];

void NVIC_EnableIRQ(IRQn_Type irqn);
void NVIC_DisableIRQ(IRQn_Type irqn);

#endif /* STM32L475XX_H */
