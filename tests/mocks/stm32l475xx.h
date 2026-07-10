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

/* Self-define the device identifier so driver TUs that guard includes    */
/* with #if defined(STM32L475xx) work without a -D flag.                 */
#ifndef STM32L475xx
#define STM32L475xx
#endif

/* ====================================================================== */
/* §RCC — Reset and Clock Control (L4)                                    */
/* Fields accumulate as drivers need them. Current owners:                */
/*   AHB2ENR  — GpioDriver (L4), I2cDriver, ModbusUartDriver             */
/*   APB1ENR1 — I2cDriver (I2C2EN), ModbusUartDriver (UART4EN), CpuDriver (PWREN) */
/*   APB2ENR  — CpuDriver (USART1EN)                                      */
/*   CR       — CpuDriver (PLLON, PLLRDY, MSIPLLEN)                      */
/*   CFGR     — CpuDriver (SW, SWS)                                       */
/*   PLLCFGR  — CpuDriver (PLLSRC, PLLM, PLLN, PLLR, PLLREN)            */
/* ====================================================================== */

typedef struct
{
    volatile uint32_t CR;       /**< Clock control register.                  */
    volatile uint32_t CFGR;     /**< Clock configuration register.            */
    volatile uint32_t PLLCFGR;  /**< PLL configuration register.              */
    volatile uint32_t AHB2ENR;  /**< AHB2 peripheral clock enable register.   */
    volatile uint32_t APB1ENR1; /**< APB1 peripheral clock enable register 1. */
    volatile uint32_t APB2ENR;  /**< APB2 peripheral clock enable register.   */
    volatile uint32_t BDCR;     /**< Backup domain control register (RtcDriver: LSE, RTCSEL, RTCEN). */
    volatile uint32_t PLLSAI1CFGR; /**< PLLSAI1 configuration register (MqttClient: 48 MHz RNG clock). */
    volatile uint32_t CCIPR;    /**< Peripherals independent clock configuration register (MqttClient: CLK48SEL). */
} RCC_TypeDef;

extern RCC_TypeDef g_mock_rcc_l4;

#define RCC (&g_mock_rcc_l4)

/* --- RCC_CR bits (CpuDriver) ----------------------------------------- */
#define RCC_CR_MSIPLLEN_Pos (2U)
#define RCC_CR_MSIPLLEN     (1UL << RCC_CR_MSIPLLEN_Pos)
#define RCC_CR_PLLON_Pos    (24U)
#define RCC_CR_PLLON        (1UL << RCC_CR_PLLON_Pos)
#define RCC_CR_PLLRDY_Pos   (25U)
#define RCC_CR_PLLRDY       (1UL << RCC_CR_PLLRDY_Pos)
#define RCC_CR_PLLSAI1ON_Pos  (26U)
#define RCC_CR_PLLSAI1ON      (1UL << RCC_CR_PLLSAI1ON_Pos)
#define RCC_CR_PLLSAI1RDY_Pos (27U)
#define RCC_CR_PLLSAI1RDY     (1UL << RCC_CR_PLLSAI1RDY_Pos)

/* --- RCC_CFGR bits (CpuDriver) --------------------------------------- */
#define RCC_CFGR_SW_Pos     (0U)
#define RCC_CFGR_SW         (0x3UL << RCC_CFGR_SW_Pos)
#define RCC_CFGR_SW_PLL     (0x3UL << RCC_CFGR_SW_Pos)  /* PLL = 11 */
#define RCC_CFGR_SWS_Pos    (2U)
#define RCC_CFGR_SWS        (0x3UL << RCC_CFGR_SWS_Pos)
#define RCC_CFGR_SWS_PLL    (0x3UL << RCC_CFGR_SWS_Pos) /* PLL = 11 */

