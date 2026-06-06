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
/*   AHB3ENR  — QspiFlashDriver (QSPIEN)                                  */
/*   APB1ENR  — DebugUartDriver (USART3EN), RtcDriver (PWREN)             */
/*   BDCR     — RtcDriver (LSE, RTCSEL, RTCEN)                            */
typedef struct
{
    volatile uint32_t AHB1ENR;
    volatile uint32_t AHB3ENR;
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
/* §I2C (I2cDriver — F469 I2C v1, peripheral I2C1)                       */
/* ====================================================================== */

/* I2C v1 register layout per RM0386 §27.                                 */
typedef struct
{
    volatile uint32_t CR1;   /**< Control register 1,   offset 0x00. */
    volatile uint32_t CR2;   /**< Control register 2,   offset 0x04. */
    volatile uint32_t OAR1;  /**< Own address 1,         offset 0x08. */
    volatile uint32_t OAR2;  /**< Own address 2,         offset 0x0C. */
    volatile uint32_t DR;    /**< Data register,         offset 0x10. */
    volatile uint32_t SR1;   /**< Status register 1,     offset 0x14. */
    volatile uint32_t SR2;   /**< Status register 2,     offset 0x18. */
    volatile uint32_t CCR;   /**< Clock control register,offset 0x1C. */
    volatile uint32_t TRISE; /**< Rise time register,    offset 0x20. */
} I2C_TypeDef;

extern I2C_TypeDef g_mock_i2c1;

#define I2C1 (&g_mock_i2c1)

/* --- RCC_APB1ENR bit — I2C1 clock enable (bit 21) -------------------- */
#define RCC_APB1ENR_I2C1EN_Pos (21U)
#define RCC_APB1ENR_I2C1EN     (1UL << RCC_APB1ENR_I2C1EN_Pos)

/* --- I2C_CR1 bits ----------------------------------------------------- */
#define I2C_CR1_PE_Pos    (0U)
#define I2C_CR1_PE        (1UL << I2C_CR1_PE_Pos)
#define I2C_CR1_START_Pos (8U)
#define I2C_CR1_START     (1UL << I2C_CR1_START_Pos)
#define I2C_CR1_STOP_Pos  (9U)
#define I2C_CR1_STOP      (1UL << I2C_CR1_STOP_Pos)
#define I2C_CR1_ACK_Pos   (10U)
#define I2C_CR1_ACK       (1UL << I2C_CR1_ACK_Pos)
#define I2C_CR1_SWRST_Pos (15U)
#define I2C_CR1_SWRST     (1UL << I2C_CR1_SWRST_Pos)

/* --- I2C_SR1 bits ----------------------------------------------------- */
#define I2C_SR1_SB_Pos   (0U)
#define I2C_SR1_SB       (1UL << I2C_SR1_SB_Pos)
#define I2C_SR1_ADDR_Pos (1U)
#define I2C_SR1_ADDR     (1UL << I2C_SR1_ADDR_Pos)
#define I2C_SR1_BTF_Pos  (2U)
#define I2C_SR1_BTF      (1UL << I2C_SR1_BTF_Pos)
#define I2C_SR1_RXNE_Pos (6U)
#define I2C_SR1_RXNE     (1UL << I2C_SR1_RXNE_Pos)
#define I2C_SR1_TXE_Pos  (7U)
#define I2C_SR1_TXE      (1UL << I2C_SR1_TXE_Pos)
#define I2C_SR1_AF_Pos   (10U)
#define I2C_SR1_AF       (1UL << I2C_SR1_AF_Pos)

/* --- I2C_SR2 bits ----------------------------------------------------- */
#define I2C_SR2_BUSY_Pos (1U)
#define I2C_SR2_BUSY     (1UL << I2C_SR2_BUSY_Pos)

/* --- I2C_CCR bits ----------------------------------------------------- */
#define I2C_CCR_FS_Pos   (15U)
#define I2C_CCR_FS       (1UL << I2C_CCR_FS_Pos)
#define I2C_CCR_DUTY_Pos (14U)
#define I2C_CCR_DUTY     (1UL << I2C_CCR_DUTY_Pos)

/* ====================================================================== */
/* §QUADSPI (QspiFlashDriver)                                             */
/* ====================================================================== */

/* Register layout per RM0386. Fields listed in order; ABR is not used by */
/* QspiFlashDriver but is included to preserve the real hardware layout.  */
typedef struct
{
    volatile uint32_t CR;  /**< Control register,          offset 0x00. */
    volatile uint32_t DCR; /**< Device configuration,      offset 0x04. */
    volatile uint32_t SR;  /**< Status register,           offset 0x08. */
    volatile uint32_t FCR; /**< Flag clear register,       offset 0x0C. */
    volatile uint32_t DLR; /**< Data length register,      offset 0x10. */
    volatile uint32_t CCR; /**< Communication config,      offset 0x14. */
    volatile uint32_t AR;  /**< Address register,          offset 0x18. */
    volatile uint32_t ABR; /**< Alternate bytes register,  offset 0x1C. */
    volatile uint32_t DR;  /**< Data register,             offset 0x20. */
} QUADSPI_TypeDef;

extern QUADSPI_TypeDef g_mock_quadspi;

#define QUADSPI (&g_mock_quadspi)

/* Byte-sequence FIFO for multi-byte indirect-read simulation in tests.
 * The driver uses QSPI_READ_DR_BYTE() which reads from this buffer in TEST
 * builds so each byte in a multi-byte read can return a distinct value. */
#define QUADSPI_MOCK_FIFO_DEPTH (256U)
extern uint8_t  g_mock_quadspi_rx_fifo[QUADSPI_MOCK_FIFO_DEPTH];
extern uint32_t g_mock_quadspi_rx_fifo_idx;

/* --- RCC_AHB3ENR bit — QUADSPI clock enable (bit 1, per RM0386) ------- */
#define RCC_AHB3ENR_QSPIEN_Pos (1U)
#define RCC_AHB3ENR_QSPIEN     (1UL << RCC_AHB3ENR_QSPIEN_Pos)

/* --- QUADSPI_CR bits -------------------------------------------------- */
#define QUADSPI_CR_EN_Pos   (0U)
#define QUADSPI_CR_EN       (1UL << QUADSPI_CR_EN_Pos)

/* --- QUADSPI_DCR bits ------------------------------------------------- */
#define QUADSPI_DCR_CKMODE_Pos (0U)
#define QUADSPI_DCR_CSHT_Pos   (8U)
#define QUADSPI_DCR_FSIZE_Pos  (16U)

/* --- QUADSPI_SR bits -------------------------------------------------- */
#define QUADSPI_SR_TCF_Pos  (1U)
#define QUADSPI_SR_TCF      (1UL << QUADSPI_SR_TCF_Pos)
#define QUADSPI_SR_FTF_Pos  (2U)
#define QUADSPI_SR_FTF      (1UL << QUADSPI_SR_FTF_Pos)
#define QUADSPI_SR_BUSY_Pos (5U)
#define QUADSPI_SR_BUSY     (1UL << QUADSPI_SR_BUSY_Pos)

/* --- QUADSPI_FCR bits ------------------------------------------------- */
#define QUADSPI_FCR_CTCF_Pos (1U)
#define QUADSPI_FCR_CTCF     (1UL << QUADSPI_FCR_CTCF_Pos)

/* --- QUADSPI_CCR bit fields ------------------------------------------- */
/* Instruction [7:0] */
#define QUADSPI_CCR_INSTRUCTION_Pos  (0U)
/* IMODE [9:8]: instruction mode (01 = single line) */
#define QUADSPI_CCR_IMODE_Pos        (8U)
/* ADMODE [11:10]: address mode (01 = single line) */
#define QUADSPI_CCR_ADMODE_Pos       (10U)
/* ADSIZE [13:12]: address size (10 = 24-bit) */
#define QUADSPI_CCR_ADSIZE_Pos       (12U)
/* DMODE [25:24]: data mode (01 = single line) */
#define QUADSPI_CCR_DMODE_Pos        (24U)
/* FMODE [27:26]: functional mode (00 = indirect write, 01 = indirect read) */
#define QUADSPI_CCR_FMODE_Pos        (26U)

/* Byte-width DR read/write intercepts for test builds.
 * These definitions shadow the driver's own QUADSPI_READ_BYTE / QUADSPI_WRITE_BYTE
 * (which use hardware QUADSPI_BASE), making byte-width DR accesses use the
 * mock rx-fifo (reads) and g_mock_quadspi.DR (writes) instead. */
#define QUADSPI_READ_BYTE     (g_mock_quadspi_rx_fifo[g_mock_quadspi_rx_fifo_idx++])
#define QUADSPI_WRITE_BYTE(b) (g_mock_quadspi.DR = (uint32_t)(uint8_t)(b))

#define __NOP(void) void
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