/* --- RCC_PLLCFGR bits (CpuDriver) ----------------------------------- */
#define RCC_PLLCFGR_PLLSRC_Pos  (0U)
#define RCC_PLLCFGR_PLLSRC_MSI  (0x1UL << RCC_PLLCFGR_PLLSRC_Pos) /* 01 = MSI */
#define RCC_PLLCFGR_PLLM_Pos    (4U)
#define RCC_PLLCFGR_PLLM        (0x7UL << RCC_PLLCFGR_PLLM_Pos)
#define RCC_PLLCFGR_PLLN_Pos    (8U)
#define RCC_PLLCFGR_PLLN        (0x7FUL << RCC_PLLCFGR_PLLN_Pos)
#define RCC_PLLCFGR_PLLREN_Pos  (24U)
#define RCC_PLLCFGR_PLLREN      (1UL << RCC_PLLCFGR_PLLREN_Pos)
#define RCC_PLLCFGR_PLLR_Pos    (25U)
#define RCC_PLLCFGR_PLLR        (0x3UL << RCC_PLLCFGR_PLLR_Pos)

/* --- AHB2ENR bits (GpioDriver L4) ------------------------------------ */
#define RCC_AHB2ENR_GPIOAEN_Pos (0U)
#define RCC_AHB2ENR_GPIOAEN     (1UL << RCC_AHB2ENR_GPIOAEN_Pos)
#define RCC_AHB2ENR_GPIOBEN_Pos (1U)
#define RCC_AHB2ENR_GPIOBEN     (1UL << RCC_AHB2ENR_GPIOBEN_Pos)
#define RCC_AHB2ENR_GPIOCEN_Pos (2U)
#define RCC_AHB2ENR_GPIOCEN     (1UL << RCC_AHB2ENR_GPIOCEN_Pos)
#define RCC_AHB2ENR_GPIODEN_Pos (3U)
#define RCC_AHB2ENR_GPIODEN     (1UL << RCC_AHB2ENR_GPIODEN_Pos)
#define RCC_AHB2ENR_GPIOEEN_Pos (4U)
#define RCC_AHB2ENR_GPIOEEN     (1UL << RCC_AHB2ENR_GPIOEEN_Pos)
#define RCC_AHB2ENR_GPIOFEN_Pos (5U)
#define RCC_AHB2ENR_GPIOFEN     (1UL << RCC_AHB2ENR_GPIOFEN_Pos)
#define RCC_AHB2ENR_GPIOGEN_Pos (6U)
#define RCC_AHB2ENR_GPIOGEN     (1UL << RCC_AHB2ENR_GPIOGEN_Pos)
#define RCC_AHB2ENR_GPIOHEN_Pos (7U)
#define RCC_AHB2ENR_GPIOHEN     (1UL << RCC_AHB2ENR_GPIOHEN_Pos)

/* --- APB1ENR1 bits (I2cDriver, ModbusUartDriver, CpuDriver, SpiDriver) - */
#define RCC_APB1ENR1_I2C2EN_Pos  (22U)
#define RCC_APB1ENR1_I2C2EN      (1UL << RCC_APB1ENR1_I2C2EN_Pos)

#define RCC_APB1ENR1_UART4EN_Pos (19U)
#define RCC_APB1ENR1_UART4EN     (1UL << RCC_APB1ENR1_UART4EN_Pos)

#define RCC_APB1ENR1_PWREN_Pos   (28U)
#define RCC_APB1ENR1_PWREN       (1UL << RCC_APB1ENR1_PWREN_Pos)

#define RCC_APB1ENR1_SPI3EN_Pos  (15U)
#define RCC_APB1ENR1_SPI3EN      (1UL << RCC_APB1ENR1_SPI3EN_Pos)

/* --- APB2ENR bits (CpuDriver, ExtiDriver) ----------------------------- */
#define RCC_APB2ENR_USART1EN_Pos (14U)
#define RCC_APB2ENR_USART1EN     (1UL << RCC_APB2ENR_USART1EN_Pos)

/* SYSCFGEN is bit 0 on L4 (RM0351) — differs from F4's bit 14. */
#define RCC_APB2ENR_SYSCFGEN_Pos (0U)
#define RCC_APB2ENR_SYSCFGEN     (1UL << RCC_APB2ENR_SYSCFGEN_Pos)

/* --- BDCR bits (RtcDriver) --------------------------------------------- */
#define RCC_BDCR_LSEON_Pos    (0U)
#define RCC_BDCR_LSEON        (1UL << RCC_BDCR_LSEON_Pos)
#define RCC_BDCR_LSERDY_Pos   (1U)
#define RCC_BDCR_LSERDY       (1UL << RCC_BDCR_LSERDY_Pos)
#define RCC_BDCR_RTCSEL_0_Pos (8U)
#define RCC_BDCR_RTCSEL_0     (1UL << RCC_BDCR_RTCSEL_0_Pos)
#define RCC_BDCR_RTCSEL_1_Pos (9U)
#define RCC_BDCR_RTCSEL_1     (1UL << RCC_BDCR_RTCSEL_1_Pos)
#define RCC_BDCR_RTCSEL       (RCC_BDCR_RTCSEL_0 | RCC_BDCR_RTCSEL_1)
#define RCC_BDCR_RTCEN_Pos    (15U)
#define RCC_BDCR_RTCEN        (1UL << RCC_BDCR_RTCEN_Pos)
#define RCC_BDCR_BDRST_Pos    (16U)
#define RCC_BDCR_BDRST        (1UL << RCC_BDCR_BDRST_Pos)

/* --- AHB2ENR bits (MqttClient RNG entropy source) ---------------------- */
#define RCC_AHB2ENR_RNGEN_Pos (18U)
#define RCC_AHB2ENR_RNGEN     (1UL << RCC_AHB2ENR_RNGEN_Pos)

/* --- PLLSAI1CFGR bits (MqttClient — 48 MHz RNG kernel clock) ------------ */
#define RCC_PLLSAI1CFGR_PLLSAI1N_Pos (8U)
#define RCC_PLLSAI1CFGR_PLLSAI1N     (0x7FUL << RCC_PLLSAI1CFGR_PLLSAI1N_Pos)
#define RCC_PLLSAI1CFGR_PLLSAI1QEN_Pos (20U)
#define RCC_PLLSAI1CFGR_PLLSAI1QEN     (1UL << RCC_PLLSAI1CFGR_PLLSAI1QEN_Pos)
#define RCC_PLLSAI1CFGR_PLLSAI1Q_Pos (21U)
#define RCC_PLLSAI1CFGR_PLLSAI1Q_0   (0x1UL << RCC_PLLSAI1CFGR_PLLSAI1Q_Pos)
#define RCC_PLLSAI1CFGR_PLLSAI1Q_1   (0x2UL << RCC_PLLSAI1CFGR_PLLSAI1Q_Pos)
#define RCC_PLLSAI1CFGR_PLLSAI1Q_Msk (0x3UL << RCC_PLLSAI1CFGR_PLLSAI1Q_Pos)

/* --- CCIPR bits (MqttClient — select PLLSAI1Q as the 48 MHz clock) ----- */
#define RCC_CCIPR_CLK48SEL_Pos (26U)
#define RCC_CCIPR_CLK48SEL_0   (0x1UL << RCC_CCIPR_CLK48SEL_Pos)
#define RCC_CCIPR_CLK48SEL_1   (0x2UL << RCC_CCIPR_CLK48SEL_Pos)
#define RCC_CCIPR_CLK48SEL_Msk (0x3UL << RCC_CCIPR_CLK48SEL_Pos)

/* ====================================================================== */
/* §RNG — True Random Number Generator (L4)                               */
/* Owner: MqttClient (prv_entropy_poll() — mbedTLS entropy source).       */
/* ====================================================================== */

typedef struct
{
    volatile uint32_t CR; /**< RNG control register, Address offset: 0x00 */
    volatile uint32_t SR; /**< RNG status register,  Address offset: 0x04 */
    volatile uint32_t DR; /**< RNG data register,    Address offset: 0x08 */
} RNG_TypeDef;

extern RNG_TypeDef g_mock_rng;

#define RNG (&g_mock_rng)

#define RNG_CR_RNGEN_Pos (2U)
#define RNG_CR_RNGEN     (1UL << RNG_CR_RNGEN_Pos)

#define RNG_SR_DRDY_Pos (0U)
#define RNG_SR_DRDY     (1UL << RNG_SR_DRDY_Pos)

/* ====================================================================== */
/* §PWR — Power control (CpuDriver)                                       */
/* ====================================================================== */

typedef struct
{
    volatile uint32_t CR1; /**< Power control register 1, offset 0x00. */
} PWR_TypeDef;

extern PWR_TypeDef g_mock_pwr;

#define PWR (&g_mock_pwr)

/* --- PWR_CR1 bits ----------------------------------------------------- */
#define PWR_CR1_DBP_Pos  (8U)
#define PWR_CR1_DBP      (1UL << PWR_CR1_DBP_Pos)
#define PWR_CR1_VOS_Pos  (9U)
#define PWR_CR1_VOS      (0x3UL << PWR_CR1_VOS_Pos)
#define PWR_CR1_VOS_0    (1UL << PWR_CR1_VOS_Pos) /* Range 1: VOS[1:0]=01 */

/* ====================================================================== */
/* §FLASH — Flash memory controller (CpuDriver)                           */
/* ====================================================================== */

typedef struct
{
    volatile uint32_t ACR; /**< Access control register, offset 0x00. */
} FLASH_TypeDef;

extern FLASH_TypeDef g_mock_flash;

#define FLASH (&g_mock_flash)

/* --- FLASH_ACR bits -------------------------------------------------- */
#define FLASH_ACR_LATENCY_Pos  (0U)
#define FLASH_ACR_LATENCY_4WS  (4UL << FLASH_ACR_LATENCY_Pos)
#define FLASH_ACR_PRFTEN_Pos   (8U)
#define FLASH_ACR_PRFTEN       (1UL << FLASH_ACR_PRFTEN_Pos)
#define FLASH_ACR_ICEN_Pos     (9U)
#define FLASH_ACR_ICEN         (1UL << FLASH_ACR_ICEN_Pos)

/* ====================================================================== */
/* §CoreDebug — Cortex-M4 debug control (CpuDriver)                       */
/* ====================================================================== */

typedef struct
{
    volatile uint32_t DEMCR; /**< Debug Exception and Monitor Control Register. */
} CoreDebug_TypeDef;

extern CoreDebug_TypeDef g_mock_core_debug;

#define CoreDebug (&g_mock_core_debug)

/* --- CoreDebug_DEMCR bits -------------------------------------------- */
#define CoreDebug_DEMCR_TRCENA_Pos (24U)
#define CoreDebug_DEMCR_TRCENA     (1UL << CoreDebug_DEMCR_TRCENA_Pos)
#define CoreDebug_DEMCR_TRCENA_Msk CoreDebug_DEMCR_TRCENA

/* ====================================================================== */
/* §DWT — Data Watchpoint and Trace unit (CpuDriver)                      */
/* ====================================================================== */

typedef struct
{
    volatile uint32_t CTRL;   /**< Control register.      */
    volatile uint32_t CYCCNT; /**< Cycle count register.  */
} DWT_TypeDef;

extern DWT_TypeDef g_mock_dwt;

#define DWT (&g_mock_dwt)

/* --- DWT_CTRL bits --------------------------------------------------- */
#define DWT_CTRL_CYCCNTENA_Pos (0U)
#define DWT_CTRL_CYCCNTENA     (1UL << DWT_CTRL_CYCCNTENA_Pos)
#define DWT_CTRL_CYCCNTENA_Msk DWT_CTRL_CYCCNTENA

/* ====================================================================== */
/* §SCB — System Control Block (CpuDriver fault status)                   */
/* ====================================================================== */

typedef struct
{
    volatile uint32_t CFSR;  /**< Configurable Fault Status Register.  */
    volatile uint32_t HFSR;  /**< Hard Fault Status Register.          */
    volatile uint32_t BFAR;  /**< Bus Fault Address Register.          */
    volatile uint32_t MMFAR; /**< MemManage Fault Address Register.    */
} SCB_TypeDef;

extern SCB_TypeDef g_mock_scb;

#define SCB (&g_mock_scb)

/* --- SCB_CFSR — MMFSR bits (MemManage, bits 7:0) --------------------- */
#define SCB_CFSR_IACCVIOL_Pos   (0U)
#define SCB_CFSR_IACCVIOL       (1UL << SCB_CFSR_IACCVIOL_Pos)
#define SCB_CFSR_IACCVIOL_Msk   SCB_CFSR_IACCVIOL
#define SCB_CFSR_DACCVIOL_Pos   (1U)
#define SCB_CFSR_DACCVIOL       (1UL << SCB_CFSR_DACCVIOL_Pos)
#define SCB_CFSR_DACCVIOL_Msk   SCB_CFSR_DACCVIOL
#define SCB_CFSR_MUNSTKERR_Pos  (3U)
#define SCB_CFSR_MUNSTKERR      (1UL << SCB_CFSR_MUNSTKERR_Pos)
#define SCB_CFSR_MUNSTKERR_Msk  SCB_CFSR_MUNSTKERR
#define SCB_CFSR_MSTKERR_Pos    (4U)
#define SCB_CFSR_MSTKERR        (1UL << SCB_CFSR_MSTKERR_Pos)
#define SCB_CFSR_MSTKERR_Msk    SCB_CFSR_MSTKERR
#define SCB_CFSR_MLSPERR_Pos    (5U)
#define SCB_CFSR_MLSPERR        (1UL << SCB_CFSR_MLSPERR_Pos)
#define SCB_CFSR_MLSPERR_Msk    SCB_CFSR_MLSPERR

/* --- SCB_CFSR — BFSR bits (BusFault, bits 15:8) ---------------------- */
#define SCB_CFSR_IBUSERR_Pos    (8U)
#define SCB_CFSR_IBUSERR        (1UL << SCB_CFSR_IBUSERR_Pos)
#define SCB_CFSR_IBUSERR_Msk    SCB_CFSR_IBUSERR
#define SCB_CFSR_PRECISERR_Pos  (9U)
#define SCB_CFSR_PRECISERR      (1UL << SCB_CFSR_PRECISERR_Pos)
#define SCB_CFSR_PRECISERR_Msk  SCB_CFSR_PRECISERR
#define SCB_CFSR_IMPRECISERR_Pos (10U)
#define SCB_CFSR_IMPRECISERR    (1UL << SCB_CFSR_IMPRECISERR_Pos)
#define SCB_CFSR_IMPRECISERR_Msk SCB_CFSR_IMPRECISERR
#define SCB_CFSR_UNSTKERR_Pos   (11U)
#define SCB_CFSR_UNSTKERR       (1UL << SCB_CFSR_UNSTKERR_Pos)
#define SCB_CFSR_UNSTKERR_Msk   SCB_CFSR_UNSTKERR
#define SCB_CFSR_STKERR_Pos     (12U)
#define SCB_CFSR_STKERR         (1UL << SCB_CFSR_STKERR_Pos)
#define SCB_CFSR_STKERR_Msk     SCB_CFSR_STKERR
#define SCB_CFSR_LSPERR_Pos     (13U)
#define SCB_CFSR_LSPERR         (1UL << SCB_CFSR_LSPERR_Pos)
#define SCB_CFSR_LSPERR_Msk     SCB_CFSR_LSPERR

/* --- SCB_CFSR — UFSR bits (UsageFault, bits 31:16) ------------------- */
#define SCB_CFSR_UNDEFINSTR_Pos  (16U)
#define SCB_CFSR_UNDEFINSTR      (1UL << SCB_CFSR_UNDEFINSTR_Pos)
#define SCB_CFSR_UNDEFINSTR_Msk  SCB_CFSR_UNDEFINSTR
#define SCB_CFSR_INVSTATE_Pos    (17U)
#define SCB_CFSR_INVSTATE        (1UL << SCB_CFSR_INVSTATE_Pos)
#define SCB_CFSR_INVSTATE_Msk    SCB_CFSR_INVSTATE
#define SCB_CFSR_INVPC_Pos       (18U)
#define SCB_CFSR_INVPC           (1UL << SCB_CFSR_INVPC_Pos)
#define SCB_CFSR_INVPC_Msk       SCB_CFSR_INVPC
#define SCB_CFSR_NOCP_Pos        (19U)
#define SCB_CFSR_NOCP            (1UL << SCB_CFSR_NOCP_Pos)
#define SCB_CFSR_NOCP_Msk        SCB_CFSR_NOCP
#define SCB_CFSR_UNALIGNED_Pos   (24U)
#define SCB_CFSR_UNALIGNED       (1UL << SCB_CFSR_UNALIGNED_Pos)
#define SCB_CFSR_UNALIGNED_Msk   SCB_CFSR_UNALIGNED
#define SCB_CFSR_DIVBYZERO_Pos   (25U)
#define SCB_CFSR_DIVBYZERO       (1UL << SCB_CFSR_DIVBYZERO_Pos)
#define SCB_CFSR_DIVBYZERO_Msk   SCB_CFSR_DIVBYZERO

/* ====================================================================== */
/* §RTC (CpuDriver post-mortem record via backup registers; RtcDriver GW  */
/* via TR/DR/CR/ISR/PRER/WPR + full backup-register range)                */
/* ====================================================================== */

/* Full register set: only the fields RtcDriver reads or writes, per the
 * same minimal-mock philosophy as stm32f469xx.h's RTC_TypeDef. Backup
 * registers are individually named (BKP0R..BKP31R, L475's full 32-register
 * range per rtc-driver.md §4.4) rather than using an array-tail shortcut,
 * matching the real CMSIS header's own naming exactly (also how CpuDriver's
 * existing BKP0R..BKP16R panic-record code already references them by
 * name). They are guaranteed contiguous (uniform uint32_t, no padding), so
 * RtcDriver's `&RTC->BKP0R + idx` pointer-arithmetic access works correctly
 * over the whole range. */
typedef struct
{
    volatile uint32_t TR;     /**< Time register. */
    volatile uint32_t DR;     /**< Date register. */
    volatile uint32_t CR;     /**< Control register. */
    volatile uint32_t ISR;    /**< Init / status register. */
    volatile uint32_t PRER;   /**< Prescaler register. */
    volatile uint32_t WPR;    /**< Write-protection register. */
    volatile uint32_t BKP0R;  /**< Backup register 0  — panic magic.      */
    volatile uint32_t BKP1R;  /**< Backup register 1  — CFSR.             */
    volatile uint32_t BKP2R;  /**< Backup register 2  — HFSR.             */
    volatile uint32_t BKP3R;  /**< Backup register 3  — BFAR/MMFAR.      */
    volatile uint32_t BKP4R;  /**< Backup register 4  — stacked PC.       */
    volatile uint32_t BKP5R;  /**< Backup register 5  — stacked LR.       */
    volatile uint32_t BKP6R;  /**< Backup register 6  — stacked SP.       */
    volatile uint32_t BKP7R;  /**< Backup register 7  — xPSR.             */
    volatile uint32_t BKP8R;  /**< Backup register 8  — R0.               */
    volatile uint32_t BKP9R;  /**< Backup register 9  — R1.               */
    volatile uint32_t BKP10R; /**< Backup register 10 — R2.               */
    volatile uint32_t BKP11R; /**< Backup register 11 — R3.               */
    volatile uint32_t BKP12R; /**< Backup register 12 — R12.              */
    volatile uint32_t BKP13R; /**< Backup register 13 — panic source.     */
    volatile uint32_t BKP14R; /**< Backup register 14 — reason DJB2 hash. */
    volatile uint32_t BKP15R; /**< Backup register 15 — assert line.      */
    volatile uint32_t BKP16R; /**< Backup register 16 — uptime ms.        */
    volatile uint32_t BKP17R;
    volatile uint32_t BKP18R;
    volatile uint32_t BKP19R;
    volatile uint32_t BKP20R;
    volatile uint32_t BKP21R;
    volatile uint32_t BKP22R;
    volatile uint32_t BKP23R;
    volatile uint32_t BKP24R;
    volatile uint32_t BKP25R;
    volatile uint32_t BKP26R;
    volatile uint32_t BKP27R;
    volatile uint32_t BKP28R;
    volatile uint32_t BKP29R;
    volatile uint32_t BKP30R;
    volatile uint32_t BKP31R; /**< Backup register 31 — last on L475 (RtcDriver, RTC_BACKUP_MAX_IDX_L475). */
} RTC_TypeDef;

extern RTC_TypeDef g_mock_rtc;

#define RTC (&g_mock_rtc)

/* --- RTC_ISR bits (RtcDriver) ------------------------------------------ */
#define RTC_ISR_INITS_Pos (4U)
#define RTC_ISR_INITS (1UL << RTC_ISR_INITS_Pos)
#define RTC_ISR_RSF_Pos (5U)
#define RTC_ISR_RSF (1UL << RTC_ISR_RSF_Pos)
#define RTC_ISR_INITF_Pos (6U)
#define RTC_ISR_INITF (1UL << RTC_ISR_INITF_Pos)
#define RTC_ISR_INIT_Pos (7U)
#define RTC_ISR_INIT (1UL << RTC_ISR_INIT_Pos)

/* --- RTC_CR bits (RtcDriver) -------------------------------------------- */
#define RTC_CR_FMT_Pos (6U)
#define RTC_CR_FMT (1UL << RTC_CR_FMT_Pos)

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
/* §USART — L4 family (ModbusUartDriver GW via UART4; CpuDriver panic path */
/* and DebugUartDriver GW both via USART1 — see debug_uart.c header note   */
/* on the shared-peripheral relationship with CpuDriver's panic output.)   */
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
extern USART_L4_TypeDef g_mock_usart1;

#define UART4  (&g_mock_uart4)
#define USART1 (&g_mock_usart1)

/* --- USART_CR1 bits (L4) — UE at bit 0 (differs from F4 bit 13) ------- */
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

/* --- USART_CR3 bits (L4) --------------------------------------------- */
#define USART_CR3_DEM_Pos (14U)
#define USART_CR3_DEM     (1UL << USART_CR3_DEM_Pos)

/* --- USART_ISR bits (L4 status register) ----------------------------- */
#define USART_ISR_PE_Pos   (0U)
#define USART_ISR_PE       (1UL << USART_ISR_PE_Pos)
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

/* --- USART_ICR bits (L4 clear register) ------------------------------ */
#define USART_ICR_PECF_Pos   (0U)
#define USART_ICR_PECF       (1UL << USART_ICR_PECF_Pos)
#define USART_ICR_FECF_Pos   (1U)
#define USART_ICR_FECF       (1UL << USART_ICR_FECF_Pos)
#define USART_ICR_NECF_Pos   (2U)
#define USART_ICR_NECF       (1UL << USART_ICR_NECF_Pos)
#define USART_ICR_ORECF_Pos  (3U)
#define USART_ICR_ORECF      (1UL << USART_ICR_ORECF_Pos)
#define USART_ICR_IDLECF_Pos (4U)
#define USART_ICR_IDLECF     (1UL << USART_ICR_IDLECF_Pos)

/* --- Debug-UART line endings (consumed by DebugUartDriver GW) --------- */
#define DEBUG_UART_CR ((uint8_t) '\r')
#define DEBUG_UART_LF ((uint8_t) '\n')

/* ====================================================================== */
/* §I2C (I2cDriver — L475 I2C v2, peripheral I2C2)                        */
/* ====================================================================== */

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
/* §SPI — SPI3 (SpiDriver GW, ISM43362 WiFi module)                       */
/* ====================================================================== */

typedef struct
{
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SR;
    volatile uint32_t DR;
} SPI_TypeDef;

extern SPI_TypeDef g_mock_spi3;

#define SPI3 (&g_mock_spi3)

/* --- SPI_CR1 bits ------------------------------------------------------ */
#define SPI_CR1_CPHA_Pos (0U)
#define SPI_CR1_CPHA     (1UL << SPI_CR1_CPHA_Pos)
#define SPI_CR1_CPOL_Pos (1U)
#define SPI_CR1_CPOL     (1UL << SPI_CR1_CPOL_Pos)
#define SPI_CR1_MSTR_Pos (2U)
#define SPI_CR1_MSTR     (1UL << SPI_CR1_MSTR_Pos)
#define SPI_CR1_BR_Pos   (3U)
#define SPI_CR1_BR       (0x7UL << SPI_CR1_BR_Pos)
#define SPI_CR1_SPE_Pos  (6U)
#define SPI_CR1_SPE      (1UL << SPI_CR1_SPE_Pos)
#define SPI_CR1_SSI_Pos  (8U)
#define SPI_CR1_SSI      (1UL << SPI_CR1_SSI_Pos)
#define SPI_CR1_SSM_Pos  (9U)
#define SPI_CR1_SSM      (1UL << SPI_CR1_SSM_Pos)

/* --- SPI_CR2 bits ------------------------------------------------------ */
#define SPI_CR2_DS_Pos     (8U)
#define SPI_CR2_DS         (0xFUL << SPI_CR2_DS_Pos)
#define SPI_CR2_FRXTH_Pos  (12U)
#define SPI_CR2_FRXTH      (1UL << SPI_CR2_FRXTH_Pos)

/* --- SPI_SR bits -------------------------------------------------------- */
#define SPI_SR_RXNE_Pos (0U)
#define SPI_SR_RXNE     (1UL << SPI_SR_RXNE_Pos)
#define SPI_SR_TXE_Pos  (1U)
#define SPI_SR_TXE      (1UL << SPI_SR_TXE_Pos)
#define SPI_SR_BSY_Pos  (7U)
#define SPI_SR_BSY      (1UL << SPI_SR_BSY_Pos)

/* ====================================================================== */
/* §SYSCFG (ExtiDriver — L4)                                               */
/* ====================================================================== */

/* Fields: MEMRMP, EXTICR[4]. SCSR/CFGR2 exist on real hardware but are not
 * touched by ExtiDriver. */
typedef struct
{
    volatile uint32_t MEMRMP;
    volatile uint32_t EXTICR[4U];
} SYSCFG_TypeDef;

extern SYSCFG_TypeDef g_mock_syscfg_l4;

#define SYSCFG (&g_mock_syscfg_l4)

/* ====================================================================== */
/* §EXTI (ExtiDriver — L4 multi-bank field names: IMR1/RTSR1/FTSR1/PR1)    */
/* ====================================================================== */

typedef struct
{
    volatile uint32_t IMR1;
    volatile uint32_t EMR1;
    volatile uint32_t RTSR1;
    volatile uint32_t FTSR1;
    volatile uint32_t SWIER1;
    volatile uint32_t PR1;
} EXTI_TypeDef;

extern EXTI_TypeDef g_mock_exti_l4;

#define EXTI (&g_mock_exti_l4)

/* ====================================================================== */
/* §NVIC — must stay last; extended per driver                            */
/* ====================================================================== */

typedef enum
{
    EXTI0_IRQn     = 6,  /* Per stm32l475xx.h CMSIS canonical value (ExtiDriver). */
    EXTI1_IRQn     = 7,  /* Per stm32l475xx.h CMSIS canonical value (ExtiDriver). */
    EXTI2_IRQn     = 8,  /* Per stm32l475xx.h CMSIS canonical value (ExtiDriver). */
    EXTI3_IRQn     = 9,  /* Per stm32l475xx.h CMSIS canonical value (ExtiDriver). */
    EXTI4_IRQn     = 10, /* Per stm32l475xx.h CMSIS canonical value (ExtiDriver). */
    EXTI9_5_IRQn   = 23, /* EXTI lines 5..9 shared vector (ExtiDriver). */
    USART1_IRQn    = 37, /* Per stm32l475xx.h CMSIS canonical value. */
    EXTI15_10_IRQn = 40, /* EXTI lines 10..15 shared vector (ExtiDriver). */
    UART4_IRQn     = 52  /* Per stm32l475xx.h CMSIS canonical value. */
} IRQn_Type;

#define NVIC_IRQ_COUNT_MAX (128U)

extern uint32_t g_mock_nvic_enable_count[NVIC_IRQ_COUNT_MAX];
extern uint32_t g_mock_nvic_disable_count[NVIC_IRQ_COUNT_MAX];
extern uint32_t g_mock_nvic_priority[NVIC_IRQ_COUNT_MAX];

void NVIC_EnableIRQ(IRQn_Type irqn);
void NVIC_DisableIRQ(IRQn_Type irqn);
void NVIC_SetPriority(IRQn_Type irqn, uint32_t priority);

/* ====================================================================== */
/* §CpuDriver hw-abstraction stubs (test builds only)                     */
/* Implementations and call counters live in stm32l475_cmsis_mock.c.      */
/* cpu_hw.h declares these same prototypes; including both headers is     */
/* safe because they are identical and guarded by #ifdef TEST.            */
/* ====================================================================== */

void cpu_hw_disable_irq(void);
void cpu_hw_breakpoint(void);
void cpu_hw_system_reset(void);

extern uint32_t g_cpu_hw_disable_irq_count;
extern uint32_t g_cpu_hw_breakpoint_count;
extern uint32_t g_cpu_hw_system_reset_count;

#endif /* STM32L475XX_H */
